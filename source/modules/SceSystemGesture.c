/*
 * SceSystemGesture: the primitive touch recognizer.
 *
 * Some engines (Fort Defense) do not read SceTouchData themselves: they feed
 * every frame's touch sample to the primitive recognizer and then consume the
 * primitive touch events. While the library was missing, the recognizer calls
 * failed and the games saw no input at all.
 *
 * Only the primitive recognizer is implemented: raw touches become down, move
 * and up events. The higher level recognizers (tap, pinch, ...) accept their
 * setup calls and stay inert, so a title that depends on gesture recognition
 * simply never reports a gesture.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <psp2/touch.h>

#include "module.h"

#define PRIMITIVE_TYPE_DOWN 1
#define PRIMITIVE_TYPE_MOVE 2
#define PRIMITIVE_TYPE_UP 3

/* No public header ships this error, but the value follows the usual layout. */
#define SCE_SYSTEM_GESTURE_ERROR_INVALID_ARG 0x80418001

/*
 * Titles index the event buffer with a 0x60 stride and read the type at 0, the
 * touch id at 8 and the panel coordinates at 0x0c/0x0e (the engine then halves
 * them to screen space). The remaining bytes are reserved.
 */
struct primitive_touch_event {
    int32_t type;
    int32_t unknown_04;
    int32_t touch_id;
    int16_t x;
    int16_t y;
    int16_t unknown_10;
    int16_t unknown_12;
    uint8_t reserved[0x60 - 0x14];
};

#define MAX_PRIMITIVE_EVENTS 16

static bool g_primitive_initialized;
static bool g_touch_down;
static int16_t g_touch_x;
static int16_t g_touch_y;
static struct primitive_touch_event g_events[MAX_PRIMITIVE_EVENTS];
static unsigned int g_event_count;

static bool gesture_debug(void)
{
    static int enabled = -1;

    if (enabled < 0)
        enabled = getenv("VITA2NATIVE_INPUT_DEBUG") != NULL;
    return enabled;
}

static void push_event(int32_t type, int16_t x, int16_t y)
{
    struct primitive_touch_event *event;

    if (g_event_count >= MAX_PRIMITIVE_EVENTS)
        return;

    event = &g_events[g_event_count++];
    memset(event, 0, sizeof(*event));
    event->type = type;
    event->touch_id = 0;
    event->x = x;
    event->y = y;

    if (gesture_debug()) {
        fprintf(stderr, "vita2native: primitive touch type=%d %d,%d\n", (int)type, x, y);
        fflush(stderr);
    }
}

EXPORT(SceSystemGesture, 0x6078A08B, int, sceSystemGestureInitializePrimitiveTouchRecognizer, void)
{
    g_primitive_initialized = true;
    g_touch_down = false;
    g_event_count = 0;
    return 0;
}

EXPORT(SceSystemGesture, 0xB3875104, int, sceSystemGestureFinalizePrimitiveTouchRecognizer, void)
{
    g_primitive_initialized = false;
    g_touch_down = false;
    g_event_count = 0;
    return 0;
}

EXPORT(SceSystemGesture, 0xFD5A6504, int, sceSystemGestureResetPrimitiveTouchRecognizer, void)
{
    g_touch_down = false;
    g_event_count = 0;
    return 0;
}

EXPORT(SceSystemGesture, 0xDF4C665A, int, sceSystemGestureUpdatePrimitiveTouchRecognizer,
       const SceTouchData *touch_data, uint32_t flags)
{
    bool down;
    int16_t x;
    int16_t y;

    (void)flags;

    if (!g_primitive_initialized || !touch_data)
        return 0;

    down = touch_data->reportNum > 0;
    x = down ? touch_data->report[0].x : g_touch_x;
    y = down ? touch_data->report[0].y : g_touch_y;

    if (down && !g_touch_down) {
        push_event(PRIMITIVE_TYPE_DOWN, x, y);
    } else if (down && (x != g_touch_x || y != g_touch_y)) {
        push_event(PRIMITIVE_TYPE_MOVE, x, y);
    } else if (!down && g_touch_down) {
        push_event(PRIMITIVE_TYPE_UP, g_touch_x, g_touch_y);
    }

    g_touch_down = down;
    if (down) {
        g_touch_x = x;
        g_touch_y = y;
    }

    return 0;
}

EXPORT(SceSystemGesture, 0xC750D3DA, int, sceSystemGestureGetPrimitiveTouchEvents,
       struct primitive_touch_event *events, unsigned int max_events, unsigned int *count)
{
    unsigned int num = g_event_count;

    if (num > max_events)
        num = max_events;

    if (events && num)
        memcpy(events, g_events, num * sizeof(g_events[0]));

    if (count)
        *count = num;

    g_event_count = 0;
    return 0;
}

EXPORT(SceSystemGesture, 0xBAB8ECCB, int, sceSystemGestureGetPrimitiveTouchEventsCount,
       unsigned int *count)
{
    if (count)
        *count = g_event_count;
    return 0;
}

EXPORT(SceSystemGesture, 0xE0577765, int, sceSystemGestureGetPrimitiveTouchEventByIndex,
       unsigned int index, struct primitive_touch_event *event)
{
    if (!event || index >= g_event_count)
        return SCE_SYSTEM_GESTURE_ERROR_INVALID_ARG;

    *event = g_events[index];
    return 0;
}

EXPORT(SceSystemGesture, 0x480564C9, int, sceSystemGestureGetPrimitiveTouchEventByPrimitiveID,
       unsigned int id, struct primitive_touch_event *event)
{
    (void)id;
    (void)event;

    return SCE_SYSTEM_GESTURE_ERROR_INVALID_ARG;
}

/*
 * Gesture recognizers. Titles create one before feeding touches, so the call
 * has to succeed; nothing is ever recognised through it.
 */
EXPORT(SceSystemGesture, 0xC3367370, int, sceSystemGestureCreateTouchRecognizer, void *recognizer,
       unsigned int port, unsigned int type, const void *param, const void *option)
{
    (void)recognizer;
    (void)port;
    (void)type;
    (void)param;
    (void)option;

    return 0;
}

EXPORT(SceSystemGesture, 0x851FB144, int, sceSystemGestureUpdateTouchRecognizer, void *recognizer)
{
    (void)recognizer;
    return 0;
}

EXPORT(SceSystemGesture, 0xA9DB29F6, int, sceSystemGestureUpdateTouchRecognizerRectangle,
       void *recognizer, int region)
{
    (void)recognizer;
    (void)region;
    return 0;
}

EXPORT(SceSystemGesture, 0x0D941B90, int, sceSystemGestureResetTouchRecognizer, void *recognizer)
{
    (void)recognizer;
    return 0;
}

EXPORT(SceSystemGesture, 0xF0DB1AE5, int, sceSystemGestureGetTouchRecognizerInformation,
       void *recognizer, void *info)
{
    (void)recognizer;
    (void)info;

    return SCE_SYSTEM_GESTURE_ERROR_INVALID_ARG;
}

EXPORT(SceSystemGesture, 0x13AD2218, int, sceSystemGestureGetTouchEventsCount, void *recognizer,
       unsigned int *count)
{
    (void)recognizer;

    if (count)
        *count = 0;
    return 0;
}

EXPORT(SceSystemGesture, 0x789D867C, int, sceSystemGestureGetTouchEvents, void *recognizer,
       void *events, unsigned int max_events, unsigned int *count)
{
    (void)recognizer;
    (void)events;
    (void)max_events;

    if (count)
        *count = 0;
    return 0;
}

DECLARE_LIBRARY(SceSystemGesture, 0xAF3FE2D3);
