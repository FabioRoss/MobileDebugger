#pragma once
/* =========================================================================
 *  Self-contained ESP ROM serial-bootloader flasher.
 *  Talks the esptool SLIP protocol over UART1 to a target ESP chip whose
 *  IO0/EN we drive via TGT_IO0 / TGT_EN. No stub is uploaded: writes are
 *  uncompressed (ROM speed) but every part is MD5-verified afterwards.
 *
 *  Supported targets: ESP32, ESP32-S2, ESP32-S3, ESP32-C3/C6 (auto-detected).
 *  Runs on core 0 as a FreeRTOS task; the LVGL UI on core 1 only reads the
 *  volatile progress fields and drains the one-slot log mailbox.
 * ========================================================================= */
#include <FS.h>
#include <SD.h>

/* ---- ROM commands ---- */
#define CMD_FLASH_BEGIN 0x02
#define CMD_FLASH_DATA  0x03
#define CMD_FLASH_END   0x04
#define CMD_SYNC        0x08
#define CMD_READ_REG    0x0A
#define CMD_SPI_ATTACH  0x0D
#define CMD_CHANGE_BAUD 0x0F
#define CMD_SPI_FLASH_MD5 0x13

#define FLASH_BLOCK     1024
#define CHIP_MAGIC_REG  0x40001000

/* ---- public flash-job description (filled by the UI) ---- */
struct FlashPart { char path[128]; uint32_t offset; };
FlashPart fl_parts[8];
int       fl_nparts = 0;

const uint32_t FLASH_BAUDS[] = { 115200, 230400, 460800, 921600 };
int   fl_baud_idx = 2;         /* -> 460800 */
bool  fl_verify   = true;

/* ---- volatile status the UI polls ---- */
enum { FL_IDLE, FL_CONNECT, FL_ERASE, FL_WRITE, FL_VERIFY, FL_DONE, FL_ERR };
volatile int      fl_state = FL_IDLE;
volatile int      fl_pct   = 0;
volatile uint32_t fl_done  = 0, fl_total = 0;
volatile bool     fl_abort = false;
char              fl_chip[24] = "?";

/* ---- one-slot log mailbox (task -> UI thread) ---- */
volatile bool fl_mail_ready = false;
char          fl_mail[110];
uint8_t       fl_mail_sev = SEV_SYS;

static void fl_log(const char *s, uint8_t sev) {
    Serial.printf("[FLASH] %s\n", s);
    uint32_t dl = millis() + 800;
    while (fl_mail_ready && millis() < dl) vTaskDelay(2);
    strncpy(fl_mail, s, sizeof(fl_mail) - 1);
    fl_mail[sizeof(fl_mail) - 1] = 0;
    fl_mail_sev = sev;
    fl_mail_ready = true;
}

/* ===================== embedded MD5 (RFC 1321) ============================ */
struct MD5c { uint32_t a,b,c,d; uint32_t lo,hi; uint8_t buf[64]; };
static const uint32_t MD5K[64] = {
 0xd76aa478,0xe8c7b756,0x242070db,0xc1bdceee,0xf57c0faf,0x4787c62a,0xa8304613,0xfd469501,
 0x698098d8,0x8b44f7af,0xffff5bb1,0x895cd7be,0x6b901122,0xfd987193,0xa679438e,0x49b40821,
 0xf61e2562,0xc040b340,0x265e5a51,0xe9b6c7aa,0xd62f105d,0x02441453,0xd8a1e681,0xe7d3fbc8,
 0x21e1cde6,0xc33707d6,0xf4d50d87,0x455a14ed,0xa9e3e905,0xfcefa3f8,0x676f02d9,0x8d2a4c8a,
 0xfffa3942,0x8771f681,0x6d9d6122,0xfde5380c,0xa4beea44,0x4bdecfa9,0xf6bb4b60,0xbebfbc70,
 0x289b7ec6,0xeaa127fa,0xd4ef3085,0x04881d05,0xd9d4d039,0xe6db99e5,0x1fa27cf8,0xc4ac5665,
 0xf4292244,0x432aff97,0xab9423a7,0xfc93a039,0x655b59c3,0x8f0ccc92,0xffeff47d,0x85845dd1,
 0x6fa87e4f,0xfe2ce6e0,0xa3014314,0x4e0811a1,0xf7537e82,0xbd3af235,0x2ad7d2bb,0xeb86d391 };
static const uint8_t MD5S[64] = {
 7,12,17,22,7,12,17,22,7,12,17,22,7,12,17,22, 5,9,14,20,5,9,14,20,5,9,14,20,5,9,14,20,
 4,11,16,23,4,11,16,23,4,11,16,23,4,11,16,23, 6,10,15,21,6,10,15,21,6,10,15,21,6,10,15,21 };
#define MD5ROT(x,c) (((x)<<(c))|((x)>>(32-(c))))
static void md5_block(MD5c *m, const uint8_t *p) {
    uint32_t M[16];
    for (int i = 0; i < 16; i++)
        M[i] = p[i*4] | (p[i*4+1]<<8) | (p[i*4+2]<<16) | ((uint32_t)p[i*4+3]<<24);
    uint32_t a=m->a,b=m->b,c=m->c,d=m->d;
    for (int i = 0; i < 64; i++) {
        uint32_t f; int g;
        if      (i < 16) { f=(b&c)|(~b&d);        g=i; }
        else if (i < 32) { f=(d&b)|(~d&c);        g=(5*i+1)&15; }
        else if (i < 48) { f=b^c^d;               g=(3*i+5)&15; }
        else             { f=c^(b|~d);            g=(7*i)&15; }
        uint32_t t=d; d=c; c=b;
        b += MD5ROT(a+f+MD5K[i]+M[g], MD5S[i]);
        a=t;
    }
    m->a+=a; m->b+=b; m->c+=c; m->d+=d;
}
static void md5_init(MD5c *m){ m->a=0x67452301;m->b=0xefcdab89;m->c=0x98badcfe;m->d=0x10325476;m->lo=m->hi=0; }
static void md5_update(MD5c *m, const uint8_t *data, uint32_t len) {
    uint32_t used = (m->lo >> 3) & 63;
    m->lo += len << 3; m->hi += len >> 29;
    if (used) {
        uint32_t need = 64 - used;
        if (len < need) { memcpy(m->buf+used, data, len); return; }
        memcpy(m->buf+used, data, need); md5_block(m, m->buf);
        data += need; len -= need;
    }
    while (len >= 64) { md5_block(m, data); data += 64; len -= 64; }
    memcpy(m->buf, data, len);
}
static void md5_final(MD5c *m, uint8_t out[16]) {
    uint32_t used = (m->lo >> 3) & 63;
    m->buf[used++] = 0x80;
    if (used > 56) { memset(m->buf+used, 0, 64-used); md5_block(m, m->buf); used = 0; }
    memset(m->buf+used, 0, 56-used);
    for (int i = 0; i < 4; i++) m->buf[56+i] = (m->lo >> (8*i)) & 0xFF;
    for (int i = 0; i < 4; i++) m->buf[60+i] = (m->hi >> (8*i)) & 0xFF;
    md5_block(m, m->buf);
    uint32_t v[4] = { m->a, m->b, m->c, m->d };
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++) out[i*4+j] = (v[i] >> (8*j)) & 0xFF;
}

/* ===================== SLIP transport ==================================== */
static inline void put32(uint8_t *p, uint32_t v){ p[0]=v; p[1]=v>>8; p[2]=v>>16; p[3]=v>>24; }

static void fl_wr(const uint8_t *d, int n) {
    for (int i = 0; i < n; i++) {
        uint8_t b = d[i];
        if      (b == 0xC0) { Serial1.write(0xDB); Serial1.write(0xDC); }
        else if (b == 0xDB) { Serial1.write(0xDB); Serial1.write(0xDD); }
        else                  Serial1.write(b);
    }
}

static int fl_getb(uint32_t deadline) {
    while ((int32_t)(deadline - millis()) > 0) {
        if (Serial1.available()) return Serial1.read();
        vTaskDelay(1);
    }
    return -1;
}

static bool fl_read_pkt(uint8_t *buf, int cap, int *out, uint32_t timeout) {
    uint32_t dl = millis() + timeout;
    int c;
    do { c = fl_getb(dl); if (c < 0) return false; } while (c != 0xC0);
    int n = 0;
    for (;;) {
        c = fl_getb(dl); if (c < 0) return false;
        if (c == 0xC0) { if (n == 0) continue; break; }
        if (c == 0xDB) {
            int e = fl_getb(dl); if (e < 0) return false;
            c = (e == 0xDC) ? 0xC0 : (e == 0xDD) ? 0xDB : e;
        }
        if (n < cap) buf[n++] = (uint8_t)c;
    }
    *out = n;
    return true;
}

/* send one command, wait for the matching response; optionally return the
 * value word and the payload (data section, status bytes included).        */
static bool fl_cmd(uint8_t cmd, const uint8_t *data, uint16_t len, uint32_t chk,
                   uint32_t timeout, uint32_t *val = nullptr,
                   uint8_t *payload = nullptr, int *plen = nullptr) {
    Serial1.write(0xC0);
    uint8_t hdr[8] = { 0x00, cmd, (uint8_t)len, (uint8_t)(len >> 8),
                       (uint8_t)chk, (uint8_t)(chk >> 8),
                       (uint8_t)(chk >> 16), (uint8_t)(chk >> 24) };
    fl_wr(hdr, 8);
    if (len) fl_wr(data, len);
    Serial1.write(0xC0);
    Serial1.flush();

    uint8_t rb[160];
    uint32_t dl = millis() + timeout;
    while ((int32_t)(dl - millis()) > 0) {
        int rl;
        if (!fl_read_pkt(rb, sizeof(rb), &rl, dl - millis())) return false;
        if (rl >= 10 && rb[0] == 0x01 && rb[1] == cmd) {
            uint16_t size = rb[2] | (rb[3] << 8);
            if (val) *val = rb[4] | (rb[5] << 8) | (rb[6] << 16) | ((uint32_t)rb[7] << 24);
            if (8 + (int)size > rl) size = (rl >= 8) ? rl - 8 : 0;
            if (payload && plen) { *plen = size; if (size) memcpy(payload, rb + 8, size); }
            uint8_t status = (size >= 2) ? rb[8 + size - 2] : 0;
            return status == 0;
        }
    }
    return false;
}

/* ===================== target reset control ============================== */
/* Three ways to get the target into ROM download mode:
 *   0 AUTO   - drive TGT_IO0 / TGT_EN (needs the two extra wires)
 *   1 SOFT   - ask the running firmware to reboot into download mode itself
 *              (target must implement the command; TX/RX only)
 *   2 MANUAL - you hold BOOT and tap RST, we keep retrying the sync
 * A PC does this over DTR/RTS, which plain TX/RX does not give us.          */
int  fl_reset_mode = 0;
char fl_softcmd[32] = "BOOTLOADER";

static bool tgt_pins_ok() { return (TGT_IO0) >= 0 && (TGT_EN) >= 0; }

static void tgt_download() {
    if (!tgt_pins_ok()) return;
    pinMode(TGT_IO0, OUTPUT); pinMode(TGT_EN, OUTPUT);
    digitalWrite(TGT_IO0, LOW);
    digitalWrite(TGT_EN,  LOW);  delay(100);
    digitalWrite(TGT_EN,  HIGH); delay(50);   /* boots, samples IO0 low -> loader */
    digitalWrite(TGT_IO0, HIGH); delay(20);
    pinMode(TGT_IO0, INPUT);                  /* release to target pull-ups */
    pinMode(TGT_EN,  INPUT);
    delay(50);
}
static void tgt_run() {
    if (!tgt_pins_ok()) { fl_log("reset the target by hand to run the new app", SEV_WARN); return; }
    pinMode(TGT_EN, OUTPUT);
    digitalWrite(TGT_EN, LOW); delay(100); digitalWrite(TGT_EN, HIGH);
    delay(20); pinMode(TGT_EN, INPUT);
}

/* ===================== high-level steps ================================== */
static bool fl_try_sync(int attempts) {
    uint8_t s[36];
    s[0]=0x07; s[1]=0x07; s[2]=0x12; s[3]=0x20;
    memset(s + 4, 0x55, 32);
    for (int i = 0; i < attempts; i++) {
        if (fl_abort) return false;
        if (fl_cmd(CMD_SYNC, s, 36, 0, 120)) {
            uint8_t rb[64]; int rl;              /* drain the extra sync echoes */
            for (int k = 0; k < 8; k++) if (!fl_read_pkt(rb, sizeof(rb), &rl, 40)) break;
            return true;
        }
    }
    return false;
}

static bool fl_sync() {
    if (fl_reset_mode == 0 && tgt_pins_ok()) {
        for (int round = 0; round < 2; round++) {
            tgt_download();
            if (fl_try_sync(7)) return true;
        }
        return false;
    }

    if (fl_reset_mode == 1) {
        char m[64];
        snprintf(m, sizeof(m), "sending \"%s\" to target...", fl_softcmd);
        fl_log(m, SEV_SYS);
        Serial1.print(fl_softcmd); Serial1.print("\r\n"); Serial1.flush();
        delay(700);
        while (Serial1.available()) Serial1.read();
        if (fl_try_sync(12)) return true;
        fl_log("soft command did not reach the loader", SEV_WARN);
    }

    /* manual mode, or a fallback after the soft command failed */
    fl_log("hold BOOT on the target, tap RST, release BOOT", SEV_WARN);
    uint32_t dl = millis() + 25000;
    while ((int32_t)(dl - millis()) > 0 && !fl_abort) {
        if (fl_try_sync(3)) return true;
        vTaskDelay(80);
    }
    return false;
}

static void fl_detect_chip(bool *append_encrypt, bool *need_attach) {
    *append_encrypt = true;   /* default: treat as S3-like */
    *need_attach    = true;
    uint32_t magic = 0;
    uint8_t d[4]; put32(d, CHIP_MAGIC_REG);
    if (!fl_cmd(CMD_READ_REG, d, 4, 0, 500, &magic)) { strcpy(fl_chip, "ESP (assumed S3)"); return; }
    switch (magic) {
        case 0x00f01d83: strcpy(fl_chip, "ESP32");    *append_encrypt = false; break;
        case 0x000007c6: strcpy(fl_chip, "ESP32-S2"); break;
        case 0x00000009: strcpy(fl_chip, "ESP32-S3"); break;
        case 0x6921506f: case 0x1b31506f: case 0x4881606f:
        case 0x4361606f: strcpy(fl_chip, "ESP32-C3"); break;
        case 0x2ce0806f: strcpy(fl_chip, "ESP32-C6"); break;
        case 0xfff0c101: strcpy(fl_chip, "ESP8266"); *append_encrypt = false; *need_attach = false; break;
        default: snprintf(fl_chip, sizeof(fl_chip), "unknown %08lX", (unsigned long)magic); break;
    }
}

static bool fl_flash_part(File &f, uint32_t offset, uint32_t size, bool append_encrypt) {
    uint32_t nblk = (size + FLASH_BLOCK - 1) / FLASH_BLOCK;
    uint8_t begin[20]; int blen = 16;
    put32(begin,     size);
    put32(begin + 4, nblk);
    put32(begin + 8, FLASH_BLOCK);
    put32(begin + 12, offset);
    if (append_encrypt) { put32(begin + 16, 0); blen = 20; }

    uint32_t to_begin = 3000 + (uint32_t)((uint64_t)size * 30000 / (1024*1024));
    fl_state = FL_ERASE;
    if (!fl_cmd(CMD_FLASH_BEGIN, begin, blen, 0, to_begin)) { fl_log("flash_begin failed (erase)", SEV_ERR); return false; }

    static uint8_t pkt[16 + FLASH_BLOCK];
    MD5c md5; md5_init(&md5);
    uint32_t seq = 0, sent = 0;
    fl_state = FL_WRITE;
    while (sent < size) {
        if (fl_abort) { fl_log("aborted by user", SEV_WARN); return false; }
        uint32_t n = size - sent; if (n > FLASH_BLOCK) n = FLASH_BLOCK;
        if (f.read(pkt + 16, n) != (int)n) { fl_log("SD read error", SEV_ERR); return false; }
        md5_update(&md5, pkt + 16, n);
        if (n < FLASH_BLOCK) memset(pkt + 16 + n, 0xFF, FLASH_BLOCK - n);
        put32(pkt,     FLASH_BLOCK);
        put32(pkt + 4, seq);
        put32(pkt + 8, 0);
        put32(pkt + 12, 0);
        uint8_t chk = 0xEF;
        for (int i = 0; i < FLASH_BLOCK; i++) chk ^= pkt[16 + i];
        if (!fl_cmd(CMD_FLASH_DATA, pkt, 16 + FLASH_BLOCK, chk, 3000)) {
            fl_log("flash_data failed", SEV_ERR); return false;
        }
        seq++; sent += n; fl_done += n;
        if (fl_total) fl_pct = (int)((uint64_t)fl_done * 100 / fl_total);
        if ((seq & 7) == 0) vTaskDelay(1);       /* feed the watchdog */
    }

    uint8_t end[4]; put32(end, 1);               /* 1 = stay in loader */
    fl_cmd(CMD_FLASH_END, end, 4, 0, 3000);

    if (fl_verify) {
        fl_state = FL_VERIFY;
        uint8_t host[16]; md5_final(&md5, host);
        uint8_t m[16]; put32(m, offset); put32(m + 4, size); put32(m + 8, 0); put32(m + 12, 0);
        uint8_t resp[64]; int rlen = 0; uint32_t val;
        uint32_t to_md5 = 3000 + (uint32_t)((uint64_t)size * 8000 / (1024*1024));
        if (!fl_cmd(CMD_SPI_FLASH_MD5, m, 16, 0, to_md5, &val, resp, &rlen)) {
            fl_log("MD5 read failed (flash OK, unverified)", SEV_WARN);
        } else {
            int dig = rlen - 2;                  /* strip 2 status bytes */
            bool match = false;
            if (dig == 32) {                     /* hex string form (ROM) */
                char hh[33];
                for (int i = 0; i < 16; i++) sprintf(hh + i*2, "%02x", host[i]);
                match = true;
                for (int i = 0; i < 32; i++)
                    if (tolower(resp[i]) != hh[i]) { match = false; break; }
            } else if (dig == 16) {              /* raw form (stub) */
                match = memcmp(resp, host, 16) == 0;
            }
            if (!match) { fl_log("MD5 MISMATCH — flash corrupt!", SEV_ERR); return false; }
            fl_log("verified OK", SEV_OK);
        }
    }
    return true;
}

/* ===================== the task ========================================= */
static void fl_run() {
    fl_state = FL_CONNECT; fl_pct = 0; fl_done = 0;
    fl_log("connecting to target...", SEV_SYS);

    Serial1.end(); delay(10);
    Serial1.setRxBufferSize(4096);
    Serial1.begin(115200, SERIAL_8N1, UART_RX_PIN, UART_TX_PIN);

    if (!fl_sync()) {
        fl_log("no response — check wiring / that target is an ESP", SEV_ERR);
        fl_state = FL_ERR; return;
    }
    bool append_encrypt, need_attach;
    fl_detect_chip(&append_encrypt, &need_attach);
    { char b[48]; snprintf(b, sizeof(b), "target: %s", fl_chip); fl_log(b, SEV_OK); }

    if (need_attach) {
        uint8_t a[8] = {0};
        if (!fl_cmd(CMD_SPI_ATTACH, a, 8, 0, 3000)) { fl_log("SPI attach failed", SEV_ERR); fl_state = FL_ERR; return; }
    }

    uint32_t baud = FLASH_BAUDS[fl_baud_idx];
    if (baud != 115200) {
        uint8_t b[8]; put32(b, baud); put32(b + 4, 0);
        if (fl_cmd(CMD_CHANGE_BAUD, b, 8, 0, 3000)) {
            Serial1.updateBaudRate(baud);
            delay(50); while (Serial1.available()) Serial1.read();
            char m[40]; snprintf(m, sizeof(m), "baud -> %lu", (unsigned long)baud); fl_log(m, SEV_SYS);
        } else {
            fl_log("baud change rejected, staying at 115200", SEV_WARN);
        }
    }

    /* total size across all parts, for the progress bar */
    fl_total = 0;
    for (int i = 0; i < fl_nparts; i++) {
        File f = SD.open(fl_parts[i].path);
        if (f) { fl_total += f.size(); f.close(); }
    }

    bool ok = true;
    for (int i = 0; i < fl_nparts && ok; i++) {
        File f = SD.open(fl_parts[i].path);
        if (!f) { fl_log("cannot open part on SD", SEV_ERR); ok = false; break; }
        uint32_t sz = f.size();
        char b[120];
        const char *nm = strrchr(fl_parts[i].path, '/'); nm = nm ? nm + 1 : fl_parts[i].path;
        snprintf(b, sizeof(b), "writing %s -> 0x%lX (%lu bytes)", nm,
                 (unsigned long)fl_parts[i].offset, (unsigned long)sz);
        fl_log(b, SEV_INFO);
        ok = fl_flash_part(f, fl_parts[i].offset, sz, append_encrypt);
        f.close();
    }

    /* restore UART1 for the monitor, then boot the target */
    uart_apply();
    tgt_run();

    if (ok) { fl_state = FL_DONE; fl_pct = 100; fl_log("DONE — target rebooted", SEV_OK); }
    else    { fl_state = FL_ERR;  fl_log("FAILED — target reset", SEV_ERR); }
}

static void fl_task(void *) {
    fl_run();
    g_flashing = false;
    vTaskDelete(NULL);
}

bool flasher_start() {
    if (g_flashing || fl_nparts == 0) return false;
    fl_abort = false;
    g_flashing = true;
    xTaskCreatePinnedToCore(fl_task, "flash", 8192, nullptr, 1, nullptr, 0);
    return true;
}
