/*
 * Guest-side controller mapping probe.
 *
 * Runs as a normal Vita executable under vita2native. It brings up the display
 * (which starts the host's SDL video thread, so both input sources are live),
 * then draws the live pad state into a 960x544 A8B8G8R8 framebuffer and polls
 * sceCtrlPeekBufferPositive every 50 ms. Every button/axis change and a
 * one-second heartbeat are also logged with sceClibPrintf.
 */

#include "font8x8_basic.h"

typedef unsigned char u8;
typedef unsigned int u32;

/* SceCtrlData is 0x20 bytes. */
typedef struct {
    unsigned long long timestamp;
    u32 buttons;
    u8 lx, ly, rx, ry;
    u8 up, right, down, left;
    u8 lt, rt, l1, r1;
    u8 triangle, circle, cross, square;
    u8 reserved[4];
} CtrlData;

/* SceDisplayFrameBuf is 0x18 bytes. */
typedef struct {
    u32 size;
    const void *base;
    u32 pitch;
    u32 pixelformat;
    u32 width;
    u32 height;
} DisplayFrameBuf;

_Static_assert(sizeof(CtrlData) == 0x20, "unexpected SceCtrlData layout");
_Static_assert(sizeof(DisplayFrameBuf) == 0x18, "unexpected SceDisplayFrameBuf layout");

/* Import stubs patched by the loader; see ctrl_test.S. */
extern int guest_peek(int port, CtrlData *data, int count);
extern int guest_printf(const char *fmt, ...);
extern int guest_delay(u32 usec);
extern int guest_exit(int code);
extern int guest_setfb(const DisplayFrameBuf *fb, int sync);

#define SCE_CTRL_SELECT   0x00000001u
#define SCE_CTRL_L3       0x00000002u
#define SCE_CTRL_R3       0x00000004u
#define SCE_CTRL_START    0x00000008u
#define SCE_CTRL_UP       0x00000010u
#define SCE_CTRL_RIGHT    0x00000020u
#define SCE_CTRL_DOWN     0x00000040u
#define SCE_CTRL_LEFT     0x00000080u
#define SCE_CTRL_L2       0x00000100u
#define SCE_CTRL_R2       0x00000200u
#define SCE_CTRL_L1       0x00000400u
#define SCE_CTRL_R1       0x00000800u
#define SCE_CTRL_TRIANGLE 0x00001000u
#define SCE_CTRL_CIRCLE   0x00002000u
#define SCE_CTRL_CROSS    0x00004000u
#define SCE_CTRL_SQUARE   0x00008000u

#define SCREEN_WIDTH  960
#define SCREEN_HEIGHT 544
#define SAMPLE_USEC   50000u
#define HEARTBEAT     20u
#define EXIT_HOLD     40u

static CtrlData g_pad;
static DisplayFrameBuf g_fb;
static u32 g_framebuffer[SCREEN_WIDTH * SCREEN_HEIGHT];
static u32 g_last_buttons = 0xFFFFFFFFu;
static u32 g_last_axes = 0xFFFFFFFFu;
static u32 g_heartbeat;
static u32 g_exit_hold;
static int g_peek_failed;

/* ------------------------------------------------------------------------- */
/* drawing                                                                   */
/* ------------------------------------------------------------------------- */

/* A8B8G8R8 little-endian word: R in the low byte. */
static u32 rgb(u32 r, u32 g, u32 b)
{
    return 0xFF000000u | (b << 16) | (g << 8) | r;
}

static void fill_rect(int x, int y, int w, int h, u32 color)
{
    int x0 = x, y0 = y, x1 = x + w, y1 = y + h;

    if (x0 < 0)
        x0 = 0;
    if (y0 < 0)
        y0 = 0;
    if (x1 > SCREEN_WIDTH)
        x1 = SCREEN_WIDTH;
    if (y1 > SCREEN_HEIGHT)
        y1 = SCREEN_HEIGHT;

    for (int yy = y0; yy < y1; yy++) {
        u32 *row = &g_framebuffer[(u32)yy * SCREEN_WIDTH];

        for (int xx = x0; xx < x1; xx++)
            row[xx] = color;
    }
}

static void frame_rect(int x, int y, int w, int h, int thickness, u32 color)
{
    fill_rect(x, y, w, thickness, color);
    fill_rect(x, y + h - thickness, w, thickness, color);
    fill_rect(x, y, thickness, h, color);
    fill_rect(x + w - thickness, y, thickness, h, color);
}

static void put_pixel(int x, int y, u32 color)
{
    if ((unsigned int)x < SCREEN_WIDTH && (unsigned int)y < SCREEN_HEIGHT)
        g_framebuffer[(u32)y * SCREEN_WIDTH + x] = color;
}

static void fill_circle(int cx, int cy, int radius, u32 color)
{
    int r2 = radius * radius;

    for (int dy = -radius; dy <= radius; dy++) {
        for (int dx = -radius; dx <= radius; dx++) {
            if (dx * dx + dy * dy <= r2)
                put_pixel(cx + dx, cy + dy, color);
        }
    }
}

static void circle_outline(int cx, int cy, int radius, int thickness, u32 color)
{
    int r2 = radius * radius;
    int inner = radius - thickness;
    int inner2 = inner * inner;

    for (int dy = -radius; dy <= radius; dy++) {
        for (int dx = -radius; dx <= radius; dx++) {
            int d2 = dx * dx + dy * dy;

            if (d2 <= r2 && d2 >= inner2)
                put_pixel(cx + dx, cy + dy, color);
        }
    }
}

static int text_width(const char *text, int scale)
{
    int count = 0;

    while (*text++)
        count++;
    return count * 8 * scale;
}

static void draw_char(int x, int y, int scale, u32 color, char c)
{
    unsigned int glyph = (unsigned char)c;

    if (glyph < 32 || glyph > 126)
        glyph = '?';

    for (int row = 0; row < 8; row++) {
        unsigned int bits = (unsigned char)font8x8_basic[glyph][row];

        for (int col = 0; col < 8; col++) {
            if (bits & (1u << col))
                fill_rect(x + col * scale, y + row * scale, scale, scale, color);
        }
    }
}

static void draw_text(int x, int y, int scale, u32 color, const char *text)
{
    for (; *text; text++) {
        draw_char(x, y, scale, color, *text);
        x += 8 * scale;
    }
}

static void draw_text_centered(int cx, int y, int scale, u32 color, const char *text)
{
    draw_text(cx - text_width(text, scale) / 2, y, scale, color, text);
}

static void draw_uint3(int x, int y, int scale, u32 color, u32 value)
{
    char digits[3];

    if (value > 999)
        value = 999;
    digits[0] = value >= 100 ? (char)('0' + value / 100) : ' ';
    digits[1] = value >= 10 ? (char)('0' + (value / 10) % 10) : ' ';
    digits[2] = (char)('0' + value % 10);

    for (int i = 0; i < 3; i++)
        draw_char(x + i * 8 * scale, y, scale, color, digits[i]);
}

static void draw_hex32(int x, int y, int scale, u32 color, u32 value)
{
    static const char hex[] = "0123456789ABCDEF";

    for (int i = 7; i >= 0; i--)
        draw_char(x + (7 - i) * 8 * scale, y, scale, color, hex[(value >> (i * 4)) & 0xF]);
}

/* ------------------------------------------------------------------------- */
/* controller UI                                                             */
/* ------------------------------------------------------------------------- */

static const u32 COL_BG = 0xFF18120Eu;       /* rgb(14, 18, 24) */
static const u32 COL_PANEL = 0xFF29201Au;    /* rgb(26, 32, 41) */
static const u32 COL_PANEL_DOWN = 0xFF304A16u; /* rgb(22, 74, 48) */
static const u32 COL_LINE = 0xFF483A30u;     /* rgb(48, 58, 72) */
static const u32 COL_BORDER = 0xFF625246u;   /* rgb(70, 82, 98) */
static const u32 COL_BORDER_DOWN = 0xFF82DC50u; /* rgb(80, 220, 130) */
static const u32 COL_TEXT = 0xFFE8DED6u;     /* rgb(214, 222, 232) */
static const u32 COL_DIM = 0xFF948478u;      /* rgb(120, 132, 148) */
static const u32 COL_ACCENT = 0xFFF0A658u;   /* rgb(88, 166, 255) */
static const u32 COL_DOWN = 0xFFB4FF96u;     /* rgb(150, 255, 180) */

static void draw_button(int x, int y, int w, int h, const char *label, int scale, int pressed)
{
    u32 fill = pressed ? COL_PANEL_DOWN : COL_PANEL;
    u32 border = pressed ? COL_BORDER_DOWN : COL_BORDER;
    u32 text = pressed ? COL_DOWN : COL_TEXT;
    int text_y = y + (h - 8 * scale) / 2;

    fill_rect(x, y, w, h, fill);
    frame_rect(x, y, w, h, 2, border);
    draw_text_centered(x + w / 2, text_y, scale, text, label);
}

static void draw_stick(int cx, int cy, int radius, int axis_x, int axis_y, int pressed,
                       const char *label)
{
    int px = cx + ((axis_x - 128) * (radius - 12)) / 128;
    int py = cy + ((axis_y - 128) * (radius - 12)) / 128;

    fill_circle(cx, cy, radius, rgb(30, 38, 48));
    circle_outline(cx, cy, radius, 2, pressed ? COL_BORDER_DOWN : COL_BORDER);

    fill_rect(cx - 1, cy - radius + 10, 2, 2 * (radius - 10), rgb(45, 52, 62));
    fill_rect(cx - radius + 10, cy - 1, 2 * (radius - 10), 2, rgb(45, 52, 62));

    fill_circle(px, py, pressed ? 10 : 7, pressed ? COL_BORDER_DOWN : COL_ACCENT);
    draw_text_centered(cx, cy + radius + 8, 1, COL_DIM, label);
}

static void draw_ui(void)
{
    u32 b = g_pad.buttons;

    fill_rect(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT, COL_BG);

    draw_text(24, 14, 2, COL_TEXT, "VITA2NATIVE CONTROLLER TEST");
    draw_text(636, 16, 2, COL_ACCENT, "MASK 0x");
    draw_hex32(636 + 7 * 16, 16, 2, COL_ACCENT, b);
    fill_rect(24, 46, SCREEN_WIDTH - 48, 2, COL_LINE);

    draw_button(24, 64, 100, 38, "L2", 2, (b & SCE_CTRL_L2) != 0);
    draw_button(132, 64, 100, 38, "L1", 2, (b & SCE_CTRL_L1) != 0);
    draw_button(728, 64, 100, 38, "R1", 2, (b & SCE_CTRL_R1) != 0);
    draw_button(836, 64, 100, 38, "R2", 2, (b & SCE_CTRL_R2) != 0);

    draw_button(173, 139, 54, 54, "U", 2, (b & SCE_CTRL_UP) != 0);
    draw_button(173, 247, 54, 54, "D", 2, (b & SCE_CTRL_DOWN) != 0);
    draw_button(119, 193, 54, 54, "L", 2, (b & SCE_CTRL_LEFT) != 0);
    draw_button(227, 193, 54, 54, "R", 2, (b & SCE_CTRL_RIGHT) != 0);

    draw_button(733, 139, 54, 54, "TRI", 1, (b & SCE_CTRL_TRIANGLE) != 0);
    draw_button(787, 193, 54, 54, "CIR", 1, (b & SCE_CTRL_CIRCLE) != 0);
    draw_button(733, 247, 54, 54, "CRO", 1, (b & SCE_CTRL_CROSS) != 0);
    draw_button(679, 193, 54, 54, "SQR", 1, (b & SCE_CTRL_SQUARE) != 0);

    draw_button(400, 472, 72, 36, "SEL", 2, (b & SCE_CTRL_SELECT) != 0);
    draw_button(488, 472, 72, 36, "STA", 2, (b & SCE_CTRL_START) != 0);

    draw_stick(200, 400, 54, g_pad.lx, g_pad.ly, (b & SCE_CTRL_L3) != 0, "LSTICK");
    draw_stick(760, 400, 54, g_pad.rx, g_pad.ry, (b & SCE_CTRL_R3) != 0, "RSTICK");

    draw_text(144, 476, 1, COL_DIM, "L ");
    draw_uint3(160, 476, 1, COL_TEXT, g_pad.lx);
    draw_char(184, 476, 1, COL_DIM, ',');
    draw_uint3(192, 476, 1, COL_TEXT, g_pad.ly);

    draw_text(704, 476, 1, COL_DIM, "R ");
    draw_uint3(720, 476, 1, COL_TEXT, g_pad.rx);
    draw_char(744, 476, 1, COL_DIM, ',');
    draw_uint3(752, 476, 1, COL_TEXT, g_pad.ry);

    draw_text_centered(SCREEN_WIDTH / 2, 522, 1, COL_DIM, "HOLD SELECT+START TO EXIT");
}

/* ------------------------------------------------------------------------- */
/* logging                                                                   */
/* ------------------------------------------------------------------------- */

static u32 axes_packed(void)
{
    return (u32)g_pad.lx | ((u32)g_pad.ly << 8) | ((u32)g_pad.rx << 16) |
           ((u32)g_pad.ry << 24);
}

static void log_state(const char *tag)
{
    u32 b = g_pad.buttons;

    guest_printf("%s buttons=0x%08X lx=%u ly=%u rx=%u ry=%u\n", tag, b, g_pad.lx, g_pad.ly,
                 g_pad.rx, g_pad.ry);
    guest_printf("  SEL=%u STA=%u L3=%u R3=%u UP=%u RGT=%u DWN=%u LFT=%u L2=%u R2=%u "
                 "L1=%u R1=%u TRI=%u CIR=%u CRO=%u SQR=%u\n",
                 (b & SCE_CTRL_SELECT) != 0, (b & SCE_CTRL_START) != 0,
                 (b & SCE_CTRL_L3) != 0, (b & SCE_CTRL_R3) != 0,
                 (b & SCE_CTRL_UP) != 0, (b & SCE_CTRL_RIGHT) != 0, (b & SCE_CTRL_DOWN) != 0,
                 (b & SCE_CTRL_LEFT) != 0, (b & SCE_CTRL_L2) != 0, (b & SCE_CTRL_R2) != 0,
                 (b & SCE_CTRL_L1) != 0, (b & SCE_CTRL_R1) != 0, (b & SCE_CTRL_TRIANGLE) != 0,
                 (b & SCE_CTRL_CIRCLE) != 0, (b & SCE_CTRL_CROSS) != 0,
                 (b & SCE_CTRL_SQUARE) != 0);
}

/* ------------------------------------------------------------------------- */
/* main                                                                      */
/* ------------------------------------------------------------------------- */

void module_start(void)
{
    int res;

    guest_printf("\nv2n ctrl test: controller mapping probe\n");
    guest_printf("bit legend: SEL=0x0001 STA=0x0008 UP=0x0010 RGT=0x0020 DWN=0x0040 "
                 "LFT=0x0080\n");
    guest_printf("            L3=0x0002 R3=0x0004\n");
    guest_printf("            L2=0x0100 R2=0x0200 L1=0x0400 R1=0x0800 TRI=0x1000 "
                 "CIR=0x2000 CRO=0x4000 SQR=0x8000\n");
    guest_printf("sticks: 0..255, 128 = centered, 0 = up/left\n");
    guest_printf("starting display so the host SDL thread samples its input source too\n");

    g_fb.size = sizeof(g_fb);
    g_fb.base = g_framebuffer;
    g_fb.pitch = SCREEN_WIDTH;
    g_fb.pixelformat = 0; /* SCE_DISPLAY_PIXELFORMAT_A8B8G8R8 */
    g_fb.width = SCREEN_WIDTH;
    g_fb.height = SCREEN_HEIGHT;
    res = guest_setfb(&g_fb, 0);
    guest_printf("setfb returned %d\n", res);

    guest_printf("polling every %u ms; changes and a one-second heartbeat are logged\n",
                 SAMPLE_USEC / 1000u);
    guest_printf("hold SELECT+START for two seconds to exit\n");

    draw_ui();
    guest_setfb(&g_fb, 0);

    for (;;) {
        u32 axes;
        int changed = 0;

        res = guest_peek(0, &g_pad, 1);
        if (res < 0) {
            if (!g_peek_failed) {
                guest_printf("peek failed: %d\n", res);
                g_peek_failed = 1;
            }
            guest_delay(100000);
            continue;
        }
        g_peek_failed = 0;

        axes = axes_packed();
        if (g_pad.buttons != g_last_buttons || axes != g_last_axes) {
            g_last_buttons = g_pad.buttons;
            g_last_axes = axes;
            log_state("change");
            g_heartbeat = 0;
            changed = 1;
        } else if (++g_heartbeat >= HEARTBEAT) {
            g_heartbeat = 0;
            log_state("tick");
        }

        if ((g_pad.buttons & (SCE_CTRL_SELECT | SCE_CTRL_START)) ==
            (SCE_CTRL_SELECT | SCE_CTRL_START)) {
            if (++g_exit_hold >= EXIT_HOLD) {
                guest_printf("select+start held, exiting\n");
                guest_exit(0);
                g_exit_hold = 0;
            }
        } else {
            g_exit_hold = 0;
        }

        if (changed) {
            draw_ui();
            guest_setfb(&g_fb, 0);
        }

        guest_delay(SAMPLE_USEC);
    }
}
