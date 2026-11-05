/*
 * Scripted headless controller test.
 *
 * The controller backend is keyed to the display frame counter, so the test
 * supplies the counter and verifies press/release transitions.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include <psp2/ctrl.h>

#include "modules/SceDisplay.h"

static unsigned int g_frame;

unsigned int SceDisplay_get_frame_count(void)
{
    return g_frame;
}

uint64_t host_monotonic_ns(void)
{
    return (uint64_t)g_frame * 1000000u;
}

int sceCtrlPeekBufferPositive(int port, SceCtrlData *pad_data, int count);

static int peek(unsigned int *buttons)
{
    SceCtrlData pad;

    if (sceCtrlPeekBufferPositive(0, &pad, 1) < 0)
        return -1;
    *buttons = pad.buttons;
    return 0;
}

static int expect(unsigned int frame, unsigned int buttons)
{
    unsigned int actual = 0;

    g_frame = frame;
    if (peek(&actual) != 0) {
        fprintf(stderr, "peek failed at frame %u\n", frame);
        return -1;
    }
    if (actual != buttons) {
        fprintf(stderr, "frame %u: buttons 0x%08x, expected 0x%08x\n", frame, actual, buttons);
        return -1;
    }
    return 0;
}

int main(void)
{
    if (setenv("VITA2NATIVE_CTRL_SCRIPT", "0:down,2:,3:cross+start,5:r1,7:", 1) != 0) {
        fprintf(stderr, "setenv failed\n");
        return 1;
    }

    if (expect(0, SCE_CTRL_DOWN) != 0 || expect(1, SCE_CTRL_DOWN) != 0 ||
        expect(2, 0) != 0 || expect(4, SCE_CTRL_CROSS | SCE_CTRL_START) != 0 ||
        expect(6, SCE_CTRL_R1) != 0 || expect(7, 0) != 0 || expect(20, 0) != 0)
        return 1;

    printf("ctrl_script_smoke: ok\n");
    return 0;
}
