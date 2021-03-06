#include <stdlib.h>
#include <string.h>
#include "debug.h"
#include "kernel/task.h"
#include "kernel/errno.h"
#include "kernel/resource.h"
#include "kernel/fs.h"
#include "fs/poll.h"
#include "fs/fd.h"

struct fd *fd_create() {
    struct fd *fd = malloc(sizeof(struct fd));
    *fd = (struct fd) {};
    fd->refcount = 1;
    fd->flags = 0;
    fd->mount = NULL;
    list_init(&fd->poll_fds);
    lock_init(&fd->poll_lock);
    lock_init(&fd->lock);
    cond_init(&fd->cond);
    return fd;
}

struct fd *fd_retain(struct fd *fd) {
    fd->refcount++;
    return fd;
}

int fd_close(struct fd *fd) {
    int err = 0;
    if (--fd->refcount == 0) {
        lock(&fd->poll_lock);
        struct poll_fd *poll_fd, *tmp;
        list_for_each_entry_safe(&fd->poll_fds, poll_fd, tmp, polls) {
            lock(&poll_fd->poll->lock);
            list_remove(&poll_fd->polls);
            list_remove(&poll_fd->fds);
            unlock(&poll_fd->poll->lock);
            free(poll_fd);
        }
        unlock(&fd->poll_lock);
        if (fd->ops->close)
            err = fd->ops->close(fd);
        free(fd);
    }
    return err;
}

static int fdtable_resize(struct fdtable *table, unsigned size);

struct fdtable *fdtable_new(unsigned size) {
    struct fdtable *fdt = malloc(sizeof(struct fdtable));
    if (fdt == NULL)
        return ERR_PTR(_ENOMEM);
    fdt->refcount = 1;
    fdt->size = 0;
    fdt->files = NULL;
    fdt->cloexec = NULL;
    lock_init(&fdt->lock);
    int err = fdtable_resize(fdt, size);
    if (err < 0) {
        free(fdt);
        return ERR_PTR(err);
    }
    return fdt;
}

static int fdtable_close(struct fdtable *table, fd_t f);

// FIXME this looks like it has the classic refcount UAF
void fdtable_release(struct fdtable *table) {
    lock(&table->lock);
    if (--table->refcount == 0) {
        for (fd_t f = 0; f < table->size; f++)
            fdtable_close(table, f);
        free(table->files);
        free(table->cloexec);
        free(table);
    } else {
        unlock(&table->lock);
    }
}

static int fdtable_resize(struct fdtable *table, unsigned size) {
    // currently the only legitimate use of this is to expand the table
    assert(size > table->size);

    struct fd **files = malloc(sizeof(struct fd *) * size);
    if (files == NULL)
        return _ENOMEM;
    memset(files, 0, sizeof(struct fd *) * size);
    if (table->files)
        memcpy(files, table->files, sizeof(struct fd *) * table->size);

    bits_t *cloexec = malloc(BITS_SIZE(size));
    if (cloexec == NULL) {
        free(files);
        return _ENOMEM;
    }
    memset(cloexec, 0, BITS_SIZE(size));
    if (table->cloexec)
        memcpy(cloexec, table->cloexec, BITS_SIZE(table->size));

    free(table->files);
    table->files = files;
    free(table->cloexec);
    table->cloexec = cloexec;
    table->size = size;
    return 0;
}

struct fdtable *fdtable_copy(struct fdtable *table) {
    lock(&table->lock);
    unsigned size = table->size;
    struct fdtable *new_table = fdtable_new(size);
    if (IS_ERR(new_table)) {
        unlock(&table->lock);
        return new_table;
    }
    memcpy(new_table->files, table->files, sizeof(struct fd *) * size);
    for (fd_t f = 0; f < size; f++)
        if (new_table->files[f])
            new_table->files[f]->refcount++;
    memcpy(new_table->cloexec, table->cloexec, BITS_SIZE(size));
    unlock(&table->lock);
    return new_table;
}

static int fdtable_expand(struct fdtable *table, fd_t max) {
    unsigned size = max + 1;
    if (table->size >= size)
        return 0;
    if (size > rlimit(RLIMIT_NOFILE_))
        return _EMFILE;
    return fdtable_resize(table, max + 1);
}

struct fd *fdtable_get(struct fdtable *table, fd_t f) {
    struct fd *fd = NULL;
    if (f < current->files->size)
        fd = table->files[f];
    return fd;
}

struct fd *f_get(fd_t f) {
    lock(&current->files->lock);
    struct fd *fd = fdtable_get(current->files, f);
    unlock(&current->files->lock);
    return fd;
}

static fd_t f_install_start(struct fd *fd, fd_t start) {
    struct fdtable *table = current->files;
    unsigned size = rlimit(RLIMIT_NOFILE_);
    if (size > table->size)
        size = table->size;

    fd_t f;
    for (f = start; f < size; f++)
        if (table->files[f] == NULL)
            break;
    if (f >= size) {
        int err = fdtable_expand(table, f);
        if (err < 0)
            f = err;
    }

    if (f >= 0) {
        table->files[f] = fd;
        bit_clear(f, table->cloexec);
    } else {
        fd_close(fd);
    }
    return f;
}

fd_t f_install(struct fd *fd, int flags) {
    lock(&current->files->lock);
    fd_t f = f_install_start(fd, 0);
    if (f >= 0) {
        if (flags & O_CLOEXEC_)
            bit_set(f, current->files->cloexec);
        if (flags & O_NONBLOCK_)
            fd->flags |= O_NONBLOCK_;
    }
    unlock(&current->files->lock);
    return f;
}

static int fdtable_close(struct fdtable *table, fd_t f) {
    struct fd *fd = fdtable_get(table, f);
    if (fd == NULL)
        return _EBADF;
    int err = fd_close(fd);
    table->files[f] = NULL;
    bit_clear(f, table->cloexec);
    return err;
}

int f_close(fd_t f) {
    lock(&current->files->lock);
    int err = fdtable_close(current->files, f);
    unlock(&current->files->lock);
    return err;
}

dword_t sys_close(fd_t f) {
    STRACE("close(%d)", f);
    return f_close(f);
}

void fdtable_do_cloexec(struct fdtable *table) {
    lock(&table->lock);
    for (fd_t f = 0; f < table->size; f++)
        if (bit_test(f, table->cloexec))
            fdtable_close(table, f);
    unlock(&table->lock);
}

#define F_DUPFD_ 0
#define F_GETFD_ 1
#define F_SETFD_ 2
#define F_GETFL_ 3
#define F_SETFL_ 4

dword_t sys_dup(fd_t f) {
    struct fd *fd = f_get(f);
    if (fd == NULL)
        return _EBADF;
    fd->refcount++;
    return f_install(fd, 0);
}

dword_t sys_dup2(fd_t f, fd_t new_f) {
    STRACE("dup2(%d, %d)\n", f, new_f);
    struct fdtable *table = current->files;
    struct fd *fd = f_get(f);
    if (fd == NULL)
        return _EBADF;
    int err = fdtable_expand(table, new_f);
    if (err < 0)
        return err;
    f_close(new_f);
    fd->refcount++;
    table->files[new_f] = fd;
    return new_f;
}

dword_t sys_fcntl64(fd_t f, dword_t cmd, dword_t arg) {
    struct fdtable *table = current->files;
    struct fd *fd = f_get(f);
    if (fd == NULL)
        return _EBADF;
    switch (cmd) {
        case F_DUPFD_:
            STRACE("fcntl(%d, F_DUPFD, %d)", f, arg);
            fd->refcount++;
            return f_install_start(fd, arg);

        case F_GETFD_:
            STRACE("fcntl(%d, F_GETFD)", f);
            return bit_test(f, table->cloexec);
        case F_SETFD_:
            STRACE("fcntl(%d, F_SETFD, 0x%x)", f, arg);
            if (arg & 1)
                bit_set(f, table->cloexec);
            else
                bit_clear(f, table->cloexec);
            return 0;

        case F_GETFL_:
            STRACE("fcntl(%d, F_GETFL)", f);
            if (fd->ops->getflags == NULL)
                return fd->flags;
            return fd->ops->getflags(fd);
        case F_SETFL_:
            STRACE("fcntl(%d, F_SETFL, %#x)", f, arg);
            if (fd->ops->setflags == NULL) {
                arg &= O_APPEND_ | O_NONBLOCK_;
                fd->flags = arg;
                return 0;
            }
            return fd->ops->setflags(fd, arg);

        default:
            STRACE("fcntl(%d, %d)", f, cmd);
            return _EINVAL;
    }
}

