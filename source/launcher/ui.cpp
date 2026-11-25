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
#include <list>
#include <memory>
#include <unordered_map>
#include <vector>

#include "inter_bold.h"
#include "inter_font.h"
#include "lucide_font.h"

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
    lv_font_t *bold_10 = nullptr;
    lv_font_t *bold_11 = nullptr;
    lv_font_t *bold_13 = nullptr;
    lv_font_t *bold_15 = nullptr;
    lv_font_t *bold_16 = nullptr;
    lv_font_t *bold_19 = nullptr;
    lv_font_t *bold_20 = nullptr;
    lv_font_t *bold_22 = nullptr;
    lv_font_t *bold_24 = nullptr;
    lv_font_t *icon_15 = nullptr;
};
Fonts fonts;

struct CacheKey {
    std::string path;
    int w = 0;
    int h = 0;
    bool operator==(const CacheKey &other) const
    {
        return w == other.w && h == other.h && path == other.path;
    }
};

struct CacheKeyHash {
    size_t operator()(const CacheKey &k) const
    {
        const size_t h1 = std::hash<std::string>{}(k.path);
        const size_t h2 = std::hash<int>{}(k.w) ^ (std::hash<int>{}(k.h) << 1);
        return h1 ^ (h2 + 0x9e3779b9 + (h1 << 6) + (h1 >> 2));
    }
};

constexpr size_t image_cache_limit = 48;
std::list<CacheKey> image_lru;
std::unordered_map<CacheKey, std::pair<std::shared_ptr<ImageBuffer>, std::list<CacheKey>::iterator>,
                   CacheKeyHash>
    image_cache;

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

struct GameCardWidgets {
    lv_obj_t *root = nullptr;
    lv_obj_t *img = nullptr;
    lv_obj_t *fallback_id = nullptr;
    lv_obj_t *grad = nullptr;
    lv_obj_t *title = nullptr;
    lv_obj_t *border = nullptr;
    std::shared_ptr<ImageBuffer> art_ref;
};

struct ActionCard {
    lv_obj_t *root = nullptr;
    lv_obj_t *icon_box = nullptr;
    lv_obj_t *title = nullptr;
    lv_obj_t *subtitle = nullptr;
    lv_obj_t *arrow = nullptr;
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

struct ScreenshotCard {
    lv_obj_t *root = nullptr;
    lv_obj_t *img = nullptr;
    lv_obj_t *border = nullptr;
    std::shared_ptr<ImageBuffer> art_ref;
};

struct ConfirmModal {
    lv_obj_t *backdrop = nullptr;
    lv_obj_t *card = nullptr;
    lv_obj_t *title = nullptr;
    lv_obj_t *message = nullptr;
    lv_obj_t *confirm_icon = nullptr;
    lv_obj_t *confirm_label = nullptr;
    lv_obj_t *cancel_icon = nullptr;
    lv_obj_t *cancel_label = nullptr;
};

struct ToastOverlay {
    lv_obj_t *root = nullptr;
    lv_obj_t *icon = nullptr;
    lv_obj_t *label = nullptr;
    int32_t width = 0;
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
    std::shared_ptr<ImageBuffer> bg_prev_ref;
    std::shared_ptr<ImageBuffer> bg_curr_ref;
    lv_obj_t *bg_tint = nullptr;
    lv_obj_t *bg_grad = nullptr;

    lv_obj_t *header_layer = nullptr;
    BrandWidgets header_brand;
    lv_obj_t *header_screen = nullptr;

    lv_obj_t *home_layer = nullptr;
    lv_obj_t *home_heading = nullptr;
    lv_obj_t *home_carousel = nullptr;
    lv_obj_t *home_empty = nullptr;
    std::vector<GameCardWidgets> recent_cards;
    std::array<ActionCard, 2> action_cards;

    lv_obj_t *all_layer = nullptr;
    lv_obj_t *all_heading = nullptr;
    lv_obj_t *all_count = nullptr;
    lv_obj_t *all_viewport = nullptr;
    lv_obj_t *all_empty = nullptr;
    lv_obj_t *all_scrollbar_track = nullptr;
    lv_obj_t *all_scrollbar_thumb = nullptr;
    std::vector<GameCardWidgets> all_cards;

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

    lv_obj_t *info_layer = nullptr;
    lv_obj_t *info_boxart_card = nullptr;
    lv_obj_t *info_boxart_img = nullptr;
    lv_obj_t *info_boxart_fallback = nullptr;
    std::shared_ptr<ImageBuffer> info_boxart_ref;
    lv_obj_t *info_badges = nullptr;
    lv_obj_t *info_badge_id = nullptr;
    lv_obj_t *info_badge_id_label = nullptr;
    lv_obj_t *info_badge_cat = nullptr;
    lv_obj_t *info_badge_cat_label = nullptr;
    lv_obj_t *info_badge_rating = nullptr;
    lv_obj_t *info_badge_rating_label = nullptr;
    lv_obj_t *info_desc = nullptr;
    lv_obj_t *info_play_btn = nullptr;
    lv_obj_t *info_play_label = nullptr;
    lv_obj_t *info_shot_heading = nullptr;
    lv_obj_t *info_shot_count = nullptr;
    lv_obj_t *info_shot_empty = nullptr;
    lv_obj_t *info_shot_strip = nullptr;
    std::vector<ScreenshotCard> info_shots;

    lv_obj_t *footer_layer = nullptr;
    lv_obj_t *footer_select_icon = nullptr;
    lv_obj_t *footer_select_label = nullptr;
    lv_obj_t *footer_back_icon = nullptr;
    lv_obj_t *footer_back_label = nullptr;

    ToastOverlay toast;
    ConfirmModal confirm_modal;

    std::deque<lv_grad_dsc_t> gradients;
};
Widgets ui_objs;

enum class ViewMode { None, Error, Splash, Home, AllGames, Import, Picker, GameInfo };
ViewMode last_mode = ViewMode::None;
ConfirmKind last_confirm = ConfirmKind::None;
int last_games_revision = -1;
std::string last_config_error;
int last_bg_selected = -1;
int last_bg_previous = -1;
Uint32 bg_fade_start = 0;
int last_bg_revision = -1;
std::string last_header_text;
int last_all_focus = -1;
int all_scroll_row = 0;
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
int last_info_game = -1;
int last_info_revision = -1;
GameInfoFocus last_info_focus = GameInfoFocus::Play;
int last_info_shot_idx = -1;
int info_shot_scroll = 0;
std::string last_toast_text;
ToastKind last_toast_kind = ToastKind::Info;
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

std::shared_ptr<ImageBuffer> get_cached_image(const fs::path &path, int target_w, int target_h)
{
    if (path.empty())
        return nullptr;
    const CacheKey key{ path.string(), target_w, target_h };
    auto it = image_cache.find(key);
    if (it != image_cache.end()) {
        image_lru.splice(image_lru.begin(), image_lru, it->second.second);
        return it->second.first;
    }

    int iw = 0, ih = 0, ch = 0;
    stbi_uc *rgba = stbi_load(key.path.c_str(), &iw, &ih, &ch, 4);
    if (!rgba || iw <= 0 || ih <= 0) {
        if (rgba)
            stbi_image_free(rgba);
        return nullptr;
    }

    auto buf = std::make_shared<ImageBuffer>();
    const bool ok = make_cover_variant(rgba, iw, ih, target_w, target_h, *buf);
    stbi_image_free(rgba);
    if (!ok)
        return nullptr;

    image_lru.push_front(key);
    image_cache[key] = { buf, image_lru.begin() };
    while (image_lru.size() > image_cache_limit) {
        const CacheKey &oldest = image_lru.back();
        image_cache.erase(oldest);
        image_lru.pop_back();
    }
    return buf;
}

std::filesystem::path game_card_art(const GameEntry &g)
{
    return !g.boxart.empty() ? g.boxart : g.art;
}

std::filesystem::path game_hero_art(const GameEntry &g)
{
    return !g.art.empty() ? g.art : g.boxart;
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

GameCardWidgets build_game_card(lv_obj_t *parent, float x, float y, float w, float h,
                                const GameEntry &game, const lv_font_t *font, float label_x,
                                float label_y, float label_max_w, lv_opa_t grad_end_opa)
{
    GameCardWidgets c;
    c.root = make_gradient(parent, x, y, w, h, rgb(32, 54, 68), 255, rgb(14, 26, 36), 255);
    lv_obj_set_style_radius(c.root, sc(5), 0);
    lv_obj_set_style_clip_corner(c.root, true, 0);

    c.art_ref = get_cached_image(game_card_art(game), sc(w), sc(h));
    c.img = lv_image_create(c.root);
    init_clean_obj(c.img, 0, 0, w, h);
    if (c.art_ref && c.art_ref->valid()) {
        lv_image_set_src(c.img, &c.art_ref->dsc);
    } else {
        lv_obj_add_flag(c.img, LV_OBJ_FLAG_HIDDEN);
        c.fallback_id = make_label(c.root, 0, 0, game.title_id.c_str(), fonts.bold_16,
                                   rgb(159, 190, 201));
        lv_obj_set_style_text_opa(c.fallback_id, 65, 0);
        lv_obj_align(c.fallback_id, LV_ALIGN_CENTER, 0, -sc(10));
    }

    if (h == 136) {
        c.grad = make_gradient(c.root, 0, 65, w, 71, rgb(8, 15, 21), 0, rgb(8, 15, 21),
                               grad_end_opa);
    } else {
        c.grad = make_gradient(c.root, 0, h - 75, w, 75, rgb(8, 15, 21), 0, rgb(8, 15, 21),
                               grad_end_opa);
    }

    c.title = make_label(c.root, label_x, label_y, game.name.c_str(), font, rgb(255, 255, 255),
                         label_max_w);
    lv_obj_set_height(c.title, lv_font_get_line_height(font));
    lv_label_set_long_mode(c.title, LV_LABEL_LONG_MODE_DOTS);

    c.border = make_box(c.root, 0, 0, w, h, rgb(0, 0, 0), LV_OPA_TRANSP, 5);
    lv_obj_set_style_border_color(c.border, rgb(255, 255, 255), 0);
    lv_obj_set_style_border_opa(c.border, 60, 0);
    lv_obj_set_style_border_width(c.border, sc_border(1), 0);
    return c;
}

void create_confirm_modal(ConfirmModal &m, lv_obj_t *parent)
{
    m.backdrop = make_box(parent, 0, 0, screen_width, screen_height, rgb(6, 13, 19), 185);
    m.card = make_box(m.backdrop, 130, 163, 380, 154, rgb(20, 35, 45), 250, 8);
    lv_obj_set_style_border_color(m.card, rgb(195, 222, 230), 0);
    lv_obj_set_style_border_opa(m.card, 110, 0);
    lv_obj_set_style_border_width(m.card, sc_border(1), 0);

    m.title = make_label(m.card, 24, 22, "", fonts.bold_19, rgb(248, 251, 253), 332);
    m.message = make_label(m.card, 24, 54, "", fonts.reg_12, rgb(174, 193, 201), 332);
    lv_label_set_long_mode(m.message, LV_LABEL_LONG_MODE_WRAP);

    m.confirm_icon = lv_image_create(m.card);
    init_clean_obj(m.confirm_icon, 0, 108, 22, 22);
    lv_image_set_src(m.confirm_icon, &icon_key_a.dsc);
    m.confirm_label = make_label(m.card, 0, 114, "", fonts.bold_11, rgb(225, 237, 240));

    m.cancel_icon = lv_image_create(m.card);
    init_clean_obj(m.cancel_icon, 0, 108, 22, 22);
    lv_image_set_src(m.cancel_icon, &icon_key_b.dsc);
    m.cancel_label = make_label(m.card, 0, 114, "", fonts.bold_11, rgb(225, 237, 240));

    lv_obj_add_flag(m.backdrop, LV_OBJ_FLAG_HIDDEN);
}

void layout_modal_actions(ConfirmModal &m, const char *confirm_text, const char *cancel_text)
{
    lv_label_set_text(m.confirm_label, confirm_text);
    lv_label_set_text(m.cancel_label, cancel_text);
    lv_point_t confirm_sz{}, cancel_sz{};
    lv_text_get_size(&confirm_sz, confirm_text, fonts.bold_11, 0, 0, LV_COORD_MAX,
                     LV_TEXT_FLAG_NONE);
    lv_text_get_size(&cancel_sz, cancel_text, fonts.bold_11, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);

    const int32_t icon_w = sc(22);
    const int32_t icon_gap = sc(7);
    const int32_t pair_gap = sc(20);
    const int32_t right_edge = sc(356);
    const int32_t y_icon = sc(108);
    const int32_t y_label = sc(114);

    const int32_t cancel_x = right_edge - (icon_w + icon_gap + cancel_sz.x);
    const int32_t confirm_x = cancel_x - pair_gap - (icon_w + icon_gap + confirm_sz.x);

    lv_obj_set_pos(m.confirm_icon, confirm_x, y_icon);
    lv_obj_set_pos(m.confirm_label, confirm_x + icon_w + icon_gap, y_label);
    lv_obj_set_pos(m.cancel_icon, cancel_x, y_icon);
    lv_obj_set_pos(m.cancel_label, cancel_x + icon_w + icon_gap, y_label);
}

void set_confirm_modal(ConfirmModal &m, bool visible, const char *title, const char *message,
                       const char *confirm_text, const char *cancel_text)
{
    if (!visible) {
        lv_obj_add_flag(m.backdrop, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_label_set_text(m.title, title);
    lv_label_set_text(m.message, message);
    layout_modal_actions(m, confirm_text, cancel_text);
    lv_obj_clear_flag(m.backdrop, LV_OBJ_FLAG_HIDDEN);
}

struct ToastAppearance {
    const char *glyph;
    lv_color_t accent;
};

ToastAppearance toast_appearance(ToastKind kind)
{
    switch (kind) {
    case ToastKind::Success:
        return { "\xee\x88\xa6", rgb(98, 212, 148) }; // Lucide circle-check (U+E226)
    case ToastKind::Warning:
        return { "\xee\x86\x93", rgb(240, 194, 92) }; // Lucide triangle-alert (U+E193)
    case ToastKind::Error:
        return { "\xee\x82\x84", rgb(236, 114, 114) }; // Lucide circle-x (U+E084)
    case ToastKind::Info:
    default:
        return { "\xee\x83\xb9", rgb(106, 198, 222) }; // Lucide info (U+E0F9)
    }
}

void create_toast(ToastOverlay &t, lv_obj_t *parent)
{
    t.root = make_box(parent, 0, 388, 180, 32, rgb(16, 30, 40), 245, 16);
    lv_obj_set_style_border_color(t.root, rgb(106, 198, 222), 0);
    lv_obj_set_style_border_opa(t.root, 185, 0);
    lv_obj_set_style_border_width(t.root, sc_border(1), 0);
    t.icon = make_label(t.root, 13, 8, "", fonts.icon_15, rgb(106, 198, 222));
    t.label = make_label(t.root, 35, 9, "", fonts.reg_11, rgb(236, 245, 248), 490);
    lv_obj_set_height(t.label, lv_font_get_line_height(fonts.reg_11));
    lv_label_set_long_mode(t.label, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_add_flag(t.root, LV_OBJ_FLAG_HIDDEN);
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
    lv_obj_add_flag(ui_objs.bg_prev, LV_OBJ_FLAG_HIDDEN);

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
    ui_objs.header_screen = make_label(ui_objs.header_layer, 63, 16, "Vita2Native",
                                       fonts.bold_24, rgb(248, 251, 253), 552);
    lv_obj_set_height(ui_objs.header_screen, lv_font_get_line_height(fonts.bold_24));
    lv_label_set_long_mode(ui_objs.header_screen, LV_LABEL_LONG_MODE_DOTS);

    // Home screen
    ui_objs.home_layer = lv_obj_create(ui_objs.main_layer);
    init_clean_obj(ui_objs.home_layer, 0, 0, 640, 438);
    ui_objs.home_heading =
        make_label(ui_objs.home_layer, 25, 72, "Recent games", fonts.bold_20, rgb(248, 251, 253));

    ui_objs.home_carousel = lv_obj_create(ui_objs.home_layer);
    init_clean_obj(ui_objs.home_carousel, 0, 103, 640, 165);

    ui_objs.home_empty =
        make_box(ui_objs.home_layer, 25, 107, 590, 154, rgb(24, 40, 51), 200, 6);
    lv_obj_set_style_border_color(ui_objs.home_empty, rgb(119, 150, 162), 0);
    lv_obj_set_style_border_opa(ui_objs.home_empty, 75, 0);
    lv_obj_set_style_border_width(ui_objs.home_empty, sc_border(1), 0);
    make_label(ui_objs.home_empty, 24, 52, "No games found", fonts.bold_16, rgb(238, 247, 249));
    make_label(ui_objs.home_empty, 24, 80,
               "Import a .pkg or place title folders (*/eboot.bin) in your games directory.",
               fonts.reg_11, rgb(164, 188, 196), 542);
    lv_obj_add_flag(ui_objs.home_empty, LV_OBJ_FLAG_HIDDEN);

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
        card.subtitle = make_label(card.root, 68, 48,
                                   i == 0 ? "Browse your library" : "Add a package and license",
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
    ui_objs.all_count =
        make_label(ui_objs.all_layer, 0, 79, "0 GAMES", fonts.bold_10, rgb(174, 192, 200));
    lv_obj_align(ui_objs.all_count, LV_ALIGN_TOP_RIGHT, -sc(25), sc(79));

    ui_objs.all_viewport = lv_obj_create(ui_objs.all_layer);
    init_clean_obj(ui_objs.all_viewport, 0, 107, 640, 326);

    ui_objs.all_empty =
        make_box(ui_objs.all_layer, 25, 111, 590, 136, rgb(24, 40, 51), 200, 6);
    lv_obj_set_style_border_color(ui_objs.all_empty, rgb(119, 150, 162), 0);
    lv_obj_set_style_border_opa(ui_objs.all_empty, 75, 0);
    lv_obj_set_style_border_width(ui_objs.all_empty, sc_border(1), 0);
    make_label(ui_objs.all_empty, 24, 42, "No games in your library yet", fonts.bold_16,
               rgb(238, 247, 249));
    make_label(ui_objs.all_empty, 24, 70,
               "Use Import game on the home screen or add title folders containing eboot.bin.",
               fonts.reg_11, rgb(164, 188, 196), 542);
    lv_obj_add_flag(ui_objs.all_empty, LV_OBJ_FLAG_HIDDEN);

    ui_objs.all_scrollbar_track =
        make_box(ui_objs.all_layer, 626, 111, 3, 286, rgb(255, 255, 255), 25, 2);
    ui_objs.all_scrollbar_thumb =
        make_box(ui_objs.all_layer, 626, 111, 3, 80, rgb(225, 237, 239), 180, 2);
    lv_obj_add_flag(ui_objs.all_scrollbar_track, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(ui_objs.all_scrollbar_thumb, LV_OBJ_FLAG_HIDDEN);
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

    // Game Info screen
    ui_objs.info_layer = lv_obj_create(ui_objs.main_layer);
    init_clean_obj(ui_objs.info_layer, 0, 0, 640, 438);

    ui_objs.info_boxart_card =
        make_gradient(ui_objs.info_layer, 25, 70, 120, 152, rgb(32, 54, 68), 255, rgb(14, 26, 36),
                      255);
    lv_obj_set_style_radius(ui_objs.info_boxart_card, sc(6), 0);
    lv_obj_set_style_clip_corner(ui_objs.info_boxart_card, true, 0);
    ui_objs.info_boxart_img = lv_image_create(ui_objs.info_boxart_card);
    init_clean_obj(ui_objs.info_boxart_img, 0, 0, 120, 152);
    ui_objs.info_boxart_fallback =
        make_label(ui_objs.info_boxart_card, 0, 0, "", fonts.bold_15, rgb(159, 190, 201));
    lv_obj_set_style_text_opa(ui_objs.info_boxart_fallback, 140, 0);
    lv_obj_align(ui_objs.info_boxart_fallback, LV_ALIGN_CENTER, 0, 0);
    lv_obj_t *boxart_border =
        make_box(ui_objs.info_boxart_card, 0, 0, 120, 152, rgb(0, 0, 0), LV_OPA_TRANSP, 6);
    lv_obj_set_style_border_color(boxart_border, rgb(255, 255, 255), 0);
    lv_obj_set_style_border_opa(boxart_border, 70, 0);
    lv_obj_set_style_border_width(boxart_border, sc_border(1), 0);

    ui_objs.info_badges = lv_obj_create(ui_objs.info_layer);
    init_clean_obj(ui_objs.info_badges, 162, 72, 453, 24);
    lv_obj_set_flex_flow(ui_objs.info_badges, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(ui_objs.info_badges, sc(8), 0);

    auto make_badge = [&](lv_obj_t *&pill, lv_obj_t *&lbl, lv_color_t bg, lv_opa_t bg_opa,
                          lv_color_t fg, bool bordered) {
        pill = make_box(ui_objs.info_badges, 0, 0, 0, 22, bg, bg_opa, 11);
        lv_obj_set_width(pill, LV_SIZE_CONTENT);
        lv_obj_set_style_pad_hor(pill, sc(10), 0);
        if (bordered) {
            lv_obj_set_style_border_color(pill, rgb(162, 197, 205), 0);
            lv_obj_set_style_border_opa(pill, 75, 0);
            lv_obj_set_style_border_width(pill, sc_border(1), 0);
        }
        lbl = make_centered_label(pill, "", fonts.bold_10, fg);
    };
    make_badge(ui_objs.info_badge_id, ui_objs.info_badge_id_label, rgb(135, 174, 185), 35,
               rgb(229, 243, 245), true);
    make_badge(ui_objs.info_badge_cat, ui_objs.info_badge_cat_label, rgb(255, 255, 255), 18,
               rgb(199, 218, 224), false);
    make_badge(ui_objs.info_badge_rating, ui_objs.info_badge_rating_label, rgb(255, 255, 255), 18,
               rgb(245, 215, 130), false);

    ui_objs.info_desc =
        make_label(ui_objs.info_layer, 162, 104, "", fonts.reg_11, rgb(184, 203, 211), 453);
    lv_obj_set_height(ui_objs.info_desc, sc(72));
    lv_label_set_long_mode(ui_objs.info_desc, LV_LABEL_LONG_MODE_DOTS);

    ui_objs.info_play_btn =
        make_box(ui_objs.info_layer, 162, 186, 170, 36, rgb(225, 237, 239), 255, 5);
    lv_obj_set_style_border_color(ui_objs.info_play_btn, rgb(248, 252, 255), 0);
    lv_obj_set_style_border_opa(ui_objs.info_play_btn, 0, 0);
    lv_obj_set_style_border_width(ui_objs.info_play_btn, sc_border(1), 0);
    ui_objs.info_play_label =
        make_centered_label(ui_objs.info_play_btn, "PLAY", fonts.bold_11, rgb(14, 28, 35));

    ui_objs.info_shot_heading =
        make_label(ui_objs.info_layer, 25, 240, "Screenshots", fonts.bold_15, rgb(248, 251, 253));
    ui_objs.info_shot_count =
        make_label(ui_objs.info_layer, 0, 243, "", fonts.bold_10, rgb(174, 192, 200));
    lv_obj_align(ui_objs.info_shot_count, LV_ALIGN_TOP_RIGHT, -sc(25), sc(243));

    ui_objs.info_shot_empty =
        make_box(ui_objs.info_layer, 25, 266, 590, 148, rgb(24, 40, 51), 150, 6);
    lv_obj_set_style_border_color(ui_objs.info_shot_empty, rgb(119, 150, 162), 0);
    lv_obj_set_style_border_opa(ui_objs.info_shot_empty, 60, 0);
    lv_obj_set_style_border_width(ui_objs.info_shot_empty, sc_border(1), 0);
    make_centered_label(ui_objs.info_shot_empty, "No screenshots available.", fonts.reg_11,
                        rgb(155, 179, 188));
    lv_obj_add_flag(ui_objs.info_shot_empty, LV_OBJ_FLAG_HIDDEN);

    ui_objs.info_shot_strip = lv_obj_create(ui_objs.info_layer);
    init_clean_obj(ui_objs.info_shot_strip, 25, 266, 590, 152);
    lv_obj_add_flag(ui_objs.info_layer, LV_OBJ_FLAG_HIDDEN);

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

    create_toast(ui_objs.toast, ui_objs.main_layer);
    create_confirm_modal(ui_objs.confirm_modal, ui_objs.main_layer);
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

void rebuild_game_lists(const State &a)
{
    ui_objs.recent_cards.clear();
    lv_obj_clean(ui_objs.home_carousel);

    const int rc = a.recent_count();
    for (int i = 0; i < rc; ++i) {
        ui_objs.recent_cards.push_back(build_game_card(
            ui_objs.home_carousel, 25 + i * 320, 4, 310, 154, a.games[i], fonts.bold_19, 16,
            154 - 32, 310 - 32, 220));
    }
    set_visible(ui_objs.home_carousel, rc > 0);
    set_visible(ui_objs.home_empty, rc == 0);

    ui_objs.all_cards.clear();
    lv_obj_clean(ui_objs.all_viewport);

    const int total = static_cast<int>(a.games.size());
    for (int i = 0; i < total; ++i) {
        ui_objs.all_cards.push_back(build_game_card(
            ui_objs.all_viewport, 25 + (i % 2) * 301, 4 + (i / 2) * 150, 289, 136, a.games[i],
            fonts.bold_16, 14, 104, 260, 225));
    }
    set_visible(ui_objs.all_viewport, total > 0);
    set_visible(ui_objs.all_empty, total == 0);
    const std::string count_text =
        std::to_string(total) + (total == 1 ? " GAME" : " GAMES");
    lv_label_set_text(ui_objs.all_count, count_text.c_str());
    lv_obj_align(ui_objs.all_count, LV_ALIGN_TOP_RIGHT, -sc(25), sc(79));

    all_scroll_row = 0;
    last_all_focus = -1;
    last_info_game = -1;
    last_bg_selected = -1;
    last_bg_previous = -1;
    last_games_revision = a.games_revision;
}

void set_bg_image(lv_obj_t *img, std::shared_ptr<ImageBuffer> &ref, const State &a, int idx)
{
    if (idx >= 0 && idx < static_cast<int>(a.games.size()))
        ref = get_cached_image(game_hero_art(a.games[idx]), sc(640), sc(440));
    else
        ref.reset();

    if (ref && ref->valid()) {
        lv_image_set_src(img, &ref->dsc);
        set_visible(img, true);
    } else {
        set_visible(img, false);
    }
}

void update_background(const State &a, Uint32 now)
{
    int curr_idx = -1;
    if (!a.games.empty()) {
        if (a.screen == Screen::GameInfo)
            curr_idx = a.info_game;
        else if (a.screen == Screen::AllGames)
            curr_idx = a.all_focus;
        else
            curr_idx = a.selected;
    }

    if (last_bg_revision != a.games_revision || last_bg_selected == -1 ||
        a.screen != Screen::Home || a.picker || a.recent_count() <= 1) {
        if (last_bg_selected != curr_idx || last_bg_revision != a.games_revision ||
            bg_fade_start != 0) {
            set_bg_image(ui_objs.bg_prev, ui_objs.bg_prev_ref, a, curr_idx);
            set_visible(ui_objs.bg_curr, false);
            ui_objs.bg_curr_ref.reset();
            last_bg_selected = curr_idx;
            last_bg_previous = curr_idx;
            bg_fade_start = 0;
        }
        last_bg_revision = a.games_revision;
        return;
    }

    if (curr_idx != last_bg_selected) {
        int from_idx = last_bg_selected;
        if (bg_fade_start != 0) {
            const float prev_t = ease_out(static_cast<float>(now - bg_fade_start) /
                                          static_cast<float>(State::motion_duration));
            if (prev_t < 0.5f)
                from_idx = last_bg_previous;
        }
        last_bg_previous = from_idx;
        last_bg_selected = curr_idx;
        set_bg_image(ui_objs.bg_prev, ui_objs.bg_prev_ref, a, from_idx);
        set_bg_image(ui_objs.bg_curr, ui_objs.bg_curr_ref, a, curr_idx);
        bg_fade_start = a.moving ? a.motion_start : now;
    }

    if (bg_fade_start != 0) {
        const float raw = static_cast<float>(now - bg_fade_start) /
                          static_cast<float>(State::motion_duration);
        if (raw >= 1.0f) {
            set_bg_image(ui_objs.bg_prev, ui_objs.bg_prev_ref, a, last_bg_selected);
            set_visible(ui_objs.bg_curr, false);
            ui_objs.bg_curr_ref.reset();
            last_bg_previous = last_bg_selected;
            bg_fade_start = 0;
        } else if (ui_objs.bg_curr_ref && ui_objs.bg_curr_ref->valid()) {
            lv_obj_set_style_image_opa(ui_objs.bg_curr, opa_from_float(0.48f * ease_out(raw)), 0);
        }
    }
    last_bg_revision = a.games_revision;
}

void update_header_and_footer(const State &a)
{
    std::string name = "Vita2Native";
    if (a.picker)
        name = "Choose File";
    else if (a.screen == Screen::AllGames)
        name = "All Games";
    else if (a.screen == Screen::Import)
        name = "Import Game";
    else if (a.screen == Screen::GameInfo) {
        if (a.info_game >= 0 && a.info_game < static_cast<int>(a.games.size()))
            name = a.games[a.info_game].name;
        else
            name = "Game Info";
    }
    if (last_header_text != name || last_mode == ViewMode::None) {
        lv_label_set_text(ui_objs.header_screen, name.c_str());
        last_header_text = std::move(name);
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

    const int rc = static_cast<int>(ui_objs.recent_cards.size());
    if (rc == 0) {
        const float entry = entrance(a, now, 65);
        set_pos_if_changed(ui_objs.home_empty, sc(25), sc(107.0f + 12.0f * (1.0f - entry)));
        set_opa_if_changed(ui_objs.home_empty, opa_from_float(entry));
    } else {
        const float current_offset = a.motion_offset(now);
        constexpr float pitch = 320.0f;
        const bool recent_focused = (a.home_focus == HomeFocus::RecentGames);
        const int center_rel = static_cast<int>(std::lround(-current_offset));

        for (int id = 0; id < rc; ++id) {
            int rel = 0;
            if (rc == 2) {
                rel = (id == a.selected) ? 0 : (current_offset >= 0.0f ? -1 : 1);
            } else {
                int d = ((id - a.selected - center_rel) % rc + rc) % rc;
                if (d > rc / 2)
                    d -= rc;
                rel = center_rel + d;
            }
            const float slot = static_cast<float>(rel) + current_offset;
            const float active = (id == a.selected) ? 1.0f : 0.0f;
            const float entry = entrance(a, now, 65 + std::max(0, rel) * 50);
            auto &card = ui_objs.recent_cards[id];
            set_pos_if_changed(card.root, sc(25.0f + slot * pitch),
                               sc(4.0f + 12.0f * (1.0f - entry)));
            set_opa_if_changed(card.root, opa_from_float(entry));

            const lv_color_t edge_c = recent_focused ? rgb(248, 252, 255) : rgb(255, 255, 255);
            const lv_opa_t edge_opa = static_cast<lv_opa_t>(
                recent_focused ? std::lround(55.0f + 200.0f * active)
                               : std::lround(45.0f + 55.0f * active));
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
}

void sync_all_games(const State &a, Uint32 now)
{
    const float heading = entrance(a, now);
    const float head_off = 8.0f * (1.0f - heading);
    set_opa_if_changed(ui_objs.all_heading, opa_from_float(heading));
    set_pos_if_changed(ui_objs.all_heading, sc(25), sc(72.0f + head_off));
    set_opa_if_changed(ui_objs.all_count, opa_from_float(heading));
    lv_obj_align(ui_objs.all_count, LV_ALIGN_TOP_RIGHT, -sc(25), sc(79.0f + head_off));

    const int count = static_cast<int>(ui_objs.all_cards.size());
    if (count == 0) {
        const float show = entrance(a, now, 60);
        set_pos_if_changed(ui_objs.all_empty, sc(25), sc(111.0f + 10.0f * (1.0f - show)));
        set_opa_if_changed(ui_objs.all_empty, opa_from_float(show));
        set_visible(ui_objs.all_scrollbar_track, false);
        set_visible(ui_objs.all_scrollbar_thumb, false);
    } else {
        const int focus_row = a.all_focus / 2;
        if (focus_row < all_scroll_row)
            all_scroll_row = focus_row;
        if (focus_row >= all_scroll_row + 2)
            all_scroll_row = focus_row - 1;
        const int total_rows = (count + 1) / 2;
        all_scroll_row = std::clamp(all_scroll_row, 0, std::max(0, total_rows - 2));

        const float scroll_y = static_cast<float>(all_scroll_row * 150);
        for (int i = 0; i < count; ++i) {
            const int row = i / 2;
            const int vis_idx = std::max(0, (row - all_scroll_row) * 2 + (i % 2));
            const float show = entrance(a, now, 60 + std::min(vis_idx, 3) * 60);
            const float x = 25 + (i % 2) * 301;
            const float y = 4 + row * 150 - scroll_y + 10.0f * (1.0f - show);
            auto &card = ui_objs.all_cards[i];
            set_pos_if_changed(card.root, sc(x), sc(y));
            set_opa_if_changed(card.root, opa_from_float(show));
            if (last_all_focus != a.all_focus || last_mode != ViewMode::AllGames) {
                const bool focused = (a.all_focus == i);
                lv_obj_set_style_border_color(
                    card.border, focused ? rgb(248, 252, 255) : rgb(255, 255, 255), 0);
                lv_obj_set_style_border_opa(card.border, focused ? 255 : 60, 0);
                lv_obj_set_style_border_width(card.border, sc_border(focused ? 2 : 1), 0);
            }
        }

        const bool need_scroll = total_rows > 2;
        set_visible(ui_objs.all_scrollbar_track, need_scroll);
        set_visible(ui_objs.all_scrollbar_thumb, need_scroll);
        if (need_scroll) {
            constexpr float track_h = 286.0f;
            const float thumb_h = std::max(36.0f, track_h * 2.0f / total_rows);
            const float max_scroll = static_cast<float>(total_rows - 2);
            const float thumb_y = 111.0f + (track_h - thumb_h) * (all_scroll_row / max_scroll);
            lv_obj_set_height(ui_objs.all_scrollbar_thumb, sc(thumb_h));
            set_pos_if_changed(ui_objs.all_scrollbar_thumb, sc(626), sc(thumb_y));
        }
    }
    last_all_focus = a.all_focus;
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

void sync_game_info(const State &a, Uint32 now)
{
    if (a.info_game < 0 || a.info_game >= static_cast<int>(a.games.size()))
        return;
    const auto &game = a.games[a.info_game];

    const bool rebuilt = (last_info_game != a.info_game || last_info_revision != a.games_revision ||
                          last_mode != ViewMode::GameInfo);
    if (rebuilt) {
        ui_objs.info_boxart_ref = get_cached_image(game_card_art(game), sc(120), sc(152));
        if (ui_objs.info_boxart_ref && ui_objs.info_boxart_ref->valid()) {
            lv_image_set_src(ui_objs.info_boxart_img, &ui_objs.info_boxart_ref->dsc);
            set_visible(ui_objs.info_boxart_img, true);
            set_visible(ui_objs.info_boxart_fallback, false);
        } else {
            set_visible(ui_objs.info_boxart_img, false);
            lv_label_set_text(ui_objs.info_boxart_fallback, game.title_id.c_str());
            set_visible(ui_objs.info_boxart_fallback, true);
        }

        lv_label_set_text(ui_objs.info_badge_id_label, game.title_id.c_str());

        set_visible(ui_objs.info_badge_cat, !game.category.empty());
        if (!game.category.empty())
            lv_label_set_text(ui_objs.info_badge_cat_label, game.category.c_str());

        set_visible(ui_objs.info_badge_rating, !game.rating.empty());
        if (!game.rating.empty())
            lv_label_set_text(ui_objs.info_badge_rating_label, game.rating.c_str());

        lv_label_set_text(ui_objs.info_desc,
                          game.description.empty() ? "No description available."
                                                   : game.description.c_str());

        ui_objs.info_shots.clear();
        lv_obj_clean(ui_objs.info_shot_strip);
        const int shot_count = static_cast<int>(game.screenshots.size());
        for (int i = 0; i < shot_count; ++i) {
            ScreenshotCard scard;
            scard.root = make_box(ui_objs.info_shot_strip, i * 276, 0, 264, 148, rgb(24, 40, 51),
                                  255, 6);
            lv_obj_set_style_clip_corner(scard.root, true, 0);
            scard.art_ref = get_cached_image(game.screenshots[i], sc(264), sc(148));
            scard.img = lv_image_create(scard.root);
            init_clean_obj(scard.img, 0, 0, 264, 148);
            if (scard.art_ref && scard.art_ref->valid())
                lv_image_set_src(scard.img, &scard.art_ref->dsc);
            scard.border = make_box(scard.root, 0, 0, 264, 148, rgb(0, 0, 0), LV_OPA_TRANSP, 6);
            lv_obj_set_style_border_color(scard.border, rgb(255, 255, 255), 0);
            lv_obj_set_style_border_opa(scard.border, 55, 0);
            lv_obj_set_style_border_width(scard.border, sc_border(1), 0);
            ui_objs.info_shots.push_back(std::move(scard));
        }

        set_visible(ui_objs.info_shot_empty, shot_count == 0);
        set_visible(ui_objs.info_shot_strip, shot_count > 0);
        set_visible(ui_objs.info_shot_count, shot_count > 0);
        info_shot_scroll = 0;
        last_info_game = a.info_game;
        last_info_revision = a.games_revision;
    }

    const float top_ent = entrance(a, now);
    set_opa_if_changed(ui_objs.info_boxart_card, opa_from_float(top_ent));
    set_pos_if_changed(ui_objs.info_boxart_card, sc(25), sc(70.0f + 8.0f * (1.0f - top_ent)));

    if (rebuilt || last_info_focus != a.info_focus) {
        const bool play_focused = (a.info_focus == GameInfoFocus::Play);
        lv_obj_set_style_bg_color(ui_objs.info_play_btn,
                                  play_focused ? rgb(225, 237, 239) : rgb(40, 62, 74), 0);
        lv_obj_set_style_text_color(ui_objs.info_play_label,
                                    play_focused ? rgb(14, 28, 35) : rgb(225, 236, 239), 0);
        lv_obj_set_style_border_color(ui_objs.info_play_btn,
                                      play_focused ? rgb(248, 252, 255) : rgb(140, 173, 184), 0);
        lv_obj_set_style_border_opa(ui_objs.info_play_btn, play_focused ? 255 : 90, 0);
        lv_obj_set_style_border_width(ui_objs.info_play_btn, sc_border(play_focused ? 2 : 1), 0);
    }

    const int shot_count = static_cast<int>(ui_objs.info_shots.size());
    if (shot_count > 0) {
        if (a.screenshot_index < info_shot_scroll)
            info_shot_scroll = a.screenshot_index;
        if (a.screenshot_index >= info_shot_scroll + 2)
            info_shot_scroll = a.screenshot_index - 1;
        info_shot_scroll = std::clamp(info_shot_scroll, 0, std::max(0, shot_count - 2));

        const float shot_ent = entrance(a, now, 120);
        set_opa_if_changed(ui_objs.info_shot_strip, opa_from_float(shot_ent));
        set_pos_if_changed(ui_objs.info_shot_strip, sc(25), sc(266.0f + 8.0f * (1.0f - shot_ent)));

        for (int i = 0; i < shot_count; ++i) {
            auto &scard = ui_objs.info_shots[i];
            set_pos_if_changed(scard.root, sc((i - info_shot_scroll) * 276), 0);
            if (rebuilt || last_info_focus != a.info_focus ||
                last_info_shot_idx != a.screenshot_index) {
                const bool active =
                    (a.info_focus == GameInfoFocus::Screenshots && i == a.screenshot_index);
                lv_obj_set_style_border_color(scard.border,
                                              active ? rgb(248, 252, 255) : rgb(255, 255, 255), 0);
                lv_obj_set_style_border_opa(scard.border, active ? 255 : 55, 0);
                lv_obj_set_style_border_width(scard.border, sc_border(active ? 2 : 1), 0);
            }
        }
        if (rebuilt || last_info_shot_idx != a.screenshot_index) {
            const std::string ratio =
                std::to_string(a.screenshot_index + 1) + " / " + std::to_string(shot_count);
            lv_label_set_text(ui_objs.info_shot_count, ratio.c_str());
            lv_obj_align(ui_objs.info_shot_count, LV_ALIGN_TOP_RIGHT, -sc(25), sc(243));
        }
    }

    last_info_focus = a.info_focus;
    last_info_shot_idx = a.screenshot_index;
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
    drop(fonts.bold_10);
    drop(fonts.bold_11);
    drop(fonts.bold_13);
    drop(fonts.bold_15);
    drop(fonts.bold_16);
    drop(fonts.bold_19);
    drop(fonts.bold_20);
    drop(fonts.bold_22);
    drop(fonts.bold_24);
    drop(fonts.icon_15);
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
    fonts.bold_10 = mk(inter_bold, sizeof(inter_bold), 10);
    fonts.bold_11 = mk(inter_bold, sizeof(inter_bold), 11);
    fonts.bold_13 = mk(inter_bold, sizeof(inter_bold), 13);
    fonts.bold_15 = mk(inter_bold, sizeof(inter_bold), 15);
    fonts.bold_16 = mk(inter_bold, sizeof(inter_bold), 16);
    fonts.bold_19 = mk(inter_bold, sizeof(inter_bold), 19);
    fonts.bold_20 = mk(inter_bold, sizeof(inter_bold), 20);
    fonts.bold_22 = mk(inter_bold, sizeof(inter_bold), 22);
    fonts.bold_24 = mk(inter_bold, sizeof(inter_bold), 24);
    fonts.icon_15 = mk(lucide_font, sizeof(lucide_font), 15);
    return fonts.reg_10 && fonts.reg_11 && fonts.reg_12 && fonts.reg_14 && fonts.bold_10 &&
           fonts.bold_11 && fonts.bold_13 && fonts.bold_15 && fonts.bold_16 && fonts.bold_19 &&
           fonts.bold_20 && fonts.bold_22 && fonts.bold_24 && fonts.icon_15;
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
    ui_objs.recent_cards.clear();
    ui_objs.all_cards.clear();
    ui_objs.info_shots.clear();
    ui_objs.bg_prev_ref.reset();
    ui_objs.bg_curr_ref.reset();
    ui_objs.info_boxart_ref.reset();
    image_cache.clear();
    image_lru.clear();
    custom_logo_header.reset();
    custom_logo_splash.reset();
    icon_dpad.reset();
    icon_key_a.reset();
    icon_key_b.reset();
    ui_objs = {};
    last_mode = ViewMode::None;
    last_confirm = ConfirmKind::None;
    last_games_revision = -1;
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
    else if (a.screen == Screen::GameInfo)
        mode = ViewMode::GameInfo;

    if (mode != last_mode) {
        set_visible(ui_objs.error_layer, mode == ViewMode::Error);
        set_visible(ui_objs.splash_layer, mode == ViewMode::Splash);
        const bool show_main = (mode != ViewMode::Error && mode != ViewMode::Splash);
        set_visible(ui_objs.main_layer, show_main);
        set_visible(ui_objs.home_layer, mode == ViewMode::Home);
        set_visible(ui_objs.all_layer, mode == ViewMode::AllGames);
        set_visible(ui_objs.import_layer, mode == ViewMode::Import);
        set_visible(ui_objs.picker_layer, mode == ViewMode::Picker);
        set_visible(ui_objs.info_layer, mode == ViewMode::GameInfo);
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

    if (last_games_revision != a.games_revision)
        rebuild_game_lists(a);

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
    if (mode == ViewMode::GameInfo)
        sync_game_info(a, now);

    float toast_p = 0.0f;
    if (!a.toast.empty()) {
        if (a.toast_hide_start != 0) {
            toast_p = 1.0f - ease_out(static_cast<float>(now - a.toast_hide_start) /
                                      static_cast<float>(State::toast_anim_duration));
        } else {
            toast_p = ease_out(static_cast<float>(now - a.toast_start) /
                               static_cast<float>(State::toast_anim_duration));
        }
    }
    if (toast_p <= 0.0f) {
        set_visible(ui_objs.toast.root, false);
        last_toast_text.clear();
    } else {
        if (last_toast_text != a.toast || last_toast_kind != a.toast_kind) {
            const ToastAppearance style = toast_appearance(a.toast_kind);
            lv_obj_set_style_border_color(ui_objs.toast.root, style.accent, 0);
            lv_obj_set_style_text_color(ui_objs.toast.icon, style.accent, 0);
            lv_label_set_text(ui_objs.toast.icon, style.glyph);

            lv_point_t sz{};
            lv_text_get_size(&sz, a.toast.c_str(), fonts.reg_11, 0, 0, LV_COORD_MAX,
                             LV_TEXT_FLAG_NONE);
            const int32_t pill_w = std::clamp(sc(51) + sz.x, sc(120), sc(540));
            ui_objs.toast.width = pill_w;
            lv_obj_set_width(ui_objs.toast.root, pill_w);
            lv_obj_set_width(ui_objs.toast.label, pill_w - sc(51));
            lv_label_set_text(ui_objs.toast.label, a.toast.c_str());
            last_toast_text = a.toast;
            last_toast_kind = a.toast_kind;
        }
        set_visible(ui_objs.toast.root, true);
        set_opa_if_changed(ui_objs.toast.root, opa_from_float(toast_p));
        const float y_off = (a.toast_hide_start != 0) ? 8.0f * (1.0f - toast_p)
                                                      : 12.0f * (1.0f - toast_p);
        set_pos_if_changed(ui_objs.toast.root, (sc(screen_width) - ui_objs.toast.width) / 2,
                           sc(388.0f + y_off));
    }

    float confirm_p = 0.0f;
    if (a.confirm != ConfirmKind::None) {
        confirm_p = a.confirm_start
                        ? ease_out(static_cast<float>(now - a.confirm_start) /
                                   static_cast<float>(State::confirm_duration))
                        : 1.0f;
    } else if (a.confirm_start && now - a.confirm_start < State::confirm_duration) {
        confirm_p = 1.0f - ease_out(static_cast<float>(now - a.confirm_start) /
                                    static_cast<float>(State::confirm_duration));
    }

    if (confirm_p <= 0.0f) {
        if (last_confirm != ConfirmKind::None || last_mode == ViewMode::None) {
            set_confirm_modal(ui_objs.confirm_modal, false, "", "", "", "");
            last_confirm = ConfirmKind::None;
        }
    } else {
        const ConfirmKind kind =
            a.confirm != ConfirmKind::None ? a.confirm : a.previous_confirm;
        if (kind != last_confirm ||
            lv_obj_has_flag(ui_objs.confirm_modal.backdrop, LV_OBJ_FLAG_HIDDEN)) {
            if (kind == ConfirmKind::Quit) {
                set_confirm_modal(ui_objs.confirm_modal, true, "Quit V2N Launcher?",
                                  "Are you sure you want to exit the launcher?", "QUIT", "CANCEL");
            }
            last_confirm = kind;
        }
        const lv_opa_t back_opa = static_cast<lv_opa_t>(std::lround(185.0f * confirm_p));
        if (lv_obj_get_style_bg_opa(ui_objs.confirm_modal.backdrop, LV_PART_MAIN) != back_opa)
            lv_obj_set_style_bg_opa(ui_objs.confirm_modal.backdrop, back_opa, 0);
        set_opa_if_changed(ui_objs.confirm_modal.card, opa_from_float(confirm_p));
        set_pos_if_changed(ui_objs.confirm_modal.card, sc(130),
                           sc(163.0f + 12.0f * (1.0f - confirm_p)));
    }

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
