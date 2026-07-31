#pragma once
/* FLEET tab — push a RaceBoard image to the whole fleet over LoRa + ESP-NOW.
 *
 * Same shape as the FLASH tab: browse the SD card, pick a .bin, hit the button.
 * The difference is that this one reaches every powered board at once instead
 * of the single target on the end of a UART cable.
 *
 * The left-hand list toggles between the file browser and the board roster,
 * because 480x272 has no room for both at once. */

#include <ctype.h>

extern lv_obj_t *scr_monitor;      /* both defined later in ui_monitor.h */
extern lv_obj_t *scr_tools;

static lv_obj_t *fo_lbl_sd   = nullptr;
static lv_obj_t *fo_list     = nullptr;
static lv_obj_t *fo_lbl_sel  = nullptr;
static lv_obj_t *fo_bar      = nullptr;
static lv_obj_t *fo_lbl_stat = nullptr;
static lv_obj_t *fo_btn_files= nullptr;
static lv_obj_t *fo_btn_brds = nullptr;

static int  fo_view = 0;            /* 0 = files, 1 = boards */
static bool fo_have_sel = false;

/* ---------- selection ---------------------------------------------------- */

/* Pull a version out of the filename so the announce carries something the
 * operator recognises: "raceboard-0.9.11.bin" -> "0.9.11". Falls back to the
 * stem if there are no digits. */
static void fo_version_from_name(const char *path) {
    const char *nm = strrchr(path, '/');
    nm = nm ? nm + 1 : path;

    const char *s = nullptr, *e = nullptr;
    for (const char *p = nm; *p; p++) {
        if (isdigit((unsigned char)*p)) { if (!s) s = p; e = p + 1; }
        else if (*p == '.' && s && isdigit((unsigned char)p[1])) continue;
        else if (s && *p != '.') break;
    }
    if (s && e && (e - s) < OTA_VERSION_LEN) {
        size_t n = e - s;
        memcpy(fo_version, s, n);
        fo_version[n] = 0;
    } else {
        strncpy(fo_version, nm, OTA_VERSION_LEN);
        fo_version[OTA_VERSION_LEN] = 0;
    }
}

static void fo_set_image(const char *path) {
    strncpy(fo_path, path, sizeof(fo_path) - 1);
    fo_path[sizeof(fo_path) - 1] = 0;
    fo_version_from_name(path);
    fo_have_sel = true;

    const char *nm = strrchr(path, '/'); nm = nm ? nm + 1 : path;
    char b[180];
    snprintf(b, sizeof(b), "%s   ->  announce as v%s", nm, fo_version);
    lv_label_set_text(fo_lbl_sel, b);
}

/* ---------- list --------------------------------------------------------- */

static void fo_populate();

static void fo_item_cb(lv_event_t *e) {
    if (fo_running) return;
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (idx < 0 || idx >= sd_entry_count) return;
    FileEntry &en = sd_entries[idx];
    if (en.isDir) { sd_cd(en.name); fo_populate(); return; }
    char path[200]; sd_join(path, sizeof(path), sd_cwd, en.name);
    fo_set_image(path);
}

static const char *fo_state_str(uint8_t s) {
    switch (s) {
        case OTA_STATE_APP:      return "app";
        case OTA_STATE_DEFERRED: return "queued";
        case OTA_STATE_UPDATER:  return "updater";
        case OTA_STATE_OK:       return "OK";
        case OTA_STATE_FAILED:   return "FAILED";
    }
    return "?";
}

static const char *fo_err_str(uint8_t e) {
    switch (e) {
        case OTA_ERR_NONE:      return "";
        case OTA_ERR_NO_PSRAM:  return " psram";
        case OTA_ERR_TOO_BIG:   return " too-big";
        case OTA_ERR_SHA:       return " sha";
        case OTA_ERR_FLASH:     return " flash";
        case OTA_ERR_TIMEOUT:   return " timeout";
        case OTA_ERR_NO_PART:   return " no-part";
        case OTA_ERR_BAD_IMAGE: return " bad-img";
    }
    return " ?";
}

static void fo_populate() {
    lv_obj_clean(fo_list);

    if (fo_view == 1) {
        char h[64];
        snprintf(h, sizeof(h), "%d board(s) heard", fo_node_count);
        lv_label_set_text(fo_lbl_sd, h);
        for (int i = 0; i < fo_node_count; i++) {
            FleetNode &n = fo_nodes[i];
            char row[110];
            snprintf(row, sizeof(row), "%02X%02X%02X  %-7s %3d%%  v%s%s",
                     n.mac[3], n.mac[4], n.mac[5], fo_state_str(n.state),
                     n.progress, n.version[0] ? n.version : "?", fo_err_str(n.error));
            lv_obj_t *b = lv_list_add_button(fo_list, nullptr, row);
            lv_obj_set_style_bg_color(b, COL_SURFACE, 0);
            lv_obj_set_style_text_font(b, FONT_S, 0);
            lv_obj_set_style_pad_ver(b, 4, 0);
            lv_obj_set_style_text_color(b,
                n.error ? COL_RED : (n.complete ? COL_GREEN :
                (n.ready ? COL_BLUE : COL_TEXT)), 0);
        }
        if (!fo_node_count) {
            lv_obj_t *b = lv_list_add_button(fo_list, nullptr,
                "no boards yet - tap ROLLCALL");
            lv_obj_set_style_bg_color(b, COL_SURFACE, 0);
            lv_obj_set_style_text_font(b, FONT_S, 0);
            lv_obj_set_style_text_color(b, COL_LABEL, 0);
        }
        return;
    }

    if (!sd_list()) { lv_label_set_text(fo_lbl_sd, "SD: not mounted (tap MOUNT)"); return; }
    char b[64];
    snprintf(b, sizeof(b), "SD %s  %s", sd_type_str(), sd_cwd);
    lv_label_set_text(fo_lbl_sd, b);

    for (int i = 0; i < sd_entry_count; i++) {
        FileEntry &en = sd_entries[i];
        /* Only .bin is meaningful here — the fleet path takes a single merged
         * app image, not a multi-part manifest. */
        if (!en.isDir && !ends_with_ci(en.name, ".bin")) continue;
        char row[110];
        if (en.isDir) snprintf(row, sizeof(row), LV_SYMBOL_DIRECTORY " %s", en.name);
        else snprintf(row, sizeof(row), LV_SYMBOL_FILE " %s  (%lu KB)", en.name,
                      (unsigned long)((en.size + 1023) / 1024));
        lv_obj_t *btn = lv_list_add_button(fo_list, nullptr, row);
        lv_obj_set_style_bg_color(btn, COL_SURFACE, 0);
        lv_obj_set_style_bg_color(btn, COL_SURFACE2, LV_STATE_PRESSED);
        lv_obj_set_style_text_color(btn, en.isDir ? COL_BLUE : COL_TEXT, 0);
        lv_obj_set_style_text_font(btn, FONT_S, 0);
        lv_obj_set_style_pad_ver(btn, 4, 0);
        lv_obj_add_event_cb(btn, fo_item_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    }
}

/* ---------- controls ----------------------------------------------------- */

static void fo_ev_mount(lv_event_t *e)   { (void)e; sd_ok = false; sd_mount(); fo_view = 0; fo_populate(); }
static void fo_ev_files(lv_event_t *e)   { (void)e; fo_view = 0; fo_populate(); }
static void fo_ev_boards(lv_event_t *e)  { (void)e; fo_view = 1; fo_populate(); }

static void fo_ev_rollcall(lv_event_t *e) {
    (void)e;
    if (!fleet_lora_begin()) { log_push("LoRa init failed", SEV_ERR); return; }
    log_push("rollcall sent", SEV_SYS);
    fleet_rollcall();
    fo_view = 1;
    fo_populate();
}

static void fo_ev_abort(lv_event_t *e) {
    (void)e;
    if (fo_running) { fo_abort = true; fleet_abort_broadcast(); log_push("fleet abort", SEV_WARN); }
}

static void fo_ev_flash(lv_event_t *e) {
    (void)e;
    if (fo_running) return;
    if (!fo_have_sel) { lv_label_set_text(fo_lbl_sel, "select a .bin first"); return; }
    if (!fleet_begin()) return;
    log_push("=== starting fleet update ===", SEV_SYS);
    lv_screen_load(scr_monitor);          /* watch it live, same as FLASH */
    fleet_start();
}

/* ---------- tick --------------------------------------------------------- */

static void fo_tick(lv_timer_t *t) {
    (void)t;
    if (fo_mail_ready) { log_push(fo_mail, fo_mail_sev); fo_mail_ready = false; }

    /* LoRa is drained from loop() at full rate - see the note there. Polling
     * it from this 400 ms timer is what made ROLLCALL replies go missing. */

    if (!fo_bar) return;
    lv_bar_set_value(fo_bar, fo_pct, LV_ANIM_OFF);

    const char *st = "idle";
    switch (fo_state) {
        case FO_LOADING:  st = "loading";   break;
        case FO_ANNOUNCE: st = "announcing";break;
        case FO_SENDING:  st = "sending";   break;
        case FO_REPAIR:   st = "repairing"; break;
        case FO_DONE:     st = "done";      break;
        case FO_ERR:      st = "error";     break;
    }
    int ready = 0, ok = 0;
    for (int i = 0; i < fo_node_count; i++) {
        if (fo_nodes[i].ready) ready++;
        if (fo_nodes[i].state == OTA_STATE_OK) ok++;
    }
    char b[80];
    if (fo_round) snprintf(b, sizeof(b), "%s %d%%  r%d  %d ready  %d ok",
                           st, fo_pct, fo_round, ready, ok);
    else          snprintf(b, sizeof(b), "%s %d%%  %d ready  %d ok", st, fo_pct, ready, ok);
    lv_label_set_text(fo_lbl_stat, b);

    /* Once the image is parsed, show what it actually is rather than what the
     * filename claims - the last chance to catch a wrong .bin before it goes
     * out to the whole fleet. */
    if (fo_proj[0]) {
        char s[180];
        const char *nm = strrchr(fo_path, '/'); nm = nm ? nm + 1 : fo_path;
        snprintf(s, sizeof(s), "%s  ->  %s v%s", nm, fo_proj, fo_version);
        lv_label_set_text(fo_lbl_sel, s);
    }

    lv_obj_set_style_bg_color(fo_bar,
        fo_state == FO_ERR ? COL_RED : (fo_state == FO_DONE ? COL_GREEN : COL_CYAN),
        LV_PART_INDICATOR);

    /* keep the roster live while it is on screen */
    if (fo_view == 1 && lv_screen_active() == scr_tools) fo_populate();
}

/* ---------- build -------------------------------------------------------- */

void fleet_build_tab(lv_obj_t *parent) {
    fo_lbl_sd = mk_lbl(parent, 6, 4, "SD: tap MOUNT", FONT_S, COL_LABEL);
    mk_btn(parent, 312, 2, 80, 20, "MOUNT",    fo_ev_mount,    nullptr, COL_GREEN);
    mk_btn(parent, 396, 2, 78, 20, "ROLLCALL", fo_ev_rollcall, nullptr, COL_BLUE);

    fo_btn_files = mk_btn(parent, 4,  22, 74, 18, "FILES",  fo_ev_files,  nullptr, COL_BLUE);
    fo_btn_brds  = mk_btn(parent, 82, 22, 74, 18, "BOARDS", fo_ev_boards, nullptr, COL_CYAN);

    fo_list = lv_list_create(parent);
    lv_obj_set_pos(fo_list, 4, 42);
    lv_obj_set_size(fo_list, 300, 134);
    lv_obj_set_style_bg_color(fo_list, COL_BG, 0);
    lv_obj_set_style_border_color(fo_list, COL_BORDER, 0);
    lv_obj_set_style_border_width(fo_list, 1, 0);
    lv_obj_set_style_pad_all(fo_list, 2, 0);

    /* right column */
    mk_lbl(parent, 312, 28,
           "LoRa announces,\nESP-NOW carries the\nimage. All boards\nupdate at once.",
           FONT_S, COL_LABEL);
    mk_lbl(parent, 312, 84, "boards must be\npowered and idle", FONT_S, COL_AMBER);

    mk_btn(parent, 312, 118, 162, 30, LV_SYMBOL_UPLOAD " FLASH FLEET",
           fo_ev_flash, nullptr, COL_GREEN);
    mk_btn(parent, 312, 152, 162, 20, "ABORT", fo_ev_abort, nullptr, COL_RED);

    fo_lbl_sel = mk_lbl(parent, 4, 180, "select a .bin", FONT_S, COL_AMBER);
    lv_obj_set_width(fo_lbl_sel, 300);

    fo_bar = lv_bar_create(parent);
    lv_obj_set_pos(fo_bar, 4, 198);
    lv_obj_set_size(fo_bar, 300, 16);
    lv_bar_set_range(fo_bar, 0, 100);
    lv_bar_set_value(fo_bar, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(fo_bar, COL_SURFACE2, 0);
    lv_obj_set_style_bg_color(fo_bar, COL_CYAN, LV_PART_INDICATOR);
    lv_obj_set_style_radius(fo_bar, 2, 0);

    fo_lbl_stat = mk_lbl(parent, 312, 180, "idle", FONT_S, COL_TEXT);
    lv_obj_set_width(fo_lbl_stat, 162);

    lv_timer_create(fo_tick, 400, nullptr);
}
