/*
 * ScePgf: the Vita font library.
 *
 * There is no public specification for the system PGF font container, and the
 * files are not redistributable. This implementation instead exposes a
 * synthetic font with real metrics and glyph bitmaps from the public-domain
 * 8x8 VGA font scaled to 12x16. Titles that only need the SceFont API to render
 * text (VitaShell, most homebrew) work unchanged; the glyphs are readable
 * ASCII. Non-ASCII code points map to '?'.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <psp2/pgf.h>

#include "module.h"

#include "font8x8_basic.h"

#define FONT_LIB_MAGIC 0x464C4942u /* "FLIB" */
#define FONT_MAGIC     0x464F4E54u /* "FONT" */

#define GLYPH_WIDTH       12
#define GLYPH_HEIGHT      16
#define GLYPH_ADVANCE_W   (GLYPH_WIDTH << 6)  /* 26.6 fixed point */
#define GLYPH_ADVANCE_H   (GLYPH_HEIGHT << 6) /* 26.6 fixed point */
#define FONT_DPI          220.0f

typedef struct {
    uint32_t magic;
    uint32_t num_fonts;
    float hres;
    float vres;
} VitaFontLib;

typedef struct {
    uint32_t magic;
    VitaFontLib *lib;
} VitaFont;

static void set_error(unsigned int *error, unsigned int value)
{
    if (error)
        *error = value;
}

static VitaFontLib *lib_from_handle(SceFontLibHandle handle)
{
    VitaFontLib *lib = handle;

    if (!lib || lib->magic != FONT_LIB_MAGIC)
        return NULL;
    return lib;
}

static VitaFont *font_from_handle(SceFontHandle handle)
{
    VitaFont *font = handle;

    if (!font || font->magic != FONT_MAGIC)
        return NULL;
    return font;
}

static SceFontHandle open_font(SceFontLibHandle lib_handle, unsigned int *error)
{
    VitaFontLib *lib = lib_from_handle(lib_handle);
    VitaFont *font;

    if (!lib) {
        set_error(error, SCE_FONT_ERROR_INVALID_LIBID);
        return NULL;
    }

    font = calloc(1, sizeof(*font));
    if (!font) {
        set_error(error, SCE_FONT_ERROR_OUT_OF_MEMORY);
        return NULL;
    }
    font->magic = FONT_MAGIC;
    font->lib = lib;

    set_error(error, 0);
    return font;
}

EXPORT(ScePgf, 0x1055ABA3, SceFontLibHandle, sceFontNewLib, SceFontNewLibParams *params,
       unsigned int *error)
{
    VitaFontLib *lib;

    if (!params) {
        set_error(error, SCE_FONT_ERROR_INVALID_PARAMETER);
        return NULL;
    }

    lib = calloc(1, sizeof(*lib));
    if (!lib) {
        set_error(error, SCE_FONT_ERROR_OUT_OF_MEMORY);
        return NULL;
    }
    lib->magic = FONT_LIB_MAGIC;
    lib->num_fonts = params->numFonts ? params->numFonts : 1;
    lib->hres = 80.0f;
    lib->vres = 80.0f;

    set_error(error, 0);
    return lib;
}

EXPORT(ScePgf, 0x07EE1733, int, sceFontDoneLib, SceFontLibHandle lib_handle)
{
    VitaFontLib *lib = lib_from_handle(lib_handle);

    if (!lib)
        return SCE_FONT_ERROR_INVALID_LIBID;
    lib->magic = 0;
    free(lib);
    return 0;
}

EXPORT(ScePgf, 0xBD2DFCFF, SceFontHandle, sceFontOpen, SceFontLibHandle lib_handle, int index,
       int mode, unsigned int *error)
{
    (void)index;
    (void)mode;
    return open_font(lib_handle, error);
}

EXPORT(ScePgf, 0xE260E740, SceFontHandle, sceFontOpenUserFile, SceFontLibHandle lib_handle,
       char *file, int mode, unsigned int *error)
{
    (void)file;
    (void)mode;
    return open_font(lib_handle, error);
}

EXPORT(ScePgf, 0xB23ED47C, SceFontHandle, sceFontOpenUserMemory, SceFontLibHandle lib_handle,
       void *font_data, SceSize font_size, unsigned int *error)
{
    (void)font_data;
    (void)font_size;
    return open_font(lib_handle, error);
}

EXPORT(ScePgf, 0x4A7293E9, int, sceFontClose, SceFontHandle font_handle)
{
    VitaFont *font = font_from_handle(font_handle);

    if (!font)
        return SCE_FONT_ERROR_INVALID_PARAMETER;
    font->magic = 0;
    free(font);
    return 0;
}

EXPORT(ScePgf, 0x9F842307, int, sceFontGetNumFontList, SceFontLibHandle lib_handle,
       unsigned int *error)
{
    VitaFontLib *lib = lib_from_handle(lib_handle);

    if (!lib) {
        set_error(error, SCE_FONT_ERROR_INVALID_LIBID);
        return SCE_FONT_ERROR_INVALID_LIBID;
    }
    set_error(error, 0);
    return 1;
}

EXPORT(ScePgf, 0x8DFBAE1B, int, sceFontFindOptimumFont, SceFontLibHandle lib_handle,
       SceFontStyle *style, unsigned int *error)
{
    VitaFontLib *lib = lib_from_handle(lib_handle);

    (void)style;

    if (!lib) {
        set_error(error, SCE_FONT_ERROR_INVALID_LIBID);
        return SCE_FONT_ERROR_INVALID_LIBID;
    }
    set_error(error, 0);
    return 0;
}

EXPORT(ScePgf, 0x51061D87, int, sceFontFindFont, SceFontLibHandle lib_handle,
       SceFontStyle *style, unsigned int *error)
{
    return sceFontFindOptimumFont(lib_handle, style, error);
}

static void fill_style(SceFontStyle *style, float hres, float vres)
{
    memset(style, 0, sizeof(*style));
    style->fontH = 8.0f;
    style->fontV = 8.0f;
    style->fontHRes = hres;
    style->fontVRes = vres;
    style->fontWeight = 1.0f;
    style->fontFamily = SCE_FONT_FAMILY_SANS_SERIF;
    style->fontStyle = SCE_FONT_STYLE_REGULAR;
    style->fontLanguage = SCE_FONT_LANGUAGE_DEFAULT;
    snprintf(style->fontName, sizeof(style->fontName), "vita2native builtin");
    snprintf(style->fontFileName, sizeof(style->fontFileName), "builtin");
}

EXPORT(ScePgf, 0xD56DCCEA, int, sceFontGetFontList, SceFontLibHandle lib_handle,
       SceFontStyle *fontStyle, int numFonts)
{
    VitaFontLib *lib = lib_from_handle(lib_handle);

    if (!lib)
        return SCE_FONT_ERROR_INVALID_LIBID;
    if (!fontStyle || numFonts <= 0)
        return 0;

    fill_style(&fontStyle[0], lib->hres, lib->vres);
    return 1;
}

EXPORT(ScePgf, 0xAB034738, int, sceFontGetFontInfoByIndexNumber, SceFontLibHandle lib_handle,
       SceFontStyle *fontStyle, int unknown, int font_index)
{
    VitaFontLib *lib = lib_from_handle(lib_handle);

    (void)unknown;
    (void)font_index;

    if (!lib)
        return SCE_FONT_ERROR_INVALID_LIBID;
    if (!fontStyle)
        return SCE_FONT_ERROR_INVALID_PARAMETER;

    fill_style(fontStyle, lib->hres, lib->vres);
    return 0;
}

EXPORT(ScePgf, 0xF9414FA2, int, sceFontGetFontInfo, SceFontHandle font_handle,
       SceFontInfo *font_info)
{
    VitaFont *font = font_from_handle(font_handle);

    if (!font)
        return SCE_FONT_ERROR_INVALID_PARAMETER;
    if (!font_info)
        return SCE_FONT_ERROR_INVALID_PARAMETER;

    memset(font_info, 0, sizeof(*font_info));

    font_info->maxGlyphWidthI = GLYPH_ADVANCE_W;
    font_info->maxGlyphHeightI = GLYPH_ADVANCE_H;
    font_info->maxGlyphAscenderI = GLYPH_ADVANCE_H;
    font_info->maxGlyphDescenderI = 0;
    font_info->maxGlyphLeftXI = 0;
    font_info->maxGlyphBaseYI = GLYPH_ADVANCE_H;
    font_info->minGlyphCenterXI = 0;
    font_info->maxGlyphTopYI = GLYPH_ADVANCE_H;
    font_info->maxGlyphAdvanceXI = GLYPH_ADVANCE_W;
    font_info->maxGlyphAdvanceYI = 0;

    font_info->maxGlyphWidthF = (float)GLYPH_WIDTH;
    font_info->maxGlyphHeightF = (float)GLYPH_HEIGHT;
    font_info->maxGlyphAscenderF = (float)GLYPH_HEIGHT;
    font_info->maxGlyphDescenderF = 0.0f;
    font_info->maxGlyphLeftXF = 0.0f;
    font_info->maxGlyphBaseYF = (float)GLYPH_HEIGHT;
    font_info->minGlyphCenterXF = 0.0f;
    font_info->maxGlyphTopYF = (float)GLYPH_HEIGHT;
    font_info->maxGlyphAdvanceXF = (float)GLYPH_WIDTH;
    font_info->maxGlyphAdvanceYF = 0.0f;

    font_info->maxGlyphWidth = GLYPH_WIDTH;
    font_info->maxGlyphHeight = GLYPH_HEIGHT;
    fill_style(&font_info->fontStyle, font->lib->hres, font->lib->vres);
    font_info->BPP = 8;

    return 0;
}

EXPORT(ScePgf, 0x6FD1BA65, int, sceFontGetCharInfo, SceFontHandle font_handle,
       unsigned int char_code, SceFontCharInfo *char_info)
{
    VitaFont *font = font_from_handle(font_handle);

    if (!font)
        return SCE_FONT_ERROR_INVALID_PARAMETER;
    if (!char_info)
        return SCE_FONT_ERROR_INVALID_PARAMETER;

    memset(char_info, 0, sizeof(*char_info));
    char_info->bitmapWidth = GLYPH_WIDTH;
    char_info->bitmapHeight = GLYPH_HEIGHT;
    char_info->bitmapLeft = 0;
    char_info->bitmapTop = GLYPH_HEIGHT;

    char_info->sfp26Width = GLYPH_ADVANCE_W;
    char_info->sfp26Height = GLYPH_ADVANCE_H;
    char_info->sfp26Ascender = GLYPH_ADVANCE_H;
    char_info->sfp26Descender = 0;
    char_info->sfp26BearingHX = 0;
    char_info->sfp26BearingHY = 0;
    char_info->sfp26BearingVX = 0;
    char_info->sfp26BearingVY = 0;
    char_info->sfp26AdvanceH = GLYPH_ADVANCE_W;
    char_info->sfp26AdvanceV = 0;

    (void)char_code;
    return 0;
}

EXPORT(ScePgf, 0x70C86B3E, int, sceFontGetCharImageRect, SceFontHandle font_handle,
       unsigned int char_code, SceFontImageRect *char_rect)
{
    VitaFont *font = font_from_handle(font_handle);

    (void)char_code;

    if (!font)
        return SCE_FONT_ERROR_INVALID_PARAMETER;
    if (!char_rect)
        return SCE_FONT_ERROR_INVALID_PARAMETER;

    char_rect->width = GLYPH_WIDTH;
    char_rect->height = GLYPH_HEIGHT;
    return 0;
}

static void draw_glyph(unsigned int char_code, SceFontGlyphImage *image)
{
    unsigned int index = char_code;
    uint8_t *base;
    unsigned int bpl;
    int origin_x;
    int origin_y;

    if (index < 0x20 || index > 0x7E)
        index = '?';

    base = (uint8_t *)(uintptr_t)image->bufferPtr;
    bpl = image->bytesPerLine ? image->bytesPerLine : image->bufWidth;
    origin_x = image->xPos64 >> 6;
    origin_y = image->yPos64 >> 6;

    for (unsigned int y = 0; y < GLYPH_HEIGHT; y++) {
        int dst_y = origin_y + (int)y;

        if (dst_y < 0 || dst_y >= (int)image->bufHeight)
            continue;

        for (unsigned int x = 0; x < GLYPH_WIDTH; x++) {
            int dst_x = origin_x + (int)x;
            unsigned int src_x = (x * 8) / GLYPH_WIDTH;
            unsigned int src_y = (y * 8) / GLYPH_HEIGHT;
            int on;

            if (dst_x < 0 || dst_x >= (int)image->bufWidth)
                continue;

            on = (font8x8_basic[index][src_y] >> src_x) & 1;
            base[(size_t)dst_y * bpl + (size_t)dst_x] = on ? 0xFF : 0x00;
        }
    }
}

EXPORT(ScePgf, 0xAB45AAD3, int, sceFontGetCharGlyphImage, SceFontHandle font_handle,
       unsigned int char_code, SceFontGlyphImage *glyph_image)
{
    VitaFont *font = font_from_handle(font_handle);

    if (!font)
        return SCE_FONT_ERROR_INVALID_PARAMETER;
    if (!glyph_image || !glyph_image->bufferPtr || glyph_image->bufWidth == 0 ||
        glyph_image->bufHeight == 0)
        return SCE_FONT_ERROR_INVALID_PARAMETER;
    if (glyph_image->pixelFormat != SCE_FONT_PIXELFORMAT_8)
        return SCE_FONT_ERROR_INVALID_PARAMETER;

    draw_glyph(char_code, glyph_image);
    return 0;
}

EXPORT(ScePgf, 0xEB589530, int, sceFontGetCharGlyphImage_Clip, SceFontHandle font_handle,
       unsigned int char_code, SceFontGlyphImage *glyph_image, int clip_x, int clip_y,
       int clip_width, int clip_height)
{
    (void)clip_x;
    (void)clip_y;
    (void)clip_width;
    (void)clip_height;

    return sceFontGetCharGlyphImage(font_handle, char_code, glyph_image);
}

static int font_set_resolution_softfp(SceFontLibHandle lib_handle, float hres, float vres)
    GUEST_SOFTFP_ABI;
static int font_set_resolution_softfp(SceFontLibHandle lib_handle, float hres, float vres)
{
    VitaFontLib *lib = lib_from_handle(lib_handle);

    if (!lib)
        return SCE_FONT_ERROR_INVALID_LIBID;
    lib->hres = hres;
    lib->vres = vres;
    return 0;
}
EXPORT_FUNC(ScePgf, 0xDE47674C, font_set_resolution_softfp);

EXPORT(ScePgf, 0x8D5B44DF, int, sceFontSetAltCharacterCode, SceFontLibHandle lib_handle,
       unsigned int char_code)
{
    VitaFontLib *lib = lib_from_handle(lib_handle);

    (void)char_code;

    if (!lib)
        return SCE_FONT_ERROR_INVALID_LIBID;
    return 0;
}

EXPORT(ScePgf, 0x7D8CB13B, int, sceFontFlush, SceFontHandle font_handle)
{
    VitaFont *font = font_from_handle(font_handle);

    if (!font)
        return SCE_FONT_ERROR_INVALID_PARAMETER;
    return 0;
}

/* Float-argument/return entry points use the guest softfp ABI. */

static float font_pixel_to_point_h_softfp(SceFontLibHandle lib_handle, float pixels,
                                          unsigned int *error) GUEST_SOFTFP_ABI;
static float font_pixel_to_point_h_softfp(SceFontLibHandle lib_handle, float pixels,
                                          unsigned int *error)
{
    VitaFontLib *lib = lib_from_handle(lib_handle);

    if (!lib) {
        set_error(error, SCE_FONT_ERROR_INVALID_LIBID);
        return 0.0f;
    }
    set_error(error, 0);
    return pixels * 72.0f / lib->hres;
}
EXPORT_FUNC(ScePgf, 0x9E38F4D6, font_pixel_to_point_h_softfp);

static float font_pixel_to_point_v_softfp(SceFontLibHandle lib_handle, float pixels,
                                          unsigned int *error) GUEST_SOFTFP_ABI;
static float font_pixel_to_point_v_softfp(SceFontLibHandle lib_handle, float pixels,
                                          unsigned int *error)
{
    VitaFontLib *lib = lib_from_handle(lib_handle);

    if (!lib) {
        set_error(error, SCE_FONT_ERROR_INVALID_LIBID);
        return 0.0f;
    }
    set_error(error, 0);
    return pixels * 72.0f / lib->vres;
}
EXPORT_FUNC(ScePgf, 0x7B45E2D1, font_pixel_to_point_v_softfp);

static float font_point_to_pixel_h_softfp(SceFontLibHandle lib_handle, float points,
                                          unsigned int *error) GUEST_SOFTFP_ABI;
static float font_point_to_pixel_h_softfp(SceFontLibHandle lib_handle, float points,
                                          unsigned int *error)
{
    VitaFontLib *lib = lib_from_handle(lib_handle);

    if (!lib) {
        set_error(error, SCE_FONT_ERROR_INVALID_LIBID);
        return 0.0f;
    }
    set_error(error, 0);
    return points * lib->hres / 72.0f;
}
EXPORT_FUNC(ScePgf, 0x39B9AEFF, font_point_to_pixel_h_softfp);

static float font_point_to_pixel_v_softfp(SceFontLibHandle lib_handle, float points,
                                          unsigned int *error) GUEST_SOFTFP_ABI;
static float font_point_to_pixel_v_softfp(SceFontLibHandle lib_handle, float points,
                                          unsigned int *error)
{
    VitaFontLib *lib = lib_from_handle(lib_handle);

    if (!lib) {
        set_error(error, SCE_FONT_ERROR_INVALID_LIBID);
        return 0.0f;
    }
    set_error(error, 0);
    return points * lib->vres / 72.0f;
}
EXPORT_FUNC(ScePgf, 0x03F10EC8, font_point_to_pixel_v_softfp);

DECLARE_LIBRARY(ScePgf, 0x9F3504F2);
