#pragma once
/* Full screen serial monitor: 24 px status bar + colour coded log rows.
 * One label per visible row — cheaper than a giant recolored label and it
 * lets every line carry its own colour.                                     */

lv_obj_t *scr_monitor = nullptr;
lv_obj_t *scr_tools   = nullptr;      /* built in ui_tools.h */

static lv_obj_t *log_area   = nullptr;
static lv_obj_t *row_lbl[MAX_ROWS];
static lv_obj_t *sb_track   = nullptr;
static lv_obj_t *sb_thumb   = nullptr;
static lv_obj_t *lbl_port   = nullptr;
static lv_obj_t *lbl_stats  = nullptr;
static lv_obj_t *dot_status = nullptr;
static lv_obj_t *btn_pause_lbl = nullptr;

void monitor_mark_dirty() { ui_dirty = true; }

/* ---------- rendering ----------------------------------------------------- */
/* A stored log entry can be far wider than the panel. Rather than clipping it
 * (which silently ate the middle of long lines) it is wrapped across as many
 * screen rows as it needs, every row keeping the entry's severity colour.
 * The fonts are proportional, so break points come from summing real glyph
 * advances rather than assuming a fixed column count.                       */

#define MAX_SUBROWS 10
#define LOG_MAXW    (SCREEN_W - 10)

/* rows[] caches the wrapped height of each ring slot so the scrollbar total
 * does not re-measure every line on every frame. Invalidated when anything
 * that affects wrapping changes.                                            */
static uint8_t *s_rowcache  = nullptr;
static uint32_t s_cache_seq = 0;
static uint32_t s_cache_gen = 0xFFFFFFFF;

static uint32_t wrap_gen() { return (uint32_t)cfg_font_idx * 2u + (cfg_timestamps ? 1u : 0u); }

/* full display text of an entry (timestamp prefix included), returns length */
static int line_text(const LogLine *l, char *buf, size_t n) {
    int len;
    if (cfg_timestamps) {
        len = snprintf(buf, n, "%4lu.%03lu %s",
                       (unsigned long)(l->ms / 1000), (unsigned long)(l->ms % 1000), l->text);
    } else {
        len = snprintf(buf, n, "%s", l->text);
    }
    if (len < 0) len = 0;
    if (len > (int)n - 1) len = (int)n - 1;
    return len;
}

/* fill starts[] with the first character index of each wrapped row */
static int wrap_line(const char *s, int len, const lv_font_t *f,
                     int16_t *starts, int maxrows) {
    int n = 1, w = 0;
    starts[0] = 0;
    for (int i = 0; i < len; i++) {
        uint32_t ch = (uint8_t)s[i];
        uint32_t nx = (i + 1 < len) ? (uint32_t)(uint8_t)s[i + 1] : 0;
        int gw = lv_font_get_glyph_width(f, ch, nx);
        if (gw <= 0) gw = 1;
        if (w + gw > LOG_MAXW && i > starts[n - 1]) {
            if (n >= maxrows) break;
            starts[n++] = (int16_t)i;
            w = 0;
        }
        w += gw;
    }
    return n;
}

static uint8_t rows_of(const LogLine *l) {
    char buf[LOG_LINE_LEN + 24];
    int16_t st[MAX_SUBROWS];
    int len = line_text(l, buf, sizeof(buf));
    return (uint8_t)wrap_line(buf, len, LOG_FONTS[cfg_font_idx], st, MAX_SUBROWS);
}

static void refresh_rowcache() {
    if (!s_rowcache || !log_cap()) return;
    uint32_t gen = wrap_gen();
    if (gen != s_cache_gen) {                    /* font/timestamps changed */
        for (int i = 0; i < (int)log_count(); i++) {
            uint16_t slot = log_slot_of((uint16_t)i);
            s_rowcache[slot] = rows_of(log_at((uint16_t)i));
        }
        s_cache_gen = gen;
        s_cache_seq = log_seq;
        return;
    }
    uint32_t fresh = log_seq - s_cache_seq;      /* only measure new arrivals */
    if (fresh > log_cap()) fresh = log_cap();
    for (uint32_t k = 0; k < fresh; k++) {
        int idx = (int)log_count() - 1 - (int)k;
        if (idx < 0) break;
        uint16_t slot = log_slot_of((uint16_t)idx);
        s_rowcache[slot] = rows_of(log_at((uint16_t)idx));
    }
    s_cache_seq = log_seq;
}

static int visible_row_total() {
    int t = 0;
    for (int i = 0; i < (int)log_count(); i++) {
        const LogLine *l = log_at((uint16_t)i);
        if (!log_line_visible(l)) continue;
        uint8_t r = s_rowcache ? s_rowcache[log_slot_of((uint16_t)i)] : 1;
        t += r ? r : 1;
    }
    return t;
}

static void monitor_render() {
    int rowh = LOG_ROW_H[cfg_font_idx];
    int rows = LOG_H / rowh;
    if (rows > MAX_ROWS) rows = MAX_ROWS;

    refresh_rowcache();

    int total  = visible_row_total();
    int maxoff = total - rows; if (maxoff < 0) maxoff = 0;
    if (scroll_off > maxoff) scroll_off = maxoff;
    if (scroll_off < 0)      scroll_off = 0;

    char    buf[LOG_LINE_LEN + 24];
    char    chunk[LOG_LINE_LEN + 24];
    int16_t st[MAX_SUBROWS];
    int     r = rows - 1;       /* fill upward from the bottom */
    int     skipped = 0;

    for (int i = (int)log_count() - 1; i >= 0 && r >= 0; i--) {
        const LogLine *l = log_at((uint16_t)i);
        if (!log_line_visible(l)) continue;
        int len  = line_text(l, buf, sizeof(buf));
        int nsub = wrap_line(buf, len, LOG_FONTS[cfg_font_idx], st, MAX_SUBROWS);

        /* last chunk sits lowest, so walk the chunks backwards too */
        for (int c = nsub - 1; c >= 0 && r >= 0; c--) {
            if (skipped < scroll_off) { skipped++; continue; }
            int start = st[c];
            int end   = (c + 1 < nsub) ? st[c + 1] : len;
            int cl    = end - start;
            if (cl < 0) cl = 0;
            if (cl > (int)sizeof(chunk) - 1) cl = sizeof(chunk) - 1;
            memcpy(chunk, buf + start, cl);
            chunk[cl] = 0;

            lv_obj_remove_flag(row_lbl[r], LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_pos(row_lbl[r], 2, r * rowh);
            lv_obj_set_style_text_font(row_lbl[r], LOG_FONTS[cfg_font_idx], 0);
            lv_obj_set_style_text_color(row_lbl[r], sev_color(l->sev), 0);
            lv_label_set_text(row_lbl[r], chunk);
            r--;
        }
    }
    for (int k = 0; k <= r; k++)          lv_obj_add_flag(row_lbl[k], LV_OBJ_FLAG_HIDDEN);
    for (int k = rows; k < MAX_ROWS; k++) lv_obj_add_flag(row_lbl[k], LV_OBJ_FLAG_HIDDEN);

    /* scrollbar thumb */
    if (total <= rows) {
        lv_obj_add_flag(sb_thumb, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_remove_flag(sb_thumb, LV_OBJ_FLAG_HIDDEN);
        int th = (int)((float)rows / (float)total * LOG_H);
        if (th < 12) th = 12;
        float pos = (float)(maxoff - scroll_off) / (float)maxoff;   /* 1.0 = newest */
        int ty = (int)(pos * (LOG_H - th));
        lv_obj_set_size(sb_thumb, 4, th);
        lv_obj_set_pos(sb_thumb, 0, ty);
    }
    ui_dirty = false;
}

/* ---------- status bar ---------------------------------------------------- */
static void monitor_update_status() {
    static uint32_t last_rx = 0;
    char b[64];
    snprintf(b, sizeof(b), "%lu %s", (unsigned long)BAUD_TABLE[cfg_baud_idx],
             FORMAT_NAMES[cfg_format_idx]);
    lv_label_set_text(lbl_port, b);

    char noise[24] = "";
    if (rx_noise) snprintf(noise, sizeof(noise), "  N %lu", (unsigned long)rx_noise);
    snprintf(b, sizeof(b), "RX %lu  TX %lu  L %lu%s%s%s",
             (unsigned long)rx_bytes, (unsigned long)tx_bytes, (unsigned long)rx_lines,
             noise, cfg_filter[0] ? "  F:" : "", cfg_filter[0] ? cfg_filter : "");
    lv_label_set_text(lbl_stats, b);

    lv_color_t c = COL_INACTIVE;
    if (paused)               c = COL_AMBER;
    else if (rx_bytes != last_rx) c = COL_GREEN;
    else if (rx_bytes)        c = COL_BLUE;
    lv_obj_set_style_bg_color(dot_status, c, 0);
    last_rx = rx_bytes;
}

static void monitor_tick(lv_timer_t *t) {
    (void)t;
    monitor_update_status();
    if (ui_dirty) monitor_render();
}

/* ---------- events -------------------------------------------------------- */
static void ev_pause(lv_event_t *e) {
    (void)e;
    paused = !paused;
    lv_label_set_text(btn_pause_lbl, paused ? LV_SYMBOL_PLAY : LV_SYMBOL_PAUSE);
    log_push(paused ? "-- capture frozen --" : "-- capture resumed --", SEV_SYS);
}
static void ev_clear(lv_event_t *e)  { (void)e; log_clear(); }
static void ev_kb(lv_event_t *e)     { (void)e; kb_open(KB_SEND, 0, ""); }
static void ev_end(lv_event_t *e)    { (void)e; scroll_off = 0; kb_close(); monitor_mark_dirty(); }
static void ev_tools(lv_event_t *e)  { (void)e; kb_close(); lv_screen_load(scr_tools); }

/* drag anywhere on the log to scroll back through history */
static lv_point_t drag_start;
static int32_t    drag_off0;

static void ev_log_drag(lv_event_t *e) {
    lv_event_code_t c = lv_event_get_code(e);
    lv_indev_t *indev = lv_indev_active();
    if (!indev) return;
    lv_point_t p;
    lv_indev_get_point(indev, &p);
    if (c == LV_EVENT_PRESSED) {
        drag_start = p;
        drag_off0  = scroll_off;
    } else if (c == LV_EVENT_PRESSING) {
        int rowh = LOG_ROW_H[cfg_font_idx];
        int32_t off = drag_off0 + (p.y - drag_start.y) / rowh;
        if (off != scroll_off) { scroll_off = off; monitor_mark_dirty(); }
    }
}

/* ---------- construction -------------------------------------------------- */
void monitor_build() {
    scr_monitor = lv_obj_create(NULL);
    flatten(scr_monitor);
    lv_obj_set_style_bg_color(scr_monitor, COL_BG, 0);
    lv_obj_set_style_bg_opa(scr_monitor, LV_OPA_COVER, 0);

    /* --- status bar --- */
    lv_obj_t *bar = mk_panel(scr_monitor, 0, 0, SCREEN_W, TOPBAR_H, COL_SURFACE);
    lv_obj_set_style_border_width(bar, 1, 0);
    lv_obj_set_style_border_color(bar, COL_BORDER, 0);
    lv_obj_set_style_border_side(bar, LV_BORDER_SIDE_BOTTOM, 0);

    dot_status = mk_panel(bar, 5, 8, 8, 8, COL_INACTIVE);
    lv_obj_set_style_radius(dot_status, 4, 0);
    lbl_port  = mk_lbl(bar, 18, 7, "----", FONT_S, COL_BLUE);
    lbl_stats = mk_lbl(bar, 96, 7, "", FONT_S, COL_LABEL);

    lv_obj_t *b;
    b = mk_btn(bar, 276, 2, 38, 20, LV_SYMBOL_PAUSE, ev_pause, nullptr, COL_AMBER);
    btn_pause_lbl = lv_obj_get_child(b, 0);
    mk_btn(bar, 316, 2, 38, 20, LV_SYMBOL_TRASH,    ev_clear, nullptr, COL_RED);
    mk_btn(bar, 356, 2, 38, 20, LV_SYMBOL_KEYBOARD, ev_kb,    nullptr, COL_GREEN);
    mk_btn(bar, 396, 2, 38, 20, LV_SYMBOL_DOWN,     ev_end,   nullptr, COL_BLUE);
    mk_btn(bar, 436, 2, 40, 20, LV_SYMBOL_SETTINGS, ev_tools, nullptr, COL_VIOLET);

    /* --- log area --- */
    log_area = mk_panel(scr_monitor, 0, LOG_Y, SCREEN_W - 4, LOG_H, COL_BG);
    lv_obj_add_flag(log_area, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(log_area, ev_log_drag, LV_EVENT_PRESSED,  nullptr);
    lv_obj_add_event_cb(log_area, ev_log_drag, LV_EVENT_PRESSING, nullptr);

    for (int i = 0; i < MAX_ROWS; i++) {
        row_lbl[i] = lv_label_create(log_area);
        lv_label_set_long_mode(row_lbl[i], LV_LABEL_LONG_CLIP);
        lv_obj_set_width(row_lbl[i], SCREEN_W - 8);
        lv_obj_set_style_text_font(row_lbl[i], LOG_FONTS[cfg_font_idx], 0);
        lv_obj_set_style_text_color(row_lbl[i], COL_TEXT, 0);
        lv_obj_set_style_text_letter_space(row_lbl[i], 0, 0);
        lv_label_set_text(row_lbl[i], "");
        lv_obj_set_pos(row_lbl[i], 2, i * LOG_ROW_H[cfg_font_idx]);
        lv_obj_add_flag(row_lbl[i], LV_OBJ_FLAG_HIDDEN);
    }

    sb_track = mk_panel(scr_monitor, SCREEN_W - 4, LOG_Y, 4, LOG_H, COL_SURFACE);
    sb_thumb = mk_panel(sb_track, 0, 0, 4, 20, COL_VIOLET);
    lv_obj_add_flag(sb_thumb, LV_OBJ_FLAG_HIDDEN);

    /* one byte per ring slot, mirrors the log buffer */
    if (log_cap()) {
        s_rowcache = (uint8_t *)heap_caps_malloc(log_cap(), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_rowcache) s_rowcache = (uint8_t *)heap_caps_malloc(log_cap(), MALLOC_CAP_8BIT);
        if (s_rowcache) memset(s_rowcache, 1, log_cap());
    }

    lv_timer_create(monitor_tick, 120, nullptr);
}
