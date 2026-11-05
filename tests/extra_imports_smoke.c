/*
 * Smoke test for the optional VitaShell import libraries: ad-hoc networking,
 * media export, promoter, USB virtual storage, MTP and Udcd.
 */
#include <stdio.h>
#include <string.h>

#include <psp2/mtpif.h>
#include <psp2/musicexport.h>
#include <psp2/photoexport.h>
#include <psp2/promoterutil.h>
#include <psp2/pspnet_adhoc.h>
#include <psp2/pspnet_adhocctl.h>
#include <psp2/udcd.h>
#include <psp2/usbstorvstor.h>
#include <psp2/videoexport.h>

/* Present in the NID database but not in the public header. */
int scePhotoExportIsAvailableFromFile(const char *path);
int scePhotoExportIsAvailableFromData(const void *data, SceSize size);

const void *module_get_func_export(uint32_t lib_nid, uint32_t func_nid, void *importer);

static int check_export(uint32_t lib_nid, uint32_t func_nid, const char *what)
{
    if (module_get_func_export(lib_nid, func_nid, NULL))
        return 0;
    fprintf(stderr, "%s (0x%08x/0x%08x) is not in the export table\n", what, lib_nid,
            func_nid);
    return 1;
}

static int failures;

#define CHECK(condition, message)                                                                  \
    do {                                                                                           \
        if (!(condition)) {                                                                        \
            fprintf(stderr, "%s\n", message);                                                      \
            failures++;                                                                            \
        }                                                                                          \
    } while (0)

int main(void)
{
    int value;

    failures += check_export(0x19C6AFDC, 0x87805E13, "sceNetAdhocInit");
    failures += check_export(0x19C6AFDC, 0xE2FE9DD6, "sceNetAdhocctlInit");
    failures += check_export(0xF2E30EED, 0x08000579, "sceMusicExportFromFile");
    failures += check_export(0xF0812A7C, 0x4F60A3C8, "sceVideoExportFromFile");
    failures += check_export(0x79166BD9, 0x84FD9FC5, "scePhotoExportFromFile");
    failures += check_export(0x31F237B6, 0x93451536, "scePromoterUtilityInit");
    failures += check_export(0x17F294B9, 0xB606F1AF, "sceUsbstorVStorStart");
    failures += check_export(0x5EFA4138, 0xC5327CAC, "sceMtpIfStartDriver");
    failures += check_export(0xA84BDE8A, 0xFCD31220, "sceUdcdGetDeviceState");

    /* Ad-hoc networking reports an empty network without blocking. */
    CHECK(sceNetAdhocInit() == 0, "sceNetAdhocInit failed");
    CHECK(sceNetAdhocPtpOpen(NULL, 0, NULL, 0, 0, 0, 0, 0) < 0, "ptp open unexpectedly worked");
    value = -1;
    CHECK(sceNetAdhocctlGetPeerList(&value, NULL) == 0 && value == 0,
          "peer list was not empty");
    {
        SceNetEtherAddr addr;

        memset(&addr, 0, sizeof(addr));
        CHECK(sceNetAdhocctlGetEtherAddr(&addr) == 0 && addr.data[0] == 0x02,
              "local ether address was not reported");
    }
    CHECK(sceNetAdhocTerm() == 0, "sceNetAdhocTerm failed");

    /* Media export is explicitly unavailable. */
    {
        char out[64] = { 0 };

        CHECK(scePhotoExportIsAvailableFromFile("ux0:/photo.jpg") == 0,
              "photo export claimed to be available");
        CHECK(scePhotoExportIsAvailableFromData(out, sizeof(out)) == 0,
              "photo data export claimed to be available");
        CHECK(scePhotoExportFromFile("ux0:/photo.jpg", NULL, NULL, NULL, NULL, out,
                                     sizeof(out)) < 0,
              "photo export unexpectedly succeeded");
        CHECK(sceMusicExportFromFile("ux0:/song.mp3", NULL, NULL, NULL, NULL, NULL, out,
                                     sizeof(out)) < 0,
              "music export unexpectedly succeeded");
        CHECK(sceVideoExportFromFile(NULL, 0, NULL, NULL, NULL, NULL, 0, NULL) < 0,
              "video export unexpectedly succeeded");
    }

    /* Promoter lifecycle works; installations fail. */
    CHECK(scePromoterUtilityInit() == 0, "promoter init failed");
    value = -1;
    CHECK(scePromoterUtilityGetState(&value) == 0 && value == 0, "promoter state failed");
    value = -1;
    CHECK(scePromoterUtilityGetResult(&value) == 0 && value == 0, "promoter result failed");
    value = -1;
    CHECK(scePromoterUtilityCheckExist("PCSA00001", &value) == 0 && value == 0,
          "promoter existence check failed");
    CHECK(scePromoterUtilityPromotePkg("ux0:/app/PCSA00001", 1) < 0,
          "package promotion unexpectedly succeeded");
    CHECK(scePromoterUtilityExit() == 0, "promoter exit failed");

    /* USB storage and MTP cannot start, but configuration is accepted. */
    CHECK(sceUsbstorVStorSetImgFilePath("ux0:/image.bin") == 0, "vstor path failed");
    CHECK(sceUsbstorVStorSetDeviceInfo("vita2native", "1.0") == 0, "vstor info failed");
    CHECK(sceUsbstorVStorStart(SCE_USBSTOR_VSTOR_TYPE_FAT) < 0, "vstor start worked");
    CHECK(sceUsbstorVStorStop() == 0, "vstor stop failed");
    CHECK(sceMtpIfStartDriver(1) < 0, "MTP start worked");
    CHECK(sceMtpIfStopDriver(1) == 0, "MTP stop failed");

    /* USB device controller reports a disconnected cable. */
    {
        SceUdcdDeviceState state;

        memset(&state, 0xFF, sizeof(state));
        CHECK(sceUdcdGetDeviceState(&state) == 0 && state.state == 0 && state.cable == 0,
              "udcd state was not zeroed");
    }
    CHECK(sceUdcdGetDrvState("usbstor") == SCE_UDCD_STATUS_DRIVER_REGISTERED,
          "udcd driver state failed");
    CHECK(sceUdcdRegisterCallback(1, 0) == 0, "udcd register failed");
    CHECK(sceUdcdUnregisterCallback(1) == 0, "udcd unregister failed");
    CHECK(sceUdcdWaitState(NULL, 0) < 0, "udcd wait unexpectedly matched");

    if (failures) {
        fprintf(stderr, "extra_imports_smoke: %d failure(s)\n", failures);
        return 1;
    }

    printf("extra_imports_smoke: ok\n");
    return 0;
}
