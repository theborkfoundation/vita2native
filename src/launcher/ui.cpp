#define GL_SILENCE_DEPRECATION
#include "ui.h"
#include "nanovg.h"
#include "logo.h"
#include "state.h"
#ifdef __APPLE__
#include <OpenGL/gl3.h>
#define NANOVG_GL3
#else
#include <GLES2/gl2.h>
#define NANOVG_GLES2
#endif
#include <algorithm>
#include <cstdio>

#include "cover_0.h"
#include "cover_1.h"
#include "cover_2.h"
#include "cover_3.h"
#include "inter_bold.h"
#include "inter_font.h"
#include "nanovg_gl.h"

namespace launcher::ui
{
static NVGcontext *vg;
static int artwork[game_count] = {};
static int logo_image;


bool initialize(const std::filesystem::path &logo_path, std::string &warning)
{
#ifdef __APPLE__
    vg = nvgCreateGL3(NVG_ANTIALIAS | NVG_STENCIL_STROKES);
#else
    vg = nvgCreateGLES2(NVG_ANTIALIAS | NVG_STENCIL_STROKES);
#endif
    if (!vg)
        return false;
    if (nvgCreateFontMem(vg, "inter", inter_font, sizeof(inter_font), 0) < 0 ||
        nvgCreateFontMem(vg, "inter-bold", inter_bold, sizeof(inter_bold), 0) < 0) {
        shutdown();
        return false;
    }
    if (!logo_path.empty()) {
        try {
            logo_image = load_logo(vg, logo_path);
        } catch (const std::exception &error) {
            fprintf(stderr, "Could not load logo %s: %s\n", logo_path.string().c_str(), error.what());
        }
        if (!logo_image) {
            fprintf(stderr, "Could not load PNG or SVG logo: %s\n", logo_path.string().c_str());
            warning = "Could not load logo; using the built-in logo.";
        }
    }
    unsigned char *covers[] = { cover_0, cover_1, cover_2, cover_3 };
    int sizes[] = { sizeof(cover_0), sizeof(cover_1), sizeof(cover_2), sizeof(cover_3) };
    for (int i = 0; i < game_count; ++i) {
        artwork[i] = nvgCreateImageMem(vg, 0, covers[i], sizes[i]);
        if (!artwork[i])
            fprintf(stderr, "Could not load cover %d\n", i);
    }
    return true;
}
void shutdown()
{
    if (!vg)
        return;
    if (logo_image) {
        nvgDeleteImage(vg, logo_image);
        logo_image = 0;
    }
#ifdef __APPLE__
    nvgDeleteGL3(vg);
#else
    nvgDeleteGLES2(vg);
#endif
    vg = nullptr;
}
void begin_frame(int width)
{
    nvgBeginFrame(vg, screen_width, screen_height, static_cast<float>(width) / screen_width);
}
void end_frame()
{
    nvgEndFrame(vg);
}
static NVGcolor rgba(int r, int g, int b, int a = 255)
{
    return nvgRGBA(r, g, b, a);
}
static void rect(float x, float y, float w, float h, NVGcolor c, float radius = 0)
{
    nvgBeginPath(vg);
    nvgRoundedRect(vg, x, y, w, h, radius);
    nvgFillColor(vg, c);
    nvgFill(vg);
}
static void outline(float x, float y, float w, float h, float radius, NVGcolor c, float width = 1)
{
    nvgBeginPath(vg);
    nvgRoundedRect(vg, x, y, w, h, radius);
    nvgStrokeColor(vg, c);
    nvgStrokeWidth(vg, width);
    nvgStroke(vg);
}
static void gradient(float x, float y, float w, float h, NVGcolor a, NVGcolor b)
{
    nvgBeginPath(vg);
    nvgRect(vg, x, y, w, h);
    nvgFillPaint(vg, nvgLinearGradient(vg, x, y, x, y + h, a, b));
    nvgFill(vg);
}
static void label(float x, float y, const std::string &s, float size, NVGcolor c, bool bold = false,
                  float maxw = 590)
{
    nvgSave(vg);
    nvgIntersectScissor(vg, x, y - 3, maxw, size + 8);
    nvgFontFace(vg, bold ? "inter-bold" : "inter");
    nvgFontSize(vg, size);
    nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_TOP);
    nvgFillColor(vg, c);
    nvgText(vg, x, y, s.c_str(), nullptr);
    nvgRestore(vg);
}
static void centered(float x, float y, const char *s, float size, NVGcolor c, bool bold = false)
{
    nvgFontFace(vg, bold ? "inter-bold" : "inter");
    nvgFontSize(vg, size);
    nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
    nvgFillColor(vg, c);
    nvgText(vg, x, y, s, nullptr);
}
static void brand(float x, float y, float size)
{
    if (logo_image) {
        int width = 0, height = 0;
        nvgImageSize(vg, logo_image, &width, &height);
        if (width > 0 && height > 0) {
            float scale = std::min(size / width, size / height);
            float w = width * scale, h = height * scale;
            float left = x + (size - w) / 2, top = y + (size - h) / 2;
            nvgBeginPath(vg);
            nvgRect(vg, left, top, w, h);
            nvgFillPaint(vg, nvgImagePattern(vg, left, top, w, h, 0, logo_image, 1));
            nvgFill(vg);
            return;
        }
    }
    outline(x, y, size, size, 5, rgba(238, 246, 250, 220), 1.2);
    nvgBeginPath(vg);
    nvgMoveTo(vg, x + size * .20f, y + size * .27f);
    nvgLineTo(vg, x + size * .38f, y + size * .73f);
    nvgLineTo(vg, x + size * .50f, y + size * .45f);
    nvgLineTo(vg, x + size * .62f, y + size * .73f);
    nvgLineTo(vg, x + size * .80f, y + size * .27f);
    nvgStrokeWidth(vg, size * .07f);
    nvgStrokeColor(vg, rgba(255, 255, 255));
    nvgStroke(vg);
}
static void image_cover(int image, float x, float y, float w, float h, float radius,
                        float alpha = 1)
{
    if (!image) {
        rect(x, y, w, h, rgba(51, 68, 76), radius);
        return;
    }
    int iw = 0, ih = 0;
    nvgImageSize(vg, image, &iw, &ih);
    if (iw <= 0 || ih <= 0)
        return;
    float scale = std::max(w / iw, h / ih), dw = iw * scale, dh = ih * scale;
    nvgBeginPath(vg);
    nvgRoundedRect(vg, x, y, w, h, radius);
    nvgFillPaint(vg,
                 nvgImagePattern(vg, x + (w - dw) / 2, y + (h - dh) / 2, dw, dh, 0, image, alpha));
    nvgFill(vg);
}
static float ease_out(float t)
{
    t = std::clamp(t, 0.0f, 1.0f);
    return 1.0f - (1.0f - t) * (1.0f - t) * (1.0f - t);
}
static float entrance(const State &a, int delay = 0)
{
    return ease_out(((float)(SDL_GetTicks() - a.screen_start) - delay) / 360.0f);
}
// Inter's capital letters sit above the midpoint of its line box; this offset
// centers their visible height on the button circles, not the font's line box.
static constexpr float footer_center_y = 459.0f;
static constexpr float footer_label_y = footer_center_y - 4.25f;
static float footer_text_width(const char *text)
{
    nvgFontFace(vg, "inter-bold");
    nvgFontSize(vg, 10);
    nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_TOP);
    return nvgTextBounds(vg, 0, 0, text, nullptr, nullptr);
}
static void footer_key(float cx, char key, const char *meaning)
{
    constexpr float cy = footer_center_y;
    nvgBeginPath(vg);
    nvgCircle(vg, cx, cy, 11);
    nvgFillColor(vg, rgba(214, 232, 237));
    nvgFill(vg);
    // Draw both glyphs on the same centered, nine-pixel-high grid.
    nvgBeginPath(vg);
    if (key == 'A') {
        nvgMoveTo(vg, cx - 3.8f, cy + 4.5f);
        nvgLineTo(vg, cx, cy - 4.5f);
        nvgLineTo(vg, cx + 3.8f, cy + 4.5f);
        nvgMoveTo(vg, cx - 2.3f, cy + 1.1f);
        nvgLineTo(vg, cx + 2.3f, cy + 1.1f);
    } else {
        nvgMoveTo(vg, cx - 3.2f, cy - 4.5f);
        nvgLineTo(vg, cx - 3.2f, cy + 4.5f);
        nvgLineTo(vg, cx + 0.5f, cy + 4.5f);
        nvgBezierTo(vg, cx + 4.5f, cy + 4.5f, cx + 4.5f, cy, cx + 0.5f, cy);
        nvgLineTo(vg, cx - 3.2f, cy);
        nvgMoveTo(vg, cx - 3.2f, cy - 4.5f);
        nvgLineTo(vg, cx + 0.5f, cy - 4.5f);
        nvgBezierTo(vg, cx + 4.1f, cy - 4.5f, cx + 4.1f, cy, cx + 0.5f, cy);
    }
    nvgStrokeColor(vg, rgba(18, 35, 44));
    nvgStrokeWidth(vg, 1.5f);
    nvgLineCap(vg, NVG_ROUND);
    nvgLineJoin(vg, NVG_ROUND);
    nvgStroke(vg);
    label(cx + 19, footer_label_y, meaning, 10, rgba(191, 210, 217), true);
}
static void footer_dpad(float cx, float cy)
{
    nvgBeginPath(vg);
    nvgCircle(vg, cx, cy, 11);
    nvgFillColor(vg, rgba(214, 232, 237));
    nvgFill(vg);
    // Controller D-pad silhouette, with separate direction markers on each arm.
    nvgBeginPath(vg);
    nvgMoveTo(vg, cx - 2.5f, cy - 8);
    nvgLineTo(vg, cx + 2.5f, cy - 8);
    nvgLineTo(vg, cx + 2.5f, cy - 2.5f);
    nvgLineTo(vg, cx + 8, cy - 2.5f);
    nvgLineTo(vg, cx + 8, cy + 2.5f);
    nvgLineTo(vg, cx + 2.5f, cy + 2.5f);
    nvgLineTo(vg, cx + 2.5f, cy + 8);
    nvgLineTo(vg, cx - 2.5f, cy + 8);
    nvgLineTo(vg, cx - 2.5f, cy + 2.5f);
    nvgLineTo(vg, cx - 8, cy + 2.5f);
    nvgLineTo(vg, cx - 8, cy - 2.5f);
    nvgLineTo(vg, cx - 2.5f, cy - 2.5f);
    nvgClosePath(vg);
    nvgFillColor(vg, rgba(18, 35, 44));
    nvgFill(vg);
    nvgBeginPath(vg);
    nvgCircle(vg, cx, cy, 1.2f);
    nvgFillColor(vg, rgba(214, 232, 237));
    nvgFill(vg);
}
static void header(const State &a)
{
    brand(25, 17, 26);
    label(60, 19, "V2N", 16, rgba(244, 248, 250), true);
    label(60, 37, "LAUNCHER", 8, rgba(157, 177, 188), true);
    if (a.screen != Screen::Home) {
        const char *name = a.picker                       ? "CHOOSE FILE"
                           : a.screen == Screen::AllGames ? "ALL GAMES"
                                                          : "IMPORT GAME";
        nvgFontFace(vg, "inter-bold");
        nvgFontSize(vg, 11);
        nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_TOP);
        float width = nvgTextBounds(vg, 0, 0, name, nullptr, nullptr) + 30;
        rect(615 - width, 18, width, 27, rgba(135, 174, 185, 35), 13.5f);
        outline(615 - width, 18, width, 27, 13.5f, rgba(162, 197, 205, 75));
        centered(615 - width / 2, 31.5f, name, 11, rgba(229, 243, 245), true);
    }
}
static void footer(const State &a)
{
    rect(0, 438, 640, 42, rgba(12, 26, 35, 250));
    rect(0, 438, 640, 1, rgba(159, 190, 201, 40));
    footer_dpad(35, footer_center_y);
    label(54, footer_label_y, "NAVIGATE", 10, rgba(191, 210, 217), true);
    const char *back = a.picker || a.screen != Screen::Home ? "BACK" : "EXIT";
    const float back_cx = 615 - footer_text_width(back) - 19;
    const float select_cx = back_cx - 11 - 22 - footer_text_width("SELECT") - 19;
    footer_key(select_cx, 'A', "SELECT");
    footer_key(back_cx, 'B', back);
}
static void splash(Uint32 elapsed)
{
    float appear = ease_out(elapsed / 600.0f);
    rect(0, 0, 640, 480, rgba(10, 19, 27));
    gradient(0, 0, 640, 480, rgba(43, 64, 73), rgba(7, 13, 20));
    nvgSave(vg);
    nvgGlobalAlpha(vg, appear);
    nvgTranslate(vg, 0, 12 * (1 - appear));
    brand(284, 170, 72);
    nvgRestore(vg);
    rect(240, 282, 160, 3, rgba(255, 255, 255, 35), 1.5);
    rect(240, 282, 160 * ease_out(elapsed / static_cast<float>(splash_duration)), 3,
         rgba(229, 240, 242), 1.5);
}
static void cover_card(int id, float x, float active, bool focused, float entry)
{
    constexpr float width = 310, y = 107, h = 154;
    nvgSave(vg);
    nvgGlobalAlpha(vg, entry);
    nvgTranslate(vg, 0, 12 * (1 - entry));
    image_cover(artwork[id], x, y, width, h, 5);
    nvgSave(vg);
    nvgIntersectScissor(vg, x, y, width, h);
    gradient(x, y + h - 75, width, 75, rgba(8, 15, 21, 0), rgba(8, 15, 21, 220));
    nvgRestore(vg);
    label(x + 16, y + h - 32, game_names[id], 19, rgba(255, 255, 255), true, width - 32);
    NVGcolor edge = focused ? rgba(248, 252, 255, (int)(55 + 200 * active))
                            : rgba(255, 255, 255, (int)(45 + 55 * active));
    outline(x, y, width, h, 5, edge, 1 + active);
    nvgRestore(vg);
}
static void draw_games(const State &a)
{
    float heading = entrance(a);
    nvgSave(vg);
    nvgGlobalAlpha(vg, heading);
    nvgTranslate(vg, 0, 8 * (1 - heading));
    label(25, 72, "Recent games", 20, rgba(248, 251, 253), true);
    nvgRestore(vg);
    nvgSave(vg);
    nvgScissor(vg, 0, 103, 640, 165);
    float t = a.moving ? ease_out((SDL_GetTicks() - a.motion_start) / (float)State::motion_duration)
                       : 1.0f;
    int first;
    float first_weight, second_weight, x;
    constexpr float pitch = 320;
    if (!a.moving) {
        first = a.selected;
        first_weight = 1;
        second_weight = 0;
        x = 25;
    } else if (a.direction > 0) {
        first = a.previous;
        first_weight = 1 - t;
        second_weight = t;
        x = 25 - pitch * t;
    } else {
        first = a.selected;
        first_weight = t;
        second_weight = 1 - t;
        x = 25 - pitch * (1 - t);
    }
    for (int i = 0; i < game_count; i++) {
        float active = i == 0 ? first_weight : i == 1 ? second_weight : 0;
        cover_card((first + i) % game_count, x, active, a.home_focus == HomeFocus::RecentGames,
                   entrance(a, 65 + i * 50));
        x += pitch;
    }
    nvgRestore(vg);
    float fp = a.focus_start
                   ? ease_out((SDL_GetTicks() - a.focus_start) / static_cast<float>(focus_duration))
                   : 1.0f;
    for (int i = 0; i < 2; i++) {
        float show = entrance(a, 240 + i * 80);
        nvgSave(vg);
        nvgGlobalAlpha(vg, show);
        nvgTranslate(vg, 0, 12 * (1 - show));
        HomeFocus target = i == 0 ? HomeFocus::AllGames : HomeFocus::Import;
        float focused =
            (a.home_focus == target ? fp : 0) + (a.previous_home_focus == target ? 1 - fp : 0);
        float x = 25 + i * 301;
        rect(x, 321, 289, 92, rgba(32, 52, 64, 225), 7);
        outline(x, 321, 289, 92, 7,
                rgba(187 + (int)(52 * focused), 211 + (int)(37 * focused),
                     219 + (int)(30 * focused), 60 + (int)(195 * focused)),
                1 + focused);
        rect(x + 15, 347, 40, 40, rgba(204, 230, 234, 24), 7);
        if (i == 0) {
            for (int row = 0; row < 2; row++)
                for (int col = 0; col < 2; col++)
                    rect(x + 25 + col * 11, 357 + row * 11, 7, 7, rgba(213, 232, 235), 1);
        } else {
            nvgBeginPath(vg);
            nvgMoveTo(vg, x + 35, 357);
            nvgLineTo(vg, x + 35, 377);
            nvgMoveTo(vg, x + 25, 367);
            nvgLineTo(vg, x + 45, 367);
            nvgStrokeColor(vg, rgba(213, 232, 235));
            nvgStrokeWidth(vg, 2);
            nvgStroke(vg);
        }
        label(x + 68, 343, i == 0 ? "View all games" : "Import game", 15, rgba(245, 250, 251), true,
              190);
        label(x + 68, 369, i == 0 ? "Browse your library" : "Add a package and license", 10,
              rgba(164, 188, 196), false, 203);
        label(x + 258, 346, "→", 16, rgba(195, 218, 223), true, 24);
        nvgRestore(vg);
    }
    if (!a.message.empty()) {
        rect(25, 282, 430, 23, rgba(16, 27, 34, 245), 4);
        label(33, 288, a.message, 10, rgba(232, 239, 241), false, 410);
    }
}
static void draw_all_games(const State &a)
{
    float heading = entrance(a);
    nvgSave(vg);
    nvgGlobalAlpha(vg, heading);
    nvgTranslate(vg, 0, 8 * (1 - heading));
    label(25, 72, "All games", 20, rgba(248, 251, 253), true);
    label(525, 79, std::to_string(game_count) + " GAMES", 10, rgba(174, 192, 200), true);
    nvgRestore(vg);
    for (int i = 0; i < game_count; i++) {
        float show = entrance(a, 60 + i * 60);
        float x = 25 + (i % 2) * 301, y = 111 + (i / 2) * 150;
        nvgSave(vg);
        nvgGlobalAlpha(vg, show);
        nvgTranslate(vg, 0, 10 * (1 - show));
        image_cover(artwork[i], x, y, 289, 136, 5);
        nvgSave(vg);
        nvgIntersectScissor(vg, x, y, 289, 136);
        gradient(x, y + 65, 289, 71, rgba(8, 15, 21, 0), rgba(8, 15, 21, 225));
        nvgRestore(vg);
        label(x + 14, y + 104, game_names[i], 16, rgba(255, 255, 255), true, 260);
        outline(x, y, 289, 136, 5, a.all_focus == i ? rgba(248, 252, 255) : rgba(255, 255, 255, 60),
                a.all_focus == i ? 2 : 1);
        nvgRestore(vg);
    }
    if (!a.message.empty()) {
        rect(25, 411, 480, 21, rgba(16, 27, 34, 245), 4);
        label(33, 416, a.message, 10, rgba(232, 239, 241), false, 460);
    }
}
static void draw_import(const State &a)
{
    float heading = entrance(a);
    nvgSave(vg);
    nvgGlobalAlpha(vg, heading);
    nvgTranslate(vg, 0, 10 * (1 - heading));
    label(25, 72, "Import a game", 20, rgba(248, 251, 253), true);
    label(25, 99, "Select your package and its license to add a game.", 12, rgba(174, 193, 201));
    nvgRestore(vg);
    for (int i = 0; i < 2; i++) {
        float show = entrance(a, 80 + i * 90);
        nvgSave(vg);
        nvgGlobalAlpha(vg, show);
        nvgTranslate(vg, 0, 12 * (1 - show));
        float y = 135 + i * 76;
        rect(25, y, 590, 64, rgba(24, 40, 51, 225), 5);
        outline(25, y, 590, 64, 5,
                a.import_focus == (i == 0 ? ImportFocus::Package : ImportFocus::License)
                    ? rgba(241, 248, 250)
                    : rgba(119, 150, 162, 90),
                a.import_focus == (i == 0 ? ImportFocus::Package : ImportFocus::License) ? 2 : 1);
        rect(39, y + 14, 36, 36, rgba(255, 255, 255, 20), 4);
        centered(57, y + 32, i == 0 ? "P" : "L", 15, rgba(226, 238, 240), true);
        label(89, y + 11, i == 0 ? "Game package" : "License file", 13, rgba(244, 249, 251), true);
        std::string name = (i == 0 ? a.pkg : a.license).filename().string();
        label(89, y + 34,
              name.empty() ? (i == 0 ? "Choose a .pkg file" : "Choose a work.bin file") : name, 11,
              rgba(173, 194, 202), false, 350);
        rect(534, y + 17, 64, 30, rgba(255, 255, 255, 24), 4);
        centered(566, y + 32, "BROWSE", 10, rgba(240, 247, 249), true);
        nvgRestore(vg);
    }
    float details = entrance(a, 260);
    nvgSave(vg);
    nvgGlobalAlpha(vg, details);
    nvgTranslate(vg, 0, 10 * (1 - details));
    label(26, 297, "IMPORT LOCATION", 10, rgba(155, 179, 188), true);
    label(26, 315, a.config.games_path.string(), 11, rgba(224, 235, 237), false, 395);
    rect(453, 304, 162, 40,
         a.import_focus == ImportFocus::Start ? rgba(225, 237, 239) : rgba(92, 115, 124), 5);
    centered(534, 324, a.import_running() ? "IMPORTING..." : "IMPORT GAME", 11,
             a.import_focus == ImportFocus::Start ? rgba(14, 28, 35) : rgba(225, 236, 239), true);
    rect(25, 365, 590, 47, rgba(255, 255, 255, 12), 5);
    label(39, 373, a.message.empty() ? "Your files will be prepared for vita2native." : a.message,
          11, rgba(199, 218, 224), false, 560);
    if (a.message.empty())
        label(39, 391, "Your title is saved in the import location above.", 10, rgba(143, 168, 178));
    nvgRestore(vg);
}
static void draw_picker(const State &a)
{
    float heading = entrance(a);
    nvgSave(vg);
    nvgGlobalAlpha(vg, heading);
    nvgTranslate(vg, 0, 8 * (1 - heading));
    label(25, 72, a.pick_kind == PickerKind::Package ? "Choose a package" : "Choose work.bin", 20,
          rgba(248, 251, 253), true);
    label(25, 100, a.dir.string(), 11, rgba(174, 193, 201), false, 590);
    nvgRestore(vg);
    if (a.items.empty())
        label(36, 156, "No matching files or folders.", 12, rgba(168, 189, 197));
    for (int row = 0; row < picker_page_size && a.scroll + row < (int)a.items.size(); row++) {
        int idx = a.scroll + row;
        float show = entrance(a, 50 + row * 35);
        nvgSave(vg);
        nvgGlobalAlpha(vg, show);
        nvgTranslate(vg, 0, 8 * (1 - show));
        float y = 127 + row * 37;
        const auto &it = a.items[idx];
        if (idx == a.pick_index) {
            rect(25, y, 590, 33, rgba(255, 255, 255, 24), 4);
            outline(25, y, 590, 33, 4, rgba(239, 247, 250), 1.5);
        }
        label(38, y + 9, it.dir ? "DIR" : "FILE", 10, rgba(159, 188, 197), true);
        std::string name = it.path == a.dir.parent_path() ? ".." : it.path.filename().string();
        label(90, y + 8, name, 12, rgba(238, 247, 249), false, 505);
        nvgRestore(vg);
    }
}
void draw(const State &a)
{
    if (!a.config_error.empty()) {
        rect(0, 0, 640, 480, rgba(10, 19, 27));
        gradient(0, 0, 640, 480, rgba(43, 64, 73), rgba(7, 13, 20));
        label(32, 72, "Configuration error", 22, rgba(248, 251, 253), true);
        nvgFontFace(vg, "inter");
        nvgFontSize(vg, 14);
        nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_TOP);
        nvgFillColor(vg, rgba(220, 231, 235));
        nvgSave(vg);
        nvgIntersectScissor(vg, 32, 120, 576, 250);
        nvgTextBox(vg, 32, 120, 576, a.config_error.c_str(), nullptr);
        nvgRestore(vg);
        label(32, 404, "Fix launcher.toml and restart.", 12, rgba(174, 193, 201));
        label(32, 446, "A / B  EXIT", 11, rgba(229, 243, 245), true);
        return;
    }
    Uint32 elapsed = SDL_GetTicks() - a.start;
    if (elapsed < splash_duration) {
        splash(elapsed);
        return;
    }
    if (a.moving) {
        float t = ease_out((SDL_GetTicks() - a.motion_start) / (float)State::motion_duration);
        image_cover(artwork[a.previous], 0, 0, 640, 440, 0, .48f);
        image_cover(artwork[a.selected], 0, 0, 640, 440, 0, .48f * t);
    } else
        image_cover(artwork[a.selected], 0, 0, 640, 440, 0, .48f);
    rect(0, 0, 640, 440, rgba(12, 25, 35, 190));
    gradient(0, 0, 640, 440, rgba(30, 43, 52, 75), rgba(10, 22, 30, 240));
    header(a);
    if (a.picker)
        draw_picker(a);
    else if (a.screen == Screen::Home)
        draw_games(a);
    else if (a.screen == Screen::AllGames)
        draw_all_games(a);
    else
        draw_import(a);
    footer(a);
}

} // namespace launcher::ui
