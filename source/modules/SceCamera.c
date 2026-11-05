/*
 * SceCamera: there is no camera device on the host. Open fails with
 * NOT_MOUNTED so QR scanners and similar features take their error path.
 */

#include <psp2/camera.h>

#include "module.h"

EXPORT(SceCamera, 0xA462F801, int, sceCameraOpen, int devnum, SceCameraInfo *info)
{
    (void)devnum;
    (void)info;

    return SCE_CAMERA_ERROR_NOT_MOUNTED;
}

EXPORT(SceCamera, 0xCD6E1CFC, int, sceCameraClose, int devnum)
{
    (void)devnum;

    return SCE_CAMERA_ERROR_NOT_OPEN;
}

EXPORT(SceCamera, 0xA8FEAE35, int, sceCameraStart, int devnum)
{
    (void)devnum;

    return SCE_CAMERA_ERROR_NOT_OPEN;
}

EXPORT(SceCamera, 0x1DD9C9CE, int, sceCameraStop, int devnum)
{
    (void)devnum;

    return SCE_CAMERA_ERROR_NOT_START;
}

EXPORT(SceCamera, 0x79B5C2DE, int, sceCameraRead, int devnum, SceCameraRead *read)
{
    (void)devnum;
    (void)read;

    return SCE_CAMERA_ERROR_NOT_START;
}

DECLARE_LIBRARY(SceCamera, 0xDA91B3ED);
