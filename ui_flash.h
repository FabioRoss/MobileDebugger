#pragma once
#include <ArduinoJson.h>

/* FLASH tab — browse the SD card, pick a .bin (flashed at 0x0) or a
 * manifest.json (esp-web-tools style parts list), then flash the target. */

extern lv_obj_t *scr_monitor;      /* defined later in ui_monitor.h */

static lv_obj_t *fl_lbl_sd  = nullptr;
static lv_obj_t *fl_list    = nullptr;
static lv_obj_t *fl_lbl_sel = nullptr;
static lv_obj_t *fl_bar     = nullptr;
static lv_obj_t *fl_lbl_pct = nullptr;
static lv_obj_t *fl_btn_flash = nullptr;
static lv_obj_t *fl_lbl_cmd   = nullptr;

/* ---------- manifest / selection ---------------------------------------- */
static void fl_clear_job() { fl_nparts = 0; }

static void fl_set_single(const char *path) {
    fl_clear_job();
    strncpy(fl_parts[0].path, path, sizeof(fl_parts[0].path) - 1);
    fl_parts[0].path[sizeof(fl_parts[0].path)-1] = 0;
    fl_parts[0].offset = 0x0;
    fl_nparts = 1;
    char b[160];
    const char *nm = strrchr(path, '/'); nm = nm ? nm + 1 : path;
    snprintf(b, sizeof(b), "%s  ->  0x0  (merged image)", nm);
    lv_label_set_text(fl_lbl_sel, b);
}

/* parse a manifest; supports {"parts":[...]} or esp-web-tools
 * {"builds":[{"parts":[...]}]}, each part {"path":..,"offset":0xNNN} */
static bool fl_load_manifest(const char *path) {
    File f = SD.open(path);
    if (!f) { lv_label_set_text(fl_lbl_sel, "cannot open manifest"); return false; }
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, f);
    f.close();
    if (err) { lv_label_set_text(fl_lbl_sel, "manifest JSON parse error"); return false; }

    JsonArray parts;
    if (doc["parts"].is<JsonArray>())            parts = doc["parts"].as<JsonArray>();
    else if (doc["builds"][0]["parts"].is<JsonArray>()) parts = doc["builds"][0]["parts"].as<JsonArray>();
    else { lv_label_set_text(fl_lbl_sel, "manifest has no 'parts' array"); return false; }

    /* directory the manifest lives in, to resolve relative part paths */
    char dir[160]; strncpy(dir, path, sizeof(dir) - 1); dir[sizeof(dir)-1] = 0;
    char *slash = strrchr(dir, '/'); if (slash) *slash = 0; if (!dir[0]) strcpy(dir, "/");

    fl_clear_job();
    for (JsonObject p : parts) {
        if (fl_nparts >= 8) break;
        const char *pp = p["path"] | (const char*)nullptr;
        if (!pp) continue;
        uint32_t off = 0;
        if (p["offset"].is<const char*>()) off = strtoul(p["offset"].as<const char*>(), nullptr, 0);
        else                               off = p["offset"] | 0;
        FlashPart &fp = fl_parts[fl_nparts++];
        if (pp[0] == '/') { strncpy(fp.path, pp, sizeof(fp.path)-1); fp.path[sizeof(fp.path)-1]=0; }
        else              sd_join(fp.path, sizeof(fp.path), dir, pp);
        fp.offset = off;
    }
    if (!fl_nparts) { lv_label_set_text(fl_lbl_sel, "manifest parts empty"); return false; }

    char b[200]; int p = 0;
    p += snprintf(b + p, sizeof(b) - p, "%d part(s):", fl_nparts);
    for (int i = 0; i < fl_nparts && p < (int)sizeof(b) - 24; i++) {
        const char *nm = strrchr(fl_parts[i].path, '/'); nm = nm ? nm + 1 : fl_parts[i].path;
        p += snprintf(b + p, sizeof(b) - p, " %s@0x%lX", nm, (unsigned long)fl_parts[i].offset);
    }
    lv_label_set_text(fl_lbl_sel, b);
    return true;
}

/* ---------- file list ---------------------------------------------------- */
static void fl_populate();

static void fl_item_cb(lv_event_t *e) {
    if (g_flashing) return;
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (idx < 0 || idx >= sd_entry_count) return;
    FileEntry &en = sd_entries[idx];
    if (en.isDir) { sd_cd(en.name); fl_populate(); return; }
    char path[200]; sd_join(path, sizeof(path), sd_cwd, en.name);
    if (ends_with_ci(en.name, ".json")) fl_load_manifest(path);
    else                                fl_set_single(path);
}

static void fl_populate() {
    lv_obj_clean(fl_list);
    if (!sd_list()) {
        lv_label_set_text(fl_lbl_sd, "SD: not mounted (tap MOUNT)");
        return;
    }
    char b[64];
    snprintf(b, sizeof(b), "SD %s  %s", sd_type_str(), sd_cwd);
    lv_label_set_text(fl_lbl_sd, b);

    for (int i = 0; i < sd_entry_count; i++) {
        FileEntry &en = sd_entries[i];
        char row[110];
        if (en.isDir) snprintf(row, sizeof(row), LV_SYMBOL_DIRECTORY " %s", en.name);
        else {
            const char *ic = ends_with_ci(en.name, ".json") ? LV_SYMBOL_LIST : LV_SYMBOL_FILE;
            snprintf(row, sizeof(row), "%s %s  (%lu KB)", ic, en.name,
                     (unsigned long)((en.size + 1023) / 1024));
        }
        lv_obj_t *btn = lv_list_add_button(fl_list, nullptr, row);
        lv_obj_set_style_bg_color(btn, COL_SURFACE, 0);
        lv_obj_set_style_bg_color(btn, COL_SURFACE2, LV_STATE_PRESSED);
        lv_obj_set_style_text_color(btn, en.isDir ? COL_BLUE : COL_TEXT, 0);
        lv_obj_set_style_text_font(btn, FONT_S, 0);
        lv_obj_set_style_pad_ver(btn, 4, 0);
        lv_obj_add_event_cb(btn, fl_item_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    }
}

/* ---------- controls ----------------------------------------------------- */
static void fl_ev_mount(lv_event_t *e)   { (void)e; sd_ok = false; sd_mount(); fl_populate(); }
static void fl_ev_refresh(lv_event_t *e) { (void)e; fl_populate(); }
static void fl_ev_baud(lv_event_t *e)    { fl_baud_idx = lv_dropdown_get_selected((lv_obj_t*)lv_event_get_target(e)); }
static void fl_ev_verify(lv_event_t *e)  { fl_verify = lv_obj_has_state((lv_obj_t*)lv_event_get_target(e), LV_STATE_CHECKED); }
static void fl_ev_abort(lv_event_t *e)   { (void)e; if (g_flashing) fl_abort = true; }
static void fl_ev_rmode(lv_event_t *e)   { fl_reset_mode = lv_dropdown_get_selected((lv_obj_t*)lv_event_get_target(e)); }
static void fl_ev_cmd(lv_event_t *e)     { (void)e; kb_open(KB_SOFTCMD, 0, fl_softcmd); }

void flash_refresh_softcmd() {
    if (fl_lbl_cmd) lv_label_set_text(fl_lbl_cmd, fl_softcmd);
}

static void fl_ev_flash(lv_event_t *e) {
    (void)e;
    if (g_flashing) return;
    if (fl_nparts == 0) { lv_label_set_text(fl_lbl_sel, "select a .bin or manifest first"); return; }
    log_push("=== starting flash ===", SEV_SYS);
    lv_screen_load(scr_monitor);        /* watch it live on the monitor */
    flasher_start();
}

/* poll flasher state, drive the bar, drain the log mailbox */
static void fl_tick(lv_timer_t *t) {
    (void)t;
    if (fl_mail_ready) { log_push(fl_mail, fl_mail_sev); fl_mail_ready = false; }
    if (!fl_bar) return;
    lv_bar_set_value(fl_bar, fl_pct, LV_ANIM_OFF);
    const char *st = "idle";
    switch (fl_state) {
        case FL_CONNECT: st = "connecting"; break;
        case FL_ERASE:   st = "erasing";    break;
        case FL_WRITE:   st = "writing";    break;
        case FL_VERIFY:  st = "verifying";  break;
        case FL_DONE:    st = "done";       break;
        case FL_ERR:     st = "error";      break;
    }
    char b[64];
    snprintf(b, sizeof(b), "%s  %d%%  (%s)", st, fl_pct, fl_chip);
    lv_label_set_text(fl_lbl_pct, b);
    lv_obj_set_style_bg_color(fl_bar,
        fl_state == FL_ERR ? COL_RED : (fl_state == FL_DONE ? COL_GREEN : COL_VIOLET),
        LV_PART_INDICATOR);
}

void flash_build_tab(lv_obj_t *parent) {
    fl_lbl_sd = mk_lbl(parent, 6, 4, "SD: tap MOUNT", FONT_S, COL_LABEL);
    mk_btn(parent, 312, 2, 80, 20, "MOUNT",   fl_ev_mount,   nullptr, COL_GREEN);
    mk_btn(parent, 396, 2, 78, 20, "REFRESH", fl_ev_refresh, nullptr, COL_BLUE);

    fl_list = lv_list_create(parent);
    lv_obj_set_pos(fl_list, 4, 24);
    lv_obj_set_size(fl_list, 300, 152);
    lv_obj_set_style_bg_color(fl_list, COL_BG, 0);
    lv_obj_set_style_border_color(fl_list, COL_BORDER, 0);
    lv_obj_set_style_border_width(fl_list, 1, 0);
    lv_obj_set_style_pad_all(fl_list, 2, 0);

    /* right column */
    mk_lbl(parent, 312, 26, "Speed", FONT_S, COL_LABEL);
    mk_dd (parent, 312, 36, 162, "115200\n230400\n460800\n921600", fl_baud_idx, fl_ev_baud, nullptr);
    mk_lbl(parent, 312, 68, "Enter bootloader", FONT_S, COL_LABEL);
    mk_btn(parent, 430, 64, 44, 16, "CMD", fl_ev_cmd, nullptr, COL_BLUE);
    mk_dd (parent, 312, 78, 162, "auto (IO0+EN)\nsoft command\nmanual BOOT+RST",
           fl_reset_mode, fl_ev_rmode, nullptr);
    fl_lbl_cmd = mk_lbl(parent, 312, 108, fl_softcmd, FONT_S, COL_VIOLET);
    mk_sw (parent, 312, 120, fl_verify, fl_ev_verify, nullptr);
    mk_lbl(parent, 360, 124, "verify (MD5)", FONT_M, COL_TEXT);
    fl_btn_flash = mk_btn(parent, 312, 146, 162, 28, LV_SYMBOL_DOWNLOAD " FLASH", fl_ev_flash, nullptr, COL_GREEN);
    mk_btn(parent, 312, 176, 162, 18, "ABORT", fl_ev_abort, nullptr, COL_RED);

    fl_lbl_sel = mk_lbl(parent, 4, 180, "select a .bin or manifest", FONT_S, COL_AMBER);
    lv_obj_set_width(fl_lbl_sel, 300);

    fl_bar = lv_bar_create(parent);
    lv_obj_set_pos(fl_bar, 4, 198);
    lv_obj_set_size(fl_bar, 300, 16);
    lv_bar_set_range(fl_bar, 0, 100);
    lv_bar_set_value(fl_bar, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(fl_bar, COL_SURFACE2, 0);
    lv_obj_set_style_bg_color(fl_bar, COL_VIOLET, LV_PART_INDICATOR);
    lv_obj_set_style_radius(fl_bar, 2, 0);
    lv_obj_set_style_radius(fl_bar, 2, LV_PART_INDICATOR);
    fl_lbl_pct = mk_lbl(parent, 8, 199, "idle", FONT_S, COL_TEXT);

    lv_timer_create(fl_tick, 200, nullptr);
}
