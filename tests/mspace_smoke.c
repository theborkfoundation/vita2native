/* Exercises the vendored dlmalloc mspace wrapper: bounds, chunk headers,
   overlap, realloc/memalign behaviour and coalescing after freeing. */

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mspace.h"

#define REGION_SIZE (256 * 1024)
#define MAX_BLOCKS  512
#define PATTERN     0xa5

/* dlmalloc's chunk overhead is one size_t (the head word; prev_foot belongs
   to the previous chunk while this one is in use). MALLOC_ALIGNMENT is twice
   the word size on both 32- and 64-bit builds. */
#define CHUNK_OVERHEAD (sizeof(size_t))
#define CHUNK_ALIGN    (2 * sizeof(size_t))

typedef struct {
    void *ptr;
    size_t size;
    int seed;
    int live;
} Block;

static uint8_t *g_region;
static size_t g_region_size;
static Block g_blocks[MAX_BLOCKS];
static int g_num_blocks;

static size_t region_end_offset(const void *p, size_t n)
{
    uintptr_t start = (uintptr_t)g_region;
    uintptr_t address = (uintptr_t)p;

    assert(address >= start);
    assert(address + n <= start + g_region_size);
    assert((address & (CHUNK_ALIGN - 1)) == 0);

    return address - start;
}

static void check_chunk(const void *ptr, size_t requested)
{
    size_t head;
    size_t size;

    memcpy(&head, (const uint8_t *)ptr - CHUNK_OVERHEAD, sizeof(head));
    size = head & ~(size_t)7;

    /* dlmalloc chunk header: bit 0 PINUSE describes the previous physical
       chunk and is legitimately clear when that neighbour is free, so only
       bit 1 (CINUSE) must be set on an allocated chunk. The chunk includes a
       header word, so usable space is size - sizeof(size_t). */
    if (!(head & 2) || size < requested + CHUNK_OVERHEAD || (size & 7) != 0) {
        fprintf(stderr, "chunk: ptr=%p req=%zu head=%zx size=%zu\n", ptr, requested, head, size);
        abort();
    }
}

static void check_no_overlap(int ignore)
{
    for (int i = 0; i < g_num_blocks; i++) {
        uintptr_t a0, a1;

        if (!g_blocks[i].live || i == ignore)
            continue;

        a0 = (uintptr_t)g_blocks[i].ptr;
        a1 = a0 + g_blocks[i].size;

        for (int j = i + 1; j < g_num_blocks; j++) {
            uintptr_t b0, b1;

            if (!g_blocks[j].live || j == ignore)
                continue;

            b0 = (uintptr_t)g_blocks[j].ptr;
            b1 = b0 + g_blocks[j].size;

            assert(a1 <= b0 || b1 <= a0);
        }
    }
}

static void fill(Block *block, int seed)
{
    memset(block->ptr, PATTERN ^ seed, block->size);
}

static void verify(const Block *block, int seed)
{
    const uint8_t *p = block->ptr;

    for (size_t i = 0; i < block->size; i++)
        assert(p[i] == (uint8_t)(PATTERN ^ seed));
}

static void add_block(VitaMspace m, void *ptr, size_t size, int seed)
{
    Block *block;

    assert(ptr != NULL);
    region_end_offset(ptr, size);
    check_chunk(ptr, size);
    check_no_overlap(-1);

    assert(g_num_blocks < MAX_BLOCKS);
    block = &g_blocks[g_num_blocks++];
    block->ptr = ptr;
    block->size = size;
    block->seed = seed;
    block->live = 1;

    fill(block, seed);
    verify(block, seed);
}

int main(void)
{
    VitaMspace m;
    unsigned seed = 12345;
    size_t big;

    g_region_size = REGION_SIZE;
    g_region = malloc(REGION_SIZE);
    assert(g_region != NULL);

    m = vita_mspace_create(g_region, g_region_size);
    assert(m != NULL);
    assert(vita_mspace_is_empty(m));

    /* A spread of sizes, including the smallest and some odd ones. */
    for (int i = 0; i < 200; i++) {
        size_t size;

        seed = seed * 1103515245u + 12345u;
        size = 1 + (seed >> 8) % 512;

        add_block(m, vita_mspace_malloc(m, size), size, i);
        assert(vita_mspace_usable_size(m, g_blocks[i].ptr) >= size);
    }

    for (int i = 0; i < g_num_blocks; i++)
        verify(&g_blocks[i], i);

    /* Free a third of them and make sure reuse stays inside live blocks. */
    for (int i = 0; i < g_num_blocks; i += 3) {
        vita_mspace_free(m, g_blocks[i].ptr);
        g_blocks[i].live = 0;
    }

    for (int i = 0; i < 120; i++) {
        size_t size;

        seed = seed * 1103515245u + 12345u;
        size = 1 + (seed >> 8) % 300;
        add_block(m, vita_mspace_malloc(m, size), size, i + 1000);
    }

    for (int i = 0; i < g_num_blocks; i++) {
        if (g_blocks[i].live)
            verify(&g_blocks[i], g_blocks[i].seed);
    }

    /* Grow and shrink a few, checking that the contents survive. */
    for (int i = 1; i < 200; i += 17) {
        Block *block = &g_blocks[i];
        uint8_t *q;
        size_t new_size = block->size * 3 + 7;

        if (!block->live)
            continue;

        q = vita_mspace_realloc(m, block->ptr, new_size);
        assert(q != NULL);
        region_end_offset(q, new_size);
        check_chunk(q, new_size);

        for (size_t k = 0; k < block->size; k++)
            assert(q[k] == (uint8_t)(PATTERN ^ i));

        block->ptr = q;
        block->size = new_size;
        fill(block, block->seed);
    }

    /* Aligned allocations. */
    for (int i = 0; i < 64; i++) {
        size_t alignment = (size_t)16 << (i % 5);
        size_t size = 24 + (size_t)(i * 13);
        void *p = vita_mspace_memalign(m, alignment, size);

        assert(p != NULL);
        assert(((uintptr_t)p % alignment) == 0);
        add_block(m, p, size, i + 2000);
    }

    /* Realloc with alignment. */
    for (int i = 0; i < 8; i++) {
        Block *block = &g_blocks[g_num_blocks - 1 - i];
        void *p = vita_mspace_reallocalign(m, block->ptr, block->size + 40, 64);

        assert(p != NULL);
        assert(((uintptr_t)p % 64) == 0);
        region_end_offset(p, block->size + 40);
        block->ptr = p;
        block->size += 40;
        block->seed = i + 3000;
        fill(block, block->seed);
    }

    /* Free everything; the space must coalesce back into one usable block. */
    for (int i = 0; i < g_num_blocks; i++) {
        if (g_blocks[i].live) {
            vita_mspace_free(m, g_blocks[i].ptr);
            g_blocks[i].live = 0;
        }
    }

    assert(vita_mspace_is_empty(m));

    big = REGION_SIZE - 16384;
    {
        void *p = vita_mspace_malloc(m, big);

        assert(p != NULL);
        region_end_offset(p, big);
        memset(p, 0x5a, big);
        vita_mspace_free(m, p);
    }

    assert(vita_mspace_is_empty(m));

    vita_mspace_destroy(m);
    free(g_region);

    puts("mspace_smoke: ok");
    return 0;
}
