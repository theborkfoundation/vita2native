/*
 * SceAppMgrUser: the subset of the application manager that homebrew touches.
 *
 * There is no application manager on this host: we are not a system app, we
 * do not mount content, and there is no live-area budget to hand out. Budget
 * queries fail on purpose so that libraries such as libvita2d fall back to the
 * normal (render target) path instead of the shared-framebuffer one.
 */

#include <psp2/appmgr.h>
#include <string.h>

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

DECLARE_LIBRARY(SceAppMgrUser, 0xa6605d6f);
