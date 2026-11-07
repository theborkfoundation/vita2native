#include <m-dict.h>
#include <psp2/kernel/error.h>
#include <psp2/kernel/sysmem.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "modules/SceSysmem.h"
#include "gfx.h"
#include "host.h"
#include "log.h"
#include "module.h"
#include "util.h"

static _Atomic SceUID g_last_uid = 1;

#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE 0x100000
#endif

/*
 * Vita user memory lives in 0x81000000-0xBFFFFFFF. Titles assume their memory
 * blocks are in that range (some pack the block id into the address bits or
 * validate pointers against it), so map blocks there instead of wherever the
 * host allocator happens to put them.
 */
#define VITA_MEMBLOCK_BASE 0x8a000000u
#define VITA_MEMBLOCK_LIMIT 0xb0000000u

static _Atomic uintptr_t g_memblock_hint = VITA_MEMBLOCK_BASE;

static void *alloc_vita_range(size_t size, size_t alignment)
{
    size_t page = (size_t)sysconf(_SC_PAGESIZE);
    size_t length = (size + page - 1) & ~(page - 1);

    if (page == 0)
        page = 4096;

    for (int attempt = 0; attempt < 64; attempt++) {
        uintptr_t hint = (atomic_fetch_add(&g_memblock_hint, length + alignment) + alignment - 1) &
                         ~(uintptr_t)(alignment - 1);
        void *mapped;

        if (hint < VITA_MEMBLOCK_BASE || hint + length > VITA_MEMBLOCK_LIMIT)
            return NULL;

        mapped = mmap((void *)hint, length, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
        if (mapped == (void *)hint)
            return mapped;
        if (mapped != MAP_FAILED)
            munmap(mapped, length);
    }

    return NULL;
}

DICT_DEF2(vita_memblock_info_dict, SceUID, M_DEFAULT_OPLIST, VitaMemBlockInfo *, M_POD_OPLIST)
static vita_memblock_info_dict_t g_vita_memblock_infos;
static RwLock g_vita_memblock_infos_lock;

static VitaMemBlockInfo *get_memblock_info_for_uid(SceUID uid)
{
    VitaMemBlockInfo *block;

    VitaMemBlockInfo **slot;

    rwlockReadLock(&g_vita_memblock_infos_lock);
    slot = vita_memblock_info_dict_get(g_vita_memblock_infos, uid);
    block = slot ? *slot : NULL;
    rwlockReadUnlock(&g_vita_memblock_infos_lock);

    return block;
}

EXPORT(SceSysmem, 0xB9D5EBDE, SceUID, sceKernelAllocMemBlock, const char *name,
       SceKernelMemBlockType type, SceSize size, SceKernelAllocMemBlockOpt *opt)
{
    VitaMemBlockInfo *block;
    uint32_t alignment;
    void *base;

    LOG("sceKernelAllocMemBlock: name: %s, size: 0x%x, type: 0x%x, align: 0x%x", name, size, type,
        opt ? opt->alignment : 0);

    block = malloc(sizeof(*block));
    if (!block)
        return SCE_KERNEL_ERROR_NO_MEMORY;
    memset(block, 0, sizeof(*block));

    if (type == SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW)
        alignment = 256 * 1024;
    else if (type == SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_PHYCONT_RW ||
             type == SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_PHYCONT_NC_RW)
        alignment = 1024 * 1024;
    else
        alignment = 4 * 1024;

    if (opt && (opt->attr & SCE_KERNEL_ALLOC_MEMBLOCK_ATTR_HAS_ALIGNMENT) &&
        opt->alignment > alignment)
        alignment = opt->alignment;

    /* The firmware aligns a block to the largest power of two dividing its
       size; engines rely on this when carving pools out of a block. */
    if ((size & (~size + 1)) > alignment)
        alignment = size & (~size + 1);

    if (alignment & (alignment - 1)) {
        uint32_t rounded = 4096;

        while (rounded < alignment)
            rounded <<= 1;
        alignment = rounded;
    }

    base = alloc_vita_range(size, alignment);
    if (base) {
        block->mapped_size = (uint32_t)(((size + 4095) & ~4095u));
    } else if (posix_memalign(&base, alignment, ALIGN(size, alignment)) == 0) {
        block->mapped_size = 0;
    } else {
        base = NULL;
    }
    if (!base) {
        free(block);
        return SCE_KERNEL_ERROR_NO_MEMORY;
    }

    block->uid = SceSysmem_get_next_uid();
    if (name)
        strncpy(block->name, name, sizeof(block->name) - 1);
    block->base = base;
    block->size = size;
    block->type = type;
    block->gpu_memblock = host_gpu_memblock_create(block->base, block->size);
    if (!block->gpu_memblock) {
        if (block->mapped_size)
            munmap(block->base, block->mapped_size);
        else
            free(block->base);
        free(block);
        return SCE_KERNEL_ERROR_NO_MEMORY;
    }

    rwlockWriteLock(&g_vita_memblock_infos_lock);
    vita_memblock_info_dict_set_at(g_vita_memblock_infos, block->uid, block);
    rwlockWriteUnlock(&g_vita_memblock_infos_lock);

    LOG("sceKernelAllocMemBlock: \"%s\" -> 0x%x base=%p size=0x%x align=0x%x", name,
        block->uid, block->base, size, alignment);

    return block->uid;
}

EXPORT(SceSysmem, 0xA91E15EE, int, sceKernelFreeMemBlock, SceUID uid)
{
    VitaMemBlockInfo *block = get_memblock_info_for_uid(uid);

    if (!block)
        return SCE_KERNEL_ERROR_INVALID_UID;

    host_gpu_memblock_destroy(block->gpu_memblock);
    if (block->mapped_size)
        munmap(block->base, block->mapped_size);
    else
        free(block->base);
    rwlockWriteLock(&g_vita_memblock_infos_lock);
    vita_memblock_info_dict_erase(g_vita_memblock_infos, uid);
    rwlockWriteUnlock(&g_vita_memblock_infos_lock);

    return 0;
}

EXPORT(SceSysmem, 0xB8EF5818, int, sceKernelGetMemBlockBase, SceUID uid, void **base)
{
    VitaMemBlockInfo *block = get_memblock_info_for_uid(uid);

    if (!block)
        return SCE_KERNEL_ERROR_INVALID_UID;

    if (!base)
        return SCE_KERNEL_ERROR_ILLEGAL_ADDR;

    *base = block->base;

    return 0;
}

DECLARE_LIBRARY(SceSysmem, 0x37fe725a);

int SceSysmem_init(HostGpuDevice gpu_device)
{
    (void)gpu_device;
    vita_memblock_info_dict_init(g_vita_memblock_infos);
    rwlockInit(&g_vita_memblock_infos_lock);

    return 0;
}

SceUID SceSysmem_get_next_uid(void)
{
    return atomic_fetch_add(&g_last_uid, 1);
}

VitaMemBlockInfo *SceSysmem_get_vita_memblock_info_for_addr(const void *addr)
{
    VitaMemBlockInfo *block = NULL;
    vita_memblock_info_dict_it_t it;
    struct vita_memblock_info_dict_pair_s *pair;

    rwlockReadLock(&g_vita_memblock_infos_lock);
    for (vita_memblock_info_dict_it(it, g_vita_memblock_infos); !vita_memblock_info_dict_end_p(it);
         vita_memblock_info_dict_next(it)) {
        pair = vita_memblock_info_dict_ref(it);
        if (addr >= pair->value->base && addr < (pair->value->base + pair->value->size)) {
            block = pair->value;
            break;
        }
    }
    rwlockReadUnlock(&g_vita_memblock_infos_lock);

    return block;
}

EXPORT(SceSysmem, 0xD0D4F729, int, sceKernelGetModel, void)
{
    return SCE_KERNEL_MODEL_VITA;
}

EXPORT(SceSysmem, 0xA2CB322F, int, sceKernelGetModelForCDialog, void)
{
    return SCE_KERNEL_MODEL_VITA;
}

EXPORT(SceSysmem, 0x1453A5E5, int, sceKernelIsPSVitaTV, void)
{
    return 0;
}

EXPORT(SceSysmem, 0x0144FBD9, int, sceKernelCheckModelCapability, int mask)
{
    (void)mask;

    return 0;
}

EXPORT(SceSysmem, 0x87CC580B, int, sceKernelGetFreeMemorySize, SceKernelFreeMemorySizeInfo *info)
{
    if (!info)
        return SCE_KERNEL_ERROR_ILLEGAL_ADDR;

    info->size = sizeof(*info);
    /* Report what a Vita would, so titles that size pools from this keep a
       sensible layout even though the host has more memory. */
    info->size_user = 0x6000000;
    info->size_cdram = 0x1000000;
    info->size_phycont = 0x800000;

    return 0;
}

EXPORT(SceSysmem, 0x8EB8DFBB, int, sceKernelOpenMemBlock, const char *name, int flags)
{
    VitaMemBlockInfo *block = NULL;
    vita_memblock_info_dict_it_t it;

    (void)flags;
    if (!name)
        return SCE_KERNEL_ERROR_ILLEGAL_ADDR;

    rwlockReadLock(&g_vita_memblock_infos_lock);
    for (vita_memblock_info_dict_it(it, g_vita_memblock_infos);
         !vita_memblock_info_dict_end_p(it); vita_memblock_info_dict_next(it)) {
        VitaMemBlockInfo *candidate = vita_memblock_info_dict_ref(it)->value;

        if (strncmp(candidate->name, name, sizeof(candidate->name)) == 0) {
            block = candidate;
            break;
        }
    }
    rwlockReadUnlock(&g_vita_memblock_infos_lock);

    return block ? (int)block->uid : SCE_KERNEL_ERROR_INVALID_UID;
}

EXPORT(SceSysmem, 0xB680E3A0, int, sceKernelCloseMemBlock, SceUID uid)
{
    return get_memblock_info_for_uid(uid) ? 0 : SCE_KERNEL_ERROR_INVALID_UID;
}

EXPORT(SceSysmem, 0xA33B99D1, SceUID, sceKernelFindMemBlockByAddr, const void *addr,
       SceSize size)
{
    VitaMemBlockInfo *block = SceSysmem_get_vita_memblock_info_for_addr(addr);

    if (!block)
        return SCE_KERNEL_ERROR_INVALID_UID;
    if (size && (uintptr_t)addr + size > (uintptr_t)block->base + block->size)
        return SCE_KERNEL_ERROR_INVALID_UID;

    return block->uid;
}

static int memblock_fill_info(VitaMemBlockInfo *block, SceKernelMemBlockInfo *info)
{
    if (!info)
        return SCE_KERNEL_ERROR_ILLEGAL_ADDR;

    memset(info, 0, sizeof(*info));
    info->size = sizeof(*info);
    info->mappedBase = block->base;
    info->mappedSize = block->size;
    info->memoryType = SCE_KERNEL_MEMORY_TYPE_NORMAL;
    info->access = SCE_KERNEL_MEMORY_ACCESS_R | SCE_KERNEL_MEMORY_ACCESS_W;
    info->type = block->type;

    return 0;
}

EXPORT(SceSysmem, 0x4010AD65, int, sceKernelGetMemBlockInfoByAddr, void *base,
       SceKernelMemBlockInfo *info)
{
    VitaMemBlockInfo *block = SceSysmem_get_vita_memblock_info_for_addr(base);

    return block ? memblock_fill_info(block, info) : SCE_KERNEL_ERROR_INVALID_UID;
}

EXPORT(SceSysmem, 0x006F3DB4, int, sceKernelGetMemBlockInfoByRange, void *base, SceSize size,
       SceKernelMemBlockInfo *info)
{
    VitaMemBlockInfo *block = SceSysmem_get_vita_memblock_info_for_addr(base);

    if (!block)
        return SCE_KERNEL_ERROR_INVALID_UID;
    if ((uintptr_t)base + size > (uintptr_t)block->base + block->size)
        return SCE_KERNEL_ERROR_INVALID_UID;

    return memblock_fill_info(block, info);
}

EXPORT(SceSysmem, 0xE2D7E137, SceUID, sceKernelAllocMemBlockForVM, const char *name, SceSize size)
{
    return sceKernelAllocMemBlock(name, SCE_KERNEL_MEMBLOCK_TYPE_USER_RW, size, NULL);
}

EXPORT(SceSysmem, 0x4EA13FEA, int, sceKernelFreeMemBlockForVM, SceUID uid)
{
    return sceKernelFreeMemBlock(uid);
}

EXPORT(SceSysmem, 0x19D2A81A, int, sceKernelSyncVMDomain, SceUID uid, void *data, SceSize size)
{
    VitaMemBlockInfo *block = get_memblock_info_for_uid(uid);

    (void)data;
    (void)size;
    return block ? 0 : SCE_KERNEL_ERROR_INVALID_UID;
}

EXPORT(SceSysmem, 0x9CA3EB2B, int, sceKernelOpenVMDomain, void)
{
    return 0;
}

EXPORT(SceSysmem, 0xD6CA56CA, int, sceKernelCloseVMDomain, void)
{
    return 0;
}

/*
 * Buddy-allocator and sub-budget introspection. The signatures are not public
 * and titles only use them for diagnostics, so report the feature as invalid
 * rather than writing an unknown output layout.
 */
EXPORT(SceSysmem, 0xEC636BCB, int, sceKernelAllocUnmapMemBlock, void)
{
    return SCE_KERNEL_ERROR_INVALID_ARGUMENT;
}

EXPORT(SceSysmem, 0x832B4A65, int, sceKernelGetSubbudgetInfo, void)
{
    return SCE_KERNEL_ERROR_INVALID_ARGUMENT;
}
