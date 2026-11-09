/*
 * SceAppUtil: save data, system parameters and the small helpers titles use
 * during boot.
 *
 * Save slots live under savedata0: (the VFS maps it to <root>/savedata), one
 * "SlotParam_<id>.bin" file per slot plus the files the title writes. That is
 * the same layout the real firmware exposes through the save data mount, so
 * titles that mount savedata0: directly keep working.
 */

#include <errno.h>
#include <limits.h>
#include <psp2/apputil.h>
#include <psp2/system_param.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/types.h>
#include <unistd.h>

#include "fs.h"
#include "log.h"
#include "module.h"

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

/* APPPARAM ids (not present in the public header). */
#define APPUTIL_APPPARAM_ID_SKU_FLAG 0

#define APPUTIL_SAFEMEMORY_SIZE 0x20000

static bool g_apputil_initialized;

static int save_debug(void)
{
    static int enabled = -1;

    if (enabled < 0)
        enabled = getenv("VITA2NATIVE_INPUT_DEBUG") != NULL;
    return enabled;
}

/* Resolves a savedata path ("data.bin" or "savedata0:/data.bin") to a host path. */
static int resolve_savedata_path(const char *path, char *out, size_t out_size)
{
    char vita_path[PATH_MAX];

    if (!path)
        return -1;

    if (strchr(path, ':') && strchr(path, ':') < (path + 8)) {
        if (snprintf(vita_path, sizeof(vita_path), "%s", path) >= (int)sizeof(vita_path))
            return -1;
    } else {
        if (snprintf(vita_path, sizeof(vita_path), "savedata0:/%s", path) >=
            (int)sizeof(vita_path))
            return -1;
    }

    return host_fs_resolve_path(vita_path, out, out_size);
}

/* Creates every parent directory of `path` (which must be a host path). */
static void create_parent_dirs(const char *path)
{
    char buffer[PATH_MAX];

    if (snprintf(buffer, sizeof(buffer), "%s", path) >= (int)sizeof(buffer))
        return;

    for (char *p = buffer + 1; *p; p++) {
        if (*p != '/')
            continue;
        *p = '\0';
        mkdir(buffer, 0755);
        *p = '/';
    }
}

static int slot_param_path(unsigned int slot_id, char *out, size_t out_size)
{
    char vita_path[64];

    snprintf(vita_path, sizeof(vita_path), "savedata0:/SlotParam_%u.bin", slot_id);
    return host_fs_resolve_path(vita_path, out, out_size);
}

static int write_slot_param(unsigned int slot_id, const SceAppUtilSaveDataSlotParam *param)
{
    char path[PATH_MAX];
    FILE *file;

    if (!param || slot_param_path(slot_id, path, sizeof(path)) != 0)
        return SCE_APPUTIL_ERROR_PARAMETER;

    create_parent_dirs(path);
    file = fopen(path, "wb");
    if (!file)
        return SCE_APPUTIL_ERROR_NO_MEMORY;

    fwrite(param, 1, sizeof(*param), file);
    fclose(file);

    return 0;
}

EXPORT(SceAppUtil, 0xDAFFE671, int, sceAppUtilInit, SceAppUtilInitParam *init_param,
       SceAppUtilBootParam *boot_param)
{
    if (!init_param || !boot_param)
        return SCE_APPUTIL_ERROR_PARAMETER;

    g_apputil_initialized = true;
    LOG("SceAppUtil: initialised (app version 0x%x)", boot_param->appVersion);

    return 0;
}

EXPORT(SceAppUtil, 0xB220B00B, int, sceAppUtilShutdown, void)
{
    g_apputil_initialized = false;
    return 0;
}

EXPORT(SceAppUtil, 0xEE0DBED9, int, sceAppUtilReceiveAppEvent,
       SceAppUtilAppEventParam *event_param)
{
    (void)event_param;

    /* There is no shell to deliver app events (LiveArea, etc.). */
    return SCE_APPUTIL_ERROR_PARAMETER;
}

EXPORT(SceAppUtil, 0x0F4EE55F, int, sceAppUtilAppEventParseLiveArea,
       const SceAppUtilAppEventParam *event_param, char *buffer)
{
    (void)event_param;
    (void)buffer;

    return SCE_APPUTIL_ERROR_APPEVENT_PARSE_INVALID_DATA;
}

EXPORT(SceAppUtil, 0x7E8FE96A, int, sceAppUtilSaveDataSlotCreate, unsigned int slot_id,
       SceAppUtilSaveDataSlotParam *param, SceAppUtilSaveDataMountPoint *mount_point)
{
    char path[PATH_MAX];
    struct stat st;
    int ret;

    (void)mount_point;

    if (save_debug())
        fprintf(stderr, "vita2native: SaveDataSlotCreate slot=%u param=%p\n", slot_id, (void *)param);

    if (slot_param_path(slot_id, path, sizeof(path)) != 0)
        return SCE_APPUTIL_ERROR_PARAMETER;
    if (stat(path, &st) == 0)
        return SCE_APPUTIL_ERROR_SAVEDATA_SLOT_EXISTS;

    ret = write_slot_param(slot_id, param);
    if (ret != 0)
        return ret;

    LOG("SceAppUtil: created save slot %u", slot_id);
    return 0;
}

EXPORT(SceAppUtil, 0x266A7646, int, sceAppUtilSaveDataSlotDelete, unsigned int slot_id,
       SceAppUtilSaveDataMountPoint *mount_point)
{
    char path[PATH_MAX];
    struct stat st;

    (void)mount_point;

    if (save_debug())
        fprintf(stderr, "vita2native: SaveDataSlotDelete slot=%u\n", slot_id);

    if (slot_param_path(slot_id, path, sizeof(path)) != 0)
        return SCE_APPUTIL_ERROR_PARAMETER;
    if (stat(path, &st) != 0)
        return SCE_APPUTIL_ERROR_SAVEDATA_SLOT_NOT_FOUND;

    remove(path);
    return 0;
}

EXPORT(SceAppUtil, 0x98630136, int, sceAppUtilSaveDataSlotSetParam, unsigned int slot_id,
       SceAppUtilSaveDataSlotParam *param, SceAppUtilSaveDataMountPoint *mount_point)
{
    (void)mount_point;

    if (save_debug())
        fprintf(stderr, "vita2native: SaveDataSlotSetParam slot=%u param=%p\n", slot_id,
                (void *)param);

    return write_slot_param(slot_id, param);
}

EXPORT(SceAppUtil, 0x93F0D89F, int, sceAppUtilSaveDataSlotGetParam, unsigned int slot_id,
       SceAppUtilSaveDataSlotParam *param, SceAppUtilSaveDataMountPoint *mount_point)
{
    char path[PATH_MAX];
    FILE *file;
    size_t read_size;

    (void)mount_point;

    if (!param || slot_param_path(slot_id, path, sizeof(path)) != 0)
        return SCE_APPUTIL_ERROR_PARAMETER;

    file = fopen(path, "rb");
    if (!file)
        return SCE_APPUTIL_ERROR_SAVEDATA_SLOT_NOT_FOUND;

    memset(param, 0, sizeof(*param));
    read_size = fread(param, 1, sizeof(*param), file);
    fclose(file);

    if (read_size != sizeof(*param))
        return SCE_APPUTIL_ERROR_PARAMETER;

    param->status = 0;
    if (save_debug()) {
        fprintf(stderr, "vita2native: SaveDataSlotGetParam slot=%u -> ok status=%u\n",
                slot_id, (unsigned)param->status);
        fflush(stderr);
    }
    return 0;
}

EXPORT(SceAppUtil, 0x607647BA, int, sceAppUtilSaveDataDataSave, SceAppUtilSaveDataFileSlot *slot,
       SceAppUtilSaveDataFile *files, unsigned int file_num,
       SceAppUtilSaveDataMountPoint *mount_point, SceSize *required_size_kb)
{
    (void)mount_point;

    if (required_size_kb)
        *required_size_kb = 0;
    if (!files && file_num)
        return SCE_APPUTIL_ERROR_PARAMETER;

    if (save_debug()) {
        fprintf(stderr, "vita2native: SaveDataDataSave slot=%u files=%u\n",
                slot ? slot->id : 0u, file_num);
        for (unsigned int i = 0; i < file_num; i++)
            fprintf(stderr, "  file[%u] path=%s buf=%p size=%u off=%lld mode=%u\n", i,
                    files[i].filePath ? files[i].filePath : "(null)", files[i].buf,
                    (unsigned)files[i].bufSize, (long long)files[i].offset,
                    (unsigned)files[i].mode);
        fflush(stderr);
    }

    for (unsigned int i = 0; i < file_num; i++) {
        char path[PATH_MAX];
        mode_t mode = files[i].mode;

        if (resolve_savedata_path(files[i].filePath, path, sizeof(path)) != 0) {
            if (save_debug()) {
                fprintf(stderr, "vita2native: SaveDataDataSave bad path %s\n",
                        files[i].filePath ? files[i].filePath : "(null)");
                fflush(stderr);
            }
            return SCE_APPUTIL_ERROR_PARAMETER;
        }

        if (mode == SCE_APPUTIL_SAVEDATA_DATA_SAVE_MODE_DIRECTORY) {
            create_parent_dirs(path);
            mkdir(path, 0755);
            continue;
        }

        create_parent_dirs(path);

        if (!files[i].buf) {
            FILE *file = fopen(path, "ab");

            if (!file) {
                if (save_debug())
                    fprintf(stderr, "vita2native: SaveDataDataSave fopen %s failed\n", path);
                return SCE_APPUTIL_ERROR_NO_MEMORY;
            }
            fclose(file);
            continue;
        }

        {
            /* Mode 1 truncates, mode 0 writes at the given offset. */
            const char *open_mode = mode == 1 ? "wb" : "r+b";
            FILE *file = fopen(path, open_mode);

            if (!file && mode != 1)
                file = fopen(path, "w+b");
            if (!file) {
                if (save_debug())
                    fprintf(stderr, "vita2native: SaveDataDataSave fopen %s failed\n", path);
                return SCE_APPUTIL_ERROR_NO_MEMORY;
            }

            if (fseek(file, (long)files[i].offset, SEEK_SET) == 0)
                fwrite(files[i].buf, 1, files[i].bufSize, file);
            fclose(file);
            if (save_debug()) {
                fprintf(stderr, "vita2native: SaveDataDataSave wrote %s (%u bytes)\n", path,
                        (unsigned)files[i].bufSize);
                fflush(stderr);
            }
        }
    }

    if (slot && slot->slotParam)
        write_slot_param(slot->id, slot->slotParam);

    return 0;
}

EXPORT(SceAppUtil, 0xD1C6AB8E, int, sceAppUtilSaveDataDataRemove, SceAppUtilSaveDataFileSlot *slot,
       SceAppUtilSaveDataRemoveItem *files, unsigned int file_num,
       SceAppUtilSaveDataMountPoint *mount_point)
{
    (void)mount_point;

    if (save_debug())
        fprintf(stderr, "vita2native: SaveDataDataRemove slot=%u files=%u\n",
                slot ? slot->id : 0u, file_num);

    if (!files && file_num)
        return SCE_APPUTIL_ERROR_PARAMETER;

    for (unsigned int i = 0; i < file_num; i++) {
        char path[PATH_MAX];

        if (resolve_savedata_path(files[i].dataPath, path, sizeof(path)) != 0)
            return SCE_APPUTIL_ERROR_PARAMETER;

        if (remove(path) != 0)
            rmdir(path);
    }

    if (slot && files && file_num &&
        files[0].mode == SCE_APPUTIL_SAVEDATA_DATA_REMOVE_MODE_DEFAULT) {
        char path[PATH_MAX];

        if (slot_param_path(slot->id, path, sizeof(path)) == 0)
            remove(path);
    }

    return 0;
}

EXPORT(SceAppUtil, 0xC560E716, int, sceAppUtilSaveDataGetQuota, SceSize *quota_size_kb,
       SceSize *used_size_kb, SceAppUtilSaveDataMountPoint *mount_point)
{
    (void)mount_point;

    if (save_debug())
        fprintf(stderr, "vita2native: SaveDataGetQuota\n");

    if (!quota_size_kb || !used_size_kb)
        return SCE_APPUTIL_ERROR_PARAMETER;

    /* No per-title quota is enforced; report a comfortable allowance. */
    *quota_size_kb = 128 * 1024;
    *used_size_kb = 0;

    return 0;
}

EXPORT(SceAppUtil, 0xE61453B0, int, sceAppUtilSaveDataMount, void)
{
    if (save_debug())
        fprintf(stderr, "vita2native: SaveDataMount\n");
    return 0;
}

EXPORT(SceAppUtil, 0xEB720402, int, sceAppUtilSaveDataUmount, void)
{
    if (save_debug())
        fprintf(stderr, "vita2native: SaveDataUmount\n");
    return 0;
}

EXPORT(SceAppUtil, 0x5DFB9CA0, int, sceAppUtilSystemParamGetInt, unsigned int param_id,
       int *value)
{
    if (!value)
        return SCE_APPUTIL_ERROR_PARAMETER;

    switch (param_id) {
    case SCE_SYSTEM_PARAM_ID_LANG:
        *value = SCE_SYSTEM_PARAM_LANG_ENGLISH_US;
        return 0;
    case SCE_SYSTEM_PARAM_ID_ENTER_BUTTON:
        *value = 0; /* cross */
        return 0;
    case SCE_SYSTEM_PARAM_ID_DATE_FORMAT:
        *value = 0; /* YYYY/MM/DD */
        return 0;
    case SCE_SYSTEM_PARAM_ID_TIME_FORMAT:
        *value = 0; /* 24 hour */
        return 0;
    case SCE_SYSTEM_PARAM_ID_TIME_ZONE:
        *value = 0;
        return 0;
    case SCE_SYSTEM_PARAM_ID_DAYLIGHT_SAVINGS:
        *value = 0;
        return 0;
    default:
        return SCE_APPUTIL_ERROR_PARAMETER;
    }
}

EXPORT(SceAppUtil, 0x6E6AA267, int, sceAppUtilSystemParamGetString, unsigned int param_id,
       SceChar8 *buffer, SceSize buffer_size)
{
    const char *text;

    if (!buffer || buffer_size == 0)
        return SCE_APPUTIL_ERROR_PARAMETER;

    switch (param_id) {
    case SCE_SYSTEM_PARAM_ID_LANG:
        text = "en";
        break;
    case SCE_SYSTEM_PARAM_ID_USERNAME:
        text = "vita2native";
        break;
    default:
        return SCE_APPUTIL_ERROR_PARAMETER;
    }

    if (strlen(text) + 1 > buffer_size)
        return SCE_APPUTIL_ERROR_PARAMETER;

    memcpy(buffer, text, strlen(text) + 1);
    return 0;
}

EXPORT(SceAppUtil, 0xCD7FD67A, int, sceAppUtilAppParamGetInt, SceAppUtilAppParamId param_id,
       int *value)
{
    if (!value)
        return SCE_APPUTIL_ERROR_PARAMETER;
    if (param_id != APPUTIL_APPPARAM_ID_SKU_FLAG)
        return SCE_APPUTIL_ERROR_PARAMETER;

    *value = 0;
    return 0;
}

EXPORT(SceAppUtil, 0xB5EDCBFF, int, sceAppUtilMusicMount, void)
{
    return 0;
}

EXPORT(SceAppUtil, 0xA7FE1BF7, int, sceAppUtilMusicUmount, void)
{
    return 0;
}

EXPORT(SceAppUtil, 0xEE85804D, int, sceAppUtilPhotoMount, void)
{
    return 0;
}

EXPORT(SceAppUtil, 0x9651B941, int, sceAppUtilPhotoUmount, void)
{
    return 0;
}

EXPORT(SceAppUtilCache, 0x0AA56143, int, sceAppUtilCacheMount, void)
{
    return 0;
}

EXPORT(SceAppUtilCache, 0x72D26BF4, int, sceAppUtilCacheUmount, void)
{
    return 0;
}

EXPORT(SceAppUtilCache, 0x1171B736, int, sceAppUtilCacheGetDevInfo, const char *dev,
       uint64_t *max_size, uint64_t *free_size)
{
    struct statvfs vfs;
    char path[PATH_MAX];

    if (!dev || host_fs_resolve_path(dev, path, sizeof(path)) < 0 || statvfs(path, &vfs) != 0)
        return SCE_APPUTIL_ERROR_PARAMETER;

    if (max_size)
        *max_size = (uint64_t)vfs.f_blocks * vfs.f_frsize;
    if (free_size)
        *free_size = (uint64_t)vfs.f_bavail * vfs.f_frsize;

    return 0;
}

static int safe_memory_rw(void *buf, SceSize buf_size, SceOff offset, bool save)
{
    char path[PATH_MAX];
    FILE *file;
    int result = 0;

    if (!buf || offset < 0 || (SceOff)buf_size + offset > APPUTIL_SAFEMEMORY_SIZE)
        return SCE_APPUTIL_ERROR_PARAMETER;

    if (host_fs_resolve_path("savedata0:/sce_sys/safemem.dat", path, sizeof(path)) != 0)
        return SCE_APPUTIL_ERROR_PARAMETER;

    if (!save) {
        memset(buf, 0, buf_size);
        file = fopen(path, "rb");
        if (!file)
            return 0;
        fseek(file, (long)offset, SEEK_SET);
        fread(buf, 1, buf_size, file);
        fclose(file);
        return 0;
    }

    create_parent_dirs(path);
    file = fopen(path, "r+b");
    if (!file)
        file = fopen(path, "w+b");
    if (!file)
        return SCE_APPUTIL_ERROR_NO_MEMORY;

    if (fseek(file, (long)offset, SEEK_SET) != 0 || fwrite(buf, 1, buf_size, file) != buf_size)
        result = SCE_APPUTIL_ERROR_NO_MEMORY;
    fclose(file);

    return result;
}

EXPORT(SceAppUtil, 0x9D8AC677, int, sceAppUtilSaveSafeMemory, void *buf, SceSize buf_size,
       SceOff offset)
{
    return safe_memory_rw(buf, buf_size, offset, true);
}

EXPORT(SceAppUtil, 0x3424D772, int, sceAppUtilLoadSafeMemory, void *buf, SceSize buf_size,
       SceOff offset)
{
    return safe_memory_rw(buf, buf_size, offset, false);
}

EXPORT(SceAppUtil, 0x85FA94EE, int, sceAppUtilStoreBrowse, SceAppUtilStoreBrowseParam *param)
{
    (void)param;

    return SCE_APPUTIL_ERROR_PARAMETER;
}

EXPORT(SceAppUtil, 0xF19D0423, int, sceAppUtilLaunchWebBrowser,
       SceAppUtilWebBrowserParam *param)
{
    (void)param;

    return SCE_APPUTIL_ERROR_PARAMETER;
}

EXPORT(SceAppUtil, 0x96F478D6, int, sceAppUtilBgdlGetStatus, SceAppUtilBgdlStatus *status)
{
    if (!status)
        return SCE_APPUTIL_ERROR_PARAMETER;

    memset(status, 0, sizeof(*status));
    return 0;
}

DECLARE_LIBRARY(SceAppUtil, 0x48e01d74);
DECLARE_LIBRARY(SceAppUtilCache, 0xe96b941b);

/*
 * The remaining SceAppUtil surface: app-event parsing (LiveArea, NP, Near,
 * Teleport, ...), DRM/addcont mounts, browser cookies and the PSP save data
 * bridge. None of those exist on this host, so they report "not available"
 * instead of pretending to work - but they resolve, so titles that merely
 * probe them keep running.
 */

EXPORT(SceAppUtil, 0xC97D5D9E, int, sceAppUtilAddCookieWebBrowser, void *param)
{
    (void)param;
    return SCE_APPUTIL_ERROR_PARAMETER;
}

EXPORT(SceAppUtil, 0x4FAAD133, int, sceAppUtilResetCookieWebBrowser, void)
{
    return 0;
}

EXPORT(SceAppUtil, 0x53B2C020, int, sceAppUtilAddcontMount, void *param)
{
    (void)param;
    return SCE_APPUTIL_ERROR_PARAMETER;
}

EXPORT(SceAppUtil, 0x1B36AF8C, int, sceAppUtilAddcontUmount, void *param)
{
    (void)param;
    return 0;
}

EXPORT(SceAppUtil, 0x2DB7BE3B, int, sceAppUtilDrmOpen, void *param)
{
    (void)param;
    return SCE_APPUTIL_ERROR_PARAMETER;
}

EXPORT(SceAppUtil, 0x6A140498, int, sceAppUtilDrmClose, void *param)
{
    (void)param;
    return SCE_APPUTIL_ERROR_PARAMETER;
}

EXPORT(SceAppUtil, 0x9942071D, int, sceAppUtilPspSaveDataGetDirNameList, void *param)
{
    (void)param;
    return SCE_APPUTIL_ERROR_PARAMETER;
}

EXPORT(SceAppUtil, 0x7402C6EA, int, sceAppUtilPspSaveDataLoad, void *param)
{
    (void)param;
    return SCE_APPUTIL_ERROR_PARAMETER;
}

EXPORT(SceAppUtil, 0xE6057A85, int, sceAppUtilSaveDataSlotSearch, void *work_buf,
       const void *cond, void *result, SceAppUtilSaveDataMountPoint *mount_point)
{
    (void)work_buf;
    (void)cond;
    (void)result;
    (void)mount_point;

    if (save_debug())
        fprintf(stderr, "vita2native: SaveDataSlotSearch (unsupported)\n");

    /* Slots can be read individually with SlotGetParam; enumeration needs the
       search condition/result layouts, which are not in the public headers. */
    return SCE_APPUTIL_ERROR_PARAMETER;
}

#define APP_EVENT_PARSER(nid, name)                                                                \
    EXPORT(SceAppUtil, nid, int, name, const SceAppUtilAppEventParam *event_param, void *out)       \
    {                                                                                              \
        (void)event_param;                                                                         \
        (void)out;                                                                                 \
        return SCE_APPUTIL_ERROR_APPEVENT_PARSE_INVALID_DATA;                                      \
    }

APP_EVENT_PARSER(0x0B04C067, sceAppUtilAppEventParseGameCustomData)
APP_EVENT_PARSER(0x22297D59, sceAppUtilAppEventParseIncomingDialog)
APP_EVENT_PARSER(0x77380601, sceAppUtilAppEventParseNearGift)
APP_EVENT_PARSER(0xF6B75651, sceAppUtilAppEventParseNpActivity)
APP_EVENT_PARSER(0x6BED9B58, sceAppUtilAppEventParseNpAppDataMessage)
APP_EVENT_PARSER(0x28C7D4F6, sceAppUtilAppEventParseNpBasicJoinablePresence)
APP_EVENT_PARSER(0xA2496814, sceAppUtilAppEventParseNpInviteMessage)
APP_EVENT_PARSER(0x2AF42D6A, sceAppUtilAppEventParseScreenShotNotification)
APP_EVENT_PARSER(0xA58E6FCE, sceAppUtilAppEventParseSessionInvitation)
APP_EVENT_PARSER(0x665A8B7E, sceAppUtilAppEventParseTeleport)
APP_EVENT_PARSER(0x8DEE696B, sceAppUtilAppEventParseTriggerUtil)
APP_EVENT_PARSER(0x8ED716F5, sceAppUtilAppEventParseWebBrowser)
