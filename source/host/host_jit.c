#include "jit.h"

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
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

/*
 * Some titles tag function pointers with bit 30 and clear it before calling.
 * That only works while executable code stays below 0xc0000000: with a higher
 * address the tag either collides with the host address or overflows when the
 * game sets it. mmap(NULL) puts the JIT near the top of the 32-bit address
 * space, so ask for the range the Vita image would normally occupy first.
 */
#define JIT_MAX_ADDR 0xc0000000u
#define JIT_TAG_MASK 0x40000000u

static void *jit_map(size_t size, int prot, int flags, int fd)
{
    static const uintptr_t hints[] = {
        0x90000000u, 0x94000000u, 0x98000000u, 0x9c000000u,
        0xa0000000u, 0xa4000000u, 0xa8000000u, 0xac000000u,
        0xb0000000u, 0xb4000000u, 0xb8000000u, 0xbc000000u,
    };

    for (size_t i = 0; i < sizeof(hints) / sizeof(hints[0]); i++) {
        void *mapped;

        if (size > (size_t)(JIT_MAX_ADDR - hints[i]))
            continue;

        mapped = mmap((void *)hints[i], size, prot, flags, fd, 0);
        if (mapped == MAP_FAILED)
            continue;
        if (((uintptr_t)mapped & JIT_TAG_MASK) == 0)
            return mapped;
        munmap(mapped, size);
    }

    {
        void *mapped = mmap(NULL, size, prot, flags, fd, 0);

        if (mapped != MAP_FAILED && ((uintptr_t)mapped & JIT_TAG_MASK) != 0)
            fprintf(stderr,
                    "vita2native: JIT mapping at %p has bit 30 set; tagged pointers may fail\n",
                    mapped);
        return mapped;
    }
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
        void *rw = jit_map(size, PROT_READ | PROT_WRITE, MAP_SHARED, fd);
        void *rx = jit_map(size, PROT_READ | PROT_EXEC, MAP_SHARED, fd);

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
    jit->rw = jit_map(size, PROT_READ | PROT_WRITE | PROT_EXEC,
                      MAP_PRIVATE | MAP_ANONYMOUS, -1);
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
