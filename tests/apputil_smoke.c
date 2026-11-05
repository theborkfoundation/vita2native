/*
 * Exercises SceAppUtil save data against a real directory: slot lifecycle,
 * file writes/removals and the system parameters titles read during boot.
 */

#include <psp2/apputil.h>
#include <psp2/system_param.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "fs.h"

int sceAppUtilInit(SceAppUtilInitParam *init_param, SceAppUtilBootParam *boot_param);
int sceAppUtilShutdown(void);
int sceAppUtilSaveDataSlotCreate(unsigned int slot_id, SceAppUtilSaveDataSlotParam *param,
                                 SceAppUtilSaveDataMountPoint *mount_point);
int sceAppUtilSaveDataSlotDelete(unsigned int slot_id, SceAppUtilSaveDataMountPoint *mount_point);
int sceAppUtilSaveDataSlotGetParam(unsigned int slot_id, SceAppUtilSaveDataSlotParam *param,
                                   SceAppUtilSaveDataMountPoint *mount_point);
int sceAppUtilSaveDataDataSave(SceAppUtilSaveDataFileSlot *slot, SceAppUtilSaveDataFile *files,
                               unsigned int file_num, SceAppUtilSaveDataMountPoint *mount_point,
                               SceSize *required_size_kb);
int sceAppUtilSaveDataDataRemove(SceAppUtilSaveDataFileSlot *slot,
                                 SceAppUtilSaveDataRemoveItem *files, unsigned int file_num,
                                 SceAppUtilSaveDataMountPoint *mount_point);
int sceAppUtilSaveDataGetQuota(SceSize *quota_size_kb, SceSize *used_size_kb,
                               SceAppUtilSaveDataMountPoint *mount_point);
int sceAppUtilSystemParamGetInt(unsigned int param_id, int *value);
int sceAppUtilSystemParamGetString(unsigned int param_id, SceChar8 *buffer, SceSize buffer_size);

#define TEST_ROOT "/tmp/v2n-apputil"

int main(void)
{
    SceAppUtilSaveDataSlotParam param, read_back;
    SceAppUtilSaveDataMountPoint mount;
    SceAppUtilSaveDataFileSlot file_slot;
    SceAppUtilSaveDataFile file;
    SceAppUtilSaveDataRemoveItem remove_item;
    SceAppUtilInitParam init_param;
    SceAppUtilBootParam boot_param;
    SceSize quota = 0, used = 0, required = 123;
    char check_path[256];
    char text[16];
    FILE *fp;
    int value = 0;
    int ret;

    mkdir(TEST_ROOT, 0755);
    if (host_fs_init(TEST_ROOT, NULL) != 0) {
        puts("apputil_smoke: could not initialise the VFS");
        return 1;
    }

    memset(&init_param, 0, sizeof(init_param));
    memset(&boot_param, 0, sizeof(boot_param));
    memset(&mount, 0, sizeof(mount));

    if (sceAppUtilInit(&init_param, &boot_param) != 0) {
        puts("apputil_smoke: init failed");
        return 1;
    }

    /* Slot lifecycle. */
    memset(&param, 0, sizeof(param));
    param.status = 1;
    param.userParam = 0x1234;
    memcpy(param.title, "slot", 10);

    if (sceAppUtilSaveDataSlotCreate(0, &param, &mount) != 0) {
        puts("apputil_smoke: slot create failed");
        return 1;
    }
    if (sceAppUtilSaveDataSlotCreate(0, &param, &mount) !=
        (int)SCE_APPUTIL_ERROR_SAVEDATA_SLOT_EXISTS) {
        puts("apputil_smoke: duplicate slot create did not fail");
        return 1;
    }

    memset(&read_back, 0, sizeof(read_back));
    if (sceAppUtilSaveDataSlotGetParam(0, &read_back, &mount) != 0 ||
        read_back.userParam != 0x1234) {
        puts("apputil_smoke: slot get param mismatch");
        return 1;
    }

    /* Save one file. */
    memset(&file_slot, 0, sizeof(file_slot));
    file_slot.id = 0;
    memset(&file, 0, sizeof(file));
    file.filePath = "data.bin";
    file.buf = "hello";
    file.bufSize = 6;
    file.mode = 0;

    if (sceAppUtilSaveDataDataSave(&file_slot, &file, 1, &mount, &required) != 0 || required != 0) {
        puts("apputil_smoke: data save failed");
        return 1;
    }

    snprintf(check_path, sizeof(check_path), "%s/savedata/data.bin", TEST_ROOT);
    fp = fopen(check_path, "rb");
    if (!fp || fread(text, 1, sizeof(text), fp) != 6 || memcmp(text, "hello", 6) != 0) {
        puts("apputil_smoke: saved file contents are wrong");
        return 1;
    }
    fclose(fp);

    /* Remove it again. */
    memset(&remove_item, 0, sizeof(remove_item));
    remove_item.dataPath = "data.bin";
    remove_item.mode = SCE_APPUTIL_SAVEDATA_DATA_REMOVE_MODE_NO_SLOT;
    if (sceAppUtilSaveDataDataRemove(&file_slot, &remove_item, 1, &mount) != 0) {
        puts("apputil_smoke: data remove failed");
        return 1;
    }
    if (fopen(check_path, "rb") != NULL) {
        puts("apputil_smoke: file still exists after remove");
        return 1;
    }

    if (sceAppUtilSaveDataGetQuota(&quota, &used, &mount) != 0 || quota == 0) {
        puts("apputil_smoke: quota query failed");
        return 1;
    }

    if (sceAppUtilSaveDataSlotDelete(0, &mount) != 0) {
        puts("apputil_smoke: slot delete failed");
        return 1;
    }
    if ((ret = sceAppUtilSaveDataSlotGetParam(0, &read_back, &mount)) !=
        (int)SCE_APPUTIL_ERROR_SAVEDATA_SLOT_NOT_FOUND) {
        printf("apputil_smoke: deleted slot still readable: 0x%x\n", (unsigned)ret);
        return 1;
    }

    /* System parameters. */
    if (sceAppUtilSystemParamGetInt(SCE_SYSTEM_PARAM_ID_LANG, &value) != 0 ||
        value != SCE_SYSTEM_PARAM_LANG_ENGLISH_US) {
        puts("apputil_smoke: language parameter is wrong");
        return 1;
    }
    if (sceAppUtilSystemParamGetString(SCE_SYSTEM_PARAM_ID_USERNAME, (SceChar8 *)text,
                                       sizeof(text)) != 0 ||
        strcmp(text, "vita2native") != 0) {
        puts("apputil_smoke: username parameter is wrong");
        return 1;
    }

    if (sceAppUtilShutdown() != 0) {
        puts("apputil_smoke: shutdown failed");
        return 1;
    }

    puts("apputil_smoke: ok");
    return 0;
}
