#pragma once
#include <driver/gpio.h>
/* Everything that talks to UART1 (the board under test). */

const uint32_t BAUD_TABLE[] = {
    300, 1200, 2400, 4800, 9600, 19200, 38400, 57600,
    74880, 115200, 230400, 460800, 921600, 1000000, 1500000
};
#define BAUD_COUNT (sizeof(BAUD_TABLE) / sizeof(BAUD_TABLE[0]))
static const char *BAUD_OPTS =
    "300\n1200\n2400\n4800\n9600\n19200\n38400\n57600\n"
    "74880\n115200\n230400\n460800\n921600\n1000000\n1500000";

const uint32_t FORMAT_TABLE[] = { SERIAL_8N1, SERIAL_8E1, SERIAL_8O1, SERIAL_8N2, SERIAL_7E1 };
const char *FORMAT_NAMES[]    = { "8N1", "8E1", "8O1", "8N2", "7E1" };
static const char *FORMAT_OPTS = "8N1\n8E1\n8O1\n8N2\n7E1";

static const char *EOL_OPTS  = "none\nLF (\\n)\nCR+LF\nCR (\\r)";
static const char *EOL_STR[] = { "", "\n", "\r\n", "\r" };

/* --- line assembly state ------------------------------------------------- */
static char     s_line[LOG_LINE_LEN];
static uint8_t  s_linelen   = 0;
static uint8_t  s_line_sev  = 0xFF;   /* 0xFF = decide by keyword at commit */
static bool     s_cr_seen   = false;
static uint8_t  s_ansi_state = 0;     /* 0 idle, 1 got ESC, 2 in CSI */
static char     s_ansi_buf[16];
static uint8_t  s_ansi_len  = 0;

/* hex view state */
static uint8_t  s_hexbuf[16];
static uint8_t  s_hexlen = 0;
static uint32_t s_hexaddr = 0;

void uart_apply() {
    Serial1.end();
    delay(5);
    Serial1.setRxBufferSize(8192);
    Serial1.begin(BAUD_TABLE[cfg_baud_idx], FORMAT_TABLE[cfg_format_idx],
                  UART_RX_PIN, UART_TX_PIN);
    /* Keep RX pulled high so an unplugged / hi-Z target line idles cleanly
     * instead of picking up glitches as 0xFF bytes. gpio_set_pull_mode only
     * touches the pull config, so the UART matrix routing survives. */
    gpio_set_pull_mode((gpio_num_t)UART_RX_PIN, GPIO_PULLUP_ONLY);
    s_linelen = 0; s_cr_seen = false; s_ansi_state = 0; s_line_sev = 0xFF;
    s_hexlen = 0;
}

/* --- internals ----------------------------------------------------------- */
static void commit_line() {
    s_line[s_linelen] = 0;
    log_push(s_line, s_line_sev);
    s_linelen  = 0;
    s_line_sev = 0xFF;
}

static void flush_hex() {
    if (!s_hexlen) return;
    char out[LOG_LINE_LEN];
    int p = snprintf(out, sizeof(out), "%04lX  ", (unsigned long)s_hexaddr);
    for (uint8_t i = 0; i < 16; i++) {
        if (i < s_hexlen) p += snprintf(out + p, sizeof(out) - p, "%02X ", s_hexbuf[i]);
        else              p += snprintf(out + p, sizeof(out) - p, "   ");
        if (i == 7) p += snprintf(out + p, sizeof(out) - p, " ");
    }
    p += snprintf(out + p, sizeof(out) - p, " |");
    for (uint8_t i = 0; i < s_hexlen; i++) {
        char c = (s_hexbuf[i] >= 0x20 && s_hexbuf[i] < 0x7F) ? (char)s_hexbuf[i] : '.';
        if (p < (int)sizeof(out) - 2) out[p++] = c;
    }
    if (p < (int)sizeof(out) - 1) out[p++] = '|';
    out[p] = 0;
    log_push(out, SEV_PLAIN);
    s_hexaddr += s_hexlen;
    s_hexlen = 0;
}

/* map an ANSI SGR sequence onto one of our severity colours */
static void apply_sgr(const char *params) {
    int codes[8], n = 0;
    const char *p = params;
    while (*p && n < 8) {
        codes[n++] = atoi(p);
        while (*p && *p != ';') p++;
        if (*p == ';') p++;
    }
    for (int i = 0; i < n; i++) {
        switch (codes[i]) {
            case 0:  s_line_sev = 0xFF;      break;   /* reset -> heuristic */
            case 31: case 91: s_line_sev = SEV_ERR;   break;
            case 33: case 93: s_line_sev = SEV_WARN;  break;
            case 32: case 92: s_line_sev = SEV_OK;    break;
            case 34: case 94: s_line_sev = SEV_INFO;  break;
            case 36: case 96: s_line_sev = SEV_DEBUG; break;
            default: break;
        }
    }
}

static void append_char(char c) {
    if (s_linelen >= LOG_LINE_LEN - 1) {
        /* Buffer full: split the entry, but lock in the colour first so the
           continuation is not re-classified (and rendered) differently. */
        uint8_t keep = s_line_sev;
        if (keep == 0xFF) keep = log_classify(s_line);
        commit_line();
        s_line_sev = keep;
    }
    s_line[s_linelen++] = c;
}

static void feed_byte(uint8_t b) {
    if (cfg_hexview) {
        s_hexbuf[s_hexlen++] = b;
        if (s_hexlen == 16) flush_hex();
        return;
    }

    /* ANSI escape handling */
    if (s_ansi_state == 1) {
        if (b == '[') { s_ansi_state = 2; s_ansi_len = 0; }
        else            s_ansi_state = 0;
        return;
    }
    if (s_ansi_state == 2) {
        if (b >= 0x40 && b <= 0x7E) {                 /* final byte */
            s_ansi_buf[s_ansi_len] = 0;
            if (b == 'm' && cfg_ansi) apply_sgr(s_ansi_buf);
            s_ansi_state = 0;
        } else if (s_ansi_len < sizeof(s_ansi_buf) - 1) {
            s_ansi_buf[s_ansi_len++] = (char)b;
        }
        return;
    }
    if (b == 0x1B) { s_ansi_state = 1; return; }

    /* line breaks: handles \n, \r\n and bare \r */
    if (b == '\r') { s_cr_seen = true; return; }
    if (b == '\n') { s_cr_seen = false; commit_line(); return; }
    if (s_cr_seen) { s_cr_seen = false; commit_line(); }

    if (b == '\t') { append_char(' '); append_char(' '); return; }
    if (b >= 0x20 && b < 0x7F) { append_char((char)b); return; }

    /* Anything reaching here is a control byte or has bit 7 set, which on a
     * text log is almost always line noise rather than data. A glitch on an
     * idle-high line frames as 0xFF, a slightly wider one as 0xFE or 0xFC,
     * a line held low as 0x00 - hence filtering the whole class, not a fixed
     * list of values. Hex view is deliberately never filtered. */
    if (cfg_hidenoise) { rx_noise++; return; }

    /* otherwise show it escaped, so framing/baud errors stay visible */
    char esc[5];
    snprintf(esc, sizeof(esc), "\\%02X", b);
    for (int i = 0; esc[i]; i++) append_char(esc[i]);
}

/* --- main pump, called from loop() --------------------------------------- */
void uart_poll() {
    if (g_flashing) return;          /* the flasher owns UART1 during a flash */
    static uint32_t last_flush = 0;
    int budget = 2048;
    while (Serial1.available() > 0 && budget-- > 0) {
        int c = Serial1.read();
        if (c < 0) break;
        rx_bytes++;
        if (cfg_bridge) Serial.write((uint8_t)c);
        if (!paused) feed_byte((uint8_t)c);
    }
    /* commit a dangling partial line so prompts without newline still show */
    if (!cfg_hexview && s_linelen && millis() - last_flush > 300) {
        last_flush = millis();
        commit_line();
    }
    if (cfg_hexview && s_hexlen && millis() - last_flush > 300) {
        last_flush = millis();
        flush_hex();
    }
    if (cfg_bridge) {
        while (Serial.available() > 0) {
            int c = Serial.read();
            if (c < 0) break;
            Serial1.write((uint8_t)c);
            tx_bytes++;
        }
    }
}

void uart_send(const char *txt) {
    if (!txt || !*txt) return;
    size_t n = strlen(txt);
    Serial1.write((const uint8_t *)txt, n);
    const char *eol = EOL_STR[cfg_eol_idx];
    if (*eol) Serial1.write((const uint8_t *)eol, strlen(eol));
    tx_bytes += n + strlen(eol);

    char echo[LOG_LINE_LEN];
    snprintf(echo, sizeof(echo), ">> %s", txt);
    log_push(echo, SEV_TX);
}

/* ------------------------------------------------------------------------ *
 *  AUTO-BAUD DETECTION
 *  Walks the baud table, scores ~350 ms of traffic per rate on how much of
 *  it looks like plain text, then locks onto the winner. Runs as an LVGL
 *  timer so the UI stays alive.
 * ------------------------------------------------------------------------ */
static lv_timer_t *ab_timer   = nullptr;
static int8_t      ab_idx     = -1;
static int32_t     ab_best    = -1;
static int32_t     ab_bestscore = -1000000;
static char        ab_status[64] = "";

static void ab_step(lv_timer_t *t) {
    /* score whatever arrived at the rate we just configured */
    if (ab_idx >= 0) {
        int32_t good = 0, bad = 0, lines = 0;
        while (Serial1.available()) {
            int c = Serial1.read();
            if (c < 0) break;
            if (c == '\n') { lines++; good++; }
            else if (c == '\r' || c == '\t' || (c >= 0x20 && c < 0x7F)) good++;
            else bad++;
        }
        int32_t score = good - 4 * bad + 5 * lines;
        if (good + bad == 0) score = -1000;          /* silence proves nothing */
        if (score > ab_bestscore) { ab_bestscore = score; ab_best = ab_idx; }
    }

    ab_idx++;
    if (ab_idx >= (int8_t)BAUD_COUNT) {
        lv_timer_delete(t);
        ab_timer = nullptr;
        if (ab_best >= 0 && ab_bestscore > 0) {
            cfg_baud_idx = (uint8_t)ab_best;
            save_settings();
            snprintf(ab_status, sizeof(ab_status), "Locked: %lu baud (score %ld)",
                     (unsigned long)BAUD_TABLE[ab_best], (long)ab_bestscore);
        } else {
            snprintf(ab_status, sizeof(ab_status), "No readable traffic found");
        }
        uart_apply();
        log_push(ab_status, SEV_SYS);
        tools_set_autobaud_status(ab_status);
        return;
    }

    Serial1.end();
    delay(2);
    Serial1.setRxBufferSize(8192);
    Serial1.begin(BAUD_TABLE[ab_idx], FORMAT_TABLE[cfg_format_idx], UART_RX_PIN, UART_TX_PIN);
    snprintf(ab_status, sizeof(ab_status), "Testing %lu ... (%d/%d)",
             (unsigned long)BAUD_TABLE[ab_idx], ab_idx + 1, (int)BAUD_COUNT);
    tools_set_autobaud_status(ab_status);
}

void autobaud_start() {
    if (ab_timer) return;
    ab_idx = -1; ab_best = -1; ab_bestscore = -1000000;
    log_push("Auto-baud: sweeping, keep the target talking...", SEV_SYS);
    ab_timer = lv_timer_create(ab_step, 350, nullptr);
}

bool autobaud_running() { return ab_timer != nullptr; }
