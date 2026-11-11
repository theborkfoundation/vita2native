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
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <psp2/common_dialog.h>
#include <psp2/ime_dialog.h>
#include <psp2/message_dialog.h>

#include "host.h"
#include "module.h"
#include "modules/SceAppUtil.h"
#include "modules/SceSysmem.h"
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

static bool g_trophy_dialog_active;
static bool g_trophy_dialog_done;
static int g_trophy_dialog_result;
static uint64_t g_trophy_start_ns;

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

/* The system prepares trophy data while this dialog is up. */
static void trophy_update_done(void)
{
    if (g_trophy_dialog_active && !g_trophy_dialog_done &&
        host_monotonic_ns() - g_trophy_start_ns >= 200000000ull) {
        g_trophy_dialog_done = true;
        g_trophy_dialog_result = SCE_COMMON_DIALOG_RESULT_OK;
    }
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
    trophy_update_done();

    return 0;
}

EXPORT(SceCommonDialog, 0x043A353E, int, sceCommonDialogIsRunning, void)
{
    msg_update_done();
    ime_update_done();
    trophy_update_done();

    return ((g_msg_dialog_active && !g_msg_dialog_done) ||
            (g_ime_active && !g_ime_done) || (g_trophy_dialog_active && !g_trophy_dialog_done))
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

/*
 * Trophy setup dialog. Titles commonly call Init, poll GetStatus until it
 * reports FINISHED, then read GetResult and call Term. Finishing immediately
 * (like Vita3K with options=0) keeps that sequence from spinning forever;
 * there is no system UI to show here.
 */
EXPORT(SceCommonDialog, 0x9E2C02C9, int, sceNpTrophySetupDialogInit, const void *param)
{
    if (!param)
        return SCE_COMMON_DIALOG_ERROR_NULL;
    if ((g_msg_dialog_active && !g_msg_dialog_done) ||
        (g_ime_active && !g_ime_done) || g_trophy_dialog_active)
        return SCE_COMMON_DIALOG_ERROR_BUSY;

    g_trophy_dialog_active = true;
    g_trophy_dialog_done = false;
    g_trophy_dialog_result = SCE_COMMON_DIALOG_RESULT_OK;
    g_trophy_start_ns = host_monotonic_ns();

    if (getenv("VITA2NATIVE_INPUT_DEBUG")) {
        fprintf(stderr, "vita2native: TrophySetupDialog init\n");
        fflush(stderr);
    }

    return 0;
}

EXPORT(SceCommonDialog, 0xC3A59547, int, sceNpTrophySetupDialogGetStatus, void)
{
    trophy_update_done();

    if (!g_trophy_dialog_active)
        return SCE_COMMON_DIALOG_STATUS_NONE;
    return g_trophy_dialog_done ? SCE_COMMON_DIALOG_STATUS_FINISHED
                                : SCE_COMMON_DIALOG_STATUS_RUNNING;
}

EXPORT(SceCommonDialog, 0xE37069D5, int, sceNpTrophySetupDialogGetResult, void *result)
{
    trophy_update_done();

    if (!g_trophy_dialog_active || !g_trophy_dialog_done)
        return SCE_COMMON_DIALOG_ERROR_NOT_FINISHED;
    if (result)
        ((int *)result)[0] = g_trophy_dialog_result;
    return 0;
}

EXPORT(SceCommonDialog, 0xDC346979, int, sceNpTrophySetupDialogAbort, void)
{
    if (!g_trophy_dialog_active)
        return SCE_COMMON_DIALOG_ERROR_NOT_IN_USE;

    g_trophy_dialog_done = true;
    g_trophy_dialog_result = SCE_COMMON_DIALOG_RESULT_ABORTED;
    return 0;
}

EXPORT(SceCommonDialog, 0xA81082DD, int, sceNpTrophySetupDialogTerm, void)
{
    if (!g_trophy_dialog_active)
        return SCE_COMMON_DIALOG_ERROR_NOT_IN_USE;
    if (!g_trophy_dialog_done)
        return SCE_COMMON_DIALOG_ERROR_NOT_FINISHED;

    g_trophy_dialog_active = false;
    g_trophy_dialog_done = false;
    g_trophy_dialog_result = SCE_COMMON_DIALOG_RESULT_OK;
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

/*
 * Save-data dialogs. The game uses these as the UI around its own save/load
 * job: the dialog params carry the slot id and the message, while the actual
 * file I/O goes through SceAppUtil/ sceIo. We accept the dialog and report it
 * finished immediately so the save job can proceed. For the "Failed to load"
 * user message we answer "No" (overwrite), otherwise the game retries a
 * non-existent save forever.
 */
#define SAVEDATA_DIALOG_MODE_FIXED 1
#define SAVEDATA_DIALOG_MODE_LIST 2
#define SAVEDATA_DIALOG_MODE_USER_MSG 3
#define SAVEDATA_DIALOG_MODE_SYSTEM_MSG 4

static bool g_savedata_dialog_active;
static int g_savedata_dialog_mode;
static int g_savedata_dialog_disp;
static uint32_t g_savedata_dialog_slot;
static uint32_t g_savedata_dialog_userdata;
static const char *g_savedata_dialog_message;

static bool savedata_debug(void)
{
    return getenv("VITA2NATIVE_INPUT_DEBUG") != NULL;
}

EXPORT(SceCommonDialog, 0xBF5248FA, SceInt32, sceSaveDataDialogInit, void *param_ptr)
{
    const uint8_t *p = param_ptr;
    uint32_t fixed_param, user_param, sys_param;

    if (!p)
        return SCE_COMMON_DIALOG_ERROR_NULL;

    g_savedata_dialog_mode = *(const int32_t *)(p + 0x50);
    g_savedata_dialog_disp = *(const int32_t *)(p + 0x54);
    fixed_param = *(const uint32_t *)(p + 0x58);
    user_param = *(const uint32_t *)(p + 0x60);
    sys_param = *(const uint32_t *)(p + 0x64);
    g_savedata_dialog_userdata = *(const uint32_t *)(p + 0x78);
    g_savedata_dialog_slot = 0;
    g_savedata_dialog_message = NULL;

    if (g_savedata_dialog_mode == SAVEDATA_DIALOG_MODE_FIXED && fixed_param) {
        g_savedata_dialog_slot = *(const uint32_t *)fixed_param;
    } else if (g_savedata_dialog_mode == SAVEDATA_DIALOG_MODE_USER_MSG && user_param) {
        const uint8_t *msg = (const uint8_t *)user_param;

        g_savedata_dialog_slot = *(const uint32_t *)(msg + 8);
        g_savedata_dialog_message = *(const char *const *)(msg + 4);
    } else if (g_savedata_dialog_mode == SAVEDATA_DIALOG_MODE_SYSTEM_MSG && sys_param) {
        g_savedata_dialog_slot = *(const uint32_t *)((const uint8_t *)sys_param + 8);
    }

    g_savedata_dialog_active = true;
    if (savedata_debug()) {
        fprintf(stderr,
                "vita2native: SaveDataDialog init mode=%d disp=%d slot=%u msg=%s\n",
                g_savedata_dialog_mode, g_savedata_dialog_disp, g_savedata_dialog_slot,
                g_savedata_dialog_message ? g_savedata_dialog_message : "(none)");
        fflush(stderr);
    }
    return 0;
}

EXPORT(SceCommonDialog, 0x6E258046, SceCommonDialogStatus, sceSaveDataDialogGetStatus, void)
{
    return g_savedata_dialog_active ? SCE_COMMON_DIALOG_STATUS_FINISHED
                                    : SCE_COMMON_DIALOG_STATUS_NONE;
}

EXPORT(SceCommonDialog, 0xBA0542CA, SceCommonDialogStatus, sceSaveDataDialogGetSubStatus, void)
{
    return g_savedata_dialog_active ? SCE_COMMON_DIALOG_STATUS_FINISHED
                                    : SCE_COMMON_DIALOG_STATUS_NONE;
}

/*
 * SceSaveDataDialogResult. Titles allocate the slot description themselves and
 * hand its address in `slotInfo`; the dialog fills it in place and leaves the
 * pointer alone. Only the fields below are written: the caller keeps its own
 * locals directly behind the header on the same stack frame, so a blanket
 * memset of the structure would corrupt them.
 */
struct savedata_dialog_result_header {
    int32_t mode;
    int32_t result;
    int32_t button_id;
    int32_t slot_id;
    uint32_t slot_info;
    uint32_t userdata;
};

struct savedata_dialog_slot_info {
    uint32_t is_exist;
    uint32_t slot_param;
    uint8_t reserved[32];
};

/* Slot info fallback for titles that leave `slotInfo` NULL. */
#define SAVEDATA_DIALOG_SCRATCH_SIZE 0x800

static void *savedata_dialog_scratch(void)
{
    static uint8_t *scratch;

    if (!scratch)
        scratch = SceSysmem_alloc_guest_scratch("SaveDataDialogSlot",
                                                SAVEDATA_DIALOG_SCRATCH_SIZE);
    return scratch;
}

EXPORT(SceCommonDialog, 0xB2FF576E, SceInt32, sceSaveDataDialogGetResult, void *result_ptr)
{
    struct savedata_dialog_result_header *result = result_ptr;
    struct savedata_dialog_slot_info *info;
    SceAppUtilSaveDataSlotParam param;
    int32_t button_id = SCE_MSG_DIALOG_BUTTON_ID_YES;
    bool param_ok;

    if (!result)
        return SCE_COMMON_DIALOG_ERROR_NULL;

    if (g_savedata_dialog_mode == SAVEDATA_DIALOG_MODE_USER_MSG &&
        g_savedata_dialog_message && strstr(g_savedata_dialog_message, "Failed to load"))
        button_id = SCE_MSG_DIALOG_BUTTON_ID_NO;

    result->mode = g_savedata_dialog_mode;
    result->result = SCE_COMMON_DIALOG_RESULT_OK;
    result->button_id = button_id;
    result->slot_id = (int32_t)g_savedata_dialog_slot;
    result->userdata = g_savedata_dialog_userdata;

    info = (struct savedata_dialog_slot_info *)(uintptr_t)result->slot_info;
    if (!info) {
        uint8_t *scratch = savedata_dialog_scratch();

        if (scratch) {
            info = (struct savedata_dialog_slot_info *)scratch;
            info->slot_param = (uint32_t)(uintptr_t)(scratch + sizeof(*info));
        }
    }

    param_ok = SceAppUtil_read_slot_param(g_savedata_dialog_slot, &param) == 0;
    if (info) {
        info->is_exist = param_ok ? 1u : 0u;
        if (info->slot_param) {
            if (param_ok)
                memcpy((void *)(uintptr_t)info->slot_param, &param, sizeof(param));
            else
                memset((void *)(uintptr_t)info->slot_param, 0, sizeof(param));
        }
        result->slot_info = (uint32_t)(uintptr_t)info;
    }

    if (savedata_debug()) {
        fprintf(stderr,
                "vita2native: SaveDataDialog get_result mode=%d button=%d slot=%u exist=%d\n",
                g_savedata_dialog_mode, button_id, g_savedata_dialog_slot,
                info ? (int)info->is_exist : -1);
        fflush(stderr);
    }
    return 0;
}

EXPORT(SceCommonDialog, 0x6C49924B, SceInt32, sceSaveDataDialogFinish, void)
{
    return 0;
}

EXPORT(SceCommonDialog, 0x19192C8B, SceInt32, sceSaveDataDialogContinue, void)
{
    return 0;
}

EXPORT(SceCommonDialog, 0x2192A10A, SceInt32, sceSaveDataDialogTerm, void)
{
    g_savedata_dialog_active = false;
    return 0;
}

EXPORT(SceCommonDialog, 0x013E7F74, SceInt32, sceSaveDataDialogAbort, void)
{
    g_savedata_dialog_active = false;
    return 0;
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
