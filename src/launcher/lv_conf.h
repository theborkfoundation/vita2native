/**
 * @file lv_conf.h
 * LVGL configuration for v2n-launcher (v9.5.0).
 */

#ifndef LV_CONF_H
#define LV_CONF_H

/*====================
   COLOR SETTINGS
 *====================*/

#define LV_COLOR_DEPTH 32

/*=========================
   STDLIB WRAPPER SETTINGS
 *=========================*/

#define LV_USE_STDLIB_MALLOC    LV_STDLIB_CLIB
#define LV_USE_STDLIB_STRING    LV_STDLIB_CLIB
#define LV_USE_STDLIB_SPRINTF   LV_STDLIB_CLIB

#define LV_STDINT_INCLUDE       <stdint.h>
#define LV_STDDEF_INCLUDE       <stddef.h>
#define LV_STDBOOL_INCLUDE      <stdbool.h>
#define LV_INTTYPES_INCLUDE     <inttypes.h>
#define LV_LIMITS_INCLUDE       <limits.h>
#define LV_STDARG_INCLUDE       <stdarg.h>

/*====================
   HAL SETTINGS
 *====================*/

#define LV_DEF_REFR_PERIOD      16
#define LV_DPI_DEF              130

/*=================
 * OPERATING SYSTEM
 *=================*/

#define LV_USE_OS               LV_OS_NONE

/*========================
 * RENDERING CONFIGURATION
 *========================*/

#define LV_DRAW_BUF_STRIDE_ALIGN        1
#define LV_DRAW_BUF_ALIGN               4
#define LV_DRAW_TRANSFORM_USE_MATRIX    0
#define LV_DRAW_LAYER_SIMPLE_BUF_SIZE   (64 * 1024)
#define LV_DRAW_LAYER_MAX_MEMORY        0

#define LV_USE_DRAW_SW                  1
#if LV_USE_DRAW_SW == 1
    #define LV_DRAW_SW_SUPPORT_RGB565               0
    #define LV_DRAW_SW_SUPPORT_RGB565_SWAPPED       0
    #define LV_DRAW_SW_SUPPORT_RGB565A8             0
    #define LV_DRAW_SW_SUPPORT_RGB888               1
    #define LV_DRAW_SW_SUPPORT_XRGB8888             1
    #define LV_DRAW_SW_SUPPORT_ARGB8888             1
    #define LV_DRAW_SW_SUPPORT_ARGB8888_PREMULTIPLIED 1
    #define LV_DRAW_SW_SUPPORT_L8                   0
    #define LV_DRAW_SW_SUPPORT_AL88                 0
    #define LV_DRAW_SW_SUPPORT_A8                   1
    #define LV_DRAW_SW_SUPPORT_I1                   0
    #define LV_DRAW_SW_DRAW_UNIT_CNT                1
    #define LV_DRAW_SW_COMPLEX                      1
    #define LV_DRAW_SW_SHADOW_CACHE_SIZE            0
    #define LV_DRAW_SW_CIRCLE_CACHE_SIZE            8
    #define LV_USE_DRAW_SW_ASM                      LV_DRAW_SW_ASM_NONE
    #define LV_USE_DRAW_SW_COMPLEX_GRADIENTS        0
#endif

/*=======================
 * FEATURE CONFIGURATION
 *=======================*/

#define LV_USE_LOG              1
#if LV_USE_LOG
    #define LV_LOG_LEVEL        LV_LOG_LEVEL_WARN
    #define LV_LOG_PRINTF       1
    #define LV_LOG_USE_TIMESTAMP 0
    #define LV_LOG_USE_FILE_LINE 1
    #define LV_LOG_TRACE_MEM        0
    #define LV_LOG_TRACE_TIMER      0
    #define LV_LOG_TRACE_INDEV      0
    #define LV_LOG_TRACE_DISP_REFR  0
    #define LV_LOG_TRACE_EVENT      0
    #define LV_LOG_TRACE_OBJ_CREATE 0
    #define LV_LOG_TRACE_LAYOUT     0
    #define LV_LOG_TRACE_ANIM       0
    #define LV_LOG_TRACE_CACHE      0
#endif

#define LV_USE_ASSERT_NULL          1
#define LV_USE_ASSERT_MALLOC        1
#define LV_USE_ASSERT_STYLE         0
#define LV_USE_ASSERT_MEM_INTEGRITY 0
#define LV_USE_ASSERT_OBJ           0
#define LV_ASSERT_HANDLER_INCLUDE   <stdlib.h>
#define LV_ASSERT_HANDLER           abort();

#define LV_CACHE_DEF_SIZE               (4 * 1024 * 1024)
#define LV_IMAGE_HEADER_CACHE_DEF_CNT   16
#define LV_GRADIENT_MAX_STOPS           2
#define LV_OBJ_STYLE_CACHE              1

#define LV_USE_FLOAT                    0
#define LV_USE_MATRIX                   0

/*==================
 *   FONT USAGE
 *===================*/

#define LV_FONT_MONTSERRAT_14           1
#define LV_FONT_DEFAULT                 &lv_font_montserrat_14
#define LV_USE_FONT_PLACEHOLDER         1
#define LV_TXT_ENC                      LV_TXT_ENC_UTF8

/*==================
 * WIDGETS
 *================*/

#define LV_WIDGETS_HAS_DEFAULT_VALUE    0

#define LV_USE_ANIMIMG      0
#define LV_USE_ARC          0
#define LV_USE_ARCLABEL     0
#define LV_USE_BAR          1
#define LV_USE_BUTTON       0
#define LV_USE_BUTTONMATRIX 0
#define LV_USE_CALENDAR     0
#define LV_USE_CANVAS       0
#define LV_USE_CHART        0
#define LV_USE_CHECKBOX     0
#define LV_USE_DROPDOWN     0
#define LV_USE_IMAGE        1
#define LV_USE_IMAGEBUTTON  0
#define LV_USE_KEYBOARD     0
#define LV_USE_LABEL        1
#if LV_USE_LABEL
    #define LV_LABEL_TEXT_SELECTION 0
    #define LV_LABEL_LONG_TXT_HINT  0
#endif
#define LV_USE_LED          0
#define LV_USE_LINE         1
#define LV_USE_LIST         0
#define LV_USE_LOTTIE       0
#define LV_USE_MENU         0
#define LV_USE_MSGBOX       0
#define LV_USE_ROLLER       0
#define LV_USE_SCALE        0
#define LV_USE_SLIDER       0
#define LV_USE_SPAN         0
#define LV_USE_SPINBOX      0
#define LV_USE_SPINNER      0
#define LV_USE_SWITCH       0
#define LV_USE_TABLE        0
#define LV_USE_TABVIEW      0
#define LV_USE_TEXTAREA     0
#define LV_USE_TILEVIEW     0
#define LV_USE_WIN          0

/*==================
 * THEMES
 *==================*/

#define LV_USE_THEME_DEFAULT    0
#define LV_USE_THEME_SIMPLE     0
#define LV_USE_THEME_MONO       0

/*==================
 * LAYOUTS
 *==================*/

#define LV_USE_FLEX             1
#define LV_USE_GRID             1

/*====================
 * 3RD PARTY LIBRARIES
 *====================*/

#define LV_USE_TINY_TTF         1
#if LV_USE_TINY_TTF
    #define LV_TINY_TTF_FILE_SUPPORT    0
    #define LV_TINY_TTF_CACHE_GLYPH_CNT 256
    #define LV_TINY_TTF_CACHE_KERNING_CNT 256
#endif

#define LV_USE_OBSERVER         0
#define LV_USE_SDL              0

#endif /* LV_CONF_H */
