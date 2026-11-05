#include <m-dict.h>
#include <psp2/kernel/error.h>
#include <psp2/kernel/sysmem.h>
#include <stdatomic.h>
#include <stdlib.h>

#include "modules/SceSysmem.h"
#include "gfx.h"
#include "host.h"
#include "log.h"
#include "module.h"
#include "util.h"

static _Atomic SceUID g_last_uid = 1;

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

    LOG("sceKernelAllocMemBlock: name: %s, size: 0x%x", name, size);

    block = malloc(sizeof(*block));
    if (!block)
        return SCE_KERNEL_ERROR_NO_MEMORY;

    if (type == SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW)
        alignment = 256 * 1024;
    else
        alignment = 4 * 1024;

    if (posix_memalign(&base, alignment, ALIGN(size, alignment)) != 0)
        base = NULL;
    if (!base) {
        free(block);
        return SCE_KERNEL_ERROR_NO_MEMORY;
    }

    memset(block, 0, sizeof(*block));
    block->uid = SceSysmem_get_next_uid();
    block->base = base;
    block->size = size;

    (void)type;
    block->gpu_memblock = host_gpu_memblock_create(block->base, block->size);
    if (!block->gpu_memblock) {
        free(block->base);
        free(block);
        return SCE_KERNEL_ERROR_NO_MEMORY;
    }

    rwlockWriteLock(&g_vita_memblock_infos_lock);
    vita_memblock_info_dict_set_at(g_vita_memblock_infos, block->uid, block);
    rwlockWriteUnlock(&g_vita_memblock_infos_lock);

    return block->uid;
}

EXPORT(SceSysmem, 0xA91E15EE, int, sceKernelFreeMemBlock, SceUID uid)
{
    VitaMemBlockInfo *block = get_memblock_info_for_uid(uid);

    if (!block)
        return SCE_KERNEL_ERROR_INVALID_UID;

    host_gpu_memblock_destroy(block->gpu_memblock);
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
