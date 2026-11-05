/*
 * SceVshBridge: the small subset of VSH-only services a user application can
 * call. Mount/umount are accepted as no-ops (the host VFS already exposes every
 * device under its root); kernel module lookup reports "not found" because no
 * kernel modules are loaded.
 */

#include <psp2/types.h>
#include <psp2/vshbridge.h>

#include "module.h"

EXPORT(SceVshBridge, 0x3C522C35, int, _vshIoMount, SceVshMountId id, const char *path,
       int permission, void *buffer)
{
    (void)id;
    (void)path;
    (void)permission;
    (void)buffer;

    return 0;
}

EXPORT(SceVshBridge, 0x35BC26AC, int, vshIoUmount, SceVshMountId id, int force, int unk2,
       int unk3)
{
    (void)id;
    (void)force;
    (void)unk2;
    (void)unk3;

    return 0;
}

EXPORT(SceVshBridge, 0x232F0EEE, int, _vshKernelSearchModuleByName, const char *module_name,
       const void *buffer)
{
    (void)module_name;
    (void)buffer;

    return -1;
}

DECLARE_LIBRARY(SceVshBridge, 0x35C5ACD4);
