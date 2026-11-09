/*
 * SceCommonDialog: system dialogs.
 *
 * There is no system UI on this host. Message dialogs and the IME complete
 * immediately: the IME accepts its initial text so rename/new-name flows keep
 * working. Dialog families that need PSN, the camera, the photo application or
 * a store account report SCE_COMMON_DIALOG_ERROR_NOT_SUPPORTED from Init,
 * which lets callers take their fallback path.
 */

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <psp2/common_dialog.h>
#include <psp2/ime_dialog.h>
#include <psp2/message_dialog.h>

#include "host.h"
#include "module.h"
#include "modules/common_dialog_overlay.h"

static bool g_msg_dialog_active;
static bool g_msg_dialog_done;
static int g_msg_dialog_mode;
static char g_msg_text[SCE_MSG_DIALOG_USER_MSG_SIZE];
static char g_msg_title[64];
static char g_msg_hint[64];
static int g_msg_progress = -1;
static uint64_t g_msg_start_ns;
static unsigned int g_msg_presents;
static unsigned int g_msg_updates;

static bool g_ime_active;
static bool g_ime_done;
static SceWChar16 *g_ime_buffer;
static SceUInt32 g_ime_max_length;
static char g_ime_text[256];
static uint64_t g_ime_start_ns;
static unsigned int g_ime_presents;
static unsigned int g_ime_updates;

static void dialog_copy(char *dst, size_t size, const char *src)
{
    if (!size)
        return;
    if (!src)
        src = "";
    snprintf(dst, size, "%s", src);
}

static const char *system_message_text(int type)
{
    switch (type) {
    case SCE_MSG_DIALOG_SYSMSG_TYPE_WAIT:
    case SCE_MSG_DIALOG_SYSMSG_TYPE_WAIT_SMALL:
        return "Please wait.";
    case SCE_MSG_DIALOG_SYSMSG_TYPE_NOSPACE:
        return "There is not enough free space on the memory card.";
    case SCE_MSG_DIALOG_SYSMSG_TYPE_MAGNETIC_CALIBRATION:
        return "Move away from the source of interference and adjust the compass.";
    case SCE_MSG_DIALOG_SYSMSG_TYPE_WAIT_CANCEL:
        return "Please wait...";
    case SCE_MSG_DIALOG_SYSMSG_TYPE_NEED_MC_CONTINUE:
        return "Cannot continue. No memory card is inserted.";
    case SCE_MSG_DIALOG_SYSMSG_TYPE_NEED_MC_OPERATION:
        return "Cannot perform this operation. No memory card is inserted.";
    case SCE_MSG_DIALOG_SYSMSG_TYPE_TRC_MIC_DISABLED:
        return "You must enable the microphone.";
    case SCE_MSG_DIALOG_SYSMSG_TYPE_TRC_WIFI_REQUIRED_OPERATION:
        return "You must use Wi-Fi to do this.";
    case SCE_MSG_DIALOG_SYSMSG_TYPE_TRC_WIFI_REQUIRED_APPLICATION:
        return "You must use Wi-Fi to use this application.";
    case SCE_MSG_DIALOG_SYSMSG_TYPE_TRC_EMPTY_STORE:
        return "No content is available yet.";
    default:
        return "Please wait.";
    }
}

static const char *button_hint(int button_type)
{
    switch (button_type) {
    case SCE_MSG_DIALOG_BUTTON_TYPE_YESNO:
        return "X  Yes    O  No";
    case SCE_MSG_DIALOG_BUTTON_TYPE_NONE:
        return "";
    case SCE_MSG_DIALOG_BUTTON_TYPE_OK_CANCEL:
        return "X  OK    O  Cancel";
    case SCE_MSG_DIALOG_BUTTON_TYPE_CANCEL:
        return "O  Cancel";
    case SCE_MSG_DIALOG_BUTTON_TYPE_3BUTTONS:
    case SCE_MSG_DIALOG_BUTTON_TYPE_OK:
    default:
        return "X  OK";
    }
}

static bool dialog_timed_out(uint64_t start)
{
    return host_monotonic_ns() - start >= 1000000000ull;
}

static void msg_update_done(void)
{
    if (g_msg_dialog_active && !g_msg_dialog_done && g_msg_progress < 0 &&
        (g_msg_presents >= 2 || g_msg_updates >= 4) && dialog_timed_out(g_msg_start_ns))
        g_msg_dialog_done = true;
}

static void ime_update_done(void)
{
    if (g_ime_active && !g_ime_done && (g_ime_presents >= 2 || g_ime_updates >= 4) &&
        dialog_timed_out(g_ime_start_ns))
        g_ime_done = true;
}

EXPORT(SceCommonDialog, 0x90530F2F, int, sceCommonDialogUpdate, const SceCommonDialogUpdateParam *param)
{
    (void)param;

    if (g_msg_dialog_active && !g_msg_dialog_done)
        g_msg_updates++;
    if (g_ime_active && !g_ime_done)
        g_ime_updates++;
    msg_update_done();
    ime_update_done();

    return 0;
}

EXPORT(SceCommonDialog, 0x043A353E, int, sceCommonDialogIsRunning, void)
{
    msg_update_done();
    ime_update_done();

    return ((g_msg_dialog_active && !g_msg_dialog_done) ||
            (g_ime_active && !g_ime_done))
               ? 1
               : 0;
}

EXPORT(SceCommonDialog, 0xD4C37375, SceUID, sceCommonDialogGetWorkerThreadId, void)
{
    return 0;
}

EXPORT(SceCommonDialog, 0xBECD35C8, int, sceCommonDialogSetConfigParam,
       const SceCommonDialogConfigParam *param)
{
    (void)param;

    return 0;
}

/* Message dialogs are drawn over the guest framebuffer for a couple of frames. */
EXPORT(SceCommonDialog, 0x755FF270, int, sceMsgDialogInit, const SceMsgDialogParam *param)
{
    const SceMsgDialogUserMessageParam *user_msg;
    const SceMsgDialogProgressBarParam *progress;

    if (!param)
        return -1;

    g_msg_dialog_mode = param->mode;
    g_msg_dialog_active = true;
    g_msg_dialog_done = false;
    g_msg_progress = -1;
    g_msg_presents = 0;
    g_msg_updates = 0;
    g_msg_start_ns = host_monotonic_ns();
    dialog_copy(g_msg_title, sizeof(g_msg_title), "Message");
    dialog_copy(g_msg_hint, sizeof(g_msg_hint), button_hint(SCE_MSG_DIALOG_BUTTON_TYPE_OK));

    switch (param->mode) {
    case SCE_MSG_DIALOG_MODE_USER_MSG:
        user_msg = param->userMsgParam;
        dialog_copy(g_msg_text, sizeof(g_msg_text), user_msg ? (const char *)user_msg->msg : "");
        if (user_msg)
            dialog_copy(g_msg_hint, sizeof(g_msg_hint), button_hint(user_msg->buttonType));
        break;
    case SCE_MSG_DIALOG_MODE_SYSTEM_MSG:
        dialog_copy(g_msg_title, sizeof(g_msg_title), "System");
        dialog_copy(g_msg_text, sizeof(g_msg_text),
                    system_message_text(param->sysMsgParam ? param->sysMsgParam->sysMsgType : 0));
        break;
    case SCE_MSG_DIALOG_MODE_ERROR_CODE:
        dialog_copy(g_msg_title, sizeof(g_msg_title), "Error");
        snprintf(g_msg_text, sizeof(g_msg_text), "Error 0x%08X",
                 param->errorCodeParam ? (unsigned int)param->errorCodeParam->errorCode : 0u);
        break;
    case SCE_MSG_DIALOG_MODE_PROGRESS_BAR:
        progress = param->progBarParam;
        dialog_copy(g_msg_title, sizeof(g_msg_title), "Please wait");
        dialog_copy(g_msg_text, sizeof(g_msg_text),
                    progress && progress->msg ? (const char *)progress->msg
                                             : system_message_text(progress ? progress->sysMsgParam.sysMsgType : 0));
        dialog_copy(g_msg_hint, sizeof(g_msg_hint), "");
        g_msg_progress = 0;
        break;
    default:
        dialog_copy(g_msg_text, sizeof(g_msg_text), "");
        break;
    }

    if (getenv("VITA2NATIVE_INPUT_DEBUG")) {
        fprintf(stderr, "vita2native: MsgDialog init mode=%d text=\"%s\" hint=\"%s\"\n",
                g_msg_dialog_mode, g_msg_text, g_msg_hint);
        fflush(stderr);
    }

    return 0;
}

EXPORT(SceCommonDialog, 0x4107019E, SceCommonDialogStatus, sceMsgDialogGetStatus, void)
{
    msg_update_done();

    if (getenv("VITA2NATIVE_INPUT_DEBUG")) {
        static int last = -1;

        if (!g_msg_dialog_active)
            last = -1;
        else if (last != (g_msg_dialog_done ? 1 : 0)) {
            last = g_msg_dialog_done ? 1 : 0;
            fprintf(stderr, "vita2native: MsgDialog status %s\n",
                    g_msg_dialog_done ? "finished" : "running");
            fflush(stderr);
        }
    }

    if (!g_msg_dialog_active)
        return SCE_COMMON_DIALOG_STATUS_NONE;
    return g_msg_dialog_done ? SCE_COMMON_DIALOG_STATUS_FINISHED
                             : SCE_COMMON_DIALOG_STATUS_RUNNING;
}

EXPORT(SceCommonDialog, 0xBB3BFC89, int, sceMsgDialogGetResult, SceMsgDialogResult *result)
{
    if (!result)
        return -1;

    memset(result, 0, sizeof(*result));
    result->mode = g_msg_dialog_mode;
    result->result = SCE_COMMON_DIALOG_RESULT_OK;
    return 0;
}

EXPORT(SceCommonDialog, 0x81ACF695, int, sceMsgDialogTerm, void)
{
    g_msg_dialog_active = false;
    g_msg_dialog_done = false;
    g_msg_progress = -1;
    return 0;
}

EXPORT(SceCommonDialog, 0x0CC66115, int, sceMsgDialogAbort, void)
{
    g_msg_dialog_active = false;
    g_msg_dialog_done = false;
    g_msg_progress = -1;
    return 0;
}

EXPORT(SceCommonDialog, 0xC296D396, int, sceMsgDialogClose, void)
{
    g_msg_dialog_active = false;
    g_msg_dialog_done = false;
    g_msg_progress = -1;
    return 0;
}

EXPORT(SceCommonDialog, 0x7BE0E08B, int, sceMsgDialogProgressBarInc,
       SceMsgDialogProgressBarTarget target, SceUInt32 delta)
{
    (void)target;
    if (g_msg_progress >= 0) {
        g_msg_progress = (int)((SceUInt32)g_msg_progress + delta);
        if (g_msg_progress > 100)
            g_msg_progress = 100;
    }
    return 0;
}

EXPORT(SceCommonDialog, 0x0150A451, int, sceMsgDialogProgressBarSetMsg,
       SceMsgDialogProgressBarTarget target, const SceChar8 *message)
{
    (void)target;
    if (message)
        dialog_copy(g_msg_text, sizeof(g_msg_text), (const char *)message);
    return 0;
}

EXPORT(SceCommonDialog, 0x9CDA5E0D, int, sceMsgDialogProgressBarSetValue,
       SceMsgDialogProgressBarTarget target, SceUInt32 value)
{
    (void)target;
    g_msg_progress = value > 100 ? 100 : (int)value;
    return 0;
}

/* The IME accepts the caller's initial text ("OK" on the default value). */
EXPORT(SceCommonDialog, 0x1E7043BF, SceInt32, sceImeDialogInit, const SceImeDialogParam *param)
{
    if (!param)
        return SCE_COMMON_DIALOG_ERROR_NULL;
    if (!param->inputTextBuffer || param->maxTextLength == 0)
        return SCE_COMMON_DIALOG_ERROR_INVALID_ARGUMENT;

    g_ime_active = true;
    g_ime_done = false;
    g_ime_buffer = param->inputTextBuffer;
    g_ime_max_length = param->maxTextLength;
    g_ime_presents = 0;
    g_ime_updates = 0;
    g_ime_start_ns = host_monotonic_ns();

    if (param->initialText) {
        SceUInt32 i;

        for (i = 0; i + 1 < g_ime_max_length && param->initialText[i]; i++)
            g_ime_buffer[i] = param->initialText[i];
        g_ime_buffer[i] = 0;
    } else {
        g_ime_buffer[0] = 0;
    }

    {
        SceUInt32 i;

        for (i = 0; i + 1 < sizeof(g_ime_text) && g_ime_buffer[i]; i++) {
            SceWChar16 c = g_ime_buffer[i];

            g_ime_text[i] = (c >= 32 && c < 127) ? (char)c : '?';
        }
        g_ime_text[i] = 0;
    }

    return 0;
}

EXPORT(SceCommonDialog, 0xCF0431FD, SceCommonDialogStatus, sceImeDialogGetStatus, void)
{
    ime_update_done();

    if (!g_ime_active)
        return SCE_COMMON_DIALOG_STATUS_NONE;
    return g_ime_done ? SCE_COMMON_DIALOG_STATUS_FINISHED : SCE_COMMON_DIALOG_STATUS_RUNNING;
}

EXPORT(SceCommonDialog, 0x2EB3D046, SceInt32, sceImeDialogGetResult, SceImeDialogResult *result)
{
    if (!result)
        return SCE_COMMON_DIALOG_ERROR_NULL;

    memset(result, 0, sizeof(*result));
    result->result = SCE_COMMON_DIALOG_RESULT_OK;
    result->button = SCE_IME_DIALOG_BUTTON_ENTER;
    return 0;
}

EXPORT(SceCommonDialog, 0x594A220E, SceInt32, sceImeDialogAbort, void)
{
    g_ime_active = false;
    g_ime_done = false;
    return 0;
}

EXPORT(SceCommonDialog, 0x838A3AF4, SceInt32, sceImeDialogTerm, void)
{
    g_ime_active = false;
    g_ime_done = false;
    return 0;
}

/*
 * Generic behaviour for the dialog families that need services this host does
 * not provide. Init fails, status is NONE and results report NOT_RUNNING.
 */
static int dialog_init_unsupported(void)
{
    return SCE_COMMON_DIALOG_ERROR_NOT_SUPPORTED;
}

/* Save-data dialogs are probed by titles that prefer the system UI; log them
   separately so a title stuck on "Saving..." can be diagnosed. */
static int dialog_savedata_init(void)
{
    if (getenv("VITA2NATIVE_INPUT_DEBUG")) {
        fprintf(stderr, "vita2native: SaveDataDialogInit -> not supported\n");
        fflush(stderr);
    }
    return SCE_COMMON_DIALOG_ERROR_NOT_SUPPORTED;
}

static int dialog_status_none(void)
{
    return SCE_COMMON_DIALOG_STATUS_NONE;
}

static int dialog_result_not_running(void *result)
{
    (void)result;
    return SCE_COMMON_DIALOG_ERROR_NOT_RUNNING;
}

static int dialog_ok(void)
{
    return 0;
}

bool SceCommonDialog_getOverlay(CommonDialogOverlay *overlay)
{
    if (!overlay)
        return false;

    msg_update_done();
    ime_update_done();
    memset(overlay, 0, sizeof(*overlay));
    overlay->progress = -1;

    if (g_msg_dialog_active && !g_msg_dialog_done) {
        dialog_copy(overlay->title, sizeof(overlay->title), g_msg_title);
        dialog_copy(overlay->message, sizeof(overlay->message), g_msg_text);
        dialog_copy(overlay->hint, sizeof(overlay->hint), g_msg_hint);
        overlay->progress = g_msg_progress;
        return true;
    }

    if (g_ime_active && !g_ime_done) {
        dialog_copy(overlay->title, sizeof(overlay->title), "Input");
        dialog_copy(overlay->message, sizeof(overlay->message), g_ime_text);
        dialog_copy(overlay->hint, sizeof(overlay->hint), "X  Enter");
        return true;
    }

    return false;
}

void SceCommonDialog_overlayPresented(void)
{
    if (g_msg_dialog_active && !g_msg_dialog_done)
        g_msg_presents++;
    if (g_ime_active && !g_ime_done)
        g_ime_presents++;

    msg_update_done();
    ime_update_done();
}

#include "SceCommonDialog_exports.inc"

DECLARE_LIBRARY(SceCommonDialog, 0xe537816c);
