/*
 * Display presenter probe.
 *
 * Exercises host_console.c without the emulator: cycles a few solid colours and
 * a moving marker on the panel, printing how long each present takes. This is
 * the fastest way to tell a KMS/modeset problem apart from an emulation one.
 *
 *   ./present_probe [device|auto] [seconds]     (0 seconds = run until Ctrl-C)
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
    const char *path = argc > 1 ? argv[1] : NULL;
    double seconds = argc > 2 ? strtod(argv[2], NULL) : 15.0;
    const int width = 960;
    const int height = 544;
    uint32_t *frame;
    HostConsole *console;
    uint64_t deadline = 0;
    unsigned int frame_index = 0;
    int failures = 0;

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    console = host_console_open(path);
    if (!console) {
        fprintf(stderr, "present_probe: no display backend (tried %s)\n", path ? path : "auto");
        return 1;
    }

    printf("present_probe: backend=%s size=%dx%d stretch=%s\n", host_console_backend(console),
           host_console_width(console), host_console_height(console),
           getenv("VITA2NATIVE_STRETCH") ? "yes" : "no");

    frame = calloc((size_t)width * height, sizeof(*frame));
    if (!frame) {
        host_console_close(console);
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
        host_console_present(console, frame, width, height, width);
        elapsed = now_ns() - start;
        if (elapsed > 200000000ull) {
            failures++;
            printf("present_probe: frame %u took %.1f ms\n", frame_index, (double)elapsed / 1e6);
        }

        frame_index++;
        if (!(frame_index % 20))
            printf("present_probe: %u frames, last %.1f ms\n", frame_index,
                   (double)elapsed / 1e6);
    }

    printf("present_probe: finished after %u frames (%d slow presents)\n", frame_index, failures);
    free(frame);
    host_console_close(console);
    return 0;
}
