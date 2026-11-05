/*
 * Headless controller backend.
 *
 * Without a window there is no physical input source, so the pad is normally
 * neutral. Setting VITA2NATIVE_CTRL_SCRIPT installs a deterministic script
 * keyed to the number of presented display frames:
 *
 *   VITA2NATIVE_CTRL_SCRIPT="2:down,4:,6:down,8:,10:cross,12:"
 *
 * Each entry is `<frame>:<button>[+<button>...]`; the state applies from that
 * frame until the next entry. An empty button list releases everything. The
 * frame counter comes from the display backend, so timing is unaffected by how
 * slow a software-rendered frame is on the host.
 */
#include <ctype.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include <psp2/ctrl.h>

#include "host.h"
#include "module.h"
#include "modules/SceDisplay.h"
#include "util.h"

typedef struct {
    unsigned int frame;
    unsigned int buttons;
} CtrlScriptEntry;

static CtrlScriptEntry *g_script;
static unsigned int g_script_count;
static unsigned int g_script_index;
static unsigned int g_buttons;
static bool g_script_loaded;

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
    CtrlScriptEntry *grown =
        realloc(g_script, (g_script_count + 1) * sizeof(*g_script));

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

    free(copy);
}

static void script_update(void)
{
    unsigned int frame = SceDisplay_get_frame_count();

    while (g_script_index < g_script_count && g_script[g_script_index].frame <= frame) {
        g_buttons = g_script[g_script_index].buttons;
        g_script_index++;
    }
}

EXPORT(SceCtrl, 0xA9C3CED6, int, sceCtrlPeekBufferPositive, int port, SceCtrlData *pad_data,
       int count)
{
    (void)port;
    (void)count;

    if (!pad_data)
        return -1;

    if (!g_script_loaded)
        script_load();
    script_update();

    pad_data->timeStamp = host_monotonic_ns() / 1000;
    pad_data->buttons = g_buttons;
    pad_data->lx = 128;
    pad_data->ly = 128;
    pad_data->rx = 128;
    pad_data->ry = 128;

    return 0;
}

DECLARE_LIBRARY(SceCtrl, 0xD197E3C7);

int SceCtrl_init(void)
{
    return 0;
}
