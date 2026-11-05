/*
 * Exercises SceCommonDialog: immediate message dialogs, the auto-completing
 * IME, and the unsupported-dialog fallback path.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <psp2/common_dialog.h>
#include <psp2/ime_dialog.h>
#include <psp2/message_dialog.h>
#include <psp2/types.h>

int sceCommonDialogIsRunning(void);
SceInt32 sceImeDialogInit(const SceImeDialogParam *param);
SceCommonDialogStatus sceImeDialogGetStatus(void);
SceInt32 sceImeDialogGetResult(SceImeDialogResult *result);
SceInt32 sceImeDialogTerm(void);
int sceImeDialogAbort(void);

#define CHECK(cond, message)                                                                       \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            printf("dialog_smoke: %s (line %d)\n", message, __LINE__);                             \
            return 1;                                                                              \
        }                                                                                          \
    } while (0)

int main(void)
{
    SceImeDialogResult ime_result;
    SceMsgDialogParam msg_param;
    SceMsgDialogResult msg_result;
    SceImeDialogParam ime_param;
    SceWChar16 initial_text[] = { 'h', 'e', 'l', 'l', 'o', 0 };
    SceWChar16 input_text[16];

    CHECK(sceCommonDialogIsRunning() == 0, "a dialog is running before Init");

    memset(&msg_param, 0, sizeof(msg_param));
    msg_param.mode = SCE_MSG_DIALOG_MODE_USER_MSG;
    CHECK(sceMsgDialogInit(&msg_param) == 0, "MsgDialogInit failed");
    CHECK(sceCommonDialogIsRunning() == 1, "the message dialog is not reported as running");
    CHECK(sceMsgDialogGetStatus() == SCE_COMMON_DIALOG_STATUS_FINISHED,
          "the message dialog did not finish");
    memset(&msg_result, 0, sizeof(msg_result));
    CHECK(sceMsgDialogGetResult(&msg_result) == 0, "MsgDialogGetResult failed");
    CHECK(msg_result.result == SCE_COMMON_DIALOG_RESULT_OK, "the message dialog did not return OK");
    CHECK(sceMsgDialogTerm() == 0, "MsgDialogTerm failed");
    CHECK(sceCommonDialogIsRunning() == 0, "the message dialog is still running");

    memset(&ime_param, 0, sizeof(ime_param));
    memset(input_text, 0, sizeof(input_text));
    ime_param.inputTextBuffer = input_text;
    ime_param.initialText = initial_text;
    ime_param.maxTextLength = 16;
    CHECK(sceImeDialogInit(&ime_param) == 0, "ImeDialogInit failed");
    CHECK(sceCommonDialogIsRunning() == 1, "the IME is not reported as running");
    CHECK(sceImeDialogGetStatus() == SCE_COMMON_DIALOG_STATUS_FINISHED,
          "the IME did not finish");
    CHECK(input_text[0] == 'h' && input_text[4] == 'o', "the initial text was not copied");

    memset(&ime_result, 0, sizeof(ime_result));
    CHECK(sceImeDialogGetResult(&ime_result) == 0, "ImeDialogGetResult failed");
    CHECK(ime_result.result == SCE_COMMON_DIALOG_RESULT_OK, "the IME did not return OK");
    CHECK(ime_result.button == SCE_IME_DIALOG_BUTTON_ENTER, "the IME did not return ENTER");
    CHECK(sceImeDialogTerm() == 0, "ImeDialogTerm failed");

    CHECK(sceImeDialogInit(&ime_param) == 0, "the IME could not be restarted");
    CHECK(sceImeDialogAbort() == 0, "ImeDialogAbort failed");
    CHECK(sceImeDialogGetStatus() == SCE_COMMON_DIALOG_STATUS_NONE, "the IME is still active");

    puts("dialog_smoke: ok");
    return 0;
}
