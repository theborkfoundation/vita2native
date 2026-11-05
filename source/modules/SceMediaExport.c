/*
 * Music/video/photo export.
 *
 * Exporting to the host's media libraries is not implemented, so the
 * availability probes report "no" and the export entry points fail. Reporting
 * unavailability is better than pretending to export: applications then offer
 * their own download/share path.
 */
#include <psp2/musicexport.h>
#include <psp2/photoexport.h>
#include <psp2/videoexport.h>

#include "module.h"

EXPORT(SceMusicExport, 0x08000579, int, sceMusicExportFromFile, const char *path,
       const MusicExportParam *param, void *workingMemory, void *cancelCb,
       void (*progress)(void *, int), void *user, char *outPath, SceSize outPathSize)
{
    (void)path;
    (void)param;
    (void)workingMemory;
    (void)cancelCb;
    (void)progress;
    (void)user;
    (void)outPath;
    (void)outPathSize;
    return -1;
}

EXPORT(SceVideoExport, 0x4F60A3C8, int, sceVideoExportFromFile,
       const VideoExportInputParam *in_param, int unk, void *workingMemory, void *cancelCb,
       void (*progress)(void *, int), void *user, int unk2, VideoExportOutputParam *out_param)
{
    (void)in_param;
    (void)unk;
    (void)workingMemory;
    (void)cancelCb;
    (void)progress;
    (void)user;
    (void)unk2;
    (void)out_param;
    return -1;
}

EXPORT(ScePhotoExport, 0x84FD9FC5, int, scePhotoExportFromFile, const char *path,
       const PhotoExportParam *param, void *workingMemory, void *cancelCb, void *user,
       char *outPath, SceSize outPathSize)
{
    (void)path;
    (void)param;
    (void)workingMemory;
    (void)cancelCb;
    (void)user;
    (void)outPath;
    (void)outPathSize;
    return -1;
}

EXPORT(ScePhotoExport, 0x70512321, int, scePhotoExportFromData, const void *data, SceSize size,
       const PhotoExportParam *param, void *workingMemory, void *cancelCb, void *user,
       char *outPath, SceSize outPathSize)
{
    (void)data;
    (void)size;
    (void)param;
    (void)workingMemory;
    (void)cancelCb;
    (void)user;
    (void)outPath;
    (void)outPathSize;
    return -1;
}

EXPORT(ScePhotoExport, 0x5655F971, int, scePhotoExportIsAvailableFromFile, const char *path)
{
    (void)path;
    return 0;
}

EXPORT(ScePhotoExport, 0xE6E6AFD5, int, scePhotoExportIsAvailableFromData, const void *data,
       SceSize size)
{
    (void)data;
    (void)size;
    return 0;
}

DECLARE_LIBRARY(SceMusicExport, 0xF2E30EED);
DECLARE_LIBRARY(SceVideoExport, 0xF0812A7C);
DECLARE_LIBRARY(ScePhotoExport, 0x79166BD9);
