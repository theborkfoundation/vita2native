/*
 * Checks for the settings table: lookups, the cached accessors, the typo
 * hint, and that the table and the call sites agree.
 *
 * The last part scans the sources, so it is the guard that keeps
 * include/settings.h honest: every getenv("VITA2NATIVE_...") needs a table
 * entry, and every table entry needs a quoted use somewhere.
 *
 *     settings_smoke <source-root>
 */

#include <dirent.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "settings.h"
#include "test.h"

#define MAX_SCAN (4u << 20)

static size_t g_count;
static bool g_seen[256];
static unsigned int g_getenv_names;

static char *read_file(const char *path, size_t *size)
{
    FILE *file = fopen(path, "rb");
    char *buffer;
    size_t length;

    if (!file)
        return NULL;
    buffer = malloc(MAX_SCAN);
    if (!buffer) {
        fclose(file);
        return NULL;
    }
    length = fread(buffer, 1, MAX_SCAN, file);
    fclose(file);
    *size = length;
    return buffer;
}

/* Records every "VITA2NATIVE_*" literal in the file and checks the ones that
   are getenv() arguments against the table. */
static void scan_text(const char *path, const char *text, size_t size)
{
    const char *cursor = text;
    const char *end = text + size;

    while (cursor < end && (cursor = memchr(cursor, '"', (size_t)(end - cursor))) != NULL) {
        const char *name = cursor + 1;
        const char *close;
        char literal[96];
        size_t length;
        bool is_getenv = cursor - text >= 7 && memcmp(cursor - 7, "getenv(", 7) == 0;
        const Setting *setting;

        cursor++;
        if ((size_t)(end - name) < 12 || memcmp(name, "VITA2NATIVE_", 12) != 0)
            continue;
        close = memchr(name, '"', (size_t)(end - name));
        if (!close)
            break;
        length = (size_t)(close - name);
        if (length >= sizeof(literal))
            continue;
        memcpy(literal, name, length);
        literal[length] = '\0';

        setting = vita2native_settings_find(literal);
        if (setting) {
            size_t index = (size_t)(setting - vita2native_settings_table(NULL));

            if (index < sizeof(g_seen) / sizeof(g_seen[0]))
                g_seen[index] = true;
        }
        if (is_getenv) {
            g_getenv_names++;
            CHECK(setting != NULL, "%s: getenv(\"%s\") has no entry in settings.h", path,
                  literal);
        }
        cursor = close + 1;
    }
}

static void scan_dir(const char *dir)
{
    DIR *handle = opendir(dir);
    struct dirent *entry;

    if (!handle)
        return;

    while ((entry = readdir(handle)) != NULL) {
        char path[1024];
        struct stat st;

        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
            continue;
        if (snprintf(path, sizeof(path), "%s/%s", dir, entry->d_name) >= (int)sizeof(path))
            continue;
        if (stat(path, &st) != 0)
            continue;

        if (S_ISDIR(st.st_mode)) {
            scan_dir(path);
        } else if (S_ISREG(st.st_mode)) {
            const char *dot = strrchr(entry->d_name, '.');
            size_t size = 0;
            char *text;

            if (!dot ||
                (strcmp(dot, ".c") != 0 && strcmp(dot, ".cpp") != 0 && strcmp(dot, ".h") != 0))
                continue;
            text = read_file(path, &size);
            if (!text)
                continue;
            scan_text(path, text, size);
            free(text);
        }
    }
    closedir(handle);
}

int main(int argc, char **argv)
{
    size_t count = 0;
    const Setting *table;

    table = vita2native_settings_table(&count);
    REQUIRE(table != NULL);
    CHECK(count > 100, "the table only has %zu entries", count);

    /* Every table entry must be unique. */
    for (size_t i = 0; i < count; i++) {
        for (size_t j = i + 1; j < count; j++)
            CHECK(strcmp(table[i].name, table[j].name) != 0, "duplicate entry %s",
                  table[i].name);
        CHECK(strncmp(table[i].name, "VITA2NATIVE_", 12) == 0, "%s needs the prefix",
              table[i].name);
        CHECK(table[i].description != NULL && table[i].description[0] != '\0',
              "%s has no description", table[i].name);
    }

    /* Lookup and the typo hint. */
    REQUIRE(vita2native_settings_find("VITA2NATIVE_NO_MSAA") != NULL);
    CHECK(vita2native_settings_find("VITA2NATIVE_NO_MSA") == NULL);
    CHECK(vita2native_settings_find("VITA2NATIVE_NOT_A_SETTING") == NULL);
    {
        const char *hint = vita2native_settings_suggest("VITA2NATIVE_NO_MSA");

        CHECK(hint != NULL && strcmp(hint, "VITA2NATIVE_NO_MSAA") == 0,
              "no hint for a one-letter typo");
        CHECK(vita2native_settings_suggest("VITA2NATIVE_Z") == NULL);
    }

    /* Accessors, including the first-use cache. */
    CHECK(!vita2native_env_flag("VITA2NATIVE_SETTINGS_SMOKE_UNSET"));
    CHECK(vita2native_env_int("VITA2NATIVE_SETTINGS_SMOKE_UNSET", 7) == 7);
    CHECK(vita2native_env_double("VITA2NATIVE_SETTINGS_SMOKE_UNSET", 0.5) == 0.5);

    REQUIRE(setenv("VITA2NATIVE_SETTINGS_SMOKE_FLAG", "0", 1) == 0);
    CHECK(vita2native_env_flag("VITA2NATIVE_SETTINGS_SMOKE_FLAG"));
    REQUIRE(setenv("VITA2NATIVE_SETTINGS_SMOKE_INT", "0x10", 1) == 0);
    CHECK(vita2native_env_int("VITA2NATIVE_SETTINGS_SMOKE_INT", 0) == 16);
    REQUIRE(setenv("VITA2NATIVE_SETTINGS_SMOKE_INT", "nonsense", 1) == 0);
    /* Already cached: the value seen the first time stands. */
    CHECK(vita2native_env_int("VITA2NATIVE_SETTINGS_SMOKE_INT", 0) == 16);
    REQUIRE(setenv("VITA2NATIVE_SETTINGS_SMOKE_FLOAT", "1.5", 1) == 0);
    CHECK(vita2native_env_double("VITA2NATIVE_SETTINGS_SMOKE_FLOAT", 0.0) == 1.5);
    CHECK(strcmp(vita2native_env_str("VITA2NATIVE_SETTINGS_SMOKE_FLOAT"), "1.5") == 0);
    CHECK(vita2native_env_str("VITA2NATIVE_SETTINGS_SMOKE_UNSET") == NULL);

    /* An empty value counts as unset for a flag. */
    REQUIRE(setenv("VITA2NATIVE_SETTINGS_SMOKE_EMPTY", "", 1) == 0);
    CHECK(!vita2native_env_flag("VITA2NATIVE_SETTINGS_SMOKE_EMPTY"));

    unsetenv("VITA2NATIVE_SETTINGS_SMOKE_FLAG");
    unsetenv("VITA2NATIVE_SETTINGS_SMOKE_INT");
    unsetenv("VITA2NATIVE_SETTINGS_SMOKE_FLOAT");
    unsetenv("VITA2NATIVE_SETTINGS_SMOKE_EMPTY");

    /* Table versus call sites, in both directions. */
    if (argc > 1) {
        char path[1024];

        if (snprintf(path, sizeof(path), "%s/source", argv[1]) < (int)sizeof(path))
            scan_dir(path);
        if (snprintf(path, sizeof(path), "%s/include", argv[1]) < (int)sizeof(path))
            scan_dir(path);

        REQUIRE(g_getenv_names > 100);
        for (size_t i = 0; i < count; i++)
            CHECK(g_seen[i], "%s is in the table but nothing reads it", table[i].name);
    } else {
        fprintf(stderr, "settings_smoke: no source root given, skipping the table check\n");
    }

    return test_status("settings_smoke");
}
