/*
 * Common-dialog overlay test: message/system/error/progress/IME state, the
 * dismissal timing and the software renderer.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <psp2/ime_dialog.h>
#include <psp2/message_dialog.h>

#include "modules/common_dialog_overlay.h"

const void *module_get_func_export(uint32_t lib_nid, uint32_t func_nid, void *importer);

static int failures;

#define CHECK(condition, ...)                                                                      \
    do {                                                                                           \
        if (!(condition)) {                                                                        \
            fprintf(stderr, __VA_ARGS__);                                                          \
            fprintf(stderr, "\n");                                                                 \
            failures++;                                                                            \
        }                                                                                          \
    } while (0)

static int check_export(uint32_t lib_nid, uint32_t func_nid, const char *what)
{
    if (module_get_func_export(lib_nid, func_nid, NULL))
        return 0;
    fprintf(stderr, "%s (0x%08x/0x%08x) is not in the export table\n", what, lib_nid, func_nid);
    return 1;
}

static void write_bmp(const char *path, const uint32_t *pixels, int width, int height)
{
    unsigned char header[54];
    unsigned char row[480 * 3];
    unsigned int row_size = (unsigned int)((width * 3 + 3) & ~3);
    FILE *file = fopen(path, "wb");

    if (!file)
        return;

    memset(header, 0, sizeof(header));
    header[0] = 'B';
    header[1] = 'M';
    header[2] = (unsigned char)(54 + row_size * height);
    header[3] = (unsigned char)((54 + row_size * height) >> 8);
    header[10] = 54;
    header[14] = 40;
    header[18] = (unsigned char)width;
    header[19] = (unsigned char)(width >> 8);
    header[22] = (unsigned char)height;
    header[23] = (unsigned char)(height >> 8);
    header[26] = 1;
    header[28] = 24;
    fwrite(header, 1, sizeof(header), file);

    for (int y = height; y-- > 0;) {
        for (int x = 0; x < width; x++) {
            uint32_t pixel = pixels[y * width + x];

            row[x * 3 + 0] = (unsigned char)(pixel & 0xFF);
            row[x * 3 + 1] = (unsigned char)((pixel >> 8) & 0xFF);
            row[x * 3 + 2] = (unsigned char)((pixel >> 16) & 0xFF);
        }
        fwrite(row, 1, row_size, file);
    }

    fclose(file);
}

static void finish_dialog(void)
{
    SceCommonDialog_overlayPresented();
    SceCommonDialog_overlayPresented();
    usleep(1100000);
}

int main(void)
{
    SceMsgDialogParam param;
    SceMsgDialogUserMessageParam user_msg;
    SceMsgDialogSystemMessageParam system_msg;
    SceMsgDialogErrorCodeParam error_msg;
    SceMsgDialogProgressBarParam progress_msg;
    SceMsgDialogResult result;
    CommonDialogOverlay overlay;
    uint32_t *pixels;
    uint32_t non_background = 0;

    failures += check_export(0xe537816c, 0x755FF270, "sceMsgDialogInit");
    failures += check_export(0xe537816c, 0x90530F2F, "sceCommonDialogUpdate");

    /* User message. */
    memset(&param, 0, sizeof(param));
    memset(&user_msg, 0, sizeof(user_msg));
    user_msg.msg = (const SceChar8 *)"Hello from vita2native";
    user_msg.buttonType = SCE_MSG_DIALOG_BUTTON_TYPE_OK;
    param.mode = SCE_MSG_DIALOG_MODE_USER_MSG;
    param.userMsgParam = &user_msg;
    CHECK(sceMsgDialogInit(&param) == 0, "message dialog init failed");
    CHECK(sceMsgDialogGetStatus() == SCE_COMMON_DIALOG_STATUS_RUNNING,
          "dialog was already finished");

    memset(&overlay, 0, sizeof(overlay));
    CHECK(SceCommonDialog_getOverlay(&overlay), "no overlay while the dialog is running");
    CHECK(strstr(overlay.message, "Hello") != NULL, "overlay message is '%s'", overlay.message);
    CHECK(overlay.progress == -1, "message dialog reported a progress bar");

    pixels = calloc(480 * 272, sizeof(uint32_t));
    CHECK(pixels != NULL, "out of memory");
    if (pixels) {
        const char *dump = getenv("VITA2NATIVE_DIALOG_DUMP");

        CommonDialogOverlay_render(&overlay, pixels, 480, 272, 480);
        for (int i = 0; i < 480 * 272; i++)
            non_background += pixels[i] != 0;
        CHECK(non_background > 1000, "overlay rendered only %u pixels", non_background);
        if (dump)
            write_bmp(dump, pixels, 480, 272);
        free(pixels);
    }

    finish_dialog();
    CHECK(sceMsgDialogGetStatus() == SCE_COMMON_DIALOG_STATUS_FINISHED,
          "dialog did not finish after two presents and one second");
    memset(&result, 0, sizeof(result));
    CHECK(sceMsgDialogGetResult(&result) == 0, "get result failed");
    CHECK(result.mode == SCE_MSG_DIALOG_MODE_USER_MSG && result.result == SCE_COMMON_DIALOG_RESULT_OK,
          "result is mode %d result %d", result.mode, result.result);
    CHECK(sceMsgDialogTerm() == 0, "term failed");
    CHECK(sceMsgDialogGetStatus() == SCE_COMMON_DIALOG_STATUS_NONE, "dialog still active after term");
    CHECK(!SceCommonDialog_getOverlay(&overlay), "overlay still active after term");

    /* System message. */
    memset(&param, 0, sizeof(param));
    memset(&system_msg, 0, sizeof(system_msg));
    system_msg.sysMsgType = SCE_MSG_DIALOG_SYSMSG_TYPE_WAIT;
    param.mode = SCE_MSG_DIALOG_MODE_SYSTEM_MSG;
    param.sysMsgParam = &system_msg;
    CHECK(sceMsgDialogInit(&param) == 0, "system dialog init failed");
    CHECK(SceCommonDialog_getOverlay(&overlay) && strstr(overlay.message, "wait") != NULL,
          "system dialog message is '%s'", overlay.message);
    sceMsgDialogTerm();

    /* Error code. */
    memset(&param, 0, sizeof(param));
    memset(&error_msg, 0, sizeof(error_msg));
    error_msg.errorCode = 0x80010001;
    param.mode = SCE_MSG_DIALOG_MODE_ERROR_CODE;
    param.errorCodeParam = &error_msg;
    CHECK(sceMsgDialogInit(&param) == 0, "error dialog init failed");
    CHECK(SceCommonDialog_getOverlay(&overlay) && strstr(overlay.message, "80010001") != NULL,
          "error dialog message is '%s'", overlay.message);
    sceMsgDialogTerm();

    /* Progress bar: stays up until the application closes it. */
    memset(&param, 0, sizeof(param));
    memset(&progress_msg, 0, sizeof(progress_msg));
    progress_msg.msg = (const SceChar8 *)"Copying files";
    progress_msg.sysMsgParam.sysMsgType = SCE_MSG_DIALOG_SYSMSG_TYPE_WAIT;
    param.mode = SCE_MSG_DIALOG_MODE_PROGRESS_BAR;
    param.progBarParam = &progress_msg;
    CHECK(sceMsgDialogInit(&param) == 0, "progress dialog init failed");
    CHECK(sceMsgDialogProgressBarSetValue(SCE_MSG_DIALOG_PROGRESSBAR_TARGET_BAR_DEFAULT, 50) == 0,
          "progress set failed");
    CHECK(SceCommonDialog_getOverlay(&overlay) && overlay.progress == 50,
          "progress is %d", overlay.progress);
    finish_dialog();
    CHECK(sceMsgDialogGetStatus() == SCE_COMMON_DIALOG_STATUS_RUNNING,
          "progress dialog auto-finished");
    CHECK(sceMsgDialogClose() == 0 && sceMsgDialogGetStatus() == SCE_COMMON_DIALOG_STATUS_NONE,
          "progress dialog close failed");

    /* IME shows its initial text. */
    {
        SceImeDialogParam ime;
        SceWChar16 buffer[16];
        SceWChar16 initial[6] = { 'h', 'e', 'l', 'l', 'o', 0 };

        memset(&ime, 0, sizeof(ime));
        memset(buffer, 0, sizeof(buffer));
        ime.inputTextBuffer = buffer;
        ime.maxTextLength = 16;
        ime.initialText = initial;
        CHECK(sceImeDialogInit(&ime) == 0, "IME init failed");
        CHECK(sceImeDialogGetStatus() == SCE_COMMON_DIALOG_STATUS_RUNNING,
              "IME was already finished");
        CHECK(SceCommonDialog_getOverlay(&overlay) && strcmp(overlay.message, "hello") == 0,
              "IME overlay message is '%s'", overlay.message);
        finish_dialog();
        CHECK(sceImeDialogGetStatus() == SCE_COMMON_DIALOG_STATUS_FINISHED,
              "IME did not finish");
        CHECK(sceImeDialogGetResult(NULL) < 0, "null IME result was accepted");
        sceImeDialogTerm();
        CHECK(sceImeDialogGetStatus() == SCE_COMMON_DIALOG_STATUS_NONE, "IME still active");
    }

    if (failures) {
        fprintf(stderr, "dialog_overlay_smoke: %d failure(s)\n", failures);
        return 1;
    }

    printf("dialog_overlay_smoke: ok\n");
    return 0;
}
