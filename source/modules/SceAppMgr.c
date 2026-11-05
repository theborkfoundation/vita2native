/*
 * SceAppMgrUser: the subset of the application manager that homebrew touches.
 *
 * There is no application manager on this host: we are not a system app, we
 * do not mount content, and there is no live-area budget to hand out. Budget
 * queries fail on purpose so that libraries such as libvita2d fall back to the
 * normal (render target) path instead of the shared-framebuffer one.
 */

#include <limits.h>
#include <psp2/appmgr.h>
#include <string.h>
#include <sys/statvfs.h>

#include "fs.h"
#include "module.h"

EXPORT(SceAppMgrUser, 0x5F00F261, int, sceAppMgrGetBudgetInfo, SceAppMgrBudgetInfo *info)
{
    (void)info;

    return SCE_APPMGR_ERROR_STATE;
}

EXPORT(SceAppMgrUser, 0x9E57853D, int, sceAppMgrAppParamGetInt, int param_id, int *value)
{
    (void)param_id;
    (void)value;

    return SCE_APPMGR_ERROR_INVALID;
}

EXPORT(SceAppMgrUser, 0x0C733AE2, int, sceAppMgrAppParamGetString, int pid, int param,
       char *string, SceSize length)
{
    (void)pid;
    (void)param;
    (void)string;
    (void)length;

    return SCE_APPMGR_ERROR_INVALID;
}

EXPORT(SceAppMgrUser, 0x127F3BEB, int, sceAppMgrAppMount, unsigned int app_id)
{
    (void)app_id;

    return 0;
}

EXPORT(SceAppMgrUser, 0x9619C150, int, sceAppMgrAppUmount, unsigned int app_id)
{
    (void)app_id;

    return 0;
}

EXPORT(SceAppMgrUser, 0x9B3BB24D, int, sceAppMgrWorkDirMount, int id, const char *mount_point)
{
    (void)id;
    (void)mount_point;

    return 0;
}

EXPORT(SceAppMgrUser, 0x5E86319A, int, _sceAppMgrGetAppState, SceAppMgrAppState *state,
       unsigned int size, unsigned int flags)
{
    if (!state)
        return SCE_APPMGR_ERROR_NULL_POINTER;

    memset(state, 0, size < sizeof(*state) ? size : sizeof(*state));
    return 0;
}

EXPORT(SceAppMgrUser, 0x67705391, int, sceAppMgrGetDevInfo, const char *dev, uint64_t *max_size,
       uint64_t *free_size)
{
    char path[PATH_MAX];
    struct statvfs vfs;

    if (!dev)
        return SCE_APPMGR_ERROR_INVALID;
    if (host_fs_resolve_path(dev, path, sizeof(path)) < 0)
        return SCE_APPMGR_ERROR_INVALID;
    if (statvfs(path, &vfs) != 0)
        return SCE_APPMGR_ERROR_INVALID;

    if (max_size)
        *max_size = (uint64_t)vfs.f_blocks * vfs.f_frsize;
    if (free_size)
        *free_size = (uint64_t)vfs.f_bavail * vfs.f_frsize;

    return 0;
}

EXPORT(SceAppMgrUser, 0x10B5765F, int, sceAppMgrReceiveSystemEvent,
       SceAppMgrSystemEvent *system_event)
{
    if (!system_event)
        return SCE_APPMGR_ERROR_INVALID;

    /* No shell is generating resume/store events on this host. */
    memset(system_event, 0, sizeof(*system_event));
    return 0;
}

EXPORT(SceAppMgrUser, 0x5E375921, int, sceAppMgrUmount, const char *mount_point)
{
    (void)mount_point;
    return 0;
}

EXPORT(SceAppMgrUser, 0x4993876C, int, sceAppMgrGameDataMount, const char *app_path,
       const char *patch_path, const char *rif_path, char *mount_point)
{
    (void)app_path;
    (void)patch_path;
    (void)rif_path;
    (void)mount_point;
    return SCE_APPMGR_ERROR_INVALID;
}

EXPORT(SceAppMgrUser, 0x003C634F, int, sceAppMgrLaunchAppByUri, int flags, const char *uri)
{
    (void)flags;
    (void)uri;
    return SCE_APPMGR_ERROR_INVALID;
}

EXPORT(SceAppMgrUser, 0xE6774ABC, int, sceAppMgrLoadExec, const char *app_path,
       char *const argv[], const SceAppMgrExecOptParam *opt_param)
{
    (void)app_path;
    (void)argv;
    (void)opt_param;
    return SCE_APPMGR_ERROR_INVALID;
}

/* Only one application exists, so BGM arbitration is a no-op. */
EXPORT(SceAppMgr, 0xAFCEAB96, int, sceAppMgrAcquireBgmPort, void)
{
    return 0;
}

EXPORT(SceAppMgr, 0xF3717E37, int, sceAppMgrReleaseBgmPort, void)
{
    return 0;
}

EXPORT(SceAppMgr, 0x3EF143BF, int, sceAppMgrDestroyOtherApp, void)
{
    return 0;
}

static int64_t appmgr_unimplemented(void)
{
    return (int64_t)(int32_t)SCE_APPMGR_ERROR_INVALID;
}

#include "SceAppMgr_stubs.inc"

DECLARE_LIBRARY(SceAppMgrUser, 0xa6605d6f);
DECLARE_LIBRARY(SceAppMgr, 0x8af17416);
