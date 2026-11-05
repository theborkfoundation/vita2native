/*
 * SceCommonDialog: system dialogs (message boxes, IME, ...).
 *
 * There is no system UI on this host. The update pump is a successful no-op so
 * that applications which call it every frame keep running; dialog entry
 * points report "not supported" instead of pretending to work.
 */

#include <psp2/common_dialog.h>
#include <psp2/message_dialog.h>
#include <string.h>

#include "module.h"

static int g_msg_dialog_active;
static int g_msg_dialog_mode;

EXPORT(SceCommonDialog, 0x90530F2F, int, sceCommonDialogUpdate, const SceCommonDialogUpdateParam *param)
{
    (void)param;

    return 0;
}

EXPORT(SceCommonDialog, 0x043A353E, int, sceCommonDialogIsRunning, void)
{
    return 0; /* SCE_FALSE */
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
    g_msg_dialog_active = 1;
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
    g_msg_dialog_active = 0;
    return 0;
}

EXPORT(SceCommonDialog, 0x0CC66115, int, sceMsgDialogAbort, void)
{
    g_msg_dialog_active = 0;
    return 0;
}

EXPORT(SceCommonDialog, 0xC296D396, int, sceMsgDialogClose, void)
{
    g_msg_dialog_active = 0;
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

DECLARE_LIBRARY(SceCommonDialog, 0xe537816c);
