/*
 * SceSharedFb: the live-area framebuffer used by system apps.
 *
 * This host has no compositor arbitration, so the shared framebuffer does not
 * exist. Opening it fails, which makes callers fall back to their own render
 * target (libvita2d does this when sceAppMgrGetBudgetInfo fails as well).
 */

#include <psp2/appmgr.h>
#include <psp2/sharedfb.h>

#include "module.h"

EXPORT(SceSharedFb, 0xB358E1B6, SceUID, _sceSharedFbOpen, int index, int sysver)
{
    (void)index;
    (void)sysver;

    return (SceUID)SCE_APPMGR_ERROR_INVALID;
}

EXPORT(SceSharedFb, 0x33DA3428, int, sceSharedFbClose, SceUID fb_id)
{
    (void)fb_id;

    return SCE_APPMGR_ERROR_INVALID;
}

EXPORT(SceSharedFb, 0x72067C6B, int, sceSharedFbBegin, SceUID fb_id, SceSharedFbInfo *info)
{
    (void)fb_id;
    (void)info;

    return SCE_APPMGR_ERROR_INVALID;
}

EXPORT(SceSharedFb, 0xFC5A62B6, int, sceSharedFbEnd, SceUID fb_id)
{
    (void)fb_id;

    return SCE_APPMGR_ERROR_INVALID;
}

EXPORT(SceSharedFb, 0x819954FB, int, sceSharedFbGetInfo, SceUID fb_id, SceSharedFbInfo *info)
{
    (void)fb_id;
    (void)info;

    return SCE_APPMGR_ERROR_INVALID;
}

/* VSH-only entry points; the host has no shared framebuffer. */
EXPORT(SceSharedFb, 0xD6C643B5, int, sceSharedFbCreate, void)
{
    return SCE_APPMGR_ERROR_INVALID;
}

EXPORT(SceSharedFb, 0xBE5A1A81, int, sceSharedFbDelete, void)
{
    return SCE_APPMGR_ERROR_INVALID;
}

EXPORT(SceSharedFb, 0x61FEEDFA, int, sceSharedFbGetRenderingInfo, void)
{
    return SCE_APPMGR_ERROR_INVALID;
}

EXPORT(SceSharedFb, 0xAC49FC1D, int, sceSharedFbGetShellRenderPort, void)
{
    return SCE_APPMGR_ERROR_INVALID;
}

EXPORT(SceSharedFb, 0x3889ACF8, int, sceSharedFbUpdateProcess, void)
{
    return 0;
}

EXPORT(SceSharedFb, 0xF9754AD9, int, sceSharedFbUpdateProcessBegin, void)
{
    return 0;
}

EXPORT(SceSharedFb, 0x565A9AB6, int, sceSharedFbUpdateProcessEnd, void)
{
    return 0;
}

DECLARE_LIBRARY(SceSharedFb, 0x32a0442e);
