/**
 * @file nav_theme.h
 * @brief Colours and round-display geometry shared by the screens.
 *
 * Colours and round geometry from the reference navigation project.
 * Dimensions are adapted for the 240 x 240 GC9A01 panel.
 */

#pragma once

#include <math.h>
#include "lvgl.h"

#define NAV_SCREEN_SIZE     240
#define NAV_SCREEN_R        (NAV_SCREEN_SIZE / 2)   /**< 120 */

/**
 * @brief Half the width available at @p dy pixels above or below the centre.
 *
 * Use it when placing something new, rather than guessing: a panel whose half
 * width exceeds this at either of its horizontal edges has its corners cut off
 * by the bezel.
 */
static inline int32_t nav_chord_half(int32_t dy)
{
    int32_t d = dy < 0 ? -dy : dy;
    if (d >= NAV_SCREEN_R) return 0;
    return (int32_t)sqrt((double)(NAV_SCREEN_R * NAV_SCREEN_R - d * d));
}

/* Panel widths, all verified against the chord at their outermost edge. */
#define NAV_PANEL_W         148     /**< Fits the chord at y=30 on the 240 px panel. */

#define NAV_COL_BG          lv_color_hex(0x11161C)
#define NAV_COL_BG_OFF      lv_color_hex(0x7A1E14)
#define NAV_COL_BG_DONE     lv_color_hex(0x125B2E)
#define NAV_COL_ACCENT      lv_color_hex(0x4C9AFF)
#define NAV_COL_TEXT        lv_color_hex(0xF2F5F8)
#define NAV_COL_TEXT_DIM    lv_color_hex(0x9AA6B2)
#define NAV_COL_BORDER      lv_color_hex(0x2C353F)
#define NAV_COL_WARN        lv_color_hex(0xFF6B5B)
#define NAV_COL_GOOD        lv_color_hex(0x35D07F)

/** @brief The house style for a floating panel. */
static inline void nav_style_panel(lv_obj_t *obj)
{
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(obj, NAV_COL_BG, 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_90, 0);
    lv_obj_set_style_border_width(obj, 1, 0);
    lv_obj_set_style_border_color(obj, NAV_COL_BORDER, 0);
    lv_obj_set_style_border_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(obj, 10, 0);
    lv_obj_set_style_pad_all(obj, 5, 0);
    lv_obj_set_style_shadow_width(obj, 0, 0);
}

/** @brief A label with a font and colour, since every screen makes dozens. */
static inline lv_obj_t *nav_make_label(lv_obj_t *parent, const lv_font_t *font,
                                       lv_color_t color, const char *text)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, color, 0);
    lv_label_set_text(l, text);
    return l;
}
