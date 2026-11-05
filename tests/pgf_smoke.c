/*
 * Checks the synthetic ScePgf font: library/font lifetime, metrics, character
 * info, and 8-bit glyph rasterisation into a caller-provided atlas.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>

#include <psp2/pgf.h>

int main(void)
{
#define ATLAS_SIZE (64 * 64)
    static uint8_t stack_atlas[ATLAS_SIZE];
    uint8_t *atlas = stack_atlas;
    SceFontNewLibParams params;
    SceFontStyle style;
    SceFontInfo info;
    SceFontCharInfo char_info;
    SceFontImageRect image_rect;
    SceFontGlyphImage glyph;
    SceFontLibHandle lib;
    SceFontHandle font;
    unsigned int error = 1;
    int nonzero = 0;

#if defined(__x86_64__) && defined(MAP_32BIT)
    /* SceFontGlyphImage::bufferPtr is a 32-bit guest address. */
    {
        void *low = mmap(NULL, ATLAS_SIZE, PROT_READ | PROT_WRITE,
                         MAP_PRIVATE | MAP_ANONYMOUS | MAP_32BIT, -1, 0);

        if (low != MAP_FAILED)
            atlas = low;
    }
#endif

    memset(&params, 0, sizeof(params));
    params.numFonts = 1;

    lib = sceFontNewLib(&params, &error);
    if (!lib || error != 0) {
        puts("pgf_smoke: new lib failed");
        return 1;
    }

    memset(&style, 0, sizeof(style));
    if (sceFontFindOptimumFont(lib, &style, &error) != 0 || error != 0) {
        puts("pgf_smoke: find optimum font failed");
        return 1;
    }

    font = sceFontOpen(lib, 0, 0, &error);
    if (!font || error != 0) {
        puts("pgf_smoke: open failed");
        return 1;
    }

    memset(&info, 0, sizeof(info));
    if (sceFontGetFontInfo(font, &info) != 0) {
        puts("pgf_smoke: get font info failed");
        return 1;
    }
    if (info.BPP != 8 || info.maxGlyphWidth != 12 || info.maxGlyphHeight != 16 ||
        info.fontStyle.fontV != 8.0f || info.fontStyle.fontVRes != 80.0f) {
        printf("pgf_smoke: wrong font info (bpp %u, %dx%d, v %.1f/%.1f)\n", info.BPP,
               info.maxGlyphWidth, info.maxGlyphHeight, info.fontStyle.fontV,
               info.fontStyle.fontVRes);
        return 1;
    }

    memset(&char_info, 0, sizeof(char_info));
    if (sceFontGetCharInfo(font, 'A', &char_info) != 0) {
        puts("pgf_smoke: get char info failed");
        return 1;
    }
    if (char_info.bitmapWidth != 12 || char_info.bitmapHeight != 16 ||
        char_info.bitmapTop != 16 || char_info.sfp26AdvanceH != (12 << 6)) {
        printf("pgf_smoke: wrong char info (%ux%u top %u adv %d)\n", char_info.bitmapWidth,
               char_info.bitmapHeight, char_info.bitmapTop, char_info.sfp26AdvanceH);
        return 1;
    }

    if (sceFontGetCharImageRect(font, 'A', &image_rect) != 0 || image_rect.width != 12 ||
        image_rect.height != 16) {
        puts("pgf_smoke: wrong image rect");
        return 1;
    }

    memset(atlas, 0, ATLAS_SIZE);
    memset(&glyph, 0, sizeof(glyph));
    glyph.pixelFormat = SCE_FONT_PIXELFORMAT_8;
    glyph.xPos64 = 3 << 6;
    glyph.yPos64 = 4 << 6;
    glyph.bufWidth = 64;
    glyph.bufHeight = 64;
    glyph.bytesPerLine = 64;
    glyph.bufferPtr = (unsigned int)(uintptr_t)atlas;

    if (sceFontGetCharGlyphImage(font, 'A', &glyph) != 0) {
        puts("pgf_smoke: get glyph image failed");
        return 1;
    }
    for (unsigned int i = 0; i < ATLAS_SIZE; i++)
        nonzero |= atlas[i] != 0;
    if (!nonzero) {
        puts("pgf_smoke: glyph is empty");
        return 1;
    }
    /* Nothing may be written before the requested origin. */
    for (unsigned int y = 0; y < 4; y++)
        for (unsigned int x = 0; x < 64; x++)
            if (atlas[y * 64 + x] != 0) {
                puts("pgf_smoke: glyph wrote outside its origin");
                return 1;
            }

    if (sceFontClose(font) != 0 || sceFontDoneLib(lib) != 0) {
        puts("pgf_smoke: cleanup failed");
        return 1;
    }

#if defined(__x86_64__) && defined(MAP_32BIT)
    if (atlas != stack_atlas)
        munmap(atlas, ATLAS_SIZE);
#endif

    puts("pgf_smoke: ok");
    return 0;
}
