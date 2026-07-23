#pragma once
/* Reusable widget builders + the keyboard overlay.
 * The overlay lives on lv_layer_top() so it floats above every screen and a
 * single text area serves the command line, the filter and the macro editor. */

const lv_font_t *FONT_S  = &lv_font_montserrat_10;
const lv_font_t *FONT_M  = &lv_font_montserrat_12;
const lv_font_t *FONT_L  = &lv_font_montserrat_14;
const lv_font_t *FONT_XL = &lv_font_montserrat_16;

const lv_font_t *LOG_FONTS[3] = { &lv_font_montserrat_10, &lv_font_montserrat_12,
                                  &lv_font_montserrat_14 };
const int         LOG_ROW_H[3] = { 13, 16, 19 };
static const char *LOG_FONT_OPTS = "small\nmedium\nlarge";

/* ---------- builders ------------------------------------------------------ */
static lv_obj_t *mk_panel(lv_obj_t *p, int x, int y, int w, int h, lv_color_t bg) {
    lv_obj_t *o = lv_obj_create(p);
    flatten(o);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, w, h);
    lv_obj_set_style_bg_color(o, bg, 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    return o;
}

static lv_obj_t *mk_lbl(lv_obj_t *p, int x, int y, const char *txt,
                        const lv_font_t *f, lv_color_t c) {
    lv_obj_t *l = lv_label_create(p);
    lv_label_set_text(l, txt);
    lv_obj_set_style_text_font(l, f, 0);
    lv_obj_set_style_text_color(l, c, 0);
    lv_obj_set_pos(l, x, y);
    return l;
}

static lv_obj_t *mk_btn(lv_obj_t *p, int x, int y, int w, int h, const char *txt,
                        lv_event_cb_t cb, void *ud, lv_color_t accent) {
    lv_obj_t *b = lv_button_create(p);
    lv_obj_set_pos(b, x, y);
    lv_obj_set_size(b, w, h);
    lv_obj_set_style_radius(b, 4, 0);
    lv_obj_set_style_bg_color(b, COL_SURFACE2, 0);
    lv_obj_set_style_bg_color(b, accent, LV_STATE_PRESSED);
    lv_obj_set_style_border_width(b, 1, 0);
    lv_obj_set_style_border_color(b, COL_BORDER, 0);
    lv_obj_set_style_pad_all(b, 0, 0);
    /* text colour/font set on the button so the label inherits them and the
       pressed state stays readable */
    lv_obj_set_style_text_font(b, FONT_M, 0);
    lv_obj_set_style_text_color(b, accent, 0);
    lv_obj_set_style_text_color(b, COL_BG, LV_STATE_PRESSED);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, txt);
    lv_obj_center(l);
    if (cb) lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, ud);
    return b;
}

static lv_obj_t *mk_dd(lv_obj_t *p, int x, int y, int w, const char *opts,
                       uint16_t sel, lv_event_cb_t cb, void *ud) {
    lv_obj_t *d = lv_dropdown_create(p);
    lv_dropdown_set_options(d, opts);
    lv_dropdown_set_selected(d, sel);
    lv_obj_set_pos(d, x, y);
    lv_obj_set_size(d, w, 28);
    lv_obj_set_style_bg_color(d, COL_SURFACE2, 0);
    lv_obj_set_style_border_color(d, COL_BORDER, 0);
    lv_obj_set_style_text_font(d, FONT_M, 0);
    lv_obj_set_style_text_color(d, COL_TEXT, 0);
    lv_obj_set_style_pad_all(d, 4, 0);
    if (cb) lv_obj_add_event_cb(d, cb, LV_EVENT_VALUE_CHANGED, ud);
    lv_obj_t *list = lv_dropdown_get_list(d);
    if (list) {
        lv_obj_set_style_text_font(list, FONT_M, 0);
        lv_obj_set_style_bg_color(list, COL_SURFACE, 0);
        lv_obj_set_style_text_color(list, COL_TEXT, 0);
    }
    return d;
}

static lv_obj_t *mk_sw(lv_obj_t *p, int x, int y, bool on, lv_event_cb_t cb, void *ud) {
    lv_obj_t *s = lv_switch_create(p);
    lv_obj_set_pos(s, x, y);
    lv_obj_set_size(s, 44, 22);
    lv_obj_set_style_bg_color(s, COL_INACTIVE, 0);
    lv_obj_set_style_bg_color(s, COL_GREEN, LV_PART_INDICATOR | LV_STATE_CHECKED);
    if (on) lv_obj_add_state(s, LV_STATE_CHECKED);
    if (cb) lv_obj_add_event_cb(s, cb, LV_EVENT_VALUE_CHANGED, ud);
    return s;
}

/* ---------- keyboard overlay --------------------------------------------- */
enum KbPurpose : uint8_t { KB_SEND = 0, KB_FILTER, KB_MACRO, KB_SOFTCMD };

static lv_obj_t  *kb_panel = nullptr;
static lv_obj_t  *kb_ta    = nullptr;
static lv_obj_t  *kb_kbd   = nullptr;
static lv_obj_t  *kb_title = nullptr;
static uint8_t    kb_purpose = KB_SEND;
static uint8_t    kb_macro_idx = 0;

/* last 8 sent commands, cycled with the HIST button */
static char  tx_hist[8][MACRO_LEN];
static uint8_t tx_hist_n = 0, tx_hist_cursor = 0;

static void hist_add(const char *s) {
    if (tx_hist_n && strncmp(tx_hist[(tx_hist_n - 1) % 8], s, MACRO_LEN) == 0) return;
    strncpy(tx_hist[tx_hist_n % 8], s, MACRO_LEN - 1);
    tx_hist[tx_hist_n % 8][MACRO_LEN - 1] = 0;
    tx_hist_n++;
    tx_hist_cursor = 0;
}

void kb_close() {
    if (kb_panel) lv_obj_add_flag(kb_panel, LV_OBJ_FLAG_HIDDEN);
}

void kb_open(uint8_t purpose, uint8_t macro_idx, const char *initial) {
    kb_purpose   = purpose;
    kb_macro_idx = macro_idx;
    lv_textarea_set_text(kb_ta, initial ? initial : "");
    const char *t = "SEND TO TARGET";
    if (purpose == KB_FILTER)       t = "FILTER (substring, empty = off)";
    else if (purpose == KB_MACRO)   t = "EDIT MACRO";
    else if (purpose == KB_SOFTCMD) t = "COMMAND THAT REBOOTS TARGET TO BOOTLOADER";
    lv_label_set_text(kb_title, t);
    lv_obj_remove_flag(kb_panel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(kb_panel);
}

static void kb_commit() {
    const char *txt = lv_textarea_get_text(kb_ta);
    if (kb_purpose == KB_SEND) {
        if (txt[0]) { uart_send(txt); hist_add(txt); }
        lv_textarea_set_text(kb_ta, "");
    } else if (kb_purpose == KB_FILTER) {
        strncpy(cfg_filter, txt, FILTER_LEN - 1);
        cfg_filter[FILTER_LEN - 1] = 0;
        save_settings();
        scroll_off = 0;
        monitor_mark_dirty();
        kb_close();
    } else if (kb_purpose == KB_SOFTCMD) {
        strncpy(fl_softcmd, txt, sizeof(fl_softcmd) - 1);
        fl_softcmd[sizeof(fl_softcmd) - 1] = 0;
        flash_refresh_softcmd();
        kb_close();
    } else {
        strncpy(cfg_macro[kb_macro_idx], txt, MACRO_LEN - 1);
        cfg_macro[kb_macro_idx][MACRO_LEN - 1] = 0;
        save_settings();
        tools_refresh_macros();
        kb_close();
    }
}

static void kb_event(lv_event_t *e) {
    lv_event_code_t c = lv_event_get_code(e);
    if (c == LV_EVENT_READY)       kb_commit();
    else if (c == LV_EVENT_CANCEL) kb_close();
}

static void kb_send_cb(lv_event_t *e)  { (void)e; kb_commit(); }
static void kb_close_cb(lv_event_t *e) { (void)e; kb_close(); }

static void kb_hist_cb(lv_event_t *e) {
    (void)e;
    if (!tx_hist_n) return;
    uint8_t n = tx_hist_n < 8 ? tx_hist_n : 8;
    tx_hist_cursor = (uint8_t)((tx_hist_cursor + 1) % n);
    uint8_t idx = (uint8_t)((tx_hist_n - 1 - tx_hist_cursor) % 8);
    lv_textarea_set_text(kb_ta, tx_hist[idx]);
}

void kb_build() {
    kb_panel = mk_panel(lv_layer_top(), 0, SCREEN_H - 196, SCREEN_W, 196, COL_SURFACE);
    lv_obj_set_style_border_width(kb_panel, 1, 0);
    lv_obj_set_style_border_color(kb_panel, COL_BORDER, 0);
    lv_obj_set_style_border_side(kb_panel, LV_BORDER_SIDE_TOP, 0);

    kb_title = mk_lbl(kb_panel, 6, 2, "SEND TO TARGET", FONT_S, COL_LABEL);

    kb_ta = lv_textarea_create(kb_panel);
    lv_textarea_set_one_line(kb_ta, true);
    lv_textarea_set_placeholder_text(kb_ta, "command");
    lv_obj_set_pos(kb_ta, 4, 14);
    lv_obj_set_size(kb_ta, 286, 30);
    lv_obj_set_style_bg_color(kb_ta, COL_BG, 0);
    lv_obj_set_style_border_color(kb_ta, COL_BORDER, 0);
    lv_obj_set_style_text_color(kb_ta, COL_TEXT, 0);
    lv_obj_set_style_text_font(kb_ta, FONT_M, 0);
    lv_obj_set_style_pad_all(kb_ta, 4, 0);

    mk_btn(kb_panel, 294, 14, 44, 30, LV_SYMBOL_UP,   kb_hist_cb,  nullptr, COL_BLUE);
    mk_btn(kb_panel, 342, 14, 74, 30, "SEND",         kb_send_cb,  nullptr, COL_GREEN);
    mk_btn(kb_panel, 420, 14, 56, 30, LV_SYMBOL_CLOSE, kb_close_cb, nullptr, COL_RED);

    kb_kbd = lv_keyboard_create(kb_panel);
    lv_obj_set_pos(kb_kbd, 0, 48);
    lv_obj_set_size(kb_kbd, SCREEN_W, 148);
    lv_obj_set_style_bg_color(kb_kbd, COL_BG, 0);
    lv_obj_set_style_text_font(kb_kbd, FONT_M, 0);
    lv_obj_set_style_pad_all(kb_kbd, 2, 0);
    lv_keyboard_set_textarea(kb_kbd, kb_ta);
    lv_obj_add_event_cb(kb_kbd, kb_event, LV_EVENT_ALL, nullptr);

    lv_obj_add_flag(kb_panel, LV_OBJ_FLAG_HIDDEN);
}
