#pragma once
#include <ctype.h>
/* Ring buffer of parsed log lines living in PSRAM.
 * 1000 x 128 bytes = ~125 KB, well outside the 128 KB LVGL heap.            */

#define LOG_LINE_LEN     120
#define LOG_LINES_PSRAM  1000
#define LOG_LINES_INT    150      /* fallback if no PSRAM is available */

struct LogLine {
    uint32_t ms;
    uint8_t  sev;
    uint8_t  len;
    char     text[LOG_LINE_LEN];
};

static LogLine *s_log   = nullptr;
static uint16_t s_cap   = 0;
static uint16_t s_count = 0;
static uint16_t s_head  = 0;     /* next slot to write */
uint32_t        log_seq = 0;     /* monotonic push counter, never reset */

bool log_init() {
    size_t want = (size_t)LOG_LINES_PSRAM * sizeof(LogLine);
    s_log = (LogLine *)heap_caps_malloc(want, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_log) { s_cap = LOG_LINES_PSRAM; }
    else {
        s_log = (LogLine *)heap_caps_malloc((size_t)LOG_LINES_INT * sizeof(LogLine),
                                            MALLOC_CAP_8BIT);
        s_cap = s_log ? LOG_LINES_INT : 0;
    }
    s_count = s_head = 0;
    return s_log != nullptr;
}

void log_clear() {
    s_count = s_head = 0;
    rx_lines = 0;
    rx_noise = 0;
    scroll_off = 0;
    monitor_mark_dirty();
}

const LogLine *log_at(uint16_t i) {          /* i: 0 = oldest kept line */
    if (i >= s_count) return nullptr;
    uint16_t start = (uint16_t)((s_head + s_cap - s_count) % s_cap);
    return &s_log[(start + i) % s_cap];
}

uint16_t log_count() { return s_count; }
uint16_t log_cap()   { return s_cap; }
uint16_t log_head()  { return s_head; }

/* ring slot backing logical index i (0 = oldest kept line) */
uint16_t log_slot_of(uint16_t i) {
    uint16_t start = (uint16_t)((s_head + s_cap - s_count) % s_cap);
    return (uint16_t)((start + i) % s_cap);
}

/* --- case-insensitive substring, no GNU extensions ----------------------- */
static bool ci_contains(const char *hay, const char *needle) {
    if (!*needle) return true;
    for (const char *h = hay; *h; ++h) {
        const char *a = h, *b = needle;
        while (*a && *b && (tolower((unsigned char)*a) == tolower((unsigned char)*b))) { ++a; ++b; }
        if (!*b) return true;
    }
    return false;
}

/* --- keyword heuristic used when the source emits no ANSI colour --------- */
uint8_t log_classify(const char *s) {
    while (*s == ' ') s++;
    /* ESP-IDF prefix form:  "E (1234) tag: msg" */
    if (s[0] && s[1] == ' ' && s[2] == '(') {
        switch (s[0]) {
            case 'E': return SEV_ERR;
            case 'W': return SEV_WARN;
            case 'I': return SEV_INFO;
            case 'D': case 'V': return SEV_DEBUG;
        }
    }
    if (ci_contains(s, "error")  || ci_contains(s, "fail")  || ci_contains(s, "fatal")  ||
        ci_contains(s, "panic")  || ci_contains(s, "abort") || ci_contains(s, "assert") ||
        ci_contains(s, "guru meditation") || ci_contains(s, "backtrace") ||
        ci_contains(s, "exception") || ci_contains(s, "timeout") || ci_contains(s, "denied"))
        return SEV_ERR;
    if (ci_contains(s, "warn") || ci_contains(s, "retry") || ci_contains(s, "deprecat") ||
        ci_contains(s, "missing") || ci_contains(s, "unknown"))
        return SEV_WARN;
    if (ci_contains(s, "success") || ci_contains(s, " ok") || ci_contains(s, "ok ") ||
        ci_contains(s, "ready")   || ci_contains(s, "connected") || ci_contains(s, "done") ||
        ci_contains(s, "mounted") || ci_contains(s, "started"))
        return SEV_OK;
    if (ci_contains(s, "debug") || ci_contains(s, "verbose")) return SEV_DEBUG;
    if (ci_contains(s, "info"))  return SEV_INFO;
    return SEV_PLAIN;
}

void log_push(const char *txt, uint8_t sev) {
    if (!s_log || !s_cap) return;
    if (sev == 0xFF) sev = log_classify(txt);     /* 0xFF = auto-classify */
    LogLine *l = &s_log[s_head];
    l->ms  = millis();
    l->sev = sev;
    size_t n = strnlen(txt, LOG_LINE_LEN - 1);
    memcpy(l->text, txt, n);
    l->text[n] = 0;
    l->len = (uint8_t)n;
    s_head = (uint16_t)((s_head + 1) % s_cap);
    log_seq++;
    if (s_count < s_cap) s_count++;
    rx_lines++;
    /* keep the viewport anchored on the same line while scrolled back */
    if (scroll_off > 0 && scroll_off < (int32_t)s_cap) scroll_off++;
    monitor_mark_dirty();
}

bool log_line_visible(const LogLine *l) {
    if (!cfg_filter[0]) return true;
    return ci_contains(l->text, cfg_filter);
}
