#ifndef VITA2NATIVE_MSPACE_H
#define VITA2NATIVE_MSPACE_H

#include <stddef.h>

/*
 * Wrapper over the vendored dlmalloc (external/dlmalloc/malloc.c) mspace
 * allocator.
 *
 * dlmalloc is the implementation newlib's `mspace_*` was derived from, so the
 * chunk layout is exactly what titles that link their own dlmalloc (Unity's
 * IL2CPP runtime does) expect:
 *
 *   mem - 8   prev_foot   size of the preceding chunk (valid when !PINUSE)
 *   mem - 4   head        size | PINUSE | CINUSE
 *   mem + 0   payload     (free chunks store their bin links here)
 *
 * The handle owns only the region handed to vita_mspace_create(); it never
 * falls back to the host heap, because the guest's pointers must stay inside
 * the memory it reserved.
 */

typedef void *VitaMspace;

/* `base`/`size` describe the region to manage. Returns NULL if the region is
   too small or the allocation of the handle fails. */
VitaMspace vita_mspace_create(void *base, size_t size);
void vita_mspace_destroy(VitaMspace mspace);

void *vita_mspace_malloc(VitaMspace mspace, size_t size);
void vita_mspace_free(VitaMspace mspace, void *ptr);
void *vita_mspace_realloc(VitaMspace mspace, void *ptr, size_t size);
void *vita_mspace_memalign(VitaMspace mspace, size_t alignment, size_t size);
void *vita_mspace_reallocalign(VitaMspace mspace, void *ptr, size_t size, size_t alignment);
size_t vita_mspace_usable_size(VitaMspace mspace, const void *ptr);

/* True when nothing is allocated from the mspace. */
int vita_mspace_is_empty(VitaMspace mspace);

/* Footprint in bytes (region size for a base-backed mspace). */
size_t vita_mspace_footprint(VitaMspace mspace);

#endif
