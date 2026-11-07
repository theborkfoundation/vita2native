/*
 * Deterministic controller script for automated runs.
 *
 * Setting VITA2NATIVE_CTRL_SCRIPT installs a button timetable keyed to the
 * display backend's frame counter. That counter is the host vblank clock on
 * the SDL backend (about 60 steps per second, independent of how slowly the
 * guest presents), so schedule presses in those units:
 *
 *   VITA2NATIVE_CTRL_SCRIPT="120:down,150:,600:cross,660:"
 *
 * Each entry is `<frame>:<button>[+<button>...]`; the state applies from that
 * frame until the next entry. An empty button list releases everything. Hold a
 * button long enough for the guest to sample it at least once; a title running
 * at 4 fps polls the pad every ~15 vblanks. Script buttons are ORed with the
 * physical pad.
 *
 * Both pad backends honour the script; the SDL build has no pad attached when
 * a title is driven over SSH, so this is the only way to inject input there.
 */

#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <psp2/ctrl.h>

#include "modules/SceCtrl_script.h"
#include "modules/SceDisplay.h"

typedef struct {
    unsigned int frame;
    unsigned int buttons;
} CtrlScriptEntry;

static CtrlScriptEntry *g_script;
static unsigned int g_script_count;
static unsigned int g_script_index;
static unsigned int g_script_buttons;
static bool g_script_loaded;
static bool g_script_enabled;

static const struct {
    const char *name;
    unsigned int mask;
} g_button_names[] = {
    { "select", SCE_CTRL_SELECT },
    { "l3", SCE_CTRL_L3 },
    { "r3", SCE_CTRL_R3 },
    { "start", SCE_CTRL_START },
    { "up", SCE_CTRL_UP },
    { "right", SCE_CTRL_RIGHT },
    { "down", SCE_CTRL_DOWN },
    { "left", SCE_CTRL_LEFT },
    { "ltrigger", SCE_CTRL_LTRIGGER },
    { "l2", SCE_CTRL_L2 },
    { "rtrigger", SCE_CTRL_RTRIGGER },
    { "r2", SCE_CTRL_R2 },
    { "l1", SCE_CTRL_L1 },
    { "r1", SCE_CTRL_R1 },
    { "triangle", SCE_CTRL_TRIANGLE },
    { "circle", SCE_CTRL_CIRCLE },
    { "cross", SCE_CTRL_CROSS },
    { "square", SCE_CTRL_SQUARE },
};

static unsigned int button_mask(const char *name)
{
    for (unsigned int i = 0; i < sizeof(g_button_names) / sizeof(g_button_names[0]); i++) {
        if (strcmp(name, g_button_names[i].name) == 0)
            return g_button_names[i].mask;
    }

    return 0;
}

static void script_append(unsigned int frame, unsigned int buttons)
{
    CtrlScriptEntry *grown = realloc(g_script, (g_script_count + 1) * sizeof(*g_script));

    if (!grown)
        return;
    g_script = grown;
    g_script[g_script_count].frame = frame;
    g_script[g_script_count].buttons = buttons;
    g_script_count++;
}

static void script_load(void)
{
    const char *env = getenv("VITA2NATIVE_CTRL_SCRIPT");
    char *copy;
    char *token;

    g_script_loaded = true;
    if (!env || !*env)
        return;

    copy = malloc(strlen(env) + 1);
    if (!copy)
        return;
    strcpy(copy, env);

    for (token = strtok(copy, ","); token; token = strtok(NULL, ",")) {
        char *colon = strchr(token, ':');
        char *buttons;
        unsigned int frame;
        unsigned int mask = 0;

        if (!colon)
            continue;
        *colon = '\0';
        frame = (unsigned int)strtoul(token, NULL, 0);
        buttons = colon + 1;

        while (*buttons && isspace((unsigned char)*buttons))
            buttons++;
        if (*buttons) {
            char *name = buttons;

            while (name && *name) {
                char *end = name;

                while (*end && *end != '+')
                    end++;
                if (*end == '+')
                    *end++ = '\0';
                else
                    end = NULL;

                {
                    char *tail = name + strlen(name);

                    while (tail > name && isspace((unsigned char)tail[-1]))
                        *--tail = '\0';
                }
                mask |= button_mask(name);
                name = end;
            }
        }

        script_append(frame, mask);
    }

    g_script_enabled = true;
    free(copy);
}

uint32_t SceCtrl_script_buttons(void)
{
    unsigned int frame;

    if (!g_script_loaded && !g_script_enabled)
        script_load();
    if (!g_script_enabled)
        return 0;

    frame = SceDisplay_get_frame_count();
    while (g_script_index < g_script_count && g_script[g_script_index].frame <= frame) {
        g_script_buttons = g_script[g_script_index].buttons;
        g_script_index++;

        if (getenv("VITA2NATIVE_CTRL_SCRIPT_DEBUG"))
            fprintf(stderr, "vita2native: SceCtrl script frame %u -> buttons 0x%x\n", frame,
                    g_script_buttons);
    }

    return g_script_buttons;
}
