/*
 * dlmalloc configuration.
 *
 * ONLY_MSPACES compiles out the global malloc/free/sbrk/mmap machinery and
 * leaves just the mspace API, which is what newlib and Sony's libc expose.
 * Locks are off because callers (Unity's MemoryManager) already hold their own
 * heap lock around these calls, and dlmalloc's per-mspace lock would only add
 * another acquisition inside it.
 */
#define ONLY_MSPACES 1
#define HAVE_MMAP 0
#define USE_LOCKS 0

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../external/dlmalloc/malloc.c"

#include "mspace.h"

typedef struct {
    mspace inner;
    size_t live;
    void *base;
    size_t size;
} VitaMspaceImpl;

static int mspace_debug(void)
{
    static int enabled = -1;

    if (enabled < 0)
        enabled = getenv("VITA2NATIVE_MSPACE_DEBUG") != NULL;
    return enabled;
}

static void debug_chunk(const char *what, VitaMspaceImpl *impl, void *ptr)
{
    mstate ms = (mstate)impl->inner;
    mchunkptr p;

    if (!mspace_debug())
        return;

    p = mem2chunk(ptr);
    fprintf(stderr,
            "mspace %s %p: region=%p..%p head=%08x size=%zu pinuse=%d cinuse=%d "
            "prev_foot=%08x next_head=%08x top=%p topsize=%zu\n",
            what, ptr, impl->base, (char *)impl->base + impl->size, p->head,
            (size_t)chunksize(p), (int)(pinuse(p) != 0), (int)(cinuse(p) != 0),
            (unsigned)p->prev_foot, (unsigned)next_chunk(p)->head, (void *)ms->top,
            (size_t)ms->topsize);
    fflush(stderr);
}

VitaMspace vita_mspace_create(void *base, size_t size)
{
    VitaMspaceImpl *impl;

    if (!base || size == 0)
        return NULL;

    impl = calloc(1, sizeof(*impl));
    if (!impl)
        return NULL;

    impl->inner = create_mspace_with_base(base, size, 0);
    if (!impl->inner) {
        free(impl);
        return NULL;
    }

    impl->base = base;
    impl->size = size;

    return impl;
}

void vita_mspace_destroy(VitaMspace mspace)
{
    VitaMspaceImpl *impl = mspace;

    if (!impl)
        return;

    destroy_mspace(impl->inner);
    free(impl);
}

void *vita_mspace_malloc(VitaMspace mspace, size_t size)
{
    VitaMspaceImpl *impl = mspace;
    void *block;

    if (!impl)
        return NULL;

    block = mspace_malloc(impl->inner, size);
    if (block)
        __atomic_add_fetch(&impl->live, 1, __ATOMIC_RELAXED);

    return block;
}

void vita_mspace_free(VitaMspace mspace, void *ptr)
{
    VitaMspaceImpl *impl = mspace;

    if (!impl || !ptr)
        return;

    debug_chunk("free", impl, ptr);
    mspace_free(impl->inner, ptr);
    __atomic_sub_fetch(&impl->live, 1, __ATOMIC_RELAXED);
}

void *vita_mspace_realloc(VitaMspace mspace, void *ptr, size_t size)
{
    VitaMspaceImpl *impl = mspace;
    void *block;

    if (!impl)
        return NULL;
    if (!ptr)
        return vita_mspace_malloc(mspace, size);

    block = mspace_realloc(impl->inner, ptr, size);
    return block;
}

void *vita_mspace_memalign(VitaMspace mspace, size_t alignment, size_t size)
{
    VitaMspaceImpl *impl = mspace;
    void *block;

    if (!impl)
        return NULL;

    block = mspace_memalign(impl->inner, alignment, size);
    if (block)
        __atomic_add_fetch(&impl->live, 1, __ATOMIC_RELAXED);

    return block;
}

void *vita_mspace_reallocalign(VitaMspace mspace, void *ptr, size_t size, size_t alignment)
{
    VitaMspaceImpl *impl = mspace;
    void *fresh;

    if (!impl)
        return NULL;
    if (alignment == 0)
        return vita_mspace_realloc(mspace, ptr, size);

    fresh = vita_mspace_memalign(mspace, alignment, size);
    if (!fresh)
        return NULL;

    if (ptr) {
        size_t old = mspace_usable_size(ptr);

        if (mspace_debug()) {
            fprintf(stderr, "mspace reallocalign %p (usable %zu) -> %p (align %zu, size %zu)\n",
                    ptr, old, fresh, alignment, size);
            fflush(stderr);
        }
        memcpy(fresh, ptr, old < size ? old : size);
        vita_mspace_free(mspace, ptr);
    }

    return fresh;
}

size_t vita_mspace_usable_size(VitaMspace mspace, const void *ptr)
{
    (void)mspace;

    return ptr ? mspace_usable_size(ptr) : 0;
}

int vita_mspace_is_empty(VitaMspace mspace)
{
    VitaMspaceImpl *impl = mspace;

    return impl ? impl->live == 0 : 1;
}

size_t vita_mspace_footprint(VitaMspace mspace)
{
    VitaMspaceImpl *impl = mspace;

    return impl ? mspace_footprint(impl->inner) : 0;
}
