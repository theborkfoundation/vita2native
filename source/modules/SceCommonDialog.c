/*
 * SceCommonDialog: system dialogs (message boxes, IME, ...).
 *
 * There is no system UI on this host. The update pump is a successful no-op so
 * that applications which call it every frame keep running; dialog entry
 * points report "not supported" instead of pretending to work.
 */

#include <psp2/common_dialog.h>

#include "module.h"

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

DECLARE_LIBRARY(SceCommonDialog, 0xe537816c);
