/*
 * SceKernelModulemgr: load and run user modules.
 *
 * Guest modules are ordinary SCE ELF files. The existing loader already knows
 * how to map, relocate and patch their imports; this module keeps each loaded
 * image's JIT and data allocations alive and exposes the start/stop lifecycle.
 *
 * Imports are resolved against the host against the host export table. Exports
 * of loaded modules are not registered yet, so this covers the common addon
 * case (a PRX that only consumes firmware libraries).
 */

#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <psp2/kernel/error.h>
#include <psp2/kernel/modulemgr.h>
#include <psp2/types.h>

#include "fs.h"
#include "host.h"
#include "load.h"
#include "log.h"
#include "module.h"
#include "modules/SceSysmem.h"

#define VITA_MODULE_SLOTS 32

typedef struct {
    bool used;
    SceUID uid;
    int refs;
    int state;
    Jit jit;
    void *entry;
    void *stop_entry;
    LoadedModuleInfo loader;
} VitaModuleInfo;

static VitaModuleInfo g_modules[VITA_MODULE_SLOTS];
static pthread_mutex_t g_modules_lock = PTHREAD_MUTEX_INITIALIZER;

static VitaModuleInfo *module_find_locked(SceUID uid)
{
    for (int i = 0; i < VITA_MODULE_SLOTS; i++) {
        if (g_modules[i].used && g_modules[i].uid == uid)
            return &g_modules[i];
    }

    return NULL;
}

static VitaModuleInfo *module_alloc_slot(void)
{
    for (int i = 0; i < VITA_MODULE_SLOTS; i++) {
        VitaModuleInfo *module = &g_modules[i];

        if (module->used)
            continue;

        memset(module, 0, sizeof(*module));
        module->used = true;
        module->uid = SceSysmem_get_next_uid();
        module->refs = 1;
        return module;
    }

    return NULL;
}

static void module_release_locked(VitaModuleInfo *module)
{
    if (module->loader.module_info)
        module_unregister_guest_libraries(module->loader.module_info);
    if (module->jit.rx)
        jitClose(&module->jit);
    free(module->loader.data_addr);
    memset(module, 0, sizeof(*module));
}

static int resolve_module_path(const char *path, char *out, size_t out_size)
{
    if (!path)
        return SCE_KERNEL_ERROR_MODULEMGR_NOENT;

    if (host_fs_resolve_path(path, out, out_size) < 0)
        return SCE_KERNEL_ERROR_MODULEMGR_NOENT;

    return 0;
}

static SceUID module_load(const char *path)
{
    VitaModuleInfo *module;
    char host_path[512];
    void *entry = NULL;
    int ret;

    ret = resolve_module_path(path, host_path, sizeof(host_path));
    if (ret < 0)
        return ret;

    pthread_mutex_lock(&g_modules_lock);
    module = module_alloc_slot();
    pthread_mutex_unlock(&g_modules_lock);

    if (!module)
        return SCE_KERNEL_ERROR_MODULEMGR_NOMEM;

    if (load_file_info(&module->jit, host_path, &entry, &module->loader) < 0) {
        pthread_mutex_lock(&g_modules_lock);
        module_release_locked(module);
        pthread_mutex_unlock(&g_modules_lock);
        return SCE_KERNEL_ERROR_MODULEMGR_NOENT;
    }

    strncpy(module->loader.path, path, sizeof(module->loader.path) - 1);
    module->loader.path[sizeof(module->loader.path) - 1] = '\0';
    module->entry = entry;
    module->stop_entry = module->loader.module_stop;
    module->state = SCE_KERNEL_MODULE_STATE_READY;

    LOG("Loaded module \"%s\" as 0x%x (entry %p)", module->loader.name, module->uid,
        module->entry);

    return module->uid;
}

static int module_start(VitaModuleInfo *module, SceSize args, void *argp, int *status)
{
    int ret;

    if (!module->entry)
        return SCE_KERNEL_ERROR_MODULEMGR_NO_MOD_ENTRY;

    LOG("Calling module_start for \"%s\" at %p", module->loader.name, module->entry);
    ret = ((int (*)(SceSize, void *))module->entry)(args, argp);
    LOG("module_start for \"%s\" returned 0x%x", module->loader.name, ret);
    if (status)
        *status = ret;

    if (ret < 0)
        return SCE_KERNEL_ERROR_MODULEMGR_START_FAILED;

    module->state = SCE_KERNEL_MODULE_STATE_STARTED;
    return 0;
}

static int module_stop(VitaModuleInfo *module, SceSize args, void *argp, int *status)
{
    int ret;

    if (module->stop_entry) {
        ret = ((int (*)(SceSize, void *))module->stop_entry)(args, argp);
        if (status)
            *status = ret;
        if (ret < 0)
            return SCE_KERNEL_ERROR_MODULEMGR_STOP_FAIL;
    } else if (status) {
        *status = 0;
    }

    module->state = SCE_KERNEL_MODULE_STATE_ENDED;
    return 0;
}

static int module_get_info(VitaModuleInfo *module, SceKernelModuleInfo *info)
{
    if (!info)
        return SCE_KERNEL_ERROR_ILLEGAL_ADDR;

    memset(info, 0, sizeof(*info));
    info->size = sizeof(*info);
    info->modid = module->uid;
    info->modattr = module->loader.attributes;
    info->modver[0] = 1;
    info->modver[1] = 1;
    strncpy(info->module_name, module->loader.name, sizeof(info->module_name) - 1);
    info->start_entry = module->entry;
    info->stop_entry = module->stop_entry;
    strncpy(info->path, module->loader.path, sizeof(info->path) - 1);
    info->state = (SceUInt)module->state;

    for (int i = 0; i < module->loader.num_segments && i < 4; i++) {
        info->segments[i].size = sizeof(info->segments[i]);
        info->segments[i].perms = module->loader.segments[i].perms;
        info->segments[i].vaddr = module->loader.segments[i].vaddr;
        info->segments[i].memsz = module->loader.segments[i].memsz;
        info->segments[i].filesz = module->loader.segments[i].filesz;
    }

    return 0;
}

EXPORT(SceLibKernel, 0xBBE82155, SceUID, sceKernelLoadModule, const char *path, int flags,
       SceKernelLMOption *option)
{
    (void)flags;
    (void)option;
    return module_load(path);
}

EXPORT(SceModulemgr, 0xB4C5EF9E, SceUID, _sceKernelLoadModule, const char *path, SceUInt32 flags,
       const SceKernelLoadModuleOption *option)
{
    (void)flags;
    (void)option;
    return module_load(path);
}

EXPORT(SceLibKernel, 0x702425D5, int, sceKernelStartModule, SceUID modid, SceSize args, void *argp,
       int flags, void *option, int *status)
{
    VitaModuleInfo *module;
    int ret;

    (void)flags;
    (void)option;

    pthread_mutex_lock(&g_modules_lock);
    module = module_find_locked(modid);
    if (!module) {
        pthread_mutex_unlock(&g_modules_lock);
        return SCE_KERNEL_ERROR_MODULEMGR_NO_MOD;
    }
    if (module->state == SCE_KERNEL_MODULE_STATE_STARTED) {
        pthread_mutex_unlock(&g_modules_lock);
        return SCE_KERNEL_ERROR_MODULEMGR_STARTED;
    }
    module->refs++;
    pthread_mutex_unlock(&g_modules_lock);

    ret = module_start(module, args, argp, status);

    pthread_mutex_lock(&g_modules_lock);
    module->refs--;
    pthread_mutex_unlock(&g_modules_lock);
    return ret;
}

EXPORT(SceModulemgr, 0x72CD301F, int, _sceKernelStartModule, SceUID modid, SceSize args,
       void *argp, SceUInt32 flags, const void *option, int *status)
{
    return sceKernelStartModule(modid, args, argp, flags, (void *)option, status);
}

EXPORT(SceLibKernel, 0x2DCC4AFA, SceUID, sceKernelLoadStartModule, const char *path, SceSize args,
       void *argp, int flags, SceKernelLMOption *option, int *status)
{
    SceUID modid = module_load(path);
    int ret;

    if (modid < 0)
        return modid;

    ret = sceKernelStartModule(modid, args, argp, flags, option, status);
    if (ret < 0) {
        sceKernelUnloadModule(modid, 0, NULL);
        return ret;
    }

    return modid;
}

EXPORT(SceModulemgr, 0x60647592, SceUID, _sceKernelLoadStartModule, const char *path,
       SceSize args, const void *argp, SceUInt32 flags)
{
    return sceKernelLoadStartModule(path, args, (void *)argp, flags, NULL, NULL);
}

EXPORT(SceLibKernel, 0x3B2CBA09, int, sceKernelStopModule, SceUID modid, SceSize args,
       void *argp, int flags, void *option, int *status)
{
    VitaModuleInfo *module;
    int ret;

    (void)flags;
    (void)option;

    pthread_mutex_lock(&g_modules_lock);
    module = module_find_locked(modid);
    if (!module) {
        pthread_mutex_unlock(&g_modules_lock);
        return SCE_KERNEL_ERROR_MODULEMGR_NO_MOD;
    }
    module->refs++;
    pthread_mutex_unlock(&g_modules_lock);

    ret = module_stop(module, args, argp, status);

    pthread_mutex_lock(&g_modules_lock);
    module->refs--;
    pthread_mutex_unlock(&g_modules_lock);
    return ret;
}

EXPORT(SceModulemgr, 0x086867A8, int, _sceKernelStopModule, SceUID modid, SceSize args,
       const void *argp, SceUInt32 flags)
{
    return sceKernelStopModule(modid, args, (void *)argp, flags, NULL, NULL);
}

EXPORT(SceLibKernel, 0x1987920E, int, sceKernelUnloadModule, SceUID modid, int flags,
       SceKernelULMOption *option)
{
    VitaModuleInfo *module;

    (void)flags;
    (void)option;

    pthread_mutex_lock(&g_modules_lock);
    module = module_find_locked(modid);
    if (!module) {
        pthread_mutex_unlock(&g_modules_lock);
        return SCE_KERNEL_ERROR_MODULEMGR_NO_MOD;
    }

    if (module->refs > 1) {
        module->refs--;
        pthread_mutex_unlock(&g_modules_lock);
        return 0;
    }

    if (module_guest_library_has_dependents(module->loader.module_info)) {
        pthread_mutex_unlock(&g_modules_lock);
        return SCE_KERNEL_ERROR_MODULEMGR_IN_USE;
    }

    module_remove_importer(module->loader.module_info);
    module_release_locked(module);
    pthread_mutex_unlock(&g_modules_lock);

    return 0;
}

EXPORT(SceModulemgr, 0x8E4A7716, int, _sceKernelUnloadModule, SceUID modid, SceUInt32 flags,
       const SceKernelUnloadModuleOption *option)
{
    (void)option;
    return sceKernelUnloadModule(modid, flags, NULL);
}

EXPORT(SceLibKernel, 0x2415F8A4, int, sceKernelStopUnloadModule, SceUID modid, SceSize args,
       void *argp, int flags, SceKernelULMOption *option, int *status)
{
    int ret = sceKernelStopModule(modid, args, argp, flags, option, status);

    if (ret < 0)
        return ret;

    return sceKernelUnloadModule(modid, 0, NULL);
}

EXPORT(SceModulemgr, 0x86EAEA0A, int, _sceKernelStopUnloadModule, SceUID modid, SceSize args,
       const void *argp, SceUInt32 flags)
{
    return sceKernelStopUnloadModule(modid, args, (void *)argp, flags, NULL, NULL);
}

EXPORT(SceLibKernel, 0x4F2D8B15, SceUID, sceKernelOpenModule, const char *path, SceSize args,
       void *argp, int flags, void *option, int *status)
{
    (void)args;
    (void)argp;
    (void)flags;
    (void)option;
    (void)status;

    return module_load(path);
}

EXPORT(SceModulemgr, 0x9D674F45, SceUID, _sceKernelOpenModule, const char *path, SceSize args,
       const void *argp, SceUInt32 flags)
{
    return sceKernelOpenModule(path, args, (void *)argp, flags, NULL, NULL);
}

EXPORT(SceLibKernel, 0x657FA50E, int, sceKernelCloseModule, SceUID modid, SceSize args,
       void *argp, int flags, void *option, int *status)
{
    (void)args;
    (void)argp;
    (void)flags;
    (void)option;
    (void)status;

    return sceKernelUnloadModule(modid, 0, NULL);
}

EXPORT(SceModulemgr, 0x849E78BE, int, _sceKernelCloseModule, SceUID modid, SceSize args,
       const void *argp, SceUInt32 flags)
{
    return sceKernelCloseModule(modid, args, (void *)argp, flags, NULL, NULL);
}

EXPORT(SceModulemgr, 0x36585DAF, int, sceKernelGetModuleInfo, SceUID modid,
       SceKernelModuleInfo *info)
{
    VitaModuleInfo *module;
    int ret;

    pthread_mutex_lock(&g_modules_lock);
    module = module_find_locked(modid);
    ret = module ? module_get_info(module, info) : SCE_KERNEL_ERROR_MODULEMGR_NO_MOD;
    pthread_mutex_unlock(&g_modules_lock);

    return ret;
}

EXPORT(SceLibKernel, 0xD11A5103, int, sceKernelGetModuleInfoByAddr, void *addr,
       SceKernelModuleInfo *info)
{
    for (int i = 0; i < VITA_MODULE_SLOTS; i++) {
        VitaModuleInfo *module = &g_modules[i];

        if (!module->used)
            continue;
        if (addr >= module->loader.code_rx &&
            (uintptr_t)addr < (uintptr_t)module->loader.code_rx +
                                 (uintptr_t)module->jit.size) {
            return module_get_info(module, info);
        }
    }

    return SCE_KERNEL_ERROR_MODULEMGR_NO_MOD;
}

EXPORT(SceModulemgr, 0xF5798C7C, SceUID, sceKernelGetModuleIdByAddr, void *addr)
{
    for (int i = 0; i < VITA_MODULE_SLOTS; i++) {
        VitaModuleInfo *module = &g_modules[i];

        if (!module->used)
            continue;
        if (addr >= module->loader.code_rx &&
            (uintptr_t)addr < (uintptr_t)module->loader.code_rx +
                                 (uintptr_t)module->jit.size) {
            return module->uid;
        }
    }

    return SCE_KERNEL_ERROR_MODULEMGR_NO_MOD;
}

EXPORT(SceModulemgr, 0x2EF2581F, int, sceKernelGetModuleList, SceUInt8 type, SceUID *uids,
       SceSize *num)
{
    SceSize capacity;
    SceSize count = 0;

    (void)type;

    if (!num)
        return SCE_KERNEL_ERROR_ILLEGAL_ADDR;
    capacity = *num;

    pthread_mutex_lock(&g_modules_lock);
    for (int i = 0; i < VITA_MODULE_SLOTS; i++) {
        if (!g_modules[i].used)
            continue;
        if (uids && count < capacity)
            uids[count] = g_modules[i].uid;
        count++;
    }
    pthread_mutex_unlock(&g_modules_lock);

    *num = count;
    if (uids && count > capacity)
        return SCE_KERNEL_ERROR_MODULEMGR_NOMEM;

    return 0;
}

EXPORT(SceModulemgr, 0x85E6D2BB, int, sceKernelIsCalledFromSysModule, void *lr)
{
    (void)lr;
    return 0;
}

EXPORT(SceModulemgr, 0x4397FC4E, SceUInt32, sceKernelGetAllowedSdkVersionOnSystem, void)
{
    return 0x03570011;
}

EXPORT(SceModulemgr, 0x5182E212, int, sceKernelGetSystemSwVersion, SceKernelSystemSwVersion *version)
{
    if (!version)
        return SCE_KERNEL_ERROR_ILLEGAL_ADDR;

    memset(version, 0, sizeof(*version));
    version->size = sizeof(*version);
    strncpy(version->versionString, "3.60", sizeof(version->versionString) - 1);
    version->version = 0x03600000;

    return 0;
}

EXPORT(SceModulemgr, 0x6CED1F63, int, sceKernelInhibitLoadingModule, SceUInt16 module_id)
{
    (void)module_id;
    return 0;
}

/*
 * The loader does not retain its export-library table after relocating a
 * module, so there is nothing reliable to report here. Fail like the firmware
 * does for an unknown library instead of fabricating a descriptor.
 */
EXPORT(SceModulemgr, 0xEAEB1312, int, sceKernelGetLibraryInfoByNID, SceUID modid,
       SceNID libnid, SceKernelLibraryInfo *info)
{
    (void)modid;
    (void)libnid;
    (void)info;

    return SCE_KERNEL_ERROR_MODULEMGR_NOENT;
}

DECLARE_LIBRARY(SceModulemgr, 0xeaed1616);
