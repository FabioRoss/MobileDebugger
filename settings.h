#pragma once
/* Single blob in NVS. Bump SETTINGS_VERSION whenever the struct layout
 * changes — stale blobs are discarded rather than misread.                  */

#define SETTINGS_VERSION 2

struct SavedSettings {
    uint8_t version;
    uint8_t baud_idx, format_idx, eol_idx, font_idx;
    uint8_t timestamps, hexview, ansi, autoscroll, bridge, hidenoise;
    char    macro[MACRO_COUNT][MACRO_LEN];
    char    filter[FILTER_LEN];
};

void save_settings() {
    SavedSettings s{};
    s.version    = SETTINGS_VERSION;
    s.baud_idx   = cfg_baud_idx;
    s.format_idx = cfg_format_idx;
    s.eol_idx    = cfg_eol_idx;
    s.font_idx   = cfg_font_idx;
    s.timestamps = cfg_timestamps;
    s.hexview    = cfg_hexview;
    s.ansi       = cfg_ansi;
    s.autoscroll = cfg_autoscroll;
    s.bridge     = cfg_bridge;
    s.hidenoise  = cfg_hidenoise;
    memcpy(s.macro, cfg_macro, sizeof(s.macro));
    memcpy(s.filter, cfg_filter, sizeof(s.filter));

    prefs.begin("dbgtool", false);
    size_t w = prefs.putBytes("cfg", &s, sizeof(s));
    prefs.end();
    Serial.printf("[Preferences] saved %u/%u bytes\n", (unsigned)w, (unsigned)sizeof(s));
}

void load_settings() {
    prefs.begin("dbgtool", true);
    size_t len = prefs.getBytesLength("cfg");
    SavedSettings s{};
    if (len == sizeof(SavedSettings)) prefs.getBytes("cfg", &s, sizeof(s));
    prefs.end();

    if (len != sizeof(SavedSettings) || s.version != SETTINGS_VERSION) {
        Serial.printf("[Preferences] no valid blob (len=%u) — using defaults\n", (unsigned)len);
        return;
    }
    cfg_baud_idx   = s.baud_idx;
    cfg_format_idx = s.format_idx;
    cfg_eol_idx    = s.eol_idx;
    cfg_font_idx   = s.font_idx;
    cfg_timestamps = s.timestamps;
    cfg_hexview    = s.hexview;
    cfg_ansi       = s.ansi;
    cfg_autoscroll = s.autoscroll;
    cfg_bridge     = s.bridge;
    cfg_hidenoise  = s.hidenoise;
    memcpy(cfg_macro, s.macro, sizeof(cfg_macro));
    memcpy(cfg_filter, s.filter, sizeof(cfg_filter));
    Serial.println("[Preferences] settings restored");
}
