#include "jit.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#if defined(__linux__)
#include <sys/syscall.h>
#ifndef MFD_CLOEXEC
#define MFD_CLOEXEC 0x0001U
#endif
#endif

#ifndef MAP_ANONYMOUS
#define MAP_ANONYMOUS MAP_ANON
#endif

static size_t host_page_size(void)
{
    long page = sysconf(_SC_PAGESIZE);

    return page > 0 ? (size_t)page : 4096;
}

static size_t align_up(size_t value, size_t alignment)
{
    return (value + alignment - 1) & ~(alignment - 1);
}

Result jitCreate(Jit *jit, size_t size)
{
    size_t page_size;
    int fd = -1;

    (void)fd;

    if (!jit || size == 0)
        return -1;

    memset(jit, 0, sizeof(*jit));
    jit->fd = -1;

    page_size = host_page_size();
    size = align_up(size, page_size);
    jit->size = size;

#if defined(__linux__)
    fd = (int)syscall(SYS_memfd_create, "vita2native-jit", MFD_CLOEXEC);
    if (fd >= 0 && ftruncate(fd, (off_t)size) == 0) {
        void *rw = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        void *rx = mmap(NULL, size, PROT_READ | PROT_EXEC, MAP_SHARED, fd, 0);

        if (rw != MAP_FAILED && rx != MAP_FAILED) {
            jit->rw = rw;
            jit->rx = rx;
            jit->fd = fd;
            jit->dual_mapped = true;
            return 0;
        }

        if (rw != MAP_FAILED)
            munmap(rw, size);
        if (rx != MAP_FAILED)
            munmap(rx, size);
    }

    if (fd >= 0) {
        close(fd);
        fd = -1;
    }
#endif

    /*
     * Fallback for non-Linux development hosts and kernels without memfd.
     * This is a single mapping that we flip between writable and executable.
     */
    jit->rw = mmap(NULL, size, PROT_READ | PROT_WRITE | PROT_EXEC,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (jit->rw == MAP_FAILED) {
        jit->rw = NULL;
        return -1;
    }

    jit->rx = jit->rw;
    jit->dual_mapped = false;

    return 0;
}

void *jitGetRwAddr(Jit *jit)
{
    return jit ? jit->rw : NULL;
}

void *jitGetRxAddr(Jit *jit)
{
    return jit ? jit->rx : NULL;
}

Result jitTransitionToWritable(Jit *jit)
{
    if (!jit || !jit->rw)
        return -1;

    if (jit->dual_mapped) {
        jit->executable = false;
        return 0;
    }

    if (mprotect(jit->rw, jit->size, PROT_READ | PROT_WRITE) != 0)
        return -1;

    jit->executable = false;

    return 0;
}

Result jitTransitionToExecutable(Jit *jit)
{
    if (!jit || !jit->rw)
        return -1;

    __builtin___clear_cache((char *)jit->rw, (char *)jit->rw + jit->size);
    if (jit->rx && jit->rx != jit->rw)
        __builtin___clear_cache((char *)jit->rx, (char *)jit->rx + jit->size);

    if (jit->dual_mapped) {
        jit->executable = true;
        return 0;
    }

    if (mprotect(jit->rw, jit->size, PROT_READ | PROT_EXEC) != 0)
        return -1;

    jit->executable = true;

    return 0;
}

Result jitClose(Jit *jit)
{
    if (!jit)
        return -1;

    if (jit->rw && jit->rx && jit->rw != jit->rx)
        munmap(jit->rx, jit->size);

    if (jit->rw)
        munmap(jit->rw, jit->size);

    if (jit->fd >= 0)
        close(jit->fd);

    memset(jit, 0, sizeof(*jit));
    jit->fd = -1;

    return 0;
}
