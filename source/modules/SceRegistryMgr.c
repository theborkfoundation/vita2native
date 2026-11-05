/*
 * SceRegistryMgr: the firmware configuration registry.
 *
 * There is no Sony registry database on the host. Values written by the guest
 * are kept in memory for the lifetime of the process, and a small set of
 * well-known system keys has built-in defaults. Unknown keys fail with -1 and
 * a zeroed output buffer so applications take their own fallback path, which
 * is what the previous stub did.
 */
#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <psp2/types.h>

#include "module.h"

typedef enum {
    REG_INT,
    REG_STR,
    REG_BIN,
} RegType;

typedef struct {
    char *category;
    char *name;
    RegType type;
    int int_value;
    void *data;
    size_t size;
} RegEntry;

static RegEntry *g_entries;
static size_t g_entry_count;
static unsigned int g_callback_count;

static const char *skip_slashes(const char *text)
{
    while (*text == '/')
        text++;
    return text;
}

/*
 * Categories are written both as "system" and "/CONFIG/SYSTEM/" by real
 * applications. Compare case-insensitively, treating '/' as a separator and
 * ignoring leading/trailing/duplicate separators.
 */
static bool reg_string_equal(const char *a, const char *b)
{
    a = skip_slashes(a);
    b = skip_slashes(b);

    for (;;) {
        while (*a == '/')
            a++;
        while (*b == '/')
            b++;

        if (!*a || !*b)
            return !*a && !*b;
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b))
            return false;
        a++;
        b++;
    }
}

static RegEntry *reg_find(const char *category, const char *name)
{
    if (!category || !name)
        return NULL;

    for (size_t i = 0; i < g_entry_count; i++) {
        if (reg_string_equal(g_entries[i].category, category) &&
            reg_string_equal(g_entries[i].name, name))
            return &g_entries[i];
    }

    return NULL;
}

static int reg_store(const char *category, const char *name, RegType type, int int_value,
                     const void *data, size_t size)
{
    RegEntry *entry;
    void *copy = NULL;

    if (!category || !name)
        return -1;

    if (type != REG_INT) {
        copy = malloc(size ? size : 1);
        if (!copy)
            return -1;
        if (data && size)
            memcpy(copy, data, size);
        else
            memset(copy, 0, size ? size : 1);
    }

    entry = reg_find(category, name);
    if (entry) {
        free(entry->data);
    } else {
        RegEntry *grown = realloc(g_entries, (g_entry_count + 1) * sizeof(*g_entries));

        if (!grown) {
            free(copy);
            return -1;
        }
        g_entries = grown;
        entry = &g_entries[g_entry_count++];
        memset(entry, 0, sizeof(*entry));
        entry->category = strdup(category);
        entry->name = strdup(name);
        if (!entry->category || !entry->name) {
            free(copy);
            return -1;
        }
    }

    entry->type = type;
    entry->int_value = int_value;
    entry->data = copy;
    entry->size = type == REG_INT ? 0 : size;

    return 0;
}

static bool reg_default_int(const char *category, const char *name, int *value)
{
    if (reg_string_equal(category, "/CONFIG/SYSTEM/") && reg_string_equal(name, "language")) {
        *value = 1; /* English */
        return true;
    }
    if (reg_string_equal(category, "/CONFIG/SYSTEM/") &&
        reg_string_equal(name, "button_assign")) {
        *value = 0; /* cross = confirm */
        return true;
    }

    return false;
}

EXPORT(SceRegMgr, 0x0B98D646, int, sceRegMgrGetKeyBin, const char *category, const char *name,
       void *buffer, int size)
{
    RegEntry *entry = reg_find(category, name);

    if (!buffer || size < 0)
        return -1;
    if (size > 0)
        memset(buffer, 0, (size_t)size);
    if (!entry || entry->type == REG_INT)
        return -1;

    if (size > 0)
        memcpy(buffer, entry->data, entry->size < (size_t)size ? entry->size : (size_t)size);

    return 0;
}

EXPORT(SceRegMgr, 0x16DDF3DC, int, sceRegMgrGetKeyInt, const char *category, const char *name,
       int *buffer)
{
    RegEntry *entry = reg_find(category, name);

    if (!buffer)
        return -1;
    *buffer = 0;

    if (entry) {
        if (entry->type != REG_INT)
            return -1;
        *buffer = entry->int_value;
        return 0;
    }

    return reg_default_int(category, name, buffer) ? 0 : -1;
}

EXPORT(SceRegMgr, 0xE188382F, int, sceRegMgrGetKeyStr, const char *category, const char *name,
       char *buffer, const int size)
{
    RegEntry *entry = reg_find(category, name);

    if (!buffer || size <= 0)
        return -1;
    buffer[0] = '\0';
    if (!entry || entry->type == REG_INT)
        return -1;

    {
        size_t length = entry->size < (size_t)size - 1 ? entry->size : (size_t)size - 1;

        memcpy(buffer, entry->data, length);
        buffer[length] = '\0';
    }

    return 0;
}

EXPORT(SceRegMgr, 0x566A1793, int, sceRegMgrSetKeyBin, const char *category, const char *name,
       void *buffer, int size)
{
    if (!buffer || size < 0)
        return -1;

    return reg_store(category, name, REG_BIN, 0, buffer, (size_t)size);
}

EXPORT(SceRegMgr, 0xD72EA399, int, sceRegMgrSetKeyInt, const char *category, const char *name,
       int buffer)
{
    return reg_store(category, name, REG_INT, buffer, NULL, 0);
}

EXPORT(SceRegMgr, 0x41D320C5, int, sceRegMgrSetKeyStr, const char *category, const char *name,
       char *buffer, const int size)
{
    size_t length;

    if (!buffer || size <= 0)
        return -1;

    length = strlen(buffer) + 1;
    if (length > (size_t)size)
        length = (size_t)size;

    return reg_store(category, name, REG_STR, 0, buffer, length);
}

/* Bulk key access needs Sony's undocumented on-disk record layout. */
EXPORT(SceRegMgr, 0x3A9DE7C5, int, sceRegMgrGetInitVals, const char *category, char *buffer,
       const int elements_number)
{
    (void)category;
    (void)buffer;
    (void)elements_number;
    return -1;
}

EXPORT(SceRegMgr, 0x5B161504, int, sceRegMgrGetKeys, const char *category, char *buffer,
       const int elements_number)
{
    (void)category;
    (void)buffer;
    (void)elements_number;
    return -1;
}

EXPORT(SceRegMgr, 0x58421DD1, int, sceRegMgrGetKeysInfo, const char *category, char *buffer,
       const int elements_number)
{
    (void)category;
    (void)buffer;
    (void)elements_number;
    return -1;
}

EXPORT(SceRegMgr, 0x34E550DA, int, sceRegMgrSetKeys, const char *category, char *buffer,
       const int elements_number)
{
    (void)category;
    (void)buffer;
    (void)elements_number;
    return -1;
}

EXPORT(SceRegMgr, 0x74A0FB10, int, sceRegMgrGetRegVersion, int version, char *buffer)
{
    (void)version;
    (void)buffer;
    return -1;
}

EXPORT(SceRegMgr, 0x282027B7, int, sceRegMgrIsBlueScreen, void)
{
    return 0;
}

EXPORT(SceRegMgr, 0x2B5F3E20, int, sceRegMgrResetRegistryLv, void)
{
    for (size_t i = 0; i < g_entry_count; i++) {
        free(g_entries[i].category);
        free(g_entries[i].name);
        free(g_entries[i].data);
    }
    free(g_entries);
    g_entries = NULL;
    g_entry_count = 0;

    return 0;
}

EXPORT(SceRegMgr, 0xB68B5422, int, sceRegMgrDbBackup, void)
{
    return 0;
}

EXPORT(SceRegMgr, 0xA87D2562, int, sceRegMgrDbRestore, void)
{
    return 0;
}

EXPORT(SceRegMgr, 0xA86F0A71, int, sceRegMgrAddRegistryCallback, void *callback)
{
    (void)callback;
    g_callback_count++;
    return 0;
}

EXPORT(SceRegMgr, 0x82A4464D, int, sceRegMgrRegisterCallback, void *callback)
{
    (void)callback;
    g_callback_count++;
    return 0;
}

EXPORT(SceRegMgr, 0xDDB02D7F, int, sceRegMgrRegisterDrvErrCallback, void *callback)
{
    (void)callback;
    return 0;
}

EXPORT(SceRegMgr, 0xCE84FE2F, int, sceRegMgrStartCallback, void *callback)
{
    (void)callback;
    return 0;
}

EXPORT(SceRegMgr, 0x6A8E2FB4, int, sceRegMgrStopCallback, void *callback)
{
    (void)callback;
    return 0;
}

EXPORT(SceRegMgr, 0xD7BD3607, int, sceRegMgrUnregisterCallback, void *callback)
{
    (void)callback;
    if (g_callback_count > 0)
        g_callback_count--;
    return 0;
}

EXPORT(SceRegMgr, 0xD9E6B7BD, int, sceRegMgrUnregisterDrvErrCallback, void *callback)
{
    (void)callback;
    return 0;
}

/* ------------------------------------------------------- system parameters */

static void system_param_name(int id, char *buffer, size_t size)
{
    snprintf(buffer, size, "param_%08x", (unsigned int)id);
}

EXPORT(SceRegMgrForGame, 0x347C1BDB, int, sceRegMgrSystemParamGetInt, const int id, int *buffer)
{
    char name[32];
    RegEntry *entry;

    if (!buffer)
        return -1;
    system_param_name(id, name, sizeof(name));
    entry = reg_find("system", name);
    *buffer = entry && entry->type == REG_INT ? entry->int_value : 0;

    return 0;
}

EXPORT(SceRegMgrForGame, 0x877ADB3F, int, sceRegMgrSystemParamGetStr, const int id, char *buffer,
       const int size)
{
    char name[32];
    RegEntry *entry;

    if (!buffer || size <= 0)
        return -1;
    buffer[0] = '\0';
    system_param_name(id, name, sizeof(name));
    entry = reg_find("system", name);
    if (entry && entry->type == REG_STR) {
        size_t length = entry->size < (size_t)size - 1 ? entry->size : (size_t)size - 1;

        memcpy(buffer, entry->data, length);
        buffer[length] = '\0';
    }

    return 0;
}

EXPORT(SceRegMgrForGame, 0x7FFE2CDF, int, sceRegMgrSystemParamGetBin, const int id, void *buffer,
       const int size)
{
    char name[32];
    RegEntry *entry;

    if (!buffer || size < 0)
        return -1;
    if (size > 0)
        memset(buffer, 0, (size_t)size);
    system_param_name(id, name, sizeof(name));
    entry = reg_find("system", name);
    if (entry && entry->type == REG_BIN && size > 0)
        memcpy(buffer, entry->data, entry->size < (size_t)size ? entry->size : (size_t)size);

    return 0;
}

EXPORT(SceRegMgrForGame, 0xC8F73311, int, sceRegMgrSystemParamSetInt, const int id,
       const int value)
{
    char name[32];

    system_param_name(id, name, sizeof(name));
    return reg_store("system", name, REG_INT, value, NULL, 0);
}

EXPORT(SceRegMgrForGame, 0xCB3246E3, int, sceRegMgrSystemParamSetStr, const int id,
       const char *buffer, const int size)
{
    char name[32];
    size_t length;

    if (!buffer || size <= 0)
        return -1;
    length = strlen(buffer) + 1;
    if (length > (size_t)size)
        length = (size_t)size;
    system_param_name(id, name, sizeof(name));

    return reg_store("system", name, REG_STR, 0, buffer, length);
}

EXPORT(SceRegMgrForGame, 0xD5A73557, int, sceRegMgrSystemParamSetBin, const int id,
       const void *buffer, const int size)
{
    char name[32];

    if (!buffer || size < 0)
        return -1;
    system_param_name(id, name, sizeof(name));

    return reg_store("system", name, REG_BIN, 0, buffer, (size_t)size);
}

EXPORT(SceRegMgrForGame, 0x169A0D1D, int, sceRegMgrSystemIsBlueScreen, void)
{
    return 0;
}

DECLARE_LIBRARY(SceRegMgr, 0xC436F916);
DECLARE_LIBRARY(SceRegMgrForGame, 0x0B351269);
