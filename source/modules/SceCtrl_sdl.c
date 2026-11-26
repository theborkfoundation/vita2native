/*
 * Controller backend: SDL only.
 *
 * The video layer (source/host/host_video_sdl.c) samples keyboards and game
 * controllers on its own thread, maps pads positionally where it has no
 * database entry, and latches new presses until the guest peeks. It also owns
 * hot-plug: SDL rescans controllers once a second, so a pad plugged in after
 * the title started is picked up.
 *
 * evdev is deliberately not used here. Its event nodes are opened once at
 * startup, so a pad plugged in later never appears; it does not carry the
 * trigger axes an Xbox pad uses; and its button policy depends on the device
 * name. SDL covers all of that, and nothing about presentation needs evdev.
 *
 * Setting VITA2NATIVE_INPUT_PAD_SCRIPT installs a deterministic script on top,
 * keyed to the number of presented display frames:
 *
 *   VITA2NATIVE_INPUT_PAD_SCRIPT="2:down,4:,6:down,8:,10:cross,12:"
 *
 * Each entry is `<frame>:<button>[+<button>...]`; the state applies from that
 * frame until the next entry. An empty button list releases everything. The
 * frame counter comes from the display backend, so timing is unaffected by how
 * slow a software-rendered frame is on the host. Scripted buttons are ORed with
 * the pad, which keeps automated runs working on machines that also have a
 * controller attached.
 */
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include <psp2/ctrl.h>

#include "host.h"
#include "module.h"
#include "modules/SceCtrl_script.h"
#include "settings.h"
#include "util.h"

static bool g_debug;
static unsigned long long g_peek_calls;
static unsigned int g_peek_logged_buttons;

/* The guest provides room for `count` samples. The Vita fills every slot and
   returns how many it wrote, which is the whole count for a peek. Titles such
   as Angry Birds Trilogy pass 64 and treat a zero return as "no new input",
   so duplicate the current state across the requested samples as hardware
   does (older timestamps first). */
#define CTRL_MAX_BUFFERED 64

EXPORT(SceCtrl, 0xA9C3CED6, int, sceCtrlPeekBufferPositive, int port, SceCtrlData *pad_data,
       int count)
{
    HostPadState sdl_pad;
    unsigned int script_buttons;
    SceCtrlData sample;
    bool have_sdl_pad;

    if (port < 0 || port > 1)
        return SCE_CTRL_ERROR_NO_DEVICE;

    if (!pad_data || count <= 0)
        return -1;
    if (count > CTRL_MAX_BUFFERED)
        count = CTRL_MAX_BUFFERED;

    script_buttons = SceCtrl_script_buttons();

    /* Sampled by the SDL video thread, which also clears the press latch: take
       exactly one snapshot per peek. */
    have_sdl_pad = host_video_pad_state(&sdl_pad);

    memset(&sample, 0, sizeof(sample));
    sample.lx = 128;
    sample.ly = 128;
    sample.rx = 128;
    sample.ry = 128;

    if (have_sdl_pad) {
        sample.buttons = sdl_pad.buttons | sdl_pad.pressed;
        /* Axes only while a controller is attached: a keyboard-only session
           must keep the sticks centred. */
        if (sdl_pad.active) {
            sample.lx = sdl_pad.lx;
            sample.ly = sdl_pad.ly;
            sample.rx = sdl_pad.rx;
            sample.ry = sdl_pad.ry;
        }
    }

    sample.buttons |= script_buttons;
    sample.timeStamp = host_monotonic_ns() / 1000;

    memset(pad_data, 0, sizeof(*pad_data) * (size_t)count);
    for (int i = 0; i < count; i++) {
        pad_data[i] = sample;
        pad_data[i].timeStamp -= (uint64_t)i * 16667ull;
    }

    if (g_debug) {
        /* Rate-limited so a 60 Hz guest does not flood the log: always report a
           state change, otherwise a heartbeat every ~600 polls. */
        g_peek_calls++;
        if (sample.buttons != g_peek_logged_buttons || (g_peek_calls % 600) == 0) {
            fprintf(stderr, "vita2native: SceCtrl peek #%llu buttons=0x%x lx=%u ly=%u count=%d\n",
                    g_peek_calls, sample.buttons, sample.lx, sample.ly, count);
            g_peek_logged_buttons = sample.buttons;
        }
    }

    return count;
}

DECLARE_LIBRARY(SceCtrl, 0xD197E3C7);

int SceCtrl_init(void)
{
    g_debug = vita2native_env_flag("VITA2NATIVE_DEBUG_INPUT");

    if (g_debug)
        fprintf(stderr, "vita2native: SceCtrl reads input from SDL only\n");

    return 0;
}
