#ifndef SCE_SYSMEM_H
#define SCE_SYSMEM_H

#include <psp2/kernel/sysmem.h>
#include <psp2/types.h>

#include "gfx.h"

typedef struct VitaMemBlockInfo {
    uint32_t index;
    SceUID uid;
    char name[32];
    void *base;
    uint32_t size;
    uint32_t mapped_size;
    SceKernelMemBlockType type;
    HostGpuMemBlock gpu_memblock;
} VitaMemBlockInfo;

int SceSysmem_init(HostGpuDevice device);
SceUID SceSysmem_get_next_uid(void);
VitaMemBlockInfo *SceSysmem_get_vita_memblock_info_for_addr(const void *addr);

#endif
