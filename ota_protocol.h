#ifndef OTA_PROTOCOL_H
#define OTA_PROTOCOL_H
/* =========================================================================
 *  WeRace fleet-OTA wire protocol — shared definitions.
 *
 *  This header is byte-identical in three places and MUST be kept in sync:
 *    RaceBoard/ota_protocol.h          (main app: LoRa trigger only)
 *    RaceBoard/updater/ota_protocol.h  (updater app: LoRa + ESP-NOW)
 *    MobileDebugger/ota_protocol.h     (fleet flasher: LoRa + ESP-NOW)
 *
 *  Two transports, two jobs:
 *
 *    LoRa    — control channel. Always listening on every board, so it is how
 *              a session is announced and how results come back. Low rate, so
 *              it never carries image bytes.
 *    ESP-NOW — bulk channel. Only alive inside the updater app during a
 *              session. Broadcast, so 1 board and 20 boards cost the same.
 *
 *  LoRa framing reuses the existing WeRaceLoRa frame so old firmware stays
 *  interoperable:
 *
 *      [len:1][payload:len][crc32:4]
 *
 *  and every OTA payload begins with OTA_LORA_MAGIC. Legacy RaceBoard
 *  firmware parses payload[0] as `group` and bails out of onReceiveSuccess()
 *  unless it is 0 or 1 — so a board that has not been updated yet silently
 *  ignores OTA traffic instead of misreading it as a race-control command.
 *  Do not ever change OTA_LORA_MAGIC to 0 or 1.
 * ========================================================================= */

#include <stdint.h>

/* ---------------------------------------------------------------- LoRa --- */

#define OTA_LORA_MAGIC      0xB7    /* != 0 and != 1, see note above */
#define OTA_PROTO_VERSION   1

/* LoRa payload[1] — packet type */
enum : uint8_t {
    OTA_LORA_ANNOUNCE = 0x01,   /* flasher -> all: a session is starting     */
    OTA_LORA_ABORT    = 0x02,   /* flasher -> all: stand down                */
    OTA_LORA_ROLLCALL = 0x03,   /* flasher -> all: who is out there?         */
    OTA_LORA_HELLO    = 0x04,   /* board -> flasher: rollcall reply / status */
    OTA_LORA_RESULT   = 0x05,   /* board -> flasher: update finished         */
};

/* Board lifecycle state, reported in HELLO/RESULT so the flasher roster can
 * show what each unit is actually doing. */
enum : uint8_t {
    OTA_STATE_APP      = 0,     /* running the main app, idle                */
    OTA_STATE_DEFERRED = 1,     /* accepted, waiting for the session to end   */
    OTA_STATE_UPDATER  = 2,     /* booted into the updater, ready to receive  */
    OTA_STATE_OK       = 3,     /* update applied, running the new app        */
    OTA_STATE_FAILED   = 4,     /* update failed, still in the updater        */
};

#define OTA_VERSION_LEN  16     /* firmware version string, NUL-padded */

/* Announce: the whole session description. 59 bytes, comfortably inside a
 * LoRa packet even at SF12. Sent repeatedly for the length of the join
 * window, so a board that misses one still joins. */
struct __attribute__((packed)) OtaAnnounce {
    uint8_t  magic;                     /* OTA_LORA_MAGIC        */
    uint8_t  type;                      /* OTA_LORA_ANNOUNCE     */
    uint8_t  proto;                     /* OTA_PROTO_VERSION     */
    uint8_t  wifi_channel;              /* 1..13, ESP-NOW channel */
    uint32_t session_id;
    uint32_t image_size;
    uint32_t block_count;
    uint8_t  sha256[32];                /* of the complete image */
    char     version[OTA_VERSION_LEN];  /* human-readable, for the UI */
};

/* HELLO / RESULT: what a board says about itself. */
struct __attribute__((packed)) OtaHello {
    uint8_t  magic;
    uint8_t  type;                      /* HELLO or RESULT       */
    uint8_t  proto;
    uint8_t  state;                     /* OTA_STATE_*           */
    uint8_t  mac[6];                    /* board identity        */
    uint32_t session_id;                /* 0 when idle           */
    uint8_t  progress;                  /* 0..100, updater only  */
    uint8_t  error;                     /* OTA_ERR_*, 0 = none   */
    char     version[OTA_VERSION_LEN];
};

/* ------------------------------------------------------------- ESP-NOW --- */

/* ESP-NOW caps a payload at 250 bytes. Header is 8, so 242 bytes of image
 * ride in each block. Keep OTA_BLOCK_DATA a multiple of 2 so the PSRAM
 * staging buffer stays halfword-aligned. */
#define OTA_ESPNOW_MAX_PAYLOAD  250
#define OTA_BLOCK_DATA          242

/* ESP-NOW packet types */
enum : uint8_t {
    OTA_NOW_READY   = 0x10,   /* board -> flasher: in updater, send it       */
    OTA_NOW_DATA    = 0x11,   /* flasher -> all:   one image block           */
    OTA_NOW_POLL    = 0x12,   /* flasher -> board: what are you missing?     */
    OTA_NOW_BITMAP  = 0x13,   /* board -> flasher: missing-block bitmap slice */
    OTA_NOW_COMPLETE= 0x14,   /* board -> flasher: image received + verified */
    OTA_NOW_COMMIT  = 0x15,   /* flasher -> all:   verify and flash now      */
    OTA_NOW_STATUS  = 0x16,   /* board -> flasher: flashing progress/result  */
    OTA_NOW_ABORT   = 0x17,   /* flasher -> all:   stand down                */
};

/* Every ESP-NOW packet starts with this. Kept at 8 bytes so OTA_BLOCK_DATA
 * lands on 242 and the whole frame is exactly 250. */
struct __attribute__((packed)) OtaNowHeader {
    uint8_t  magic;         /* OTA_LORA_MAGIC, reused as a cheap sanity gate */
    uint8_t  type;          /* OTA_NOW_*    */
    uint16_t session_lo;    /* low 16 bits of session_id — enough to reject
                             * a stale session without spending 4 bytes      */
    uint32_t block;         /* DATA: block index. Others: type-specific.     */
};

struct __attribute__((packed)) OtaNowData {
    OtaNowHeader hdr;
    uint8_t      data[OTA_BLOCK_DATA];
};

/* Bitmap slices: a 2.9 MB image is ~12100 blocks = ~1.5 KB of bitmap, so the
 * reply is split across several packets. `block` in the header carries the
 * slice's starting BYTE offset into the bitmap. */
#define OTA_BITMAP_SLICE  200

struct __attribute__((packed)) OtaNowBitmap {
    OtaNowHeader hdr;
    uint8_t      mac[6];
    uint16_t     total_bytes;   /* full bitmap length, so the flasher knows
                                 * how many slices to expect                 */
    uint8_t      bits[OTA_BITMAP_SLICE];  /* 1 = MISSING */
};

struct __attribute__((packed)) OtaNowStatus {
    OtaNowHeader hdr;
    uint8_t      mac[6];
    uint8_t      state;      /* OTA_STATE_* */
    uint8_t      progress;   /* 0..100      */
    uint8_t      error;      /* OTA_ERR_*   */
};

/* ---------------------------------------------------------------- errors -- */

enum : uint8_t {
    OTA_ERR_NONE      = 0,
    OTA_ERR_NO_PSRAM  = 1,   /* staging allocation failed - the PSRAM image  *
                              * buffer, or the small DRAM block bitmap that  *
                              * goes with it. The updater's serial log says   *
                              * which; one code covers both so the wire       *
                              * format does not grow for a diagnostic nicety. */
    OTA_ERR_TOO_BIG   = 2,   /* image exceeds the target partition           */
    OTA_ERR_SHA       = 3,   /* whole-image hash mismatch                    */
    OTA_ERR_FLASH     = 4,   /* esp_ota_write / esp_ota_end failed           */
    OTA_ERR_TIMEOUT   = 5,   /* session went quiet                           */
    OTA_ERR_NO_PART   = 6,   /* target partition not found in the table      */
    OTA_ERR_BAD_IMAGE = 7,   /* image header magic is not 0xE9               */
};

/* ---------------------------------------------------------------- timing -- */

#define OTA_JOIN_WINDOW_MS      15000   /* announce repeats for this long   */
#define OTA_ANNOUNCE_PERIOD_MS   1500   /* ...at this rate                  */
#define OTA_SESSION_IDLE_MS     120000  /* updater gives up after this      */
#define OTA_POLL_TIMEOUT_MS       600   /* wait for a board's bitmap reply  */

/* Gap between broadcast DATA packets. ESP-NOW has no flow control and the
 * receivers are also driving a display, so pacing is the only backpressure
 * we get. 1200 us is ~10x the airtime of a 250-byte frame at the 1 Mbps
 * basic rate and measured comfortably below the point where receivers start
 * dropping. Raise it if repair rounds stop converging. */
#define OTA_TX_GAP_US            1200

/* --------------------------------------------------------------- helpers -- */

static inline uint32_t otaBlockCount(uint32_t image_size) {
    return (image_size + OTA_BLOCK_DATA - 1) / OTA_BLOCK_DATA;
}

static inline uint32_t otaBitmapBytes(uint32_t block_count) {
    return (block_count + 7) / 8;
}

#endif /* OTA_PROTOCOL_H */
