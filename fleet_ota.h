#pragma once
/* =========================================================================
 *  Fleet firmware flasher — pushes a RaceBoard image to ~20 boards at once.
 *
 *  LoRa announces the session (every board is already listening on it);
 *  ESP-NOW carries the image. Broadcast is the whole point: one board and
 *  twenty boards take the same wall-clock time. Unicasting twenty boards
 *  over LoRa would take about 27 hours; this takes well under two minutes.
 *
 *  Broadcast has no acknowledgement, so reliability comes from repair
 *  rounds: blast every block once, then ask each board what it is missing,
 *  union the answers, and rebroadcast only the gaps. Two or three rounds
 *  normally converge.
 *
 *  Structure mirrors flasher.h deliberately — a job task pinned to core 0,
 *  volatile status fields the LVGL UI polls on core 1, and the same one-slot
 *  log mailbox. Nothing here touches LVGL.
 * ========================================================================= */

#include <FS.h>
#include <SD.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <esp_random.h>
#include <mbedtls/sha256.h>
#include <SPI.h>
#include <LoRa.h>
#include <CRC32.h>
#include <stdarg.h>

#include "ota_protocol.h"

/* ---- LoRa pins. Identical to the RaceBoard's map (LoRaProtocol.h) so one
 *      wiring harness serves both. That is why TGT_IO0/TGT_EN moved to 15/16
 *      in MobileDebugger.ino — they used to sit on 5 and 6. ---------------- */
#define FO_LORA_BAND  433E6
#define FO_LORA_SCK    7
#define FO_LORA_MISO  14
#define FO_LORA_MOSI   9
#define FO_LORA_SS     5
#define FO_LORA_RST    6
#define FO_LORA_DIO0  -1

#define FO_MAX_NODES  32          /* fleet is ~20; headroom for stragglers */
#define FO_WIFI_CHAN   6          /* ESP-NOW channel for the bulk transfer */

/* ---- job state the UI polls ------------------------------------------- */
enum { FO_IDLE, FO_LOADING, FO_ANNOUNCE, FO_SENDING, FO_REPAIR, FO_DONE, FO_ERR };

volatile int      fo_state   = FO_IDLE;
volatile int      fo_pct     = 0;
volatile int      fo_round   = 0;
volatile uint32_t fo_sent    = 0;
volatile bool     fo_abort   = false;
volatile bool     fo_running = false;

char     fo_path[160]   = "";
char     fo_version[OTA_VERSION_LEN + 1] = "";

/* ---- the image, staged in PSRAM --------------------------------------- */
static uint8_t  *fo_image   = nullptr;
static uint32_t  fo_size    = 0;
static uint32_t  fo_blocks  = 0;
static uint8_t   fo_sha[32];
static uint32_t  fo_session = 0;

/* ---- roster ------------------------------------------------------------ */
struct FleetNode {
    uint8_t  mac[6];
    uint8_t  state;
    uint8_t  progress;
    uint8_t  error;
    char     version[OTA_VERSION_LEN + 1];
    bool     ready;         /* sent an ESP-NOW READY: it is in the updater */
    bool     complete;
    uint32_t lastSeen;
    /* bitmap of what this node is still missing, gathered during a repair
     * poll. Owned by the job task; the callback only fills bmpRx. */
    uint8_t *missing;
    uint32_t missingBytes;
    volatile bool bmpFresh;
};

static FleetNode fo_nodes[FO_MAX_NODES];
volatile int     fo_node_count = 0;

/* union of every node's missing blocks — what the next repair round sends */
static uint8_t  *fo_need = nullptr;
static uint32_t  fo_needBytes = 0;

static const uint8_t fo_bcast[6] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};
static SPIClass fo_loraSPI;
static bool     fo_loraUp = false;

/* ---- one-slot log mailbox (job task -> UI), same idiom as flasher.h ---- */
volatile bool fo_mail_ready = false;
char          fo_mail[110];
uint8_t       fo_mail_sev = SEV_SYS;

static void fo_log(const char *s, uint8_t sev) {
    Serial.printf("[FLEET] %s\n", s);
    uint32_t dl = millis() + 800;
    while (fo_mail_ready && millis() < dl) vTaskDelay(2);
    strncpy(fo_mail, s, sizeof(fo_mail) - 1);
    fo_mail[sizeof(fo_mail) - 1] = 0;
    fo_mail_sev = sev;
    fo_mail_ready = true;
}

static void fo_logf(uint8_t sev, const char *fmt, ...) {
    char b[110];
    va_list ap; va_start(ap, fmt);
    vsnprintf(b, sizeof(b), fmt, ap);
    va_end(ap);
    fo_log(b, sev);
}

/* ======================================================================== */
/*  roster helpers                                                          */
/* ======================================================================== */

static int fo_findNode(const uint8_t *mac) {
    for (int i = 0; i < fo_node_count; i++)
        if (!memcmp(fo_nodes[i].mac, mac, 6)) return i;
    return -1;
}

static int fo_addNode(const uint8_t *mac) {
    int i = fo_findNode(mac);
    if (i >= 0) return i;
    if (fo_node_count >= FO_MAX_NODES) return -1;
    i = fo_node_count++;
    memset(&fo_nodes[i], 0, sizeof(FleetNode));
    memcpy(fo_nodes[i].mac, mac, 6);
    fo_nodes[i].lastSeen = millis();
    return i;
}

/* ======================================================================== */
/*  ESP-NOW                                                                 */
/* ======================================================================== */

static void fo_ensurePeer(const uint8_t *mac) {
    if (esp_now_is_peer_exist(mac)) return;
    esp_now_peer_info_t p = {};
    memcpy(p.peer_addr, mac, 6);
    p.channel = FO_WIFI_CHAN;
    p.encrypt = false;
    esp_now_add_peer(&p);
}

static void fo_onRecv(const uint8_t *mac, const uint8_t *data, int len) {
    if (len < (int)sizeof(OtaNowHeader)) return;
    const OtaNowHeader *h = (const OtaNowHeader *)data;
    if (h->magic != OTA_LORA_MAGIC) return;
    if (h->session_lo != (uint16_t)(fo_session & 0xFFFF)) return;

    int n = fo_findNode(mac);
    if (n < 0) n = fo_addNode(mac);
    if (n < 0) return;
    fo_nodes[n].lastSeen = millis();

    switch (h->type) {
        case OTA_NOW_READY:
        case OTA_NOW_STATUS: {
            if (len < (int)sizeof(OtaNowStatus)) return;
            const OtaNowStatus *s = (const OtaNowStatus *)data;
            fo_nodes[n].ready    = true;
            fo_nodes[n].state    = s->state;
            fo_nodes[n].progress = s->progress;
            fo_nodes[n].error    = s->error;
            if (s->state == OTA_STATE_OK) fo_nodes[n].complete = true;
            break;
        }
        case OTA_NOW_COMPLETE:
            fo_nodes[n].complete = true;
            fo_nodes[n].progress = 100;
            break;
        case OTA_NOW_BITMAP: {
            if (len < (int)sizeof(OtaNowBitmap)) return;
            const OtaNowBitmap *b = (const OtaNowBitmap *)data;
            FleetNode &nd = fo_nodes[n];
            /* The buffer is allocated by the job task before it polls —
             * never here. Allocating on the WiFi task while a broadcast is
             * in flight is a good way to lose packets. */
            if (!nd.missing || !nd.missingBytes) return;
            uint32_t off = b->hdr.block;
            if (off >= nd.missingBytes) return;
            uint32_t cnt = nd.missingBytes - off;
            if (cnt > OTA_BITMAP_SLICE) cnt = OTA_BITMAP_SLICE;
            memcpy(nd.missing + off, b->bits, cnt);
            /* last slice of the set — the job task can act on it now */
            if (off + cnt >= nd.missingBytes) nd.bmpFresh = true;
            break;
        }
        default: break;
    }
}

#if ESP_ARDUINO_VERSION_MAJOR >= 3
static void fo_onRecvShim(const esp_now_recv_info_t *info, const uint8_t *d, int len) {
    fo_onRecv(info->src_addr, d, len);
}
#else
static void fo_onRecvShim(const uint8_t *mac, const uint8_t *d, int len) {
    fo_onRecv(mac, d, len);
}
#endif

static bool fo_nowStart() {
    WiFi.mode(WIFI_STA);
    WiFi.disconnect();
    esp_wifi_set_ps(WIFI_PS_NONE);
    esp_wifi_set_channel(FO_WIFI_CHAN, WIFI_SECOND_CHAN_NONE);
    if (esp_now_init() != ESP_OK) return false;
    esp_now_register_recv_cb(fo_onRecvShim);
    fo_ensurePeer(fo_bcast);
    return true;
}

/* ======================================================================== */
/*  LoRa                                                                    */
/* ======================================================================== */

bool fleet_lora_begin() {
    if (fo_loraUp) return true;
    fo_loraSPI.begin(FO_LORA_SCK, FO_LORA_MISO, FO_LORA_MOSI, FO_LORA_SS);
    LoRa.setPins(FO_LORA_SS, FO_LORA_RST, FO_LORA_DIO0);
    LoRa.setSPI(fo_loraSPI);
    LoRa.setSPIFrequency(10000000);
    if (!LoRa.begin(FO_LORA_BAND)) { fo_loraSPI.end(); fo_loraUp = false; return false; }
    fo_loraUp = true;
    return true;
}

static void fo_loraSend(const void *payload, uint8_t len) {
    if (!fo_loraUp) return;
    CRC32 crc; crc.update((uint8_t *)payload, len);
    uint32_t c = crc.finalize();
    LoRa.beginPacket();
    LoRa.write(len);
    LoRa.write((const uint8_t *)payload, len);
    LoRa.write((uint8_t *)&c, sizeof(c));
    LoRa.endPacket();
}

/* Drain any inbound LoRa. Boards reply to roll-calls and report results here.
 * Safe to call from the job task or, when idle, from the UI timer. */
void fleet_lora_poll() {
    if (!fo_loraUp) return;
    int ps = LoRa.parsePacket();
    if (!ps) return;
    uint8_t len = LoRa.read();
    if (ps != 1 + (int)len + 4) { while (LoRa.available()) LoRa.read(); return; }

    uint8_t buf[255];
    LoRa.readBytes(buf, len);
    uint32_t rxCrc; LoRa.readBytes((uint8_t *)&rxCrc, sizeof(rxCrc));
    CRC32 crc; crc.update(buf, len);
    if (crc.finalize() != rxCrc) return;
    if (len < 2 || buf[0] != OTA_LORA_MAGIC) return;

    if ((buf[1] == OTA_LORA_HELLO || buf[1] == OTA_LORA_RESULT) &&
        len >= sizeof(OtaHello)) {
        const OtaHello *h = (const OtaHello *)buf;
        int n = fo_addNode(h->mac);
        if (n < 0) return;
        fo_nodes[n].state    = h->state;
        fo_nodes[n].progress = h->progress;
        fo_nodes[n].error    = h->error;
        fo_nodes[n].lastSeen = millis();
        memcpy(fo_nodes[n].version, h->version, OTA_VERSION_LEN);
        fo_nodes[n].version[OTA_VERSION_LEN] = 0;
        if (h->state == OTA_STATE_OK) fo_nodes[n].complete = true;
    }
}

/* Ask every board within earshot to identify itself. Replies are staggered by
 * the low byte of each MAC, so twenty boards do not collide. */
void fleet_rollcall() {
    uint8_t p[3] = { OTA_LORA_MAGIC, OTA_LORA_ROLLCALL, OTA_PROTO_VERSION };
    fo_loraSend(p, sizeof(p));
}

void fleet_abort_broadcast() {
    uint8_t p[3] = { OTA_LORA_MAGIC, OTA_LORA_ABORT, OTA_PROTO_VERSION };
    fo_loraSend(p, sizeof(p));
    OtaNowHeader h = { OTA_LORA_MAGIC, OTA_NOW_ABORT,
                       (uint16_t)(fo_session & 0xFFFF), 0 };
    esp_now_send(fo_bcast, (const uint8_t *)&h, sizeof(h));
}

/* ======================================================================== */
/*  image load                                                              */
/* ======================================================================== */

static bool fo_loadImage() {
    File f = SD.open(fo_path, FILE_READ);
    if (!f) { fo_log("cannot open image", SEV_ERR); return false; }
    fo_size = f.size();
    if (fo_size == 0) { f.close(); fo_log("image is empty", SEV_ERR); return false; }

    if (fo_image) { heap_caps_free(fo_image); fo_image = nullptr; }
    fo_image = (uint8_t *)heap_caps_malloc(fo_size, MALLOC_CAP_SPIRAM);
    if (!fo_image) { f.close(); fo_log("PSRAM alloc failed", SEV_ERR); return false; }

    uint32_t got = 0;
    while (got < fo_size) {
        int n = f.read(fo_image + got, (fo_size - got) > 4096 ? 4096 : (fo_size - got));
        if (n <= 0) break;
        got += n;
        fo_pct = (int)((uint64_t)got * 100 / fo_size);
    }
    f.close();
    if (got != fo_size) { fo_log("short read from SD", SEV_ERR); return false; }

    /* Every ESP app image starts 0xE9. Catching it here beats discovering it
     * after twenty boards have spent a minute receiving it. */
    if (fo_image[0] != 0xE9) {
        fo_log("not an ESP app image (first byte != 0xE9)", SEV_ERR);
        return false;
    }

    mbedtls_sha256_context c;
    mbedtls_sha256_init(&c);
    mbedtls_sha256_starts(&c, 0);
    for (uint32_t off = 0; off < fo_size; off += 32768) {
        uint32_t n = fo_size - off; if (n > 32768) n = 32768;
        mbedtls_sha256_update(&c, fo_image + off, n);
    }
    mbedtls_sha256_finish(&c, fo_sha);
    mbedtls_sha256_free(&c);

    fo_blocks = otaBlockCount(fo_size);
    fo_needBytes = otaBitmapBytes(fo_blocks);
    if (fo_need) { heap_caps_free(fo_need); }
    fo_need = (uint8_t *)heap_caps_malloc(fo_needBytes, MALLOC_CAP_SPIRAM);
    if (!fo_need) { fo_log("bitmap alloc failed", SEV_ERR); return false; }

    fo_logf(SEV_OK, "image %lu KB, %lu blocks, sha %02x%02x%02x%02x..",
            (unsigned long)(fo_size / 1024), (unsigned long)fo_blocks,
            fo_sha[0], fo_sha[1], fo_sha[2], fo_sha[3]);
    return true;
}

/* ======================================================================== */
/*  transfer                                                                */
/* ======================================================================== */

static void fo_sendBlock(uint32_t idx) {
    OtaNowData d;
    d.hdr.magic      = OTA_LORA_MAGIC;
    d.hdr.type       = OTA_NOW_DATA;
    d.hdr.session_lo = (uint16_t)(fo_session & 0xFFFF);
    d.hdr.block      = idx;

    uint32_t off = idx * OTA_BLOCK_DATA;
    uint32_t n   = fo_size - off;
    if (n > OTA_BLOCK_DATA) n = OTA_BLOCK_DATA;
    memcpy(d.data, fo_image + off, n);
    if (n < OTA_BLOCK_DATA) memset(d.data + n, 0, OTA_BLOCK_DATA - n);

    esp_now_send(fo_bcast, (const uint8_t *)&d, sizeof(OtaNowHeader) + n);
    fo_sent++;
    /* ESP-NOW has no flow control and the receivers are also driving a
     * display, so pacing is the only backpressure available. */
    delayMicroseconds(OTA_TX_GAP_US);
}

/* One full pass over every block. */
static void fo_sendAll() {
    for (uint32_t i = 0; i < fo_blocks && !fo_abort; i++) {
        fo_sendBlock(i);
        if ((i & 0x3F) == 0) {
            fo_pct = (int)((uint64_t)i * 100 / fo_blocks);
            vTaskDelay(1);          /* never starve the idle task / WDT */
        }
    }
    fo_pct = 100;
}

/* Ask one node what it is missing and merge the answer into fo_need. */
static bool fo_pollNode(int n) {
    FleetNode &nd = fo_nodes[n];

    /* Allocate here, on the job task, so the receive callback only has to
     * memcpy into it. Sized from our own block count — a node reporting a
     * different total is out of session and its slices get dropped. */
    if (!nd.missing) {
        nd.missing = (uint8_t *)heap_caps_malloc(fo_needBytes, MALLOC_CAP_SPIRAM);
        if (!nd.missing) return false;
        nd.missingBytes = fo_needBytes;
    }
    memset(nd.missing, 0, nd.missingBytes);
    nd.bmpFresh = false;

    OtaNowHeader h = { OTA_LORA_MAGIC, OTA_NOW_POLL,
                       (uint16_t)(fo_session & 0xFFFF), 0 };
    fo_ensurePeer(nd.mac);
    esp_now_send(nd.mac, (const uint8_t *)&h, sizeof(h));

    uint32_t dl = millis() + OTA_POLL_TIMEOUT_MS;
    while (millis() < dl && !nd.bmpFresh) vTaskDelay(2);
    if (!nd.bmpFresh || !nd.missing) return false;

    uint32_t miss = 0;
    for (uint32_t i = 0; i < nd.missingBytes && i < fo_needBytes; i++) {
        fo_need[i] |= nd.missing[i];
        uint8_t v = nd.missing[i];
        while (v) { miss += v & 1; v >>= 1; }
    }
    nd.complete = (miss == 0);
    nd.progress = fo_blocks ? (uint8_t)(100 - (uint64_t)miss * 100 / fo_blocks) : 100;
    return true;
}

/* Rebroadcast only the blocks somebody still needs. */
static uint32_t fo_sendRepairs() {
    uint32_t sent = 0;
    for (uint32_t i = 0; i < fo_blocks && !fo_abort; i++) {
        if (!(fo_need[i >> 3] & (1u << (i & 7)))) continue;
        fo_sendBlock(i);
        sent++;
        if ((sent & 0x3F) == 0) vTaskDelay(1);
    }
    return sent;
}

/* ======================================================================== */
/*  job task                                                                */
/* ======================================================================== */

static void fo_task(void *arg) {
    (void)arg;
    fo_running = true;
    fo_abort   = false;
    fo_sent    = 0;
    fo_round   = 0;
    fo_session = (uint32_t)esp_random();

    /* ---- load + hash ---- */
    fo_state = FO_LOADING;
    fo_log("loading image from SD", SEV_SYS);
    if (!fo_loadImage()) { fo_state = FO_ERR; fo_running = false; vTaskDelete(NULL); return; }

    /* ---- announce ---- */
    fo_state = FO_ANNOUNCE;
    for (int i = 0; i < fo_node_count; i++) {
        fo_nodes[i].ready = false;
        fo_nodes[i].complete = false;
    }

    OtaAnnounce a = {};
    a.magic        = OTA_LORA_MAGIC;
    a.type         = OTA_LORA_ANNOUNCE;
    a.proto        = OTA_PROTO_VERSION;
    a.wifi_channel = FO_WIFI_CHAN;
    a.session_id   = fo_session;
    a.image_size   = fo_size;
    a.block_count  = fo_blocks;
    memcpy(a.sha256, fo_sha, 32);
    strncpy(a.version, fo_version, OTA_VERSION_LEN);

    fo_logf(SEV_SYS, "announcing session %08lx for %ds",
            (unsigned long)fo_session, OTA_JOIN_WINDOW_MS / 1000);

    uint32_t end = millis() + OTA_JOIN_WINDOW_MS;
    while (millis() < end && !fo_abort) {
        fo_loraSend(&a, sizeof(a));
        uint32_t nxt = millis() + OTA_ANNOUNCE_PERIOD_MS;
        while (millis() < nxt && !fo_abort) { fleet_lora_poll(); vTaskDelay(10); }
        /* Signed subtraction: `end - millis()` on unsigned would wrap to a
         * huge value the moment the window closes. */
        int32_t left = (int32_t)(end - millis());
        if (left < 0) left = 0;
        fo_pct = (int)(100 - ((int64_t)left * 100 / OTA_JOIN_WINDOW_MS));
    }

    int ready = 0;
    for (int i = 0; i < fo_node_count; i++) if (fo_nodes[i].ready) ready++;
    if (fo_abort) { fo_log("aborted", SEV_WARN); fo_state = FO_ERR; goto done; }
    if (ready == 0) {
        fo_log("no boards reported ready - check they are powered", SEV_ERR);
        fo_state = FO_ERR;
        goto done;
    }
    fo_logf(SEV_OK, "%d board(s) ready, starting transfer", ready);

    /* ---- first full pass ---- */
    fo_state = FO_SENDING;
    fo_sendAll();

    /* ---- repair rounds ---- */
    for (fo_round = 1; fo_round <= 8 && !fo_abort; fo_round++) {
        fo_state = FO_REPAIR;
        memset(fo_need, 0, fo_needBytes);

        int polled = 0, incomplete = 0;
        for (int i = 0; i < fo_node_count && !fo_abort; i++) {
            if (!fo_nodes[i].ready || fo_nodes[i].complete) continue;
            if (fo_pollNode(i)) polled++;
            if (!fo_nodes[i].complete) incomplete++;
        }

        if (incomplete == 0) {
            fo_log("all boards report complete", SEV_OK);
            break;
        }

        uint32_t miss = 0;
        for (uint32_t i = 0; i < fo_blocks; i++)
            if (fo_need[i >> 3] & (1u << (i & 7))) miss++;

        if (miss == 0) {
            /* Nobody answered the poll but somebody is still incomplete —
             * usually a board that dropped off. Nothing to resend. */
            fo_logf(SEV_WARN, "round %d: %d board(s) silent", fo_round, incomplete);
            break;
        }

        fo_logf(SEV_INFO, "round %d: resending %lu block(s) for %d board(s)",
                fo_round, (unsigned long)miss, incomplete);
        fo_state = FO_SENDING;
        fo_sendRepairs();
    }

    /* ---- commit ---- */
    {
        OtaNowHeader c = { OTA_LORA_MAGIC, OTA_NOW_COMMIT,
                           (uint16_t)(fo_session & 0xFFFF), 0 };
        for (int i = 0; i < 3; i++) {
            esp_now_send(fo_bcast, (const uint8_t *)&c, sizeof(c));
            vTaskDelay(pdMS_TO_TICKS(50));
        }
    }

    /* Boards now verify, flash and reboot. Their new firmware reports back
     * over LoRa, so keep draining it for a while. */
    fo_log("committed - waiting for boards to flash and reboot", SEV_SYS);
    end = millis() + 90000;
    while (millis() < end && !fo_abort) {
        fleet_lora_poll();
        int ok = 0;
        for (int i = 0; i < fo_node_count; i++)
            if (fo_nodes[i].state == OTA_STATE_OK) ok++;
        if (ok >= ready) break;
        vTaskDelay(20);
    }

    {
        int ok = 0;
        for (int i = 0; i < fo_node_count; i++)
            if (fo_nodes[i].state == OTA_STATE_OK) ok++;
        fo_logf(ok >= ready ? SEV_OK : SEV_WARN,
                "=== %d/%d board(s) confirmed on the new firmware ===", ok, ready);
        fo_state = FO_DONE;
    }

done:
    fo_running = false;
    vTaskDelete(NULL);
}

/* ======================================================================== */
/*  public entry points                                                     */
/* ======================================================================== */

bool fleet_begin() {
    if (!fleet_lora_begin()) { fo_log("LoRa init failed - check wiring", SEV_ERR); return false; }
    if (!fo_nowStart())      { fo_log("ESP-NOW init failed", SEV_ERR); return false; }
    return true;
}

void fleet_start() {
    if (fo_running) return;
    xTaskCreatePinnedToCore(fo_task, "fleet_ota", 8192, nullptr, 2, nullptr, 0);
}
