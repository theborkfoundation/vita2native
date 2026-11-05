/*
 * USB mass-storage virtual storage, MTP interface and USB device controller.
 *
 * The host is not a USB gadget and does not run an MTP responder, so starting
 * either service fails. Configuration calls are accepted, and the USB device
 * state reports a disconnected cable.
 */
#include <stdio.h>
#include <string.h>

#include <psp2/mtpif.h>
#include <psp2/udcd.h>
#include <psp2/usbstorvstor.h>

#include "module.h"

static char g_image_path[256];
static char g_device_name[64];
static char g_device_version[64];

EXPORT(SceUsbstorVStor, 0x8C9F93AB, int, sceUsbstorVStorSetImgFilePath, const char *path)
{
    if (!path)
        return -1;
    snprintf(g_image_path, sizeof(g_image_path), "%s", path);
    return 0;
}

EXPORT(SceUsbstorVStor, 0x14455C20, int, sceUsbstorVStorSetDeviceInfo, const char *name,
       const char *version)
{
    if (!name || !version)
        return -1;
    snprintf(g_device_name, sizeof(g_device_name), "%s", name);
    snprintf(g_device_version, sizeof(g_device_version), "%s", version);
    return 0;
}

EXPORT(SceUsbstorVStor, 0xB606F1AF, int, sceUsbstorVStorStart, SceUsbstorVstorType type)
{
    (void)type;
    return -1;
}

EXPORT(SceUsbstorVStor, 0x0FD67059, int, sceUsbstorVStorStop, void)
{
    return 0;
}

EXPORT(SceMtpIf, 0xC5327CAC, int, sceMtpIfStartDriver, int flags)
{
    (void)flags;
    return -1;
}

EXPORT(SceMtpIf, 0xC0CFA3BC, int, sceMtpIfStopDriver, int flags)
{
    (void)flags;
    return 0;
}

EXPORT(SceUdcd, 0xFCD31220, int, sceUdcdGetDeviceState, SceUdcdDeviceState *state)
{
    if (!state)
        return -1;
    memset(state, 0, sizeof(*state));
    return 0;
}

EXPORT(SceUdcd, 0x701C87CF, int, sceUdcdGetDeviceInfo, SceUdcdDeviceInfo *devInfo)
{
    if (!devInfo)
        return -1;
    memset(devInfo, 0, sizeof(*devInfo));
    return 0;
}

EXPORT(SceUdcd, 0x99B3DA5D, int, sceUdcdGetDrvState, const char *driverName)
{
    (void)driverName;
    return SCE_UDCD_STATUS_DRIVER_REGISTERED;
}

EXPORT(SceUdcd, 0xA7070093, int, sceUdcdRegisterCallback, SceUID cbid, int state)
{
    (void)cbid;
    (void)state;
    return 0;
}

EXPORT(SceUdcd, 0xC3FBA889, int, sceUdcdUnregisterCallback, SceUID cbid)
{
    (void)cbid;
    return 0;
}

EXPORT(SceUdcd, 0x59EFFAF1, int, sceUdcdWaitState, SceUdcdWaitParam *waitParam,
       unsigned int timeout)
{
    (void)waitParam;
    (void)timeout;
    return -1;
}

DECLARE_LIBRARY(SceUsbstorVStor, 0x17F294B9);
DECLARE_LIBRARY(SceMtpIf, 0x5EFA4138);
DECLARE_LIBRARY(SceUdcd, 0xA84BDE8A);
