/*
 * Display presenter probe.
 *
 * Exercises the SDL video path without the emulator: cycles a few solid colours
 * and a moving marker, printing how long each present takes. This is the
 * fastest way to tell a display problem apart from an emulation one.
 *
 *   ./present_probe [seconds]     (0 seconds = run until Ctrl-C)
 *
 * SDL_VIDEODRIVER=dummy gives a headless (software-rendered) run.
 */
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "host.h"

static volatile sig_atomic_t g_stop;

static void on_signal(int signal_number)
{
    (void)signal_number;
    g_stop = 1;
}

static uint64_t now_ns(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static void put_pixel(uint32_t *frame, int width, int height, int x, int y, uint32_t rgba)
{
    if (x < 0 || y < 0 || x >= width || y >= height)
        return;
    frame[(size_t)y * (size_t)width + (size_t)x] = rgba;
}

static void fill_rect(uint32_t *frame, int width, int height, int x, int y, int w, int h,
                      uint32_t rgba)
{
    for (int row = 0; row < h; row++)
        for (int column = 0; column < w; column++)
            put_pixel(frame, width, height, x + column, y + row, rgba);
}

int main(int argc, char **argv)
{
    static const uint32_t colors[] = {
        0xFF0000FFu, /* A8B8G8R8: red   */
        0xFF00FF00u, /* green */
        0xFFFF0000u, /* blue  */
        0xFFFFFFFFu, /* white */
        0xFF00FFFFu, /* yellow */
        0xFFFF00FFu, /* magenta */
    };
    double seconds = argc > 1 ? strtod(argv[1], NULL) : 15.0;
    const int width = 960;
    const int height = 544;
    uint32_t *frame;
    HostVideo *video;
    uint64_t deadline = 0;
    unsigned int frame_index = 0;
    int failures = 0;
    unsigned int last_pad_buttons = ~0u;
    bool last_touch_down = false;
    int last_touch_x = -1, last_touch_y = -1;

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    video = host_video_open();
    if (!video) {
        fprintf(stderr, "present_probe: SDL video unavailable\n");
        return 1;
    }

    printf("present_probe: SDL output %dx%d stretch=%s\n", host_video_width(video),
           host_video_height(video), getenv("VITA2NATIVE_STRETCH") ? "yes" : "no");

    frame = calloc((size_t)width * height, sizeof(*frame));
    if (!frame) {
        host_video_close(video);
        return 1;
    }

    if (seconds > 0)
        deadline = now_ns() + (uint64_t)(seconds * 1e9);

    while (!g_stop && (!deadline || now_ns() < deadline)) {
        uint32_t background = colors[(frame_index / 20) % (sizeof(colors) / sizeof(colors[0]))];
        uint64_t start, elapsed;
        int marker_x;

        /* Solid field with a black border and a square travelling left to
           right, so a static capture cannot be mistaken for a frozen output. */
        for (size_t i = 0; i < (size_t)width * height; i++)
            frame[i] = background;
        fill_rect(frame, width, height, 0, 0, width, 4, 0xFF000000u);
        fill_rect(frame, width, height, 0, height - 4, width, 4, 0xFF000000u);
        fill_rect(frame, width, height, 0, 0, 4, height, 0xFF000000u);
        fill_rect(frame, width, height, width - 4, 0, 4, height, 0xFF000000u);

        marker_x = (int)((frame_index * 12) % (unsigned int)(width - 96));
        fill_rect(frame, width, height, marker_x, height / 2 - 48, 96, 96, 0xFF000000u);
        fill_rect(frame, width, height, marker_x + 8, height / 2 - 40, 80, 80, 0xFFFFFFFFu);

        /* A different pattern on the bottom edge identifies the frame index. */
        fill_rect(frame, width, height, 0, height - 64, 8 * (int)(frame_index % 100), 32,
                  0xFF000000u);

        start = now_ns();
        host_video_present(video, frame, width, height, width);
        elapsed = now_ns() - start;
        if (elapsed > 200000000ull) {
            failures++;
            printf("present_probe: frame %u took %.1f ms\n", frame_index, (double)elapsed / 1e6);
        }

        frame_index++;
        if (!(frame_index % 20))
            printf("present_probe: %u frames, last %.1f ms\n", frame_index,
                   (double)elapsed / 1e6);

        /* Surface SDL input so a remote tester can verify mappings: run under
           Xvfb and inject keys with xdotool. */
        {
            HostPadState pad;

            if (host_video_pad_state(&pad) && pad.buttons != last_pad_buttons) {
                printf("present_probe: pad buttons=0x%08x pressed=0x%08x "
                       "lx=%u ly=%u rx=%u ry=%u controller=%d\n",
                       pad.buttons, pad.pressed, pad.lx, pad.ly, pad.rx, pad.ry,
                       pad.active ? 1 : 0);
                fflush(stdout);
                last_pad_buttons = pad.buttons;
            }
        }

        {
            HostTouchState touch;

            if (host_video_touch_state(&touch) && touch.active &&
                (touch.down != last_touch_down ||
                 (touch.down && (touch.x != last_touch_x || touch.y != last_touch_y)))) {
                printf("present_probe: touch down=%d x=%u y=%u\n", touch.down ? 1 : 0,
                       (unsigned)touch.x, (unsigned)touch.y);
                fflush(stdout);
                last_touch_down = touch.down;
                last_touch_x = touch.x;
                last_touch_y = touch.y;
            }
        }
    }

    printf("present_probe: finished after %u frames (%d slow presents)\n", frame_index, failures);
    free(frame);
    host_video_close(video);
    return 0;
}
