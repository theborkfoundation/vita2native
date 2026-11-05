/*
 * SceRegMgr host-backed registry test.
 */
#include <stdio.h>
#include <string.h>

#include <psp2/registrymgr.h>

int sceRegMgrResetRegistryLv(void);
int sceRegMgrRegisterCallback(void *callback);
int sceRegMgrUnregisterCallback(void *callback);
int sceRegMgrSystemIsBlueScreen(void);
int sceRegMgrSystemParamSetInt(const int id, const int value);
int sceRegMgrSystemParamSetStr(const int id, const char *buffer, const int size);
int sceRegMgrSystemParamGetBin(const int id, void *buffer, const int size);
int sceRegMgrSystemParamSetBin(const int id, const void *buffer, const int size);

static int failures;

#define CHECK(condition, ...)                                                                      \
    do {                                                                                           \
        if (!(condition)) {                                                                        \
            fprintf(stderr, __VA_ARGS__);                                                          \
            fprintf(stderr, "\n");                                                                 \
            failures++;                                                                            \
        }                                                                                          \
    } while (0)

int main(void)
{
    int value = -1;
    char text[16];
    unsigned char binary[8];

    /* Built-in system default. */
    CHECK(sceRegMgrGetKeyInt("/CONFIG/SYSTEM/", "language", &value) == 0 && value == 1,
          "language default was not reported");

    /* Unknown keys fail and zero the output. */
    value = 1234;
    CHECK(sceRegMgrGetKeyInt("/CONFIG/TEST/", "missing", &value) < 0 && value == 0,
          "unknown key did not fail cleanly");

    /* Int round trip, with category spelling differences. */
    CHECK(sceRegMgrSetKeyInt("/CONFIG/TEST/", "counter", 42) == 0, "set int failed");
    value = 0;
    CHECK(sceRegMgrGetKeyInt("CONFIG/TEST", "counter", &value) == 0 && value == 42,
          "int round trip failed (%d)", value);

    /* String round trip and truncation. */
    CHECK(sceRegMgrSetKeyStr("/CONFIG/TEST/", "name", "hello", 6) == 0, "set str failed");
    memset(text, 0xAA, sizeof(text));
    CHECK(sceRegMgrGetKeyStr("/CONFIG/TEST/", "name", text, sizeof(text)) == 0 &&
              strcmp(text, "hello") == 0,
          "str round trip failed (%s)", text);
    memset(text, 0xAA, sizeof(text));
    CHECK(sceRegMgrGetKeyStr("/CONFIG/TEST/", "name", text, 3) == 0 && strcmp(text, "he") == 0,
          "str truncation failed (%s)", text);

    /* Binary round trip, including zero padding on a short read. */
    memset(binary, 0x11, sizeof(binary));
    CHECK(sceRegMgrSetKeyBin("/CONFIG/TEST/", "blob", binary, 4) == 0, "set bin failed");
    memset(binary, 0xEE, sizeof(binary));
    CHECK(sceRegMgrGetKeyBin("/CONFIG/TEST/", "blob", binary, sizeof(binary)) == 0 &&
              binary[0] == 0x11 && binary[3] == 0x11 && binary[4] == 0 && binary[7] == 0,
          "bin round trip failed");

    /* Type mismatch is an error, not a silent reinterpretation. */
    CHECK(sceRegMgrGetKeyStr("/CONFIG/TEST/", "counter", text, sizeof(text)) < 0,
          "int key was returned as a string");

    /* Callbacks are accepted and unregistered. */
    CHECK(sceRegMgrRegisterCallback((void *)1) == 0, "register callback failed");
    CHECK(sceRegMgrUnregisterCallback((void *)1) == 0, "unregister callback failed");

    /* System parameters round-trip by id. */
    CHECK(sceRegMgrSystemParamSetInt(0x1234, 77) == 0, "system param set int failed");
    value = 0;
    CHECK(sceRegMgrSystemParamGetInt(0x1234, &value) == 0 && value == 77,
          "system param int round trip failed");
    CHECK(sceRegMgrSystemParamSetStr(0x1235, "abc", 4) == 0, "system param set str failed");
    memset(text, 0, sizeof(text));
    CHECK(sceRegMgrSystemParamGetStr(0x1235, text, sizeof(text)) == 0 && strcmp(text, "abc") == 0,
          "system param str round trip failed");
    memset(binary, 0xEE, sizeof(binary));
    CHECK(sceRegMgrSystemParamGetBin(0x9999, binary, sizeof(binary)) == 0 && binary[0] == 0,
          "unset system param bin was not zeroed");
    CHECK(sceRegMgrSystemIsBlueScreen() == 0, "blue screen reported");

    /* Reset removes guest values but keeps the built-in defaults. */
    CHECK(sceRegMgrResetRegistryLv() == 0, "reset failed");
    CHECK(sceRegMgrGetKeyInt("/CONFIG/TEST/", "counter", &value) < 0,
          "reset did not clear the counter");
    CHECK(sceRegMgrGetKeyInt("/CONFIG/SYSTEM/", "language", &value) == 0 && value == 1,
          "reset removed the built-in language default");

    if (failures) {
        fprintf(stderr, "regmgr_smoke: %d failure(s)\n", failures);
        return 1;
    }

    printf("regmgr_smoke: ok\n");
    return 0;
}
