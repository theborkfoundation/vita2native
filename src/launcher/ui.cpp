#include "ui.h"
#include "logo.h"
#include "state.h"

#include <lvgl.h>
#include <stb_image.h>
#include <stb_image_resize2.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <vector>

#include "cover_0.h"
#include "cover_1.h"
#include "cover_2.h"
#include "cover_3.h"
#include "inter_bold.h"
#include "inter_font.h"

namespace launcher::ui
{
namespace
{
float ui_scale = 1.0f;
int fb_w = screen_width;
int fb_h = screen_height;
int fb_stride = screen_width * 4;

lv_display_t *display = nullptr;
std::vector<uint8_t> framebuffer_data;
bool frame_flushed = false;

int32_t sc(float v)
{
    return static_cast<int32_t>(std::lround(v * ui_scale));
}

int32_t sc_border(float v)
{
    if (v <= 0.0f)
        return 0;
    return std::max<int32_t>(1, sc(v));
}

struct Fonts {
    lv_font_t *reg_10 = nullptr;
    lv_font_t *reg_11 = nullptr;
    lv_font_t *reg_12 = nullptr;
    lv_font_t *reg_14 = nullptr;
    lv_font_t *bold_8 = nullptr;
    lv_font_t *bold_10 = nullptr;
    lv_font_t *bold_11 = nullptr;
    lv_font_t *bold_13 = nullptr;
    lv_font_t *bold_15 = nullptr;
    lv_font_t *bold_16 = nullptr;
    lv_font_t *bold_19 = nullptr;
    lv_font_t *bold_20 = nullptr;
    lv_font_t *bold_22 = nullptr;
    lv_font_t *bold_24 = nullptr;
};
Fonts fonts;

struct CoverSet {
    ImageBuffer bg;
    ImageBuffer recent;
    ImageBuffer grid;
};
std::array<CoverSet, game_count> covers;

ImageBuffer custom_logo_header;
ImageBuffer custom_logo_splash;
ImageBuffer icon_dpad;
ImageBuffer icon_key_a;
ImageBuffer icon_key_b;

struct BrandWidgets {
    lv_obj_t *box = nullptr;
    lv_obj_t *img = nullptr;
    lv_obj_t *outline = nullptr;
    lv_obj_t *line = nullptr;
    std::array<lv_point_precise_t, 5> points{};
};

struct RecentCard {
    lv_obj_t *root = nullptr;
    lv_obj_t *img = nullptr;
    lv_obj_t *grad = nullptr;
    lv_obj_t *title = nullptr;
    lv_obj_t *border = nullptr;
};

struct ActionCard {
    lv_obj_t *root = nullptr;
    lv_obj_t *icon_box = nullptr;
    lv_obj_t *title = nullptr;
    lv_obj_t *subtitle = nullptr;
    lv_obj_t *arrow = nullptr;
    lv_obj_t *border = nullptr;
};

struct AllCard {
    lv_obj_t *root = nullptr;
    lv_obj_t *img = nullptr;
    lv_obj_t *grad = nullptr;
    lv_obj_t *title = nullptr;
    lv_obj_t *border = nullptr;
};

struct ImportRow {
    lv_obj_t *root = nullptr;
    lv_obj_t *icon_box = nullptr;
    lv_obj_t *icon_label = nullptr;
    lv_obj_t *title = nullptr;
    lv_obj_t *value = nullptr;
    lv_obj_t *browse_btn = nullptr;
    lv_obj_t *browse_label = nullptr;
    lv_obj_t *border = nullptr;
};

struct PickerRow {
    lv_obj_t *root = nullptr;
    lv_obj_t *kind = nullptr;
    lv_obj_t *name = nullptr;
    lv_obj_t *border = nullptr;
};

struct Widgets {
    lv_obj_t *root = nullptr;

    lv_obj_t *error_layer = nullptr;
    lv_obj_t *error_body = nullptr;

    lv_obj_t *splash_layer = nullptr;
    BrandWidgets splash_brand;
    lv_obj_t *splash_track = nullptr;
    lv_obj_t *splash_fill = nullptr;

    lv_obj_t *main_layer = nullptr;
    lv_obj_t *bg_prev = nullptr;
    lv_obj_t *bg_curr = nullptr;
    lv_obj_t *bg_tint = nullptr;
    lv_obj_t *bg_grad = nullptr;

    lv_obj_t *header_layer = nullptr;
    BrandWidgets header_brand;
    lv_obj_t *header_divider = nullptr;
    lv_obj_t *header_screen = nullptr;

    lv_obj_t *home_layer = nullptr;
    lv_obj_t *home_heading = nullptr;
    lv_obj_t *home_carousel = nullptr;
    std::array<RecentCard, game_count> recent_cards;
    lv_obj_t *home_toast = nullptr;
    lv_obj_t *home_toast_label = nullptr;
    std::array<ActionCard, 2> action_cards;

    lv_obj_t *all_layer = nullptr;
    lv_obj_t *all_heading = nullptr;
    lv_obj_t *all_count = nullptr;
    std::array<AllCard, game_count> all_cards;
    lv_obj_t *all_toast = nullptr;
    lv_obj_t *all_toast_label = nullptr;

    lv_obj_t *import_layer = nullptr;
    lv_obj_t *import_heading = nullptr;
    lv_obj_t *import_subtitle = nullptr;
    std::array<ImportRow, 2> import_rows;
    lv_obj_t *import_details = nullptr;
    lv_obj_t *import_btn = nullptr;
    lv_obj_t *import_btn_label = nullptr;
    lv_obj_t *import_status_primary = nullptr;
    lv_obj_t *import_status_secondary = nullptr;

    lv_obj_t *picker_layer = nullptr;
    lv_obj_t *picker_heading = nullptr;
    lv_obj_t *picker_dir = nullptr;
    lv_obj_t *picker_empty = nullptr;
    std::array<PickerRow, picker_page_size> picker_rows;

    lv_obj_t *footer_layer = nullptr;
    lv_obj_t *footer_select_icon = nullptr;
    lv_obj_t *footer_select_label = nullptr;
    lv_obj_t *footer_back_icon = nullptr;
    lv_obj_t *footer_back_label = nullptr;

    std::deque<lv_grad_dsc_t> gradients;
};
Widgets ui_objs;

enum class ViewMode { None, Error, Splash, Home, AllGames, Import, Picker };
ViewMode last_mode = ViewMode::None;
std::string last_config_error;
int last_bg_selected = -1;
int last_bg_previous = -1;
bool last_bg_moving = false;
Screen last_header_screen = Screen::Home;
bool last_header_picker = false;
std::string last_home_msg;
int last_all_focus = -1;
std::string last_all_msg;
ImportFocus last_import_focus = ImportFocus::Package;
std::string last_import_pkg;
std::string last_import_lic;
std::string last_import_msg;
bool last_import_running = false;
PickerKind last_pick_kind = PickerKind::Package;
std::string last_picker_dir;
int last_picker_scroll = -1;
int last_picker_index = -1;
size_t last_picker_count = SIZE_MAX;
std::string last_footer_back;

lv_color_t rgb(int r, int g, int b)
{
    return lv_color_make(static_cast<uint8_t>(r), static_cast<uint8_t>(g), static_cast<uint8_t>(b));
}

float ease_out(float t)
{
    t = std::clamp(t, 0.0f, 1.0f);
    return 1.0f - (1.0f - t) * (1.0f - t) * (1.0f - t);
}

float entrance(const State &a, Uint32 now, int delay = 0)
{
    return ease_out((static_cast<float>(now - a.screen_start) - delay) / 360.0f);
}

lv_opa_t opa_from_float(float alpha)
{
    return static_cast<lv_opa_t>(std::clamp(std::lround(alpha * 255.0f), 0L, 255L));
}

void remove_interaction_flags(lv_obj_t *obj)
{
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_CLICKABLE);
}

void init_clean_obj(lv_obj_t *obj, float x, float y, float w, float h)
{
    lv_obj_remove_style_all(obj);
    remove_interaction_flags(obj);
    lv_obj_set_pos(obj, sc(x), sc(y));
    lv_obj_set_size(obj, sc(w), sc(h));
}

lv_obj_t *make_box(lv_obj_t *parent, float x, float y, float w, float h, lv_color_t color,
                   lv_opa_t opa, float radius = 0)
{
    lv_obj_t *obj = lv_obj_create(parent);
    init_clean_obj(obj, x, y, w, h);
    lv_obj_set_style_bg_color(obj, color, 0);
    lv_obj_set_style_bg_opa(obj, opa, 0);
    lv_obj_set_style_radius(obj, sc(radius), 0);
    return obj;
}

void set_vertical_gradient(lv_obj_t *obj, lv_color_t top_c, lv_opa_t top_opa, lv_color_t bot_c,
                           lv_opa_t bot_opa)
{
    lv_grad_dsc_t &grad = ui_objs.gradients.emplace_back();
    grad = {};
    grad.dir = LV_GRAD_DIR_VER;
    grad.stops_count = 2;
    grad.stops[0].color = top_c;
    grad.stops[0].opa = top_opa;
    grad.stops[0].frac = 0;
    grad.stops[1].color = bot_c;
    grad.stops[1].opa = bot_opa;
    grad.stops[1].frac = 255;
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_grad(obj, &grad, 0);
}

lv_obj_t *make_gradient(lv_obj_t *parent, float x, float y, float w, float h, lv_color_t top_c,
                        lv_opa_t top_opa, lv_color_t bot_c, lv_opa_t bot_opa)
{
    lv_obj_t *obj = lv_obj_create(parent);
    init_clean_obj(obj, x, y, w, h);
    set_vertical_gradient(obj, top_c, top_opa, bot_c, bot_opa);
    return obj;
}

lv_obj_t *make_label(lv_obj_t *parent, float x, float y, const char *text, const lv_font_t *font,
                     lv_color_t color, float max_w = 0)
{
    lv_obj_t *lbl = lv_label_create(parent);
    lv_obj_remove_style_all(lbl);
    remove_interaction_flags(lbl);
    lv_obj_set_pos(lbl, sc(x), sc(y));
    lv_obj_set_style_text_font(lbl, font, 0);
    lv_obj_set_style_text_color(lbl, color, 0);
    if (max_w > 0) {
        lv_obj_set_width(lbl, sc(max_w));
        lv_label_set_long_mode(lbl, LV_LABEL_LONG_MODE_CLIP);
    } else {
        lv_obj_set_width(lbl, LV_SIZE_CONTENT);
    }
    lv_label_set_text(lbl, text);
    return lbl;
}

lv_obj_t *make_centered_label(lv_obj_t *parent, const char *text, const lv_font_t *font,
                              lv_color_t color)
{
    lv_obj_t *lbl = lv_label_create(parent);
    lv_obj_remove_style_all(lbl);
    remove_interaction_flags(lbl);
    lv_obj_set_style_text_font(lbl, font, 0);
    lv_obj_set_style_text_color(lbl, color, 0);
    lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(lbl, text);
    lv_obj_center(lbl);
    return lbl;
}

void init_argb8888_buffer(ImageBuffer &out, int width, int height, const uint8_t *rgba,
                          bool opaque)
{
    out.pixels.resize(static_cast<size_t>(width) * height * 4);
    for (int i = 0; i < width * height; ++i) {
        out.pixels[i * 4 + 0] = rgba[i * 4 + 2];
        out.pixels[i * 4 + 1] = rgba[i * 4 + 1];
        out.pixels[i * 4 + 2] = rgba[i * 4 + 0];
        out.pixels[i * 4 + 3] = opaque ? 255 : rgba[i * 4 + 3];
    }
    out.dsc = {};
    out.dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    out.dsc.header.cf = opaque ? LV_COLOR_FORMAT_XRGB8888 : LV_COLOR_FORMAT_ARGB8888;
    out.dsc.header.w = static_cast<uint32_t>(width);
    out.dsc.header.h = static_cast<uint32_t>(height);
    out.dsc.header.stride = static_cast<uint32_t>(width * 4);
    out.dsc.data_size = static_cast<uint32_t>(out.pixels.size());
    out.dsc.data = out.pixels.data();
}

bool resize_argb8888_contain(const ImageBuffer &src, int box_size, ImageBuffer &out)
{
    if (!src.valid())
        return false;
    const float sw = static_cast<float>(src.dsc.header.w);
    const float sh = static_cast<float>(src.dsc.header.h);
    const float scale = std::min(box_size / sw, box_size / sh);
    const int dst_w = std::max(1, static_cast<int>(std::round(sw * scale)));
    const int dst_h = std::max(1, static_cast<int>(std::round(sh * scale)));
    out.pixels.resize(static_cast<size_t>(dst_w) * dst_h * 4);
    if (!stbir_resize_uint8_linear(src.pixels.data(), static_cast<int>(src.dsc.header.w),
                                   static_cast<int>(src.dsc.header.h),
                                   static_cast<int>(src.dsc.header.stride), out.pixels.data(),
                                   dst_w, dst_h, dst_w * 4, STBIR_BGRA)) {
        out.reset();
        return false;
    }
    out.dsc = {};
    out.dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    out.dsc.header.cf = LV_COLOR_FORMAT_ARGB8888;
    out.dsc.header.w = static_cast<uint32_t>(dst_w);
    out.dsc.header.h = static_cast<uint32_t>(dst_h);
    out.dsc.header.stride = static_cast<uint32_t>(dst_w * 4);
    out.dsc.data_size = static_cast<uint32_t>(out.pixels.size());
    out.dsc.data = out.pixels.data();
    return true;
}

bool make_cover_variant(const uint8_t *rgba, int iw, int ih, int tw, int th, ImageBuffer &out)
{
    const float scale = std::max(static_cast<float>(tw) / iw, static_cast<float>(th) / ih);
    const int sw = std::max(tw, static_cast<int>(std::round(iw * scale)));
    const int sh = std::max(th, static_cast<int>(std::round(ih * scale)));
    std::vector<uint8_t> scaled(static_cast<size_t>(sw) * sh * 4);
    if (!stbir_resize_uint8_linear(rgba, iw, ih, iw * 4, scaled.data(), sw, sh, sw * 4,
                                   STBIR_RGBA))
        return false;
    const int ox = std::max(0, (sw - tw) / 2);
    const int oy = std::max(0, (sh - th) / 2);
    std::vector<uint8_t> cropped(static_cast<size_t>(tw) * th * 4);
    for (int y = 0; y < th; ++y) {
        std::memcpy(cropped.data() + static_cast<size_t>(y) * tw * 4,
                    scaled.data() + (static_cast<size_t>(oy + y) * sw + ox) * 4,
                    static_cast<size_t>(tw) * 4);
    }
    init_argb8888_buffer(out, tw, th, cropped.data(), true);
    return true;
}

float dist_segment(float px, float py, float ax, float ay, float bx, float by)
{
    const float vx = bx - ax, vy = by - ay;
    const float wx = px - ax, wy = py - ay;
    const float len2 = vx * vx + vy * vy;
    const float t = len2 > 0.0f ? std::clamp((wx * vx + wy * vy) / len2, 0.0f, 1.0f) : 0.0f;
    const float dx = px - (ax + t * vx), dy = py - (ay + t * vy);
    return std::sqrt(dx * dx + dy * dy);
}

float dist_cubic(float px, float py, float x0, float y0, float x1, float y1, float x2, float y2,
                 float x3, float y3)
{
    float best = 1e9f;
    float prev_x = x0, prev_y = y0;
    constexpr int steps = 16;
    for (int i = 1; i <= steps; ++i) {
        const float t = static_cast<float>(i) / steps;
        const float u = 1.0f - t;
        const float nx = u * u * u * x0 + 3 * u * u * t * x1 + 3 * u * t * t * x2 + t * t * t * x3;
        const float ny = u * u * u * y0 + 3 * u * u * t * y1 + 3 * u * t * t * y2 + t * t * t * y3;
        best = std::min(best, dist_segment(px, py, prev_x, prev_y, nx, ny));
        prev_x = nx;
        prev_y = ny;
    }
    return best;
}

// Rasterizes the 22x22 logical footer button icons at native display scale with 4x4 supersampling.
void build_footer_icon(ImageBuffer &out, char kind, int size)
{
    const float center = size / 2.0f;
    const float scale = size / 22.0f;
    constexpr int samples = 4;
    std::vector<uint8_t> rgba(static_cast<size_t>(size) * size * 4);
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            int circle_hits = 0;
            int glyph_hits = 0;
            for (int sy = 0; sy < samples; ++sy) {
                for (int sx = 0; sx < samples; ++sx) {
                    const float px = x + (sx + 0.5f) / samples;
                    const float py = y + (sy + 0.5f) / samples;
                    const float dx = (px - center) / scale;
                    const float dy = (py - center) / scale;
                    if (dx * dx + dy * dy <= 11.0f * 11.0f) {
                        ++circle_hits;
                        bool in_glyph = false;
                        if (kind == 'D') {
                            const float ax = std::fabs(dx), ay = std::fabs(dy);
                            const bool in_cross =
                                (ax <= 2.5f && ay <= 8.0f) || (ay <= 2.5f && ax <= 8.0f);
                            const bool in_hole = (dx * dx + dy * dy) <= (1.2f * 1.2f);
                            in_glyph = in_cross && !in_hole;
                        } else if (kind == 'A') {
                            const float d1 = dist_segment(dx, dy, -3.8f, 4.5f, 0.0f, -4.5f);
                            const float d2 = dist_segment(dx, dy, 0.0f, -4.5f, 3.8f, 4.5f);
                            const float d3 = dist_segment(dx, dy, -2.3f, 1.1f, 2.3f, 1.1f);
                            in_glyph = std::min({ d1, d2, d3 }) <= 0.75f;
                        } else {
                            const float d1 = dist_segment(dx, dy, -3.2f, -4.5f, -3.2f, 4.5f);
                            const float d2 = dist_segment(dx, dy, -3.2f, 4.5f, 0.5f, 4.5f);
                            const float d3 =
                                dist_cubic(dx, dy, 0.5f, 4.5f, 4.5f, 4.5f, 4.5f, 0.0f, 0.5f, 0.0f);
                            const float d4 = dist_segment(dx, dy, 0.5f, 0.0f, -3.2f, 0.0f);
                            const float d5 = dist_segment(dx, dy, -3.2f, -4.5f, 0.5f, -4.5f);
                            const float d6 = dist_cubic(dx, dy, 0.5f, -4.5f, 4.1f, -4.5f, 4.1f, 0.0f,
                                                        0.5f, 0.0f);
                            in_glyph = std::min({ d1, d2, d3, d4, d5, d6 }) <= 0.75f;
                        }
                        if (in_glyph)
                            ++glyph_hits;
                    }
                }
            }
            const float total = samples * samples;
            const float circle_cov = circle_hits / total;
            const float glyph_cov = circle_hits > 0 ? static_cast<float>(glyph_hits) / circle_hits : 0.0f;
            const float r = 214.0f * (1.0f - glyph_cov) + 18.0f * glyph_cov;
            const float g = 232.0f * (1.0f - glyph_cov) + 35.0f * glyph_cov;
            const float b = 237.0f * (1.0f - glyph_cov) + 44.0f * glyph_cov;
            const int idx = (y * size + x) * 4;
            rgba[idx + 0] = static_cast<uint8_t>(std::lround(r));
            rgba[idx + 1] = static_cast<uint8_t>(std::lround(g));
            rgba[idx + 2] = static_cast<uint8_t>(std::lround(b));
            rgba[idx + 3] = static_cast<uint8_t>(std::lround(circle_cov * 255.0f));
        }
    }
    init_argb8888_buffer(out, size, size, rgba.data(), false);
}

void build_brand(BrandWidgets &b, lv_obj_t *parent, float x, float y, float size,
                 const ImageBuffer &custom_logo)
{
    b.box = lv_obj_create(parent);
    init_clean_obj(b.box, x, y, size, size);
    if (custom_logo.valid()) {
        b.img = lv_image_create(b.box);
        lv_obj_remove_style_all(b.img);
        remove_interaction_flags(b.img);
        lv_image_set_src(b.img, &custom_logo.dsc);
        lv_obj_center(b.img);
        return;
    }
    b.outline = make_box(b.box, 0, 0, size, size, rgb(0, 0, 0), LV_OPA_TRANSP, 5);
    lv_obj_set_style_border_color(b.outline, rgb(238, 246, 250), 0);
    lv_obj_set_style_border_opa(b.outline, 220, 0);
    lv_obj_set_style_border_width(b.outline, sc_border(1.2f), 0);

    const float px_size = size * ui_scale;
    b.points = { {
        { static_cast<lv_value_precise_t>(std::lround(px_size * 0.20f)),
          static_cast<lv_value_precise_t>(std::lround(px_size * 0.27f)) },
        { static_cast<lv_value_precise_t>(std::lround(px_size * 0.38f)),
          static_cast<lv_value_precise_t>(std::lround(px_size * 0.73f)) },
        { static_cast<lv_value_precise_t>(std::lround(px_size * 0.50f)),
          static_cast<lv_value_precise_t>(std::lround(px_size * 0.45f)) },
        { static_cast<lv_value_precise_t>(std::lround(px_size * 0.62f)),
          static_cast<lv_value_precise_t>(std::lround(px_size * 0.73f)) },
        { static_cast<lv_value_precise_t>(std::lround(px_size * 0.80f)),
          static_cast<lv_value_precise_t>(std::lround(px_size * 0.27f)) },
    } };
    b.line = lv_line_create(b.box);
    init_clean_obj(b.line, 0, 0, size, size);
    lv_line_set_points(b.line, b.points.data(), static_cast<uint32_t>(b.points.size()));
    lv_obj_set_style_line_color(b.line, rgb(255, 255, 255), 0);
    lv_obj_set_style_line_width(b.line, std::max<int32_t>(1, std::lround(px_size * 0.07f)), 0);
    lv_obj_set_style_line_rounded(b.line, false, 0);
}

void build_cover_card_common(lv_obj_t *parent, lv_obj_t *&root, lv_obj_t *&img, lv_obj_t *&grad,
                             lv_obj_t *&title, lv_obj_t *&border, float x, float y, float w,
                             float h, const ImageBuffer &cover_img, const char *name,
                             const lv_font_t *font, float label_x, float label_y,
                             float label_max_w, lv_opa_t grad_end_opa)
{
    root = make_box(parent, x, y, w, h, rgb(51, 68, 76), LV_OPA_COVER, 5);
    lv_obj_set_style_clip_corner(root, true, 0);

    img = lv_image_create(root);
    init_clean_obj(img, 0, 0, w, h);
    if (cover_img.valid())
        lv_image_set_src(img, &cover_img.dsc);

    if (h == 136) {
        grad = make_gradient(root, 0, 65, w, 71, rgb(8, 15, 21), 0, rgb(8, 15, 21), grad_end_opa);
    } else {
        grad = make_gradient(root, 0, h - 75, w, 75, rgb(8, 15, 21), 0, rgb(8, 15, 21),
                             grad_end_opa);
    }

    title = make_label(root, label_x, label_y, name, font, rgb(255, 255, 255), label_max_w);

    border = make_box(root, 0, 0, w, h, rgb(0, 0, 0), LV_OPA_TRANSP, 5);
    lv_obj_set_style_border_color(border, rgb(255, 255, 255), 0);
    lv_obj_set_style_border_opa(border, 60, 0);
    lv_obj_set_style_border_width(border, sc_border(1), 0);
}

void create_ui_tree()
{
    ui_objs.root = lv_screen_active();
    lv_obj_remove_style_all(ui_objs.root);
    lv_obj_clear_flag(ui_objs.root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(ui_objs.root, rgb(10, 19, 27), 0);
    lv_obj_set_style_bg_opa(ui_objs.root, LV_OPA_COVER, 0);

    // Config error screen
    ui_objs.error_layer = make_gradient(ui_objs.root, 0, 0, screen_width, screen_height,
                                        rgb(43, 64, 73), 255, rgb(7, 13, 20), 255);
    make_label(ui_objs.error_layer, 32, 72, "Configuration error", fonts.bold_22,
               rgb(248, 251, 253));
    ui_objs.error_body = make_label(ui_objs.error_layer, 32, 120, "", fonts.reg_14,
                                    rgb(220, 231, 235), 576);
    lv_label_set_long_mode(ui_objs.error_body, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_height(ui_objs.error_body, sc(250));
    make_label(ui_objs.error_layer, 32, 404, "Fix launcher.toml and restart.", fonts.reg_12,
               rgb(174, 193, 201));
    make_label(ui_objs.error_layer, 32, 446, "A / B  EXIT", fonts.bold_11, rgb(229, 243, 245));
    lv_obj_add_flag(ui_objs.error_layer, LV_OBJ_FLAG_HIDDEN);

    // Splash screen
    ui_objs.splash_layer = make_gradient(ui_objs.root, 0, 0, screen_width, screen_height,
                                         rgb(43, 64, 73), 255, rgb(7, 13, 20), 255);
    build_brand(ui_objs.splash_brand, ui_objs.splash_layer, 284, 170, 72, custom_logo_splash);
    ui_objs.splash_track =
        make_box(ui_objs.splash_layer, 240, 282, 160, 3, rgb(255, 255, 255), 35, 2);
    ui_objs.splash_fill =
        make_box(ui_objs.splash_layer, 240, 282, 0, 3, rgb(229, 240, 242), 255, 2);
    lv_obj_add_flag(ui_objs.splash_layer, LV_OBJ_FLAG_HIDDEN);

    // Main UI container
    ui_objs.main_layer = lv_obj_create(ui_objs.root);
    init_clean_obj(ui_objs.main_layer, 0, 0, screen_width, screen_height);
    lv_obj_add_flag(ui_objs.main_layer, LV_OBJ_FLAG_HIDDEN);

    // Background cover + crossfade + dark tint + vertical gradient
    ui_objs.bg_prev = lv_image_create(ui_objs.main_layer);
    init_clean_obj(ui_objs.bg_prev, 0, 0, 640, 440);
    lv_obj_set_style_image_opa(ui_objs.bg_prev, 122, 0);

    ui_objs.bg_curr = lv_image_create(ui_objs.main_layer);
    init_clean_obj(ui_objs.bg_curr, 0, 0, 640, 440);
    lv_obj_set_style_image_opa(ui_objs.bg_curr, 0, 0);
    lv_obj_add_flag(ui_objs.bg_curr, LV_OBJ_FLAG_HIDDEN);

    ui_objs.bg_tint = make_box(ui_objs.main_layer, 0, 0, 640, 440, rgb(12, 25, 35), 190);
    ui_objs.bg_grad = make_gradient(ui_objs.main_layer, 0, 0, 640, 440, rgb(30, 43, 52), 75,
                                    rgb(10, 22, 30), 240);

    // Header
    ui_objs.header_layer = lv_obj_create(ui_objs.main_layer);
    init_clean_obj(ui_objs.header_layer, 0, 0, 640, 64);
    build_brand(ui_objs.header_brand, ui_objs.header_layer, 25, 17, 26, custom_logo_header);
    make_label(ui_objs.header_layer, 60, 19, "V2N", fonts.bold_16, rgb(244, 248, 250));
    make_label(ui_objs.header_layer, 60, 37, "LAUNCHER", fonts.bold_8, rgb(157, 177, 188));

    ui_objs.header_divider =
        make_box(ui_objs.header_layer, 120, 18, 1, 24, rgb(159, 190, 201), 70);
    ui_objs.header_screen =
        make_label(ui_objs.header_layer, 134, 15, "", fonts.bold_24, rgb(248, 251, 253));
    lv_obj_add_flag(ui_objs.header_divider, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(ui_objs.header_screen, LV_OBJ_FLAG_HIDDEN);

    // Home screen
    ui_objs.home_layer = lv_obj_create(ui_objs.main_layer);
    init_clean_obj(ui_objs.home_layer, 0, 0, 640, 438);
    ui_objs.home_heading =
        make_label(ui_objs.home_layer, 25, 72, "Recent games", fonts.bold_20, rgb(248, 251, 253));

    ui_objs.home_carousel = lv_obj_create(ui_objs.home_layer);
    init_clean_obj(ui_objs.home_carousel, 0, 103, 640, 165);
    for (int i = 0; i < game_count; ++i) {
        auto &c = ui_objs.recent_cards[i];
        build_cover_card_common(ui_objs.home_carousel, c.root, c.img, c.grad, c.title, c.border,
                                25 + i * 320, 4, 310, 154, covers[i].recent, game_names[i],
                                fonts.bold_19, 16, 154 - 32, 310 - 32, 220);
    }

    ui_objs.home_toast = make_box(ui_objs.home_layer, 25, 282, 430, 23, rgb(16, 27, 34), 245, 4);
    ui_objs.home_toast_label =
        make_label(ui_objs.home_toast, 8, 6, "", fonts.reg_10, rgb(232, 239, 241), 410);
    lv_obj_add_flag(ui_objs.home_toast, LV_OBJ_FLAG_HIDDEN);

    for (int i = 0; i < 2; ++i) {
        auto &card = ui_objs.action_cards[i];
        const float x = 25 + i * 301;
        card.root = make_box(ui_objs.home_layer, x, 321, 289, 92, rgb(32, 52, 64), 225, 7);
        card.icon_box = make_box(card.root, 15, 26, 40, 40, rgb(204, 230, 234), 24, 7);
        if (i == 0) {
            for (int row = 0; row < 2; ++row)
                for (int col = 0; col < 2; ++col)
                    make_box(card.icon_box, 10 + col * 11, 10 + row * 11, 7, 7,
                             rgb(213, 232, 235), 255, 1);
        } else {
            make_box(card.icon_box, 19, 10, 2, 20, rgb(213, 232, 235), 255);
            make_box(card.icon_box, 10, 19, 20, 2, rgb(213, 232, 235), 255);
        }
        card.title = make_label(card.root, 68, 22, i == 0 ? "View all games" : "Import game",
                                fonts.bold_15, rgb(245, 250, 251), 190);
        card.subtitle =
            make_label(card.root, 68, 48, i == 0 ? "Browse your library" : "Add a package and license",
                       fonts.reg_10, rgb(164, 188, 196), 203);
        card.arrow = make_label(card.root, 258, 25, "→", fonts.bold_16, rgb(195, 218, 223), 24);
        card.border = make_box(card.root, 0, 0, 289, 92, rgb(0, 0, 0), LV_OPA_TRANSP, 7);
        lv_obj_set_style_border_width(card.border, sc_border(1), 0);
    }
    lv_obj_add_flag(ui_objs.home_layer, LV_OBJ_FLAG_HIDDEN);

    // All games screen
    ui_objs.all_layer = lv_obj_create(ui_objs.main_layer);
    init_clean_obj(ui_objs.all_layer, 0, 0, 640, 438);
    ui_objs.all_heading =
        make_label(ui_objs.all_layer, 25, 72, "All games", fonts.bold_20, rgb(248, 251, 253));
    const std::string count_text = std::to_string(game_count) + " GAMES";
    ui_objs.all_count =
        make_label(ui_objs.all_layer, 0, 79, count_text.c_str(), fonts.bold_10, rgb(174, 192, 200));
    lv_obj_align(ui_objs.all_count, LV_ALIGN_TOP_RIGHT, -sc(25), sc(79));
    for (int i = 0; i < game_count; ++i) {
        auto &c = ui_objs.all_cards[i];
        build_cover_card_common(ui_objs.all_layer, c.root, c.img, c.grad, c.title, c.border,
                                25 + (i % 2) * 301, 111 + (i / 2) * 150, 289, 136, covers[i].grid,
                                game_names[i], fonts.bold_16, 14, 104, 260, 225);
    }
    ui_objs.all_toast = make_box(ui_objs.all_layer, 25, 411, 480, 21, rgb(16, 27, 34), 245, 4);
    ui_objs.all_toast_label =
        make_label(ui_objs.all_toast, 8, 5, "", fonts.reg_10, rgb(232, 239, 241), 460);
    lv_obj_add_flag(ui_objs.all_toast, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(ui_objs.all_layer, LV_OBJ_FLAG_HIDDEN);

    // Import screen
    ui_objs.import_layer = lv_obj_create(ui_objs.main_layer);
    init_clean_obj(ui_objs.import_layer, 0, 0, 640, 438);
    ui_objs.import_heading =
        make_label(ui_objs.import_layer, 25, 72, "Import a game", fonts.bold_20, rgb(248, 251, 253));
    ui_objs.import_subtitle =
        make_label(ui_objs.import_layer, 25, 99,
                   "Select your package and its license to add a game.", fonts.reg_12,
                   rgb(174, 193, 201));
    for (int i = 0; i < 2; ++i) {
        auto &row = ui_objs.import_rows[i];
        const float y = 135 + i * 76;
        row.root = make_box(ui_objs.import_layer, 25, y, 590, 64, rgb(24, 40, 51), 225, 5);
        row.icon_box = make_box(row.root, 14, 14, 36, 36, rgb(255, 255, 255), 20, 4);
        row.icon_label =
            make_centered_label(row.icon_box, i == 0 ? "P" : "L", fonts.bold_15, rgb(226, 238, 240));
        row.title = make_label(row.root, 64, 11, i == 0 ? "Game package" : "License file",
                               fonts.bold_13, rgb(244, 249, 251));
        row.value = make_label(row.root, 64, 34,
                               i == 0 ? "Choose a .pkg file" : "Choose a work.bin file",
                               fonts.reg_11, rgb(173, 194, 202), 350);
        row.browse_btn = make_box(row.root, 509, 17, 64, 30, rgb(255, 255, 255), 24, 4);
        row.browse_label =
            make_centered_label(row.browse_btn, "BROWSE", fonts.bold_10, rgb(240, 247, 249));
        row.border = make_box(row.root, 0, 0, 590, 64, rgb(0, 0, 0), LV_OPA_TRANSP, 5);
        lv_obj_set_style_border_width(row.border, sc_border(1), 0);
    }
    ui_objs.import_details = lv_obj_create(ui_objs.import_layer);
    init_clean_obj(ui_objs.import_details, 0, 297, 640, 140);
    ui_objs.import_btn =
        make_box(ui_objs.import_details, 25, 0, 590, 42, rgb(92, 115, 124), 255, 5);
    ui_objs.import_btn_label =
        make_centered_label(ui_objs.import_btn, "IMPORT GAME", fonts.bold_11, rgb(225, 236, 239));
    lv_obj_t *status_box =
        make_box(ui_objs.import_details, 25, 62, 590, 47, rgb(255, 255, 255), 12, 5);
    ui_objs.import_status_primary =
        make_label(status_box, 14, 8, "", fonts.reg_11, rgb(199, 218, 224), 560);
    ui_objs.import_status_secondary =
        make_label(status_box, 14, 26, "Your title will be added to your games library.",
                   fonts.reg_10, rgb(143, 168, 178));
    lv_obj_add_flag(ui_objs.import_layer, LV_OBJ_FLAG_HIDDEN);

    // Picker screen
    ui_objs.picker_layer = lv_obj_create(ui_objs.main_layer);
    init_clean_obj(ui_objs.picker_layer, 0, 0, 640, 438);
    ui_objs.picker_heading =
        make_label(ui_objs.picker_layer, 25, 72, "Choose a package", fonts.bold_20,
                   rgb(248, 251, 253));
    ui_objs.picker_dir =
        make_label(ui_objs.picker_layer, 25, 100, "", fonts.reg_11, rgb(174, 193, 201), 590);
    ui_objs.picker_empty =
        make_label(ui_objs.picker_layer, 36, 156, "No matching files or folders.", fonts.reg_12,
                   rgb(168, 189, 197));
    lv_obj_add_flag(ui_objs.picker_empty, LV_OBJ_FLAG_HIDDEN);
    for (int row = 0; row < picker_page_size; ++row) {
        auto &r = ui_objs.picker_rows[row];
        const float y = 127 + row * 37;
        r.root = make_box(ui_objs.picker_layer, 25, y, 590, 33, rgb(255, 255, 255), 0, 4);
        r.kind = make_label(r.root, 13, 9, "DIR", fonts.bold_10, rgb(159, 188, 197));
        r.name = make_label(r.root, 65, 8, "", fonts.reg_12, rgb(238, 247, 249), 505);
        r.border = make_box(r.root, 0, 0, 590, 33, rgb(0, 0, 0), LV_OPA_TRANSP, 4);
        lv_obj_set_style_border_color(r.border, rgb(239, 247, 250), 0);
        lv_obj_set_style_border_width(r.border, 0, 0);
        lv_obj_add_flag(r.root, LV_OBJ_FLAG_HIDDEN);
    }
    lv_obj_add_flag(ui_objs.picker_layer, LV_OBJ_FLAG_HIDDEN);

    // Footer
    ui_objs.footer_layer = make_box(ui_objs.main_layer, 0, 438, 640, 42, rgb(12, 26, 35), 250);
    make_box(ui_objs.footer_layer, 0, 0, 640, 1, rgb(159, 190, 201), 40);

    lv_obj_t *dpad = lv_image_create(ui_objs.footer_layer);
    init_clean_obj(dpad, 24, 10, 22, 22);
    lv_image_set_src(dpad, &icon_dpad.dsc);
    make_label(ui_objs.footer_layer, 54, 17, "NAVIGATE", fonts.bold_10, rgb(191, 210, 217));

    ui_objs.footer_select_icon = lv_image_create(ui_objs.footer_layer);
    init_clean_obj(ui_objs.footer_select_icon, 0, 10, 22, 22);
    lv_image_set_src(ui_objs.footer_select_icon, &icon_key_a.dsc);
    ui_objs.footer_select_label =
        make_label(ui_objs.footer_layer, 0, 17, "SELECT", fonts.bold_10, rgb(191, 210, 217));

    ui_objs.footer_back_icon = lv_image_create(ui_objs.footer_layer);
    init_clean_obj(ui_objs.footer_back_icon, 0, 10, 22, 22);
    lv_image_set_src(ui_objs.footer_back_icon, &icon_key_b.dsc);
    ui_objs.footer_back_label =
        make_label(ui_objs.footer_layer, 0, 17, "EXIT", fonts.bold_10, rgb(191, 210, 217));
}

void set_visible(lv_obj_t *obj, bool show)
{
    if (show)
        lv_obj_clear_flag(obj, LV_OBJ_FLAG_HIDDEN);
    else
        lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
}

void set_opa_if_changed(lv_obj_t *obj, lv_opa_t opa)
{
    if (lv_obj_get_style_opa(obj, LV_PART_MAIN) != opa)
        lv_obj_set_style_opa(obj, opa, 0);
}

void set_pos_if_changed(lv_obj_t *obj, int32_t x, int32_t y)
{
    if (lv_obj_get_style_x(obj, LV_PART_MAIN) != x || lv_obj_get_style_y(obj, LV_PART_MAIN) != y)
        lv_obj_set_pos(obj, x, y);
}

void update_background(const State &a, Uint32 now)
{
    if (a.moving) {
        const float t =
            ease_out(static_cast<float>(now - a.motion_start) / static_cast<float>(State::motion_duration));
        if (last_bg_previous != a.previous || !last_bg_moving) {
            lv_image_set_src(ui_objs.bg_prev, &covers[a.previous].bg.dsc);
            last_bg_previous = a.previous;
        }
        if (last_bg_selected != a.selected || !last_bg_moving) {
            lv_image_set_src(ui_objs.bg_curr, &covers[a.selected].bg.dsc);
            last_bg_selected = a.selected;
        }
        set_visible(ui_objs.bg_curr, true);
        lv_obj_set_style_image_opa(ui_objs.bg_curr, opa_from_float(0.48f * t), 0);
        last_bg_moving = true;
    } else {
        if (last_bg_selected != a.selected || last_bg_moving) {
            lv_image_set_src(ui_objs.bg_prev, &covers[a.selected].bg.dsc);
            set_visible(ui_objs.bg_curr, false);
            last_bg_selected = a.selected;
            last_bg_previous = a.selected;
            last_bg_moving = false;
        }
    }
}

void update_header_and_footer(const State &a)
{
    if (last_header_screen != a.screen || last_header_picker != a.picker ||
        last_mode == ViewMode::None) {
        const bool show_screen = (a.screen != Screen::Home || a.picker);
        set_visible(ui_objs.header_divider, show_screen);
        set_visible(ui_objs.header_screen, show_screen);
        if (show_screen) {
            const char *name = a.picker                       ? "Choose File"
                               : a.screen == Screen::AllGames ? "All Games"
                                                              : "Import Game";
            lv_label_set_text(ui_objs.header_screen, name);
        }
        last_header_screen = a.screen;
        last_header_picker = a.picker;
    }

    const char *back = a.picker || a.screen != Screen::Home ? "BACK" : "EXIT";
    if (last_footer_back != back) {
        lv_point_t back_sz{}, select_sz{};
        lv_text_get_size(&back_sz, back, fonts.bold_10, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
        lv_text_get_size(&select_sz, "SELECT", fonts.bold_10, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
        const int32_t back_cx = sc(615) - back_sz.x - sc(19);
        const int32_t select_cx = back_cx - sc(11) - sc(22) - select_sz.x - sc(19);

        lv_obj_set_pos(ui_objs.footer_select_icon, select_cx - sc(11), sc(10));
        lv_obj_set_pos(ui_objs.footer_select_label, select_cx + sc(19), sc(17));
        lv_obj_set_pos(ui_objs.footer_back_icon, back_cx - sc(11), sc(10));
        lv_obj_set_pos(ui_objs.footer_back_label, back_cx + sc(19), sc(17));
        lv_label_set_text(ui_objs.footer_back_label, back);
        last_footer_back = back;
    }
}

void sync_home(const State &a, Uint32 now)
{
    const float heading = entrance(a, now);
    set_opa_if_changed(ui_objs.home_heading, opa_from_float(heading));
    set_pos_if_changed(ui_objs.home_heading, sc(25), sc(72.0f + 8.0f * (1.0f - heading)));

    const float t =
        a.moving ? ease_out(static_cast<float>(now - a.motion_start) / static_cast<float>(State::motion_duration))
                 : 1.0f;
    constexpr float pitch = 320.0f;
    // Always keep the left neighbor rendered at x = 25 - pitch so cards leaving left never vanish
    // at the end of a slide and cards entering from the left never pop in at the start.
    int base = a.selected;
    float base_x = 25.0f;
    int active_slot_0 = 0;
    float w0 = 1.0f, w1 = 0.0f;
    if (a.moving) {
        if (a.direction > 0) {
            base = a.previous;
            base_x = 25.0f - pitch * t;
            active_slot_0 = 0;
            w0 = 1.0f - t;
            w1 = t;
        } else {
            base = a.selected;
            base_x = 25.0f - pitch * (1.0f - t);
            active_slot_0 = 0;
            w0 = t;
            w1 = 1.0f - t;
        }
    }

    const bool recent_focused = (a.home_focus == HomeFocus::RecentGames);
    for (int slot = -1; slot < game_count - 1; ++slot) {
        const int id = (base + slot + game_count) % game_count;
        const float active = (slot == active_slot_0) ? w0 : (slot == active_slot_0 + 1) ? w1 : 0.0f;
        const float entry = entrance(a, now, 65 + std::max(0, slot) * 50);
        auto &card = ui_objs.recent_cards[id];
        set_pos_if_changed(card.root, sc(base_x + slot * pitch), sc(4.0f + 12.0f * (1.0f - entry)));
        set_opa_if_changed(card.root, opa_from_float(entry));

        const lv_color_t edge_c = recent_focused ? rgb(248, 252, 255) : rgb(255, 255, 255);
        const lv_opa_t edge_opa = static_cast<lv_opa_t>(
            recent_focused ? std::lround(55.0f + 200.0f * active) : std::lround(45.0f + 55.0f * active));
        const int32_t edge_w = sc_border(1.0f + active);
        if (lv_obj_get_style_border_opa(card.border, LV_PART_MAIN) != edge_opa ||
            lv_obj_get_style_border_width(card.border, LV_PART_MAIN) != edge_w ||
            lv_color_to_u32(lv_obj_get_style_border_color(card.border, LV_PART_MAIN)) !=
                lv_color_to_u32(edge_c)) {
            lv_obj_set_style_border_color(card.border, edge_c, 0);
            lv_obj_set_style_border_opa(card.border, edge_opa, 0);
            lv_obj_set_style_border_width(card.border, edge_w, 0);
        }
    }

    const float fp =
        a.focus_start ? ease_out(static_cast<float>(now - a.focus_start) / static_cast<float>(focus_duration))
                      : 1.0f;
    for (int i = 0; i < 2; ++i) {
        const float show = entrance(a, now, 240 + i * 80);
        const HomeFocus target = i == 0 ? HomeFocus::AllGames : HomeFocus::Import;
        const float focused =
            (a.home_focus == target ? fp : 0.0f) + (a.previous_home_focus == target ? 1.0f - fp : 0.0f);
        auto &card = ui_objs.action_cards[i];
        set_pos_if_changed(card.root, sc(25 + i * 301), sc(321.0f + 12.0f * (1.0f - show)));
        set_opa_if_changed(card.root, opa_from_float(show));
        const lv_color_t border_c = rgb(187 + static_cast<int>(52 * focused),
                                        211 + static_cast<int>(37 * focused),
                                        219 + static_cast<int>(30 * focused));
        const lv_opa_t border_opa = static_cast<lv_opa_t>(60 + static_cast<int>(195 * focused));
        const int32_t border_w = sc_border(1.0f + focused);
        if (lv_obj_get_style_border_opa(card.border, LV_PART_MAIN) != border_opa ||
            lv_obj_get_style_border_width(card.border, LV_PART_MAIN) != border_w ||
            lv_color_to_u32(lv_obj_get_style_border_color(card.border, LV_PART_MAIN)) !=
                lv_color_to_u32(border_c)) {
            lv_obj_set_style_border_color(card.border, border_c, 0);
            lv_obj_set_style_border_opa(card.border, border_opa, 0);
            lv_obj_set_style_border_width(card.border, border_w, 0);
        }
    }

    if (last_home_msg != a.message || last_mode != ViewMode::Home) {
        set_visible(ui_objs.home_toast, !a.message.empty());
        if (!a.message.empty())
            lv_label_set_text(ui_objs.home_toast_label, a.message.c_str());
        last_home_msg = a.message;
    }
}

void sync_all_games(const State &a, Uint32 now)
{
    const float heading = entrance(a, now);
    const float head_off = 8.0f * (1.0f - heading);
    set_opa_if_changed(ui_objs.all_heading, opa_from_float(heading));
    set_pos_if_changed(ui_objs.all_heading, sc(25), sc(72.0f + head_off));
    set_opa_if_changed(ui_objs.all_count, opa_from_float(heading));
    lv_obj_align(ui_objs.all_count, LV_ALIGN_TOP_RIGHT, -sc(25), sc(79.0f + head_off));

    for (int i = 0; i < game_count; ++i) {
        const float show = entrance(a, now, 60 + i * 60);
        const float x = 25 + (i % 2) * 301;
        const float y = 111 + (i / 2) * 150 + 10.0f * (1.0f - show);
        auto &card = ui_objs.all_cards[i];
        set_pos_if_changed(card.root, sc(x), sc(y));
        set_opa_if_changed(card.root, opa_from_float(show));
        if (last_all_focus != a.all_focus || last_mode != ViewMode::AllGames) {
            const bool focused = (a.all_focus == i);
            lv_obj_set_style_border_color(card.border, focused ? rgb(248, 252, 255) : rgb(255, 255, 255), 0);
            lv_obj_set_style_border_opa(card.border, focused ? 255 : 60, 0);
            lv_obj_set_style_border_width(card.border, sc_border(focused ? 2 : 1), 0);
        }
    }
    last_all_focus = a.all_focus;

    if (last_all_msg != a.message || last_mode != ViewMode::AllGames) {
        set_visible(ui_objs.all_toast, !a.message.empty());
        if (!a.message.empty())
            lv_label_set_text(ui_objs.all_toast_label, a.message.c_str());
        last_all_msg = a.message;
    }
}

void sync_import(const State &a, Uint32 now)
{
    const float heading = entrance(a, now);
    const float head_off = 10.0f * (1.0f - heading);
    set_opa_if_changed(ui_objs.import_heading, opa_from_float(heading));
    set_pos_if_changed(ui_objs.import_heading, sc(25), sc(72.0f + head_off));
    set_opa_if_changed(ui_objs.import_subtitle, opa_from_float(heading));
    set_pos_if_changed(ui_objs.import_subtitle, sc(25), sc(99.0f + head_off));

    const std::string pkg_name = a.pkg.filename().string();
    const std::string lic_name = a.license.filename().string();
    const bool running = a.import_running();

    for (int i = 0; i < 2; ++i) {
        const float show = entrance(a, now, 80 + i * 90);
        const float y = 135 + i * 76 + 12.0f * (1.0f - show);
        auto &row = ui_objs.import_rows[i];
        set_pos_if_changed(row.root, sc(25), sc(y));
        set_opa_if_changed(row.root, opa_from_float(show));
        if (last_import_focus != a.import_focus || last_mode != ViewMode::Import) {
            const bool focused =
                (a.import_focus == (i == 0 ? ImportFocus::Package : ImportFocus::License));
            lv_obj_set_style_border_color(row.border,
                                          focused ? rgb(241, 248, 250) : rgb(119, 150, 162), 0);
            lv_obj_set_style_border_opa(row.border, focused ? 255 : 90, 0);
            lv_obj_set_style_border_width(row.border, sc_border(focused ? 2 : 1), 0);
        }
    }
    if (last_import_pkg != pkg_name || last_mode != ViewMode::Import) {
        lv_label_set_text(ui_objs.import_rows[0].value,
                          pkg_name.empty() ? "Choose a .pkg file" : pkg_name.c_str());
        last_import_pkg = pkg_name;
    }
    if (last_import_lic != lic_name || last_mode != ViewMode::Import) {
        lv_label_set_text(ui_objs.import_rows[1].value,
                          lic_name.empty() ? "Choose a work.bin file" : lic_name.c_str());
        last_import_lic = lic_name;
    }

    const float details = entrance(a, now, 260);
    set_opa_if_changed(ui_objs.import_details, opa_from_float(details));
    set_pos_if_changed(ui_objs.import_details, 0, sc(297.0f + 10.0f * (1.0f - details)));

    if (last_import_focus != a.import_focus || last_import_running != running ||
        last_mode != ViewMode::Import) {
        const bool btn_focused = (a.import_focus == ImportFocus::Start);
        lv_obj_set_style_bg_color(ui_objs.import_btn,
                                  btn_focused ? rgb(225, 237, 239) : rgb(92, 115, 124), 0);
        lv_obj_set_style_text_color(ui_objs.import_btn_label,
                                    btn_focused ? rgb(14, 28, 35) : rgb(225, 236, 239), 0);
        lv_label_set_text(ui_objs.import_btn_label, running ? "IMPORTING..." : "IMPORT GAME");
        last_import_running = running;
    }
    if (last_import_msg != a.message || last_mode != ViewMode::Import) {
        lv_label_set_text(ui_objs.import_status_primary,
                          a.message.empty() ? "Your files will be prepared for vita2native."
                                            : a.message.c_str());
        set_visible(ui_objs.import_status_secondary, a.message.empty());
        last_import_msg = a.message;
    }
    last_import_focus = a.import_focus;
}

void sync_picker(const State &a, Uint32 now)
{
    const float heading = entrance(a, now);
    const float head_off = 8.0f * (1.0f - heading);
    set_opa_if_changed(ui_objs.picker_heading, opa_from_float(heading));
    set_pos_if_changed(ui_objs.picker_heading, sc(25), sc(72.0f + head_off));
    set_opa_if_changed(ui_objs.picker_dir, opa_from_float(heading));
    set_pos_if_changed(ui_objs.picker_dir, sc(25), sc(100.0f + head_off));

    const std::string dir_str = a.dir.string();
    if (last_pick_kind != a.pick_kind || last_mode != ViewMode::Picker) {
        lv_label_set_text(ui_objs.picker_heading,
                          a.pick_kind == PickerKind::Package ? "Choose a package" : "Choose work.bin");
        last_pick_kind = a.pick_kind;
    }
    const bool list_changed = last_picker_dir != dir_str || last_picker_scroll != a.scroll ||
                              last_picker_index != a.pick_index ||
                              last_picker_count != a.items.size() || last_mode != ViewMode::Picker;
    if (last_picker_dir != dir_str || last_mode != ViewMode::Picker) {
        lv_label_set_text(ui_objs.picker_dir, dir_str.c_str());
        last_picker_dir = dir_str;
    }
    set_visible(ui_objs.picker_empty, a.items.empty());

    const fs::path parent_path = a.dir.parent_path();
    for (int row = 0; row < picker_page_size; ++row) {
        const int idx = a.scroll + row;
        auto &r = ui_objs.picker_rows[row];
        if (idx >= static_cast<int>(a.items.size())) {
            set_visible(r.root, false);
            continue;
        }
        set_visible(r.root, true);
        const float show = entrance(a, now, 50 + row * 35);
        set_pos_if_changed(r.root, sc(25), sc(127 + row * 37 + 8.0f * (1.0f - show)));
        set_opa_if_changed(r.root, opa_from_float(show));

        if (list_changed) {
            const auto &it = a.items[idx];
            const bool selected = (idx == a.pick_index);
            lv_obj_set_style_bg_opa(r.root, selected ? 24 : 0, 0);
            lv_obj_set_style_border_width(r.border, selected ? sc_border(2) : 0, 0);
            lv_label_set_text(r.kind, it.dir ? "DIR" : "FILE");
            const std::string name =
                it.path == parent_path ? ".." : it.path.filename().string();
            lv_label_set_text(r.name, name.c_str());
        }
    }
    last_picker_scroll = a.scroll;
    last_picker_index = a.pick_index;
    last_picker_count = a.items.size();
}

void flush_cb(lv_display_t *disp, const lv_area_t *, uint8_t *)
{
    if (lv_display_flush_is_last(disp))
        frame_flushed = true;
    lv_display_flush_ready(disp);
}

void destroy_fonts()
{
    auto drop = [](lv_font_t *&f) {
        if (f) {
            lv_tiny_ttf_destroy(f);
            f = nullptr;
        }
    };
    drop(fonts.reg_10);
    drop(fonts.reg_11);
    drop(fonts.reg_12);
    drop(fonts.reg_14);
    drop(fonts.bold_8);
    drop(fonts.bold_10);
    drop(fonts.bold_11);
    drop(fonts.bold_13);
    drop(fonts.bold_15);
    drop(fonts.bold_16);
    drop(fonts.bold_19);
    drop(fonts.bold_20);
    drop(fonts.bold_22);
    drop(fonts.bold_24);
}

bool init_fonts()
{
    auto mk = [](const unsigned char *data, size_t size, int32_t px) {
        return lv_tiny_ttf_create_data(data, size, sc(static_cast<float>(px)));
    };
    fonts.reg_10 = mk(inter_font, sizeof(inter_font), 10);
    fonts.reg_11 = mk(inter_font, sizeof(inter_font), 11);
    fonts.reg_12 = mk(inter_font, sizeof(inter_font), 12);
    fonts.reg_14 = mk(inter_font, sizeof(inter_font), 14);
    fonts.bold_8 = mk(inter_bold, sizeof(inter_bold), 8);
    fonts.bold_10 = mk(inter_bold, sizeof(inter_bold), 10);
    fonts.bold_11 = mk(inter_bold, sizeof(inter_bold), 11);
    fonts.bold_13 = mk(inter_bold, sizeof(inter_bold), 13);
    fonts.bold_15 = mk(inter_bold, sizeof(inter_bold), 15);
    fonts.bold_16 = mk(inter_bold, sizeof(inter_bold), 16);
    fonts.bold_19 = mk(inter_bold, sizeof(inter_bold), 19);
    fonts.bold_20 = mk(inter_bold, sizeof(inter_bold), 20);
    fonts.bold_22 = mk(inter_bold, sizeof(inter_bold), 22);
    fonts.bold_24 = mk(inter_bold, sizeof(inter_bold), 24);
    return fonts.reg_10 && fonts.reg_11 && fonts.reg_12 && fonts.reg_14 && fonts.bold_8 &&
           fonts.bold_10 && fonts.bold_11 && fonts.bold_13 && fonts.bold_15 && fonts.bold_16 &&
           fonts.bold_19 && fonts.bold_20 && fonts.bold_22 && fonts.bold_24;
}
} // namespace

bool initialize(const std::filesystem::path &logo_path, int drawable_width, int drawable_height,
                std::string &warning)
{
    ui_scale = std::max(1.0f, std::min(static_cast<float>(drawable_width) / screen_width,
                                       static_cast<float>(drawable_height) / screen_height));
    fb_w = sc(screen_width);
    fb_h = sc(screen_height);
    fb_stride = fb_w * 4;

    lv_init();
    lv_tick_set_cb(SDL_GetTicks);

    framebuffer_data.assign(static_cast<size_t>(fb_stride) * fb_h, 0);
    display = lv_display_create(fb_w, fb_h);
    if (!display) {
        shutdown();
        return false;
    }
    lv_display_set_color_format(display, LV_COLOR_FORMAT_XRGB8888);
    lv_display_set_buffers(display, framebuffer_data.data(), nullptr, framebuffer_data.size(),
                           LV_DISPLAY_RENDER_MODE_DIRECT);
    lv_display_set_flush_cb(display, flush_cb);

    if (!init_fonts()) {
        shutdown();
        return false;
    }

    if (!logo_path.empty()) {
        ImageBuffer raw_logo;
        try {
            if (load_logo(logo_path, raw_logo)) {
                resize_argb8888_contain(raw_logo, sc(26), custom_logo_header);
                resize_argb8888_contain(raw_logo, sc(72), custom_logo_splash);
            }
        } catch (const std::exception &error) {
            fprintf(stderr, "Could not load logo %s: %s\n", logo_path.string().c_str(), error.what());
        }
        if (!custom_logo_header.valid() || !custom_logo_splash.valid()) {
            custom_logo_header.reset();
            custom_logo_splash.reset();
            fprintf(stderr, "Could not load PNG or SVG logo: %s\n", logo_path.string().c_str());
            warning = "Could not load logo; using the built-in logo.";
        }
    }

    unsigned char *raw_covers[] = { cover_0, cover_1, cover_2, cover_3 };
    int raw_sizes[] = { sizeof(cover_0), sizeof(cover_1), sizeof(cover_2), sizeof(cover_3) };
    for (int i = 0; i < game_count; ++i) {
        int iw = 0, ih = 0, ch = 0;
        stbi_uc *rgba = stbi_load_from_memory(raw_covers[i], raw_sizes[i], &iw, &ih, &ch, 4);
        if (!rgba) {
            fprintf(stderr, "Could not load cover %d\n", i);
            continue;
        }
        make_cover_variant(rgba, iw, ih, sc(640), sc(440), covers[i].bg);
        make_cover_variant(rgba, iw, ih, sc(310), sc(154), covers[i].recent);
        make_cover_variant(rgba, iw, ih, sc(289), sc(136), covers[i].grid);
        stbi_image_free(rgba);
    }

    const int icon_size = sc(22);
    build_footer_icon(icon_dpad, 'D', icon_size);
    build_footer_icon(icon_key_a, 'A', icon_size);
    build_footer_icon(icon_key_b, 'B', icon_size);

    create_ui_tree();
    return true;
}

void shutdown()
{
    destroy_fonts();
    if (display) {
        lv_deinit();
        display = nullptr;
    }
    framebuffer_data.clear();
    for (auto &c : covers) {
        c.bg.reset();
        c.recent.reset();
        c.grid.reset();
    }
    custom_logo_header.reset();
    custom_logo_splash.reset();
    icon_dpad.reset();
    icon_key_a.reset();
    icon_key_b.reset();
    ui_objs = {};
    last_mode = ViewMode::None;
}

void sync(const State &a)
{
    const Uint32 now = SDL_GetTicks();
    ViewMode mode = ViewMode::Home;
    if (!a.config_error.empty())
        mode = ViewMode::Error;
    else if (now - a.start < splash_duration)
        mode = ViewMode::Splash;
    else if (a.picker)
        mode = ViewMode::Picker;
    else if (a.screen == Screen::AllGames)
        mode = ViewMode::AllGames;
    else if (a.screen == Screen::Import)
        mode = ViewMode::Import;

    if (mode != last_mode) {
        set_visible(ui_objs.error_layer, mode == ViewMode::Error);
        set_visible(ui_objs.splash_layer, mode == ViewMode::Splash);
        const bool show_main = (mode != ViewMode::Error && mode != ViewMode::Splash);
        set_visible(ui_objs.main_layer, show_main);
        set_visible(ui_objs.home_layer, mode == ViewMode::Home);
        set_visible(ui_objs.all_layer, mode == ViewMode::AllGames);
        set_visible(ui_objs.import_layer, mode == ViewMode::Import);
        set_visible(ui_objs.picker_layer, mode == ViewMode::Picker);
    }

    if (mode == ViewMode::Error) {
        if (last_config_error != a.config_error) {
            lv_label_set_text(ui_objs.error_body, a.config_error.c_str());
            last_config_error = a.config_error;
        }
        last_mode = mode;
        return;
    }

    if (mode == ViewMode::Splash) {
        const Uint32 elapsed = now - a.start;
        const float appear = ease_out(elapsed / 600.0f);
        set_opa_if_changed(ui_objs.splash_brand.box, opa_from_float(appear));
        set_pos_if_changed(ui_objs.splash_brand.box, sc(284), sc(170.0f + 12.0f * (1.0f - appear)));
        const int32_t bar_w =
            sc(160.0f * ease_out(elapsed / static_cast<float>(splash_duration)));
        if (lv_obj_get_width(ui_objs.splash_fill) != bar_w)
            lv_obj_set_width(ui_objs.splash_fill, bar_w);
        last_mode = mode;
        return;
    }

    update_background(a, now);
    update_header_and_footer(a);

    if (mode == ViewMode::Home)
        sync_home(a, now);
    else if (mode == ViewMode::AllGames)
        sync_all_games(a, now);
    else if (mode == ViewMode::Import)
        sync_import(a, now);
    else if (mode == ViewMode::Picker)
        sync_picker(a, now);

    last_mode = mode;
}

bool render(bool force)
{
    frame_flushed = false;
    if (force && display) {
        lv_obj_invalidate(lv_screen_active());
        lv_refr_now(display);
    } else {
        lv_timer_handler();
    }
    return frame_flushed;
}

const uint8_t *framebuffer()
{
    return framebuffer_data.data();
}

int render_width()
{
    return fb_w;
}

int render_height()
{
    return fb_h;
}

} // namespace launcher::ui
