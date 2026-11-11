/*
 * FPS debug pane.
 *
 * The pane is composed directly into the guest framebuffer, like the system
 * dialog overlay, so it appears on top of the game on every display backend
 * (KMSDRM, X11 or headless captures) without an extra GPU pass.
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "font8x8_basic.h"
#include "modules/fps_overlay.h"
#include "settings.h"

#define FPS_HISTORY 60
#define PANEL_WIDTH 232
#define PANEL_HEIGHT 112
#define PANEL_MARGIN 8
#define GRAPH_WIDTH (PANEL_WIDTH - 20)
#define GRAPH_HEIGHT 58

static double g_history[FPS_HISTORY];
static int g_head;
static int g_count;

bool fps_overlay_enabled(void)
{
    static int enabled = -1;

    if (enabled < 0)
        enabled = vita2native_env_flag("VITA2NATIVE_FPS_OVERLAY");
    return enabled != 0;
}

void fps_overlay_add_sample(double fps)
{
    if (!fps_overlay_enabled())
        return;

    g_history[g_head] = fps;
    g_head = (g_head + 1) % FPS_HISTORY;
    if (g_count < FPS_HISTORY)
        g_count++;
}

static uint32_t rgb_pixel(uint8_t r, uint8_t g, uint8_t b)
{
    /* A8B8G8R8 little-endian word: R in the low byte. */
    return 0xFF000000u | ((uint32_t)b << 16) | ((uint32_t)g << 8) | r;
}

static void blend_pixel(uint32_t *pixels, int width, int height, int pitch, int x, int y,
                        uint8_t r, uint8_t g, uint8_t b, int alpha)
{
    uint32_t *dst;
    uint32_t old;
    int dr, dg, db;

    if (x < 0 || y < 0 || x >= width || y >= height)
        return;

    dst = &pixels[(size_t)y * (size_t)pitch + x];
    old = *dst;
    dr = (int)(old & 0xFF);
    dg = (int)((old >> 8) & 0xFF);
    db = (int)((old >> 16) & 0xFF);

    dr = (dr * (255 - alpha) + r * alpha) / 255;
    dg = (dg * (255 - alpha) + g * alpha) / 255;
    db = (db * (255 - alpha) + b * alpha) / 255;

    *dst = 0xFF000000u | ((uint32_t)db << 16) | ((uint32_t)dg << 8) | (uint32_t)dr;
}

static void fill_rect(uint32_t *pixels, int width, int height, int pitch, int x, int y, int w,
                      int h, uint8_t r, uint8_t g, uint8_t b, int alpha)
{
    for (int row = 0; row < h; row++) {
        for (int col = 0; col < w; col++)
            blend_pixel(pixels, width, height, pitch, x + col, y + row, r, g, b, alpha);
    }
}

static void draw_char(uint32_t *pixels, int width, int height, int pitch, int x, int y, char c,
                      int scale, uint32_t color)
{
    int glyph = (unsigned char)c;

    if (glyph < 32 || glyph > 127)
        glyph = '?';

    for (int row = 0; row < 8; row++) {
        unsigned int bits = (unsigned char)font8x8_basic[glyph][row];

        for (int col = 0; col < 8; col++) {
            if (bits & (1u << col)) {
                for (int sy = 0; sy < scale; sy++) {
                    for (int sx = 0; sx < scale; sx++) {
                        int px = x + col * scale + sx;
                        int py = y + row * scale + sy;

                        if (px >= 0 && py >= 0 && px < width && py < height)
                            pixels[(size_t)py * (size_t)pitch + px] = color;
                    }
                }
            }
        }
    }
}

static void draw_text(uint32_t *pixels, int width, int height, int pitch, int x, int y,
                      const char *text, int scale, uint32_t color)
{
    int cursor = x;

    for (const char *p = text; *p; p++) {
        draw_char(pixels, width, height, pitch, cursor, y, *p, scale, color);
        cursor += 8 * scale;
    }
}

static void draw_line(uint32_t *pixels, int width, int height, int pitch, int x0, int y0, int x1,
                      int y1, uint32_t color)
{
    int dx = abs(x1 - x0);
    int sx = x0 < x1 ? 1 : -1;
    int dy = -abs(y1 - y0);
    int sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;

    for (;;) {
        if (x0 >= 0 && y0 >= 0 && x0 < width && y0 < height)
            pixels[(size_t)y0 * (size_t)pitch + x0] = color;

        if (x0 == x1 && y0 == y1)
            break;

        {
            int e2 = 2 * err;

            if (e2 >= dy) {
                err += dy;
                x0 += sx;
            }
            if (e2 <= dx) {
                err += dx;
                y0 += sy;
            }
        }
    }
}

static double history_at(int index)
{
    int slot = (g_head - g_count + index + 2 * FPS_HISTORY) % FPS_HISTORY;

    return g_history[slot];
}

void fps_overlay_render(uint32_t *pixels, int width, int height, int pitch)
{
    int panel_x, panel_y;
    int graph_x, graph_y;
    char line[64];
    double current = g_count > 0 ? history_at(g_count - 1) : 0.0;
    double total = 0.0;
    double maximum = 1.0;
    int max_scale;

    if (!fps_overlay_enabled() || !pixels || width <= 0 || height <= 0 || pitch < width)
        return;

    panel_x = width - PANEL_WIDTH - PANEL_MARGIN;
    panel_y = PANEL_MARGIN;
    if (panel_x < 0)
        panel_x = 0;
    if (panel_y + PANEL_HEIGHT > height)
        panel_y = height > PANEL_HEIGHT ? height - PANEL_HEIGHT : 0;

    fill_rect(pixels, width, height, pitch, panel_x, panel_y, PANEL_WIDTH, PANEL_HEIGHT, 0, 0, 0,
              190);
    fill_rect(pixels, width, height, pitch, panel_x + 1, panel_y + 1, PANEL_WIDTH - 2,
              PANEL_HEIGHT - 2, 28, 32, 38, 150);

    for (int i = 0; i < g_count; i++) {
        double sample = history_at(i);

        total += sample;
        if (sample > maximum)
            maximum = sample;
    }

    snprintf(line, sizeof(line), "FPS %.1f", current);
    draw_text(pixels, width, height, pitch, panel_x + 10, panel_y + 8, line, 2,
              rgb_pixel(88, 166, 255));

    snprintf(line, sizeof(line), "avg %.1f  max %.1f", g_count ? total / g_count : 0.0, maximum);
    draw_text(pixels, width, height, pitch, panel_x + 10, panel_y + 30, line, 1,
              rgb_pixel(201, 209, 217));

    graph_x = panel_x + 10;
    graph_y = panel_y + 44;
    fill_rect(pixels, width, height, pitch, graph_x, graph_y, GRAPH_WIDTH, GRAPH_HEIGHT, 8, 10, 14,
              190);

    max_scale = (int)maximum + 1;
    if (max_scale < 10)
        max_scale = 10;

    for (int i = 1; i < 4; i++) {
        int y = graph_y + (GRAPH_HEIGHT * i) / 4;

        fill_rect(pixels, width, height, pitch, graph_x, y, GRAPH_WIDTH, 1, 48, 54, 61, 160);
    }

    for (int i = 0; i < g_count; i++) {
        double sample = history_at(i);
        int x = graph_x + (g_count > 1 ? (GRAPH_WIDTH - 1) * i / (g_count - 1) : 0);
        int y = graph_y + GRAPH_HEIGHT - 1 - (int)(sample * (GRAPH_HEIGHT - 1) / max_scale);

        if (y < graph_y)
            y = graph_y;
        if (y >= graph_y + GRAPH_HEIGHT)
            y = graph_y + GRAPH_HEIGHT - 1;

        if (i > 0) {
            double previous = history_at(i - 1);
            int px = graph_x + (GRAPH_WIDTH - 1) * (i - 1) / (g_count - 1);
            int py = graph_y + GRAPH_HEIGHT - 1 -
                     (int)(previous * (GRAPH_HEIGHT - 1) / max_scale);

            if (py < graph_y)
                py = graph_y;
            if (py >= graph_y + GRAPH_HEIGHT)
                py = graph_y + GRAPH_HEIGHT - 1;
            draw_line(pixels, width, height, pitch, px, py, x, y, rgb_pixel(88, 166, 255));
        }

        fill_rect(pixels, width, height, pitch, x, y, 2, 2, 88, 166, 255, 255);
    }
}

const uint32_t *fps_overlay_image(int *width, int *height)
{
    static uint32_t image[PANEL_WIDTH * PANEL_HEIGHT];

    if (!fps_overlay_enabled())
        return NULL;

    /* The pane is opaque after compositing onto a black canvas; render into a
       standalone image so GPU presentation can draw it over the game. */
    memset(image, 0, sizeof(image));
    fps_overlay_render(image, PANEL_WIDTH, PANEL_HEIGHT, PANEL_WIDTH);

    if (width)
        *width = PANEL_WIDTH;
    if (height)
        *height = PANEL_HEIGHT;
    return image;
}
