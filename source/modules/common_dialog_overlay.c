/*
 * System dialog overlay renderer.
 *
 * The panel is composed directly into the guest framebuffer using the public
 * domain 8x8 VGA font, so it works with both the headless and the SDL
 * presentation paths without a GPU pass.
 */
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "font8x8_basic.h"
#include "modules/common_dialog_overlay.h"

#define PANEL_MAX_WIDTH  760
#define PANEL_MIN_WIDTH  360
#define TEXT_SCALE       2
#define GLYPH_WIDTH      (8 * TEXT_SCALE)
#define GLYPH_HEIGHT     (8 * TEXT_SCALE)
#define LINE_HEIGHT      (GLYPH_HEIGHT + 6)

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

static void outline_rect(uint32_t *pixels, int width, int height, int pitch, int x, int y, int w,
                         int h, uint8_t r, uint8_t g, uint8_t b)
{
    fill_rect(pixels, width, height, pitch, x, y, w, 2, r, g, b, 255);
    fill_rect(pixels, width, height, pitch, x, y + h - 2, w, 2, r, g, b, 255);
    fill_rect(pixels, width, height, pitch, x, y, 2, h, r, g, b, 255);
    fill_rect(pixels, width, height, pitch, x + w - 2, y, 2, h, r, g, b, 255);
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

static int draw_text_line(uint32_t *pixels, int width, int height, int pitch, int x, int y,
                          const char *text, int scale, uint32_t color, int max_width)
{
    int cursor = x;

    for (const char *p = text; *p; p++) {
        if (cursor + 8 * scale > x + max_width)
            break;
        draw_char(pixels, width, height, pitch, cursor, y, *p, scale, color);
        cursor += 8 * scale;
    }

    return cursor;
}

/* Draws wrapped text and returns the height consumed. */
static int draw_wrapped(uint32_t *pixels, int width, int height, int pitch, int x, int y,
                        const char *text, int scale, uint32_t color, int max_width, int max_lines)
{
    int line = 0;
    int cursor_x = x;
    int cursor_y = y;
    const char *word = text;

    while (*word && line < max_lines) {
        const char *end = word;
        int word_len;

        while (*end && *end != ' ')
            end++;
        word_len = (int)(end - word);

        if (cursor_x + word_len * 8 * scale > x + max_width && cursor_x != x) {
            line++;
            cursor_x = x;
            cursor_y += 8 * scale + 6;
            if (line >= max_lines)
                break;
        }

        for (int i = 0; i < word_len; i++) {
            draw_char(pixels, width, height, pitch, cursor_x, cursor_y, word[i], scale, color);
            cursor_x += 8 * scale;
        }

        if (*end == ' ') {
            cursor_x += 8 * scale;
            end++;
        }
        word = end;
    }

    return (line + 1) * (8 * scale + 6);
}

void CommonDialogOverlay_render(const CommonDialogOverlay *overlay, uint32_t *pixels, int width,
                                int height, int pitch)
{
    int panel_w;
    int panel_h;
    int panel_x;
    int panel_y;
    int text_x;
    int text_y;
    int message_width;
    uint32_t text_color = rgb_pixel(235, 238, 242);
    uint32_t title_color = rgb_pixel(120, 200, 255);

    if (!overlay || !pixels || width <= 0 || height <= 0 || pitch < width)
        return;

    panel_w = width - 80;
    if (panel_w > PANEL_MAX_WIDTH)
        panel_w = PANEL_MAX_WIDTH;
    if (panel_w < PANEL_MIN_WIDTH)
        panel_w = width - 20;
    if (panel_w < 120)
        panel_w = width;

    panel_h = 190;
    if (panel_h > height - 40)
        panel_h = height - 20;
    if (panel_h < 100)
        panel_h = height;

    panel_x = (width - panel_w) / 2;
    panel_y = (height - panel_h) / 2;

    /* Dim the whole frame slightly, then draw an opaque panel. */
    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++)
            blend_pixel(pixels, width, height, pitch, x, y, 0, 0, 0, 96);
    }

    fill_rect(pixels, width, height, pitch, panel_x, panel_y, panel_w, panel_h, 18, 24, 34, 240);
    outline_rect(pixels, width, height, pitch, panel_x, panel_y, panel_w, panel_h, 120, 200, 255);

    text_x = panel_x + 24;
    text_y = panel_y + 18;
    draw_text_line(pixels, width, height, pitch, text_x, text_y, overlay->title, TEXT_SCALE,
                   title_color, panel_w - 48);
    fill_rect(pixels, width, height, pitch, text_x, text_y + 22, panel_w - 48, 1, 70, 90, 110,
              255);

    message_width = panel_w - 48;
    draw_wrapped(pixels, width, height, pitch, text_x, text_y + 40, overlay->message, TEXT_SCALE,
                 text_color, message_width, 3);

    if (overlay->progress >= 0) {
        int bar_x = panel_x + 40;
        int bar_y = panel_y + panel_h - 62;
        int bar_w = panel_w - 80;
        int bar_h = 14;
        int fill = bar_w * overlay->progress / 100;

        outline_rect(pixels, width, height, pitch, bar_x, bar_y, bar_w, bar_h, 150, 170, 190);
        if (fill > 4)
            fill_rect(pixels, width, height, pitch, bar_x + 2, bar_y + 2, fill - 4, bar_h - 4, 90,
                      190, 120, 255);
    }

    if (overlay->hint[0]) {
        draw_text_line(pixels, width, height, pitch, panel_x + 24, panel_y + panel_h - 32,
                       overlay->hint, TEXT_SCALE, rgb_pixel(255, 230, 140), panel_w - 48);
    }
}
