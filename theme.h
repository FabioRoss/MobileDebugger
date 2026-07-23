#pragma once
/* Palette per the CYD543 guidelines: true black background, saturated accents,
 * no greys on anything that carries information.                             */

#define COL_BG       lv_color_hex(0x000000)
#define COL_SURFACE  lv_color_hex(0x0D1117)
#define COL_SURFACE2 lv_color_hex(0x161B22)
#define COL_BORDER   lv_color_hex(0x30363D)
#define COL_GREEN    lv_color_hex(0x3FB950)
#define COL_BLUE     lv_color_hex(0x58A6FF)
#define COL_AMBER    lv_color_hex(0xD29922)
#define COL_RED      lv_color_hex(0xF85149)
#define COL_CYAN     lv_color_hex(0x39C5CF)
#define COL_VIOLET   lv_color_hex(0xBC8CFF)
#define COL_TEXT     lv_color_hex(0xE6EDF3)
#define COL_LABEL    lv_color_hex(0x8B949E)
#define COL_INACTIVE lv_color_hex(0x484F58)

/* Severity classes used by the log buffer and the renderer */
enum LogSev : uint8_t {
    SEV_PLAIN = 0,   /* ordinary output            */
    SEV_DEBUG,       /* D/V level, dim cyan        */
    SEV_INFO,        /* I level, blue              */
    SEV_OK,          /* success/ready, green       */
    SEV_WARN,        /* warnings, amber            */
    SEV_ERR,         /* errors/panics, red         */
    SEV_TX,          /* what we sent, violet       */
    SEV_SYS          /* tool's own messages, cyan  */
};

static inline lv_color_t sev_color(uint8_t s) {
    switch (s) {
        case SEV_DEBUG: return COL_CYAN;
        case SEV_INFO:  return COL_BLUE;
        case SEV_OK:    return COL_GREEN;
        case SEV_WARN:  return COL_AMBER;
        case SEV_ERR:   return COL_RED;
        case SEV_TX:    return COL_VIOLET;
        case SEV_SYS:   return COL_LABEL;
        default:        return COL_TEXT;
    }
}

/* LVGL 9 objects scroll and have padding by default — kill both. */
static inline void flatten(lv_obj_t *o) {
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(o, 0, 0);
    lv_obj_set_style_border_width(o, 0, 0);
    lv_obj_set_style_radius(o, 0, 0);
}
