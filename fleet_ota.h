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
#include "src/wr_lora/WRLoRa.h"
#include <CRC32.h>
#include <stdarg.h>

#include "ota_protocol.h"

/* ---- LoRa pins. Identical to the RaceBoard's map (LoRaProtocol.h) so one
 *      wiring harness serves both.
 *
 *      The radio is BIT-BANGED (src/wr_lora), not on a hardware SPI
 *      peripheral, for the same reason as the RaceBoard: this board has only
 *      SPI3 free (the QSPI display owns SPI2 permanently) and the SD card
 *      needs it. Both sdcard.h's `SPIClass sdSPI` and the old
 *      `SPIClass fo_loraSPI` defaulted to HSPI == SPI3_HOST, so mounting the
 *      card and starting the radio reset the peripheral out from under each
 *      other — which broke fo_loadImage() during a fleet flash. ------------ */
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
char     fo_proj[33]    = "";   /* project name read out of the app descriptor */

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
    volatile bool     bmpFresh;
    /* Which bitmap slices have arrived this poll. Keying completion off the
     * last slice alone threw away four good slices out of five whenever the
     * fifth was lost - and with two boards beaconing at each other, losing one
     * is routine. */
    volatile uint32_t bmpSliceMask;
    uint8_t           bmpSliceCount;
};

static FleetNode fo_nodes[FO_MAX_NODES];
volatile int     fo_node_count = 0;

/* union of every node's missing blocks — what the next repair round sends */
static uint8_t  *fo_need = nullptr;
static uint32_t  fo_needBytes = 0;

static const uint8_t fo_bcast[6] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};
static bool     fo_loraUp = false;

/* ---- ESP-NOW transmit backpressure -------------------------------------
 * esp_now_send() is ASYNCHRONOUS. It queues the frame and returns; when the
 * queue is full it returns ESP_ERR_ESPNOW_NO_MEM and the frame is dropped.
 * Pacing with a fixed delay and ignoring the return value meant almost every
 * block was discarded at the API before it ever reached the air - a 1723 KB
 * image landed 14 of 7295 blocks. It looked like a range problem and was not.
 *
 * The send callback is the only honest signal that a frame has left, so keep
 * a small window of in-flight frames and wait when it is full. */
#define FO_TX_WINDOW 4

/* Repair-round budget. More rounds than before because a round can now be a
 * no-op retry rather than a guaranteed resend. */
#define FO_MAX_ROUNDS         16
#define FO_MAX_SILENT_ROUNDS   4
#define FO_POLL_ATTEMPTS       3

static volatile uint32_t fo_txPending = 0;   /* queued, not yet reported sent */
static volatile uint32_t fo_txDropped = 0;   /* refused by esp_now_send()     */
static volatile uint32_t fo_txFailed  = 0;   /* sent, but reported failure    */

/* The first parameter of esp_now_send_cb_t changed from `const uint8_t *mac`
 * to `const wifi_tx_info_t *` in ESP-IDF 5.5 (Arduino core 3.3). Rather than
 * pin a version number, derive the type from the typedef the installed core
 * actually declares - this then compiles on both, and on whatever comes next.
 * We ignore the argument either way. */
template <typename Fn> struct FoFirstArg;
template <typename R, typename A, typename... Rest>
struct FoFirstArg<R (*)(A, Rest...)> { using type = A; };

static void fo_onSent(typename FoFirstArg<esp_now_send_cb_t>::type,
                      esp_now_send_status_t status) {
    if (fo_txPending) fo_txPending--;
    if (status != ESP_NOW_SEND_SUCCESS) fo_txFailed++;
}

/* Queue one frame, blocking briefly while the window is full. Returns false
 * only when the frame really was not accepted. */
static bool fo_nowTx(const uint8_t *mac, const void *buf, size_t len) {
    const uint32_t dl = millis() + 200;
    while (fo_txPending >= FO_TX_WINDOW && millis() < dl) delayMicroseconds(150);

    fo_txPending++;
    esp_err_t e = esp_now_send(mac, (const uint8_t *)buf, len);
    if (e != ESP_OK) {
        if (fo_txPending) fo_txPending--;
        fo_txDropped++;
        return false;
    }
    return true;
}

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
    /* 0 means "use the interface's current channel". Pinning a peer to a
     * number that disagrees with the radio silently breaks delivery. */
    p.channel = 0;
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

            const uint32_t slice = off / OTA_BITMAP_SLICE;
            if (slice < 32) nd.bmpSliceMask |= (1u << slice);
            /* Complete only when every slice has landed, in any order. */
            const uint32_t full = (nd.bmpSliceCount >= 32)
                                  ? 0xFFFFFFFFu
                                  : ((1u << nd.bmpSliceCount) - 1u);
            if (nd.bmpSliceCount && nd.bmpSliceMask == full)
                nd.bmpFresh = true;
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
    esp_wifi_set_ps(WIFI_PS_NONE);          /* power save wrecks ESP-NOW rx */
    esp_wifi_set_max_tx_power(80);          /* 80 = 20 dBm, the ceiling     */

    /* Set the channel and then READ IT BACK. esp_wifi_set_channel() fails
     * silently if the driver is not started yet, and a flasher and a board on
     * different channels only hear each other at a few centimetres - which
     * looks exactly like a range problem. */
    esp_err_t ce = esp_wifi_set_channel(FO_WIFI_CHAN, WIFI_SECOND_CHAN_NONE);
    uint8_t got = 0; wifi_second_chan_t sec;
    esp_wifi_get_channel(&got, &sec);
    if (ce != ESP_OK || got != FO_WIFI_CHAN) {
        fo_logf(SEV_ERR, "WiFi channel is %u, wanted %u (%s)",
                got, FO_WIFI_CHAN, esp_err_to_name(ce));
        return false;
    }

    if (esp_now_init() != ESP_OK) return false;
    esp_now_register_recv_cb(fo_onRecvShim);
    esp_now_register_send_cb(fo_onSent);    /* drives the backpressure window */
    fo_txPending = fo_txDropped = fo_txFailed = 0;
    fo_ensurePeer(fo_bcast);
    fo_logf(SEV_OK, "ESP-NOW up on channel %u, 20 dBm", got);
    return true;
}

/* ======================================================================== */
/*  LoRa                                                                    */
/* ======================================================================== */

bool fleet_lora_begin() {
    if (fo_loraUp) return true;
    WRLoRa.setPins(FO_LORA_SS, FO_LORA_RST, FO_LORA_DIO0);
    WRLoRa.setSPIPins(FO_LORA_SCK, FO_LORA_MISO, FO_LORA_MOSI);
    if (!WRLoRa.begin(FO_LORA_BAND)) { fo_loraUp = false; return false; }
    fo_loraUp = true;
    Serial.printf("[FLEET] LoRa up (soft SPI %lu Hz), SD keeps SPI3\n",
                  (unsigned long)WRLoRa.spiBenchmarkHz());
    return true;
}

static void fo_loraSend(const void *payload, uint8_t len) {
    if (!fo_loraUp) return;
    CRC32 crc; crc.update((uint8_t *)payload, len);
    uint32_t c = crc.finalize();
    WRLoRa.beginPacket();
    WRLoRa.write(len);
    WRLoRa.write((const uint8_t *)payload, len);
    WRLoRa.write((uint8_t *)&c, sizeof(c));
    WRLoRa.endPacket();
}

/* Drain any inbound WRLoRa. Boards reply to roll-calls and report results here.
 * Safe to call from the job task or, when idle, from the UI timer. */
void fleet_lora_poll() {
    if (!fo_loraUp) return;
    int ps = WRLoRa.parsePacket();
    if (!ps) return;
    uint8_t len = WRLoRa.read();
    if (ps != 1 + (int)len + 4) { while (WRLoRa.available()) WRLoRa.read(); return; }

    uint8_t buf[255];
    WRLoRa.readBytes(buf, len);
    uint32_t rxCrc; WRLoRa.readBytes((uint8_t *)&rxCrc, sizeof(rxCrc));
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
        fo_logf(SEV_OK, "board %02X%02X%02X: %s state=%u v%s",
                h->mac[3], h->mac[4], h->mac[5],
                buf[1] == OTA_LORA_RESULT ? "RESULT" : "HELLO",
                h->state, fo_nodes[n].version);
    } else if (len >= 2) {
        /* Something arrived with our magic but was not a board reply. Worth
         * seeing while bringing the link up. */
        fo_logf(SEV_WARN, "LoRa: unexpected OTA type 0x%02x len %u", buf[1], len);
    }
}

/* Ask every board within earshot to identify itself. Replies are staggered by
 * the low byte of each MAC, so twenty boards do not collide. */
void fleet_rollcall() {
    uint8_t p[3] = { OTA_LORA_MAGIC, OTA_LORA_ROLLCALL, OTA_PROTO_VERSION };
    uint8_t t[5] = {0, 1, 2, 0, 0};
    fo_loraSend(t, 5);
    Serial.printf("Sent LoRa Packet: %02x %02x %02x %02x %02x\n", t[0], t[1], t[2], t[3], t[4]);
    fo_loraSend(p, sizeof(p));
    Serial.printf("Sent LoRa Packet: %02x %02x %02x\n", p[0], p[1], p[2]);

}

void fleet_abort_broadcast() {
    uint8_t p[3] = { OTA_LORA_MAGIC, OTA_LORA_ABORT, OTA_PROTO_VERSION };
    fo_loraSend(p, sizeof(p));
    OtaNowHeader h = { OTA_LORA_MAGIC, OTA_NOW_ABORT,
                       (uint16_t)(fo_session & 0xFFFF), 0 };
    fo_nowTx(fo_bcast, &h, sizeof(h));
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

    /* A merged bootstrap image ALSO starts 0xE9 - that is the bootloader at
     * offset 0. OTA writes a bare app into ota_0, so sending the merged file
     * would be wrong. Tell them apart by the partition-table magic that only
     * a full-flash image carries at 0x8000. */
    if (fo_size > 0x8002 && fo_image[0x8000] == 0xAA && fo_image[0x8001] == 0x50) {
        fo_log("this is a MERGED full-flash image - OTA needs the bare app .bin",
               SEV_ERR);
        return false;
    }

    /* esp_app_desc_t sits at 0x20 in every app image (right after the 24-byte
     * image header and the 8-byte first segment header). It carries the real
     * project name and version, so the operator can confirm they picked the
     * right firmware instead of trusting the filename. */
    fo_proj[0] = 0;
    if (fo_size > 0x70) {
        uint32_t desc_magic;
        memcpy(&desc_magic, fo_image + 0x20, 4);
        if (desc_magic == 0xABCD5432) {
            char v[33] = {0}, pn[33] = {0};
            memcpy(v,  fo_image + 0x30, 32);
            memcpy(pn, fo_image + 0x50, 32);
            v[32] = pn[32] = 0;
            strncpy(fo_proj, pn, sizeof(fo_proj) - 1);
            if (v[0]) {          /* prefer the embedded version to the filename */
                strncpy(fo_version, v, OTA_VERSION_LEN);
                fo_version[OTA_VERSION_LEN] = 0;
            }
            fo_logf(SEV_INFO, "image: project '%s' version '%s'", pn, v);
        } else {
            fo_log("no app descriptor - check this is the right .bin", SEV_WARN);
        }
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

    if (fo_nowTx(fo_bcast, &d, sizeof(OtaNowHeader) + n)) fo_sent++;
    /* No fixed gap: fo_nowTx() blocks on the in-flight window instead, which
     * paces to what the radio actually achieves rather than to a guess. */
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
    if (fo_txDropped || fo_txFailed)
        fo_logf(SEV_WARN, "tx: %lu dropped, %lu failed of %lu",
                (unsigned long)fo_txDropped, (unsigned long)fo_txFailed,
                (unsigned long)fo_sent);
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
    nd.bmpSliceCount = (uint8_t)((nd.missingBytes + OTA_BITMAP_SLICE - 1)
                                 / OTA_BITMAP_SLICE);

    /* Ask more than once. A single lost slice used to lose the whole reply,
     * and with several boards on the air that is common rather than rare. */
    for (int attempt = 0; attempt < FO_POLL_ATTEMPTS && !fo_abort; attempt++) {
        memset(nd.missing, 0, nd.missingBytes);
        nd.bmpFresh = false;
        nd.bmpSliceMask = 0;

        OtaNowHeader h = { OTA_LORA_MAGIC, OTA_NOW_POLL,
                           (uint16_t)(fo_session & 0xFFFF), 0 };
        fo_ensurePeer(nd.mac);
        fo_nowTx(nd.mac, &h, sizeof(h));

        uint32_t dl = millis() + OTA_POLL_TIMEOUT_MS;
        while (millis() < dl && !nd.bmpFresh) vTaskDelay(2);
        if (nd.bmpFresh) break;
    }
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
    int silentRounds = 0;
    for (fo_round = 1; fo_round <= FO_MAX_ROUNDS && !fo_abort; fo_round++) {
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
            /* Nobody answered this time, but somebody is still incomplete.
             * Breaking here abandoned the whole session on a single unlucky
             * round - which is exactly what happened with two boards at 99%,
             * each a handful of blocks short. Try again; only give up once
             * several rounds running have produced nothing at all. */
            silentRounds++;
            fo_logf(SEV_WARN, "round %d: %d board(s) silent (%d in a row)",
                    fo_round, incomplete, silentRounds);
            if (silentRounds >= FO_MAX_SILENT_ROUNDS) {
                fo_log("boards stopped answering - giving up", SEV_ERR);
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(300));
            continue;
        }
        silentRounds = 0;

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
            fo_nowTx(fo_bcast, &c, sizeof(c));
            vTaskDelay(pdMS_TO_TICKS(50));
        }
    }

    /* Boards now verify, flash and reboot. Their new firmware reports back
     * over LoRa, so keep draining it for a while. */
    fo_log("committed - waiting for boards to flash and reboot", SEV_SYS);
    end = millis() + 90000;
    while (millis() < end && !fo_abort) {
        fleet_lora_poll();
        int ok = 0, failed = 0;
        for (int i = 0; i < fo_node_count; i++) {
            if (fo_nodes[i].state == OTA_STATE_OK)     ok++;
            if (fo_nodes[i].state == OTA_STATE_FAILED) failed++;
        }
        /* Every board has reported one way or the other - no point waiting out
         * the rest of the timeout. A FAILED board is sitting in the updater
         * and will rejoin the next session. */
        if (ok + failed >= ready) break;
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
    if (!fleet_lora_begin()) { fo_log("LoRa init failed - check wiring", SEV_ERR); return false; } else {fo_log("LoRa init success", SEV_OK); }
    if (!fo_nowStart())      { fo_log("ESP-NOW init failed", SEV_ERR); return false; } else { fo_log("ESP-NOW init success", SEV_OK); }
    return true;
}

void fleet_start() {
    if (fo_running) return;
    /* Set BEFORE the task exists. loop() skips fleet_lora_poll() while this is
     * true; if the task set it itself there would be a window where core 1
     * polls the radio while core 0 is already transmitting on it. */
    fo_running = true;
    xTaskCreatePinnedToCore(fo_task, "fleet_ota", 8192, nullptr, 2, nullptr, 0);
}
