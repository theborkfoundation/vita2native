/*
 * Exercises SceCommonDialog: immediate message dialogs, the auto-completing
 * IME, and the unsupported-dialog fallback path.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <psp2/common_dialog.h>
#include <psp2/ime_dialog.h>
#include <psp2/message_dialog.h>
#include <psp2/types.h>

int sceCommonDialogIsRunning(void);
int sceCommonDialogUpdate(const SceCommonDialogUpdateParam *param);
SceInt32 sceImeDialogInit(const SceImeDialogParam *param);
SceCommonDialogStatus sceImeDialogGetStatus(void);
SceInt32 sceImeDialogGetResult(SceImeDialogResult *result);
SceInt32 sceImeDialogTerm(void);
int sceImeDialogAbort(void);

SceInt32 sceSaveDataDialogInit(const void *param);
SceInt32 sceSaveDataDialogTerm(void);
SceCommonDialogStatus sceSaveDataDialogGetStatus(void);
SceCommonDialogStatus sceSaveDataDialogGetSubStatus(void);
SceInt32 sceSaveDataDialogGetResult(void *result);

/* SceSaveDataDialogResult as titles see it: a short header whose slotInfo
   field points at storage the caller owns. */
struct savedata_slot_info {
    uint32_t is_exist;
    uint32_t slot_param;
    uint8_t reserved[32];
};

struct savedata_result {
    int32_t mode;
    int32_t result;
    int32_t button_id;
    int32_t slot_id;
    uint32_t slot_info;
    uint32_t userdata;
};

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
    CHECK(sceMsgDialogGetStatus() == SCE_COMMON_DIALOG_STATUS_RUNNING,
          "the message dialog did not start running");
    for (int i = 0; i < 4; i++)
        sceCommonDialogUpdate(NULL);
    usleep(1100000);
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
    CHECK(sceImeDialogGetStatus() == SCE_COMMON_DIALOG_STATUS_RUNNING,
          "the IME did not start running");
    for (int i = 0; i < 4; i++)
        sceCommonDialogUpdate(NULL);
    usleep(1100000);
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

    /*
     * The save-data dialog fills the caller's slot description in place. Titles
     * keep their own locals directly behind the result header, so GetResult must
     * stop at the userdata field: a memset over the whole structure used to
     * zero the caller's slotInfo pointer (Shakedown: Hawaii crash).
     */
    {
        uint8_t dialog_param[0x80];
        uint8_t slot_param[0x34C];
        uint32_t slot_id = 0;
        struct savedata_slot_info info;
        struct savedata_result result;

        memset(dialog_param, 0, sizeof(dialog_param));
        memset(slot_param, 0xcd, sizeof(slot_param));
        *(int32_t *)(dialog_param + 0x50) = 1; /* SCE_SAVEDATA_DIALOG_MODE_FIXED */
        *(int32_t *)(dialog_param + 0x54) = 2;
        *(uint32_t *)(dialog_param + 0x58) = (uint32_t)(uintptr_t)&slot_id;

        memset(&info, 0, sizeof(info));
        info.slot_param = (uint32_t)(uintptr_t)slot_param;

        memset(&result, 0, sizeof(result));
        result.slot_info = (uint32_t)(uintptr_t)&info;

        CHECK(sceSaveDataDialogInit(dialog_param) == 0, "SaveDataDialogInit failed");
        CHECK(sceSaveDataDialogGetStatus() == SCE_COMMON_DIALOG_STATUS_FINISHED,
              "the save-data dialog did not finish");
        CHECK(sceSaveDataDialogGetSubStatus() == SCE_COMMON_DIALOG_STATUS_FINISHED,
              "the save-data dialog has no success substatus");
        CHECK(sceSaveDataDialogGetResult(&result) == 0, "SaveDataDialogGetResult failed");
        CHECK(result.mode == 1, "the save-data result mode is wrong");
        CHECK(result.result == SCE_COMMON_DIALOG_RESULT_OK, "the save-data result is not OK");
        CHECK(result.slot_id == 0, "the save-data result slot is wrong");
        CHECK(result.slot_info == (uint32_t)(uintptr_t)&info,
              "GetResult replaced the caller's slotInfo pointer");
        CHECK(info.is_exist == 0, "the empty slot was reported as existing");
        CHECK(sceSaveDataDialogTerm() == 0, "SaveDataDialogTerm failed");
    }

    puts("dialog_smoke: ok");
    return 0;
}
