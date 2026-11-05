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
#include <string.h>

#include <psp2/common_dialog.h>
#include <psp2/ime_dialog.h>
#include <psp2/message_dialog.h>

#include "module.h"

static bool g_msg_dialog_active;
static int g_msg_dialog_mode;

static bool g_ime_active;
static SceWChar16 *g_ime_buffer;
static SceUInt32 g_ime_max_length;

EXPORT(SceCommonDialog, 0x90530F2F, int, sceCommonDialogUpdate, const SceCommonDialogUpdateParam *param)
{
    (void)param;

    return 0;
}

EXPORT(SceCommonDialog, 0x043A353E, int, sceCommonDialogIsRunning, void)
{
    return (g_msg_dialog_active || g_ime_active) ? 1 : 0;
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

/* The host has no system UI: message dialogs complete immediately. */
EXPORT(SceCommonDialog, 0x755FF270, int, sceMsgDialogInit, const SceMsgDialogParam *param)
{
    if (!param)
        return -1;

    g_msg_dialog_mode = param->mode;
    g_msg_dialog_active = true;
    return 0;
}

EXPORT(SceCommonDialog, 0x4107019E, SceCommonDialogStatus, sceMsgDialogGetStatus, void)
{
    return g_msg_dialog_active ? SCE_COMMON_DIALOG_STATUS_FINISHED
                               : SCE_COMMON_DIALOG_STATUS_NONE;
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
    return 0;
}

EXPORT(SceCommonDialog, 0x0CC66115, int, sceMsgDialogAbort, void)
{
    g_msg_dialog_active = false;
    return 0;
}

EXPORT(SceCommonDialog, 0xC296D396, int, sceMsgDialogClose, void)
{
    g_msg_dialog_active = false;
    return 0;
}

EXPORT(SceCommonDialog, 0x7BE0E08B, int, sceMsgDialogProgressBarInc,
       SceMsgDialogProgressBarTarget target, SceUInt32 delta)
{
    (void)target;
    (void)delta;
    return 0;
}

EXPORT(SceCommonDialog, 0x0150A451, int, sceMsgDialogProgressBarSetMsg,
       SceMsgDialogProgressBarTarget target, const SceChar8 *message)
{
    (void)target;
    (void)message;
    return 0;
}

EXPORT(SceCommonDialog, 0x9CDA5E0D, int, sceMsgDialogProgressBarSetValue,
       SceMsgDialogProgressBarTarget target, SceUInt32 value)
{
    (void)target;
    (void)value;
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
    g_ime_buffer = param->inputTextBuffer;
    g_ime_max_length = param->maxTextLength;

    if (param->initialText) {
        SceUInt32 i;

        for (i = 0; i + 1 < g_ime_max_length && param->initialText[i]; i++)
            g_ime_buffer[i] = param->initialText[i];
        g_ime_buffer[i] = 0;
    } else {
        g_ime_buffer[0] = 0;
    }

    return 0;
}

EXPORT(SceCommonDialog, 0xCF0431FD, SceCommonDialogStatus, sceImeDialogGetStatus, void)
{
    return g_ime_active ? SCE_COMMON_DIALOG_STATUS_FINISHED : SCE_COMMON_DIALOG_STATUS_NONE;
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
    return 0;
}

EXPORT(SceCommonDialog, 0x838A3AF4, SceInt32, sceImeDialogTerm, void)
{
    g_ime_active = false;
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

#include "SceCommonDialog_exports.inc"

DECLARE_LIBRARY(SceCommonDialog, 0xe537816c);
