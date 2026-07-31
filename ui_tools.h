#pragma once
/* Tools screen (tabbed) + the two full-screen diagnostic screens. */

static lv_obj_t *tv_tools    = nullptr;
static lv_obj_t *lbl_ab      = nullptr;
static lv_obj_t *macro_lbl[MACRO_COUNT];
static lv_obj_t *lbl_sysinfo = nullptr;
static lv_obj_t *lbl_psram   = nullptr;
static lv_obj_t *lbl_i2c     = nullptr;
static lv_obj_t *dd_i2c_sda  = nullptr;
static lv_obj_t *dd_i2c_scl  = nullptr;
static lv_obj_t *dd_pin      = nullptr;
static lv_obj_t *dd_pinmode  = nullptr;
static lv_obj_t *lbl_pin     = nullptr;
static lv_obj_t *lbl_touchlive = nullptr;

lv_obj_t *scr_lcd   = nullptr;
lv_obj_t *scr_touch = nullptr;

/* pins broken out on / safe to poke at on this board family */
static const uint8_t PIN_TABLE[] = { 1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,
                                     19,20,21,38,39,40,41,42,43,44,45,46,47,48 };
#define PIN_COUNT (sizeof(PIN_TABLE)/sizeof(PIN_TABLE[0]))
static char PIN_OPTS[PIN_COUNT * 4];

static void build_pin_opts() {
    int p = 0;
    for (size_t i = 0; i < PIN_COUNT; i++)
        p += snprintf(PIN_OPTS + p, sizeof(PIN_OPTS) - p, "%s%u",
                      i ? "\n" : "", PIN_TABLE[i]);
}
static int pin_index_of(uint8_t gpio) {
    for (size_t i = 0; i < PIN_COUNT; i++) if (PIN_TABLE[i] == gpio) return (int)i;
    return 0;
}

/* ======================= callbacks ======================================== */
static void ev_back(lv_event_t *e) { (void)e; kb_close(); lv_screen_load(scr_monitor); }

static void ev_baud(lv_event_t *e) {
    cfg_baud_idx = (uint8_t)lv_dropdown_get_selected((lv_obj_t *)lv_event_get_target(e));
    uart_apply(); save_settings();
    char b[48]; snprintf(b, sizeof(b), "Baud -> %lu", (unsigned long)BAUD_TABLE[cfg_baud_idx]);
    log_push(b, SEV_SYS);
}
static void ev_format(lv_event_t *e) {
    cfg_format_idx = (uint8_t)lv_dropdown_get_selected((lv_obj_t *)lv_event_get_target(e));
    uart_apply(); save_settings();
}
static void ev_eol(lv_event_t *e) {
    cfg_eol_idx = (uint8_t)lv_dropdown_get_selected((lv_obj_t *)lv_event_get_target(e));
    save_settings();
}
static void ev_fontsize(lv_event_t *e) {
    cfg_font_idx = (uint8_t)lv_dropdown_get_selected((lv_obj_t *)lv_event_get_target(e));
    save_settings(); monitor_mark_dirty();
}
static void ev_sw(lv_event_t *e) {
    bool *target = (bool *)lv_event_get_user_data(e);
    *target = lv_obj_has_state((lv_obj_t *)lv_event_get_target(e), LV_STATE_CHECKED);
    save_settings();
    monitor_mark_dirty();
}
static void ev_autobaud(lv_event_t *e) { (void)e; autobaud_start(); }
static void ev_filter(lv_event_t *e)   { (void)e; kb_open(KB_FILTER, 0, cfg_filter); }
static void ev_filter_clr(lv_event_t *e) {
    (void)e; cfg_filter[0] = 0; save_settings(); scroll_off = 0; monitor_mark_dirty();
}

void tools_set_autobaud_status(const char *s) { if (lbl_ab) lv_label_set_text(lbl_ab, s); }
void tools_refresh_macros() {
    for (int i = 0; i < MACRO_COUNT; i++)
        if (macro_lbl[i]) lv_label_set_text(macro_lbl[i], cfg_macro[i][0] ? cfg_macro[i] : "(empty)");
}
static void ev_macro_send(lv_event_t *e) {
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (cfg_macro[i][0]) { uart_send(cfg_macro[i]); lv_screen_load(scr_monitor); }
}
static void ev_macro_edit(lv_event_t *e) {
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    kb_open(KB_MACRO, (uint8_t)i, cfg_macro[i]);
}

static void ev_sysinfo(lv_event_t *e) {
    (void)e;
    static char b[640];
    diag_sysinfo(b, sizeof(b));
    lv_label_set_text(lbl_sysinfo, b);
}
static void ev_psram(lv_event_t *e) {
    (void)e;
    static char b[160];
    diag_psram_test(b, sizeof(b));
    lv_label_set_text(lbl_psram, b);
    log_push(b, SEV_SYS);
}
static void ev_reboot(lv_event_t *e) { (void)e; save_settings(); delay(50); ESP.restart(); }

static void ev_i2c_scan(lv_event_t *e) {
    (void)e;
    static char b[512];
    int sda = PIN_TABLE[lv_dropdown_get_selected(dd_i2c_sda)];
    int scl = PIN_TABLE[lv_dropdown_get_selected(dd_i2c_scl)];
    int n = diag_i2c_scan(sda, scl, b, sizeof(b));
    char out[560];
    snprintf(out, sizeof(out), "SDA=%d SCL=%d -> %d device(s)\n%s", sda, scl, n, b);
    lv_label_set_text(lbl_i2c, out);
}

static void ev_pin_apply(lv_event_t *e) {
    (void)e;
    int pin  = PIN_TABLE[lv_dropdown_get_selected(dd_pin)];
    int mode = lv_dropdown_get_selected(dd_pinmode);
    probe_attach(pin, (uint8_t)mode);
    char b[64]; snprintf(b, sizeof(b), "Probe attached to GPIO%d", pin);
    log_push(b, SEV_SYS);
}
static void ev_pin_high(lv_event_t *e) { (void)e; if (probe_pin >= 0 && probe_mode == 3) digitalWrite(probe_pin, HIGH); }
static void ev_pin_low(lv_event_t *e)  { (void)e; if (probe_pin >= 0 && probe_mode == 3) digitalWrite(probe_pin, LOW); }
static void ev_pin_pulse(lv_event_t *e) {
    (void)e;
    if (probe_pin >= 0 && probe_mode == 3) {
        digitalWrite(probe_pin, HIGH); delayMicroseconds(200); digitalWrite(probe_pin, LOW);
    }
}
static void ev_pin_release(lv_event_t *e) { (void)e; probe_detach(); lv_label_set_text(lbl_pin, "released"); }

static void pin_tick(lv_timer_t *t) {
    (void)t;
    if (lv_screen_active() != scr_tools || !lbl_pin) return;
    char b[96];
    if (probe_pin < 0) { lv_label_set_text(lbl_pin, "no pin attached"); }
    else {
        uint32_t ed = probe_edges; probe_edges = 0;
        /* 500 ms window, 2 edges per cycle -> Hz == edge count */
        snprintf(b, sizeof(b), "GPIO%d  level=%s  ~%lu Hz",
                 probe_pin, digitalRead(probe_pin) ? "HIGH" : "LOW", (unsigned long)ed);
        lv_label_set_text(lbl_pin, b);
    }
    if (lbl_touchlive) {
        snprintf(b, sizeof(b), "raw %d,%d   mapped %d,%d   %s   taps %lu\ncal X %u-%u  Y %u-%u",
                 tt_raw_x, tt_raw_y, tt_map_x, tt_map_y, tt_pressed ? "DOWN" : "up",
                 (unsigned long)tt_events, touchMinX, touchMaxX, touchMinY, touchMaxY);
        lv_label_set_text(lbl_touchlive, b);
    }
}

/* ======================= LCD test screen ================================== */
static int       lcd_pattern = 0;
static lv_obj_t *lcd_base = nullptr, *lcd_bars = nullptr, *lcd_grid = nullptr, *lcd_name = nullptr;
static const char *PATTERN_NAMES[] = { "RED", "GREEN", "BLUE", "WHITE", "BLACK",
                                       "COLOUR BARS", "1px GRID", "GRADIENT" };

static void lcd_apply() {
    lv_obj_add_flag(lcd_bars, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(lcd_grid, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_bg_grad_dir(lcd_base, LV_GRAD_DIR_NONE, 0);
    switch (lcd_pattern) {
        case 0: lv_obj_set_style_bg_color(lcd_base, lv_color_hex(0xFF0000), 0); break;
        case 1: lv_obj_set_style_bg_color(lcd_base, lv_color_hex(0x00FF00), 0); break;
        case 2: lv_obj_set_style_bg_color(lcd_base, lv_color_hex(0x0000FF), 0); break;
        case 3: lv_obj_set_style_bg_color(lcd_base, lv_color_hex(0xFFFFFF), 0); break;
        case 4: lv_obj_set_style_bg_color(lcd_base, lv_color_hex(0x000000), 0); break;
        case 5: lv_obj_set_style_bg_color(lcd_base, lv_color_hex(0x000000), 0);
                lv_obj_remove_flag(lcd_bars, LV_OBJ_FLAG_HIDDEN); break;
        case 6: lv_obj_set_style_bg_color(lcd_base, lv_color_hex(0x000000), 0);
                lv_obj_remove_flag(lcd_grid, LV_OBJ_FLAG_HIDDEN); break;
        case 7: lv_obj_set_style_bg_color(lcd_base, lv_color_hex(0x000000), 0);
                lv_obj_set_style_bg_grad_color(lcd_base, lv_color_hex(0xFFFFFF), 0);
                lv_obj_set_style_bg_grad_dir(lcd_base, LV_GRAD_DIR_HOR, 0); break;
    }
    lv_label_set_text(lcd_name, PATTERN_NAMES[lcd_pattern]);
    lv_obj_move_foreground(lcd_name);
}
static void ev_lcd_next(lv_event_t *e) { (void)e; lcd_pattern = (lcd_pattern + 1) % 8; lcd_apply(); }
static void ev_lcd_exit(lv_event_t *e) { (void)e; lv_screen_load(scr_tools); }

static void build_lcd_screen() {
    scr_lcd = lv_obj_create(NULL);
    flatten(scr_lcd);
    lcd_base = mk_panel(scr_lcd, 0, 0, SCREEN_W, SCREEN_H, lv_color_hex(0xFF0000));
    lv_obj_add_flag(lcd_base, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(lcd_base, ev_lcd_next, LV_EVENT_CLICKED, nullptr);

    lcd_bars = mk_panel(scr_lcd, 0, 0, SCREEN_W, SCREEN_H, lv_color_hex(0x000000));
    static const uint32_t barcol[8] = { 0xFFFFFF, 0xFFFF00, 0x00FFFF, 0x00FF00,
                                        0xFF00FF, 0xFF0000, 0x0000FF, 0x000000 };
    for (int i = 0; i < 8; i++)
        mk_panel(lcd_bars, i * 60, 0, 60, SCREEN_H, lv_color_hex(barcol[i]));
    lv_obj_add_flag(lcd_bars, LV_OBJ_FLAG_HIDDEN);

    lcd_grid = mk_panel(scr_lcd, 0, 0, SCREEN_W, SCREEN_H, lv_color_hex(0x000000));
    for (int x = 0; x < SCREEN_W; x += 20) mk_panel(lcd_grid, x, 0, 1, SCREEN_H, COL_GREEN);
    for (int y = 0; y < SCREEN_H; y += 20) mk_panel(lcd_grid, 0, y, SCREEN_W, 1, COL_GREEN);
    lv_obj_add_flag(lcd_grid, LV_OBJ_FLAG_HIDDEN);

    lcd_name = mk_lbl(scr_lcd, 4, 2, "RED", FONT_S, COL_LABEL);
    mk_btn(scr_lcd, SCREEN_W - 56, 2, 54, 22, "EXIT", ev_lcd_exit, nullptr, COL_RED);
}

/* ======================= touch test screen ================================ */
#define TT_Y 62
#define TT_H (SCREEN_H - TT_Y)
static lv_obj_t *tt_canvas = nullptr, *tt_info = nullptr;
static lv_draw_buf_t tt_dbuf;

static void tt_paint_grid() {
    lv_canvas_fill_bg(tt_canvas, lv_color_hex(0x000000), LV_OPA_COVER);
    lv_color_t g = lv_color_hex(0x203040);
    for (int x = 0; x < SCREEN_W; x += 40)
        for (int y = 0; y < TT_H; y++) lv_canvas_set_px(tt_canvas, x, y, g, LV_OPA_COVER);
    for (int y = 0; y < TT_H; y += 40)
        for (int x = 0; x < SCREEN_W; x++) lv_canvas_set_px(tt_canvas, x, y, g, LV_OPA_COVER);
    lv_obj_invalidate(tt_canvas);
}
static void ev_tt_clear(lv_event_t *e)  { (void)e; tt_paint_grid(); }
static void ev_tt_calib(lv_event_t *e)  { (void)e; touch_reset_calibration(); tt_paint_grid(); }
static void ev_tt_exit(lv_event_t *e)   { (void)e; lv_screen_load(scr_tools); }

static void tt_tick(lv_timer_t *t) {
    (void)t;
    if (lv_screen_active() != scr_touch || !tt_canvas) return;
    char b[96];
    snprintf(b, sizeof(b), "raw %d,%d  mapped %d,%d  %s  taps %lu",
             tt_raw_x, tt_raw_y, tt_map_x, tt_map_y,
             tt_pressed ? "DOWN" : "up", (unsigned long)tt_events);
    lv_label_set_text(tt_info, b);

    if (!tt_pressed) return;
    int cx = tt_map_x, cy = tt_map_y - TT_Y;
    if (cy < 0) return;
    for (int dy = -2; dy <= 2; dy++)
        for (int dx = -2; dx <= 2; dx++) {
            int x = cx + dx, y = cy + dy;
            if (x >= 0 && x < SCREEN_W && y >= 0 && y < TT_H)
                lv_canvas_set_px(tt_canvas, x, y, COL_GREEN, LV_OPA_COVER);
        }
    lv_obj_invalidate(tt_canvas);
}

static void build_touch_screen() {
    scr_touch = lv_obj_create(NULL);
    flatten(scr_touch);
    lv_obj_set_style_bg_color(scr_touch, COL_BG, 0);
    lv_obj_set_style_bg_opa(scr_touch, LV_OPA_COVER, 0);

    mk_btn(scr_touch, 4, 3, 70, 24, LV_SYMBOL_LEFT " BACK", ev_tt_exit, nullptr, COL_BLUE);
    mk_btn(scr_touch, 300, 3, 84, 24, "CLEAR", ev_tt_clear, nullptr, COL_AMBER);
    mk_btn(scr_touch, 390, 3, 86, 24, "RECAL", ev_tt_calib, nullptr, COL_VIOLET);
    tt_info = mk_lbl(scr_touch, 6, 34, "", FONT_S, COL_LABEL);
    mk_lbl(scr_touch, 6, 48, "draw across the whole panel: gaps = dead zones, "
                             "offset dots = calibration drift", FONT_S, COL_INACTIVE);

    /* big canvas buffer must come from PSRAM, not the LVGL heap */
    uint32_t stride = (uint32_t)SCREEN_W * 2u;
    uint32_t bytes  = stride * (uint32_t)TT_H + LV_DRAW_BUF_ALIGN;
    static uint8_t *px = nullptr;
    px = (uint8_t *)heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!px) px = (uint8_t *)heap_caps_malloc(bytes, MALLOC_CAP_8BIT);
    if (!px) { mk_lbl(scr_touch, 6, TT_Y, "canvas alloc failed", FONT_M, COL_RED); return; }

    lv_draw_buf_init(&tt_dbuf, SCREEN_W, TT_H, LV_COLOR_FORMAT_RGB565, stride, px, bytes);
    tt_canvas = lv_canvas_create(scr_touch);
    lv_canvas_set_draw_buf(tt_canvas, &tt_dbuf);
    lv_obj_set_pos(tt_canvas, 0, TT_Y);
    lv_obj_set_size(tt_canvas, SCREEN_W, TT_H);
    tt_paint_grid();
    lv_timer_create(tt_tick, 30, nullptr);
}

static void ev_open_lcd(lv_event_t *e)   { (void)e; lcd_pattern = 0; lcd_apply(); lv_screen_load(scr_lcd); }
static void ev_open_touch(lv_event_t *e) { (void)e; lv_screen_load(scr_touch); }

/* ======================= tools screen ===================================== */
static void build_tools_screen() {
    scr_tools = lv_obj_create(NULL);
    flatten(scr_tools);
    lv_obj_set_style_bg_color(scr_tools, COL_BG, 0);
    lv_obj_set_style_bg_opa(scr_tools, LV_OPA_COVER, 0);

    lv_obj_t *bar = mk_panel(scr_tools, 0, 0, SCREEN_W, TOPBAR_H, COL_SURFACE);
    mk_btn(bar, 3, 2, 96, 20, LV_SYMBOL_LEFT " MONITOR", ev_back, nullptr, COL_GREEN);
    mk_lbl(bar, 340, 7, "CYD543 DEBUG TOOL " FW_VERSION, FONT_S, COL_LABEL);

    tv_tools = lv_tabview_create(scr_tools);
    lv_tabview_set_tab_bar_position(tv_tools, LV_DIR_BOTTOM);
    lv_tabview_set_tab_bar_size(tv_tools, 30);
    lv_obj_set_pos(tv_tools, 0, TOPBAR_H);
    lv_obj_set_size(tv_tools, SCREEN_W, SCREEN_H - TOPBAR_H);
    lv_obj_set_style_bg_color(tv_tools, COL_BG, 0);

    lv_obj_t *tb = lv_tabview_get_tab_bar(tv_tools);
    lv_obj_set_style_bg_color(tb, COL_SURFACE, 0);
    lv_obj_set_style_text_font(tb, FONT_S, 0);
    lv_obj_set_style_text_color(tb, COL_LABEL, LV_PART_ITEMS);
    lv_obj_set_style_text_color(tb, COL_VIOLET, LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_bg_color(tb, COL_SURFACE2, LV_PART_ITEMS | LV_STATE_CHECKED);

    lv_obj_t *t_uart  = lv_tabview_add_tab(tv_tools, "UART");
    lv_obj_t *t_flash = lv_tabview_add_tab(tv_tools, "FLASH");
    lv_obj_t *t_macro = lv_tabview_add_tab(tv_tools, "MACRO");
    lv_obj_t *t_sys   = lv_tabview_add_tab(tv_tools, "SYS");
    lv_obj_t *t_test  = lv_tabview_add_tab(tv_tools, "DISP");
    lv_obj_t *t_i2c   = lv_tabview_add_tab(tv_tools, "I2C");
    lv_obj_t *t_gpio  = lv_tabview_add_tab(tv_tools, "GPIO");
    lv_obj_t *t_fleet = lv_tabview_add_tab(tv_tools, "FLEET");
    lv_obj_t *pages[] = { t_uart, t_flash, t_macro, t_sys, t_test, t_i2c, t_gpio, t_fleet };
    for (int i = 0; i < 8; i++) {
        lv_obj_set_style_pad_all(pages[i], 0, 0);
        lv_obj_set_style_bg_color(pages[i], COL_BG, 0);
        lv_obj_set_style_bg_opa(pages[i], LV_OPA_COVER, 0);
    }

    /* ---------------- UART tab ---------------- */
    mk_lbl(t_uart, 6, 10, "Baud",   FONT_S, COL_LABEL);
    mk_dd (t_uart, 52, 4, 128, BAUD_OPTS,   cfg_baud_idx,   ev_baud,     nullptr);
    mk_lbl(t_uart, 6, 42, "Format", FONT_S, COL_LABEL);
    mk_dd (t_uart, 52, 36, 128, FORMAT_OPTS, cfg_format_idx, ev_format,   nullptr);
    mk_lbl(t_uart, 6, 74, "EOL",    FONT_S, COL_LABEL);
    mk_dd (t_uart, 52, 68, 128, EOL_OPTS,    cfg_eol_idx,    ev_eol,      nullptr);
    mk_lbl(t_uart, 6, 106, "Font",  FONT_S, COL_LABEL);
    mk_dd (t_uart, 52, 100, 128, "small\nmedium\nlarge", cfg_font_idx, ev_fontsize, nullptr);

    mk_btn(t_uart, 6, 134, 174, 28, "AUTO-BAUD SWEEP", ev_autobaud, nullptr, COL_AMBER);
    lbl_ab = mk_lbl(t_uart, 6, 166, "idle", FONT_S, COL_LABEL);
    lv_obj_set_width(lbl_ab, 180);

    struct { const char *n; bool *v; int y; } sws[] = {
        { "Timestamps",   &cfg_timestamps, 4   },
        { "Hex view",     &cfg_hexview,    34  },
        { "ANSI colours", &cfg_ansi,       64  },
        { "Autoscroll",   &cfg_autoscroll, 94  },
        { "USB bridge",   &cfg_bridge,     124 },
        { "Hide FF/00",   &cfg_hidenoise,  154 },
    };
    for (auto &s : sws) {
        mk_lbl(t_uart, 200, s.y + 5, s.n, FONT_M, COL_TEXT);
        mk_sw (t_uart, 420, s.y, *s.v, ev_sw, (void *)s.v);
    }
    mk_btn(t_uart, 6, 168, 84, 24, "FILTER",  ev_filter,     nullptr, COL_BLUE);
    mk_btn(t_uart, 96, 168, 84, 24, "CLR FLT", ev_filter_clr, nullptr, COL_RED);

    /* ---------------- MACRO tab ---------------- */
    mk_lbl(t_macro, 6, 2, "tap SEND to fire, EDIT to change (stored in NVS)", FONT_S, COL_LABEL);
    for (int i = 0; i < MACRO_COUNT; i++) {
        int y = 18 + i * 30;
        mk_btn(t_macro, 6, y, 66, 26, "SEND", ev_macro_send, (void *)(intptr_t)i, COL_GREEN);
        macro_lbl[i] = mk_lbl(t_macro, 80, y + 6, cfg_macro[i], FONT_M, COL_TEXT);
        lv_label_set_long_mode(macro_lbl[i], LV_LABEL_LONG_CLIP);
        lv_obj_set_width(macro_lbl[i], 310);
        mk_btn(t_macro, 400, y, 66, 26, "EDIT", ev_macro_edit, (void *)(intptr_t)i, COL_BLUE);
    }
    tools_refresh_macros();

    /* ---------------- FLASH tab ---------------- */
    flash_build_tab(t_flash);

    /* ---------------- FLEET tab ---------------- */
    fleet_build_tab(t_fleet);

    /* ---------------- SYS tab ---------------- */
    lbl_sysinfo = mk_lbl(t_sys, 6, 2, "", FONT_S, COL_TEXT);
    lv_obj_set_width(lbl_sysinfo, 468);
    lbl_psram = mk_lbl(t_sys, 6, 146, "PSRAM not tested yet", FONT_S, COL_AMBER);
    lv_obj_set_width(lbl_psram, 468);
    mk_btn(t_sys, 6,   186, 110, 28, "REFRESH",    ev_sysinfo, nullptr, COL_BLUE);
    mk_btn(t_sys, 124, 186, 140, 28, "PSRAM TEST", ev_psram,   nullptr, COL_AMBER);
    mk_btn(t_sys, 272, 186, 110, 28, "REBOOT",     ev_reboot,  nullptr, COL_RED);
    ev_sysinfo(nullptr);

    /* ---------------- LCD / TOUCH tab ---------------- */
    mk_lbl(t_test, 6, 6, "Display and touch diagnostics for this board.", FONT_M, COL_TEXT);
    mk_btn(t_test, 6, 30, 220, 34, "LCD TEST PATTERNS", ev_open_lcd, nullptr, COL_GREEN);
    mk_lbl(t_test, 234, 40, "tap to cycle: RGB, white,\nblack, bars, grid, gradient",
           FONT_S, COL_LABEL);
    mk_btn(t_test, 6, 74, 220, 34, "TOUCH TEST / PAINT", ev_open_touch, nullptr, COL_GREEN);
    mk_lbl(t_test, 234, 84, "find dead zones and\ncalibration offset", FONT_S, COL_LABEL);
    lbl_touchlive = mk_lbl(t_test, 6, 122, "", FONT_S, COL_BLUE);
    lv_obj_set_width(lbl_touchlive, 468);
    mk_btn(t_test, 6, 160, 220, 28, "RESET TOUCH CALIBRATION", ev_tt_calib, nullptr, COL_VIOLET);

    /* ---------------- I2C tab ---------------- */
    mk_lbl(t_i2c, 6, 10, "SDA", FONT_S, COL_LABEL);
    dd_i2c_sda = mk_dd(t_i2c, 38, 4, 74, PIN_OPTS, pin_index_of(8), nullptr, nullptr);
    mk_lbl(t_i2c, 124, 10, "SCL", FONT_S, COL_LABEL);
    dd_i2c_scl = mk_dd(t_i2c, 156, 4, 74, PIN_OPTS, pin_index_of(4), nullptr, nullptr);
    mk_btn(t_i2c, 240, 4, 110, 28, "SCAN", ev_i2c_scan, nullptr, COL_GREEN);
    mk_lbl(t_i2c, 358, 10, "8/4 = touch bus", FONT_S, COL_INACTIVE);
    lbl_i2c = mk_lbl(t_i2c, 6, 40, "no scan yet", FONT_S, COL_TEXT);
    lv_obj_set_width(lbl_i2c, 468);

    /* ---------------- GPIO tab ---------------- */
    mk_lbl(t_gpio, 6, 10, "GPIO", FONT_S, COL_LABEL);
    dd_pin = mk_dd(t_gpio, 44, 4, 76, PIN_OPTS, pin_index_of(17), nullptr, nullptr);
    dd_pinmode = mk_dd(t_gpio, 128, 4, 150,
                       "input\ninput pullup\ninput pulldown\noutput", 1, nullptr, nullptr);
    mk_btn(t_gpio, 286, 4, 90, 28, "ATTACH",  ev_pin_apply,   nullptr, COL_GREEN);
    mk_btn(t_gpio, 382, 4, 90, 28, "RELEASE", ev_pin_release, nullptr, COL_RED);
    lbl_pin = mk_lbl(t_gpio, 6, 42, "no pin attached", FONT_L, COL_BLUE);
    mk_btn(t_gpio, 6,   70, 100, 30, "SET HIGH", ev_pin_high,  nullptr, COL_GREEN);
    mk_btn(t_gpio, 114, 70, 100, 30, "SET LOW",  ev_pin_low,   nullptr, COL_AMBER);
    mk_btn(t_gpio, 222, 70, 100, 30, "PULSE",    ev_pin_pulse, nullptr, COL_VIOLET);
    mk_lbl(t_gpio, 6, 112,
           "Input modes report level and rough edge frequency (500 ms window).\n"
           "Output modes drive the pin - 3.3V logic only, never connect 5V.\n"
           "GPIO17/18 are the UART lines: attaching here will disturb capture.",
           FONT_S, COL_LABEL);

    lv_timer_create(pin_tick, 500, nullptr);
}

/* ======================= entry point ====================================== */
void ui_build_all() {
    build_pin_opts();
    monitor_build();
    build_tools_screen();
    build_lcd_screen();
    build_touch_screen();
    kb_build();
}
