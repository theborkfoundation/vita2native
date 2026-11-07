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
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../external/dlmalloc/malloc.c"

#include "mspace.h"

/*
 * A dlmalloc mspace handle is the `malloc_state` living at the start of the
 * guest-supplied region, not an opaque host pointer. Titles that link their
 * own dlmalloc (Unity) cast the handle back to `mstate` and use it inline, so
 * returning anything else corrupts their heap bookkeeping. Keep our own
 * metadata (region bounds, live count) in a side registry keyed by the handle.
 */
typedef struct VitaMspaceImpl {
    mspace handle;
    size_t live;
    void *base;
    size_t size;
    struct VitaMspaceImpl *next;
} VitaMspaceImpl;

static pthread_mutex_t g_mspaces_lock = PTHREAD_MUTEX_INITIALIZER;
static VitaMspaceImpl *g_mspaces;

static int mspace_debug(void)
{
    static int enabled = -1;

    if (enabled < 0)
        enabled = getenv("VITA2NATIVE_MSPACE_DEBUG") != NULL;
    return enabled;
}

static VitaMspaceImpl *mspace_lookup(mspace handle)
{
    VitaMspaceImpl *found = NULL;

    pthread_mutex_lock(&g_mspaces_lock);
    for (VitaMspaceImpl *it = g_mspaces; it; it = it->next) {
        if (it->handle == handle) {
            found = it;
            break;
        }
    }
    pthread_mutex_unlock(&g_mspaces_lock);

    return found;
}

static void mspace_register(VitaMspaceImpl *impl)
{
    pthread_mutex_lock(&g_mspaces_lock);
    impl->next = g_mspaces;
    g_mspaces = impl;
    pthread_mutex_unlock(&g_mspaces_lock);
}

static void mspace_unregister(VitaMspaceImpl *impl)
{
    pthread_mutex_lock(&g_mspaces_lock);
    for (VitaMspaceImpl **link = &g_mspaces; *link; link = &(*link)->next) {
        if (*link == impl) {
            *link = impl->next;
            break;
        }
    }
    pthread_mutex_unlock(&g_mspaces_lock);
}

static void debug_chunk(const char *what, VitaMspaceImpl *impl, void *ptr)
{
    mstate ms = (mstate)impl->handle;
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

static void mspace_note_alloc(mspace handle, int delta)
{
    VitaMspaceImpl *impl = mspace_lookup(handle);

    if (impl)
        __atomic_add_fetch(&impl->live, delta, __ATOMIC_RELAXED);
}

VitaMspace vita_mspace_create(void *base, size_t size)
{
    VitaMspaceImpl *impl;

    if (!base || size == 0)
        return NULL;

    impl = calloc(1, sizeof(*impl));
    if (!impl)
        return NULL;

    impl->handle = create_mspace_with_base(base, size, 0);
    if (!impl->handle) {
        free(impl);
        return NULL;
    }

    impl->base = base;
    impl->size = size;
    mspace_register(impl);

    return impl->handle;
}

void vita_mspace_destroy(VitaMspace mspace)
{
    VitaMspaceImpl *impl;

    if (!mspace)
        return;

    impl = mspace_lookup(mspace);
    if (impl) {
        mspace_unregister(impl);
        destroy_mspace(impl->handle);
        free(impl);
        return;
    }

    destroy_mspace(mspace);
}

void *vita_mspace_malloc(VitaMspace mspace, size_t size)
{
    void *block;

    if (!mspace)
        return NULL;

    block = mspace_malloc(mspace, size);
    if (block)
        mspace_note_alloc(mspace, 1);

    return block;
}

void vita_mspace_free(VitaMspace mspace, void *ptr)
{
    VitaMspaceImpl *impl;

    if (!mspace || !ptr)
        return;

    impl = mspace_lookup(mspace);
    if (impl)
        debug_chunk("free", impl, ptr);

    mspace_free(mspace, ptr);
    mspace_note_alloc(mspace, -1);
}

void *vita_mspace_realloc(VitaMspace mspace, void *ptr, size_t size)
{
    if (!mspace)
        return NULL;
    if (!ptr)
        return vita_mspace_malloc(mspace, size);

    return mspace_realloc(mspace, ptr, size);
}

void *vita_mspace_memalign(VitaMspace mspace, size_t alignment, size_t size)
{
    void *block;

    if (!mspace)
        return NULL;

    block = mspace_memalign(mspace, alignment, size);
    if (block)
        mspace_note_alloc(mspace, 1);

    return block;
}

void *vita_mspace_reallocalign(VitaMspace mspace, void *ptr, size_t size, size_t alignment)
{
    void *fresh;

    if (!mspace)
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
    VitaMspaceImpl *impl = mspace_lookup(mspace);

    return impl ? impl->live == 0 : 1;
}

size_t vita_mspace_footprint(VitaMspace mspace)
{
    return mspace ? mspace_footprint(mspace) : 0;
}
