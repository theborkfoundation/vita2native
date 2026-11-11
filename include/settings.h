#ifndef VITA2NATIVE_SETTINGS_H
#define VITA2NATIVE_SETTINGS_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Every VITA2NATIVE_* environment knob, in one table.
 *
 * The table exists so a typo is visible: at startup, each VITA2NATIVE_* in the
 * environment is matched against it and an unknown name is reported with the
 * closest known one. It also feeds `VITA2NATIVE_ENV_DUMP=1`, which prints every
 * knob with its current value and description.
 *
 * Values are read once and cached for the life of the process: the environment
 * does not change while a title runs, and several call sites (per-draw state,
 * per-texture-upload tracing) used to call getenv() on hot paths.
 *
 * The accessors accept any name, listed or not, so a knob can be used before it
 * is added to the table.
 */

typedef enum {
    SETTING_FLAG,   /* presence, like getenv(...) != NULL */
    SETTING_INT,    /* strtol(value, NULL, 0) */
    SETTING_FLOAT,  /* strtod */
    SETTING_STRING, /* same as getenv */
} SettingKind;

typedef struct {
    const char *name;
    SettingKind kind;
    const char *description;
} Setting;

const Setting *vita2native_settings_table(size_t *count);

/* Exact match, or NULL.  Used by the startup check and the smoke test. */
const Setting *vita2native_settings_find(const char *name);

/* Closest known name when the edit distance is small, otherwise NULL. */
const char *vita2native_settings_suggest(const char *name);

/* Cached accessors. `name` must be a string literal (or otherwise outlive the
   process): the cache stores the pointer, not a copy. */
bool vita2native_env_flag(const char *name);
const char *vita2native_env_str(const char *name);
long vita2native_env_int(const char *name, long fallback);
double vita2native_env_double(const char *name, double fallback);

/* Warn about VITA2NATIVE_* names that are not in the table. */
void vita2native_settings_check(void);

/* Print the table with the current values (VITA2NATIVE_ENV_DUMP=1). */
void vita2native_settings_dump(void);

#ifdef __cplusplus
}
#endif

#endif
