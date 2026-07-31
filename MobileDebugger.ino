/* ============================================================================
 *  CYD543 DEBUG TOOL  —  handheld UART monitor + board diagnostics
 *  Target: JC4827W543C (DISPLAY_CYD_543), ESP32-S3, 480x272, LVGL 9.x
 *
 *  WIRING TO THE BOARD UNDER TEST
 *    CYD GPIO17 (RX)  <-- TX of target
 *    CYD GPIO18 (TX)  --> RX of target
 *    GND              <-> GND      (mandatory, common ground)
 *  3.3V logic only. Do NOT feed 5V UART lines into GPIO17.
 *
 *  FEATURES
 *    - Full screen colour coded serial monitor (ANSI-aware + keyword heuristics)
 *    - Drag to scroll back through a PSRAM ring buffer, freeze, filter, hex view
 *    - On-screen keyboard + 6 persistent macro buttons for sending commands
 *    - Auto-baud detection, USB<->UART1 bridge mode
 *    - Diagnostics: system/PSRAM report, LCD test patterns, touch test/calib,
 *      I2C bus scanner, GPIO probe with frequency counter
 * ========================================================================== */

#include <lvgl.h>
#include "lv_conf.h"
#include <bb_spi_lcd.h>
#include "lv_bb_spi_lcd.h"
#include <Preferences.h>
#include <Wire.h>

/* --- touch variant ------------------------------------------------------- */
/* 1 = capacitive (default JC4827W543C), 0 = resistive                        */
#define TOUCH_CAPACITIVE 1

#if TOUCH_CAPACITIVE
  #include <bb_captouch.h>
  #define TOUCH_SDA   8
  #define TOUCH_SCL   4
  #define TOUCH_INT   3
  #define TOUCH_RST  -1
#else
  /* set these to match your resistive board revision */
  #define TOUCH_MOSI 11
  #define TOUCH_MISO 13
  #define TOUCH_CLK  12
  #define TOUCH_CS   38
#endif

/* --- constants ----------------------------------------------------------- */
#define FW_VERSION      "1.0.0"
#define SCREEN_W        480
#define SCREEN_H        272

#define UART_RX_PIN     43          /* UART1 RX — from target TX */ // 17 is 3v3, 43 is 5v
#define UART_TX_PIN     44          /* UART1 TX — to   target RX */ // 18 is 3v3, 44 is 5v

#define MACRO_COUNT     6
#define MACRO_LEN       40
#define FILTER_LEN      32

/* --- SD card (TF slot) — verified pins for JC4827W543 -------------------- *
 * The official silk labels IO11=MISO/IO13=MOSI, but on real units the two
 * are swapped, so these are the ones that actually work. Change if needed. */
#define SD_CS           10
#define SD_MOSI         11
#define SD_CLK          12
#define SD_MISO         13
#define SD_CLOCK_SPEED  20000000    /* 20 MHz; drop to 4000000 if a card is flaky */

/* --- pins that drive the TARGET board being flashed --------------------- *
 * Wire these two to the target's IO0(BOOT) and EN(RST). Any free GPIO on
 * this board works — 5/6/7/9/14/15/16/46 are broken out and unused.
 *
 * Moved from 5/6 to 15/16 so the LoRa module can use the RaceBoard's exact
 * pin map (SS=5, RST=6, SCK=7, MOSI=9, MISO=14) and one wiring harness
 * serves both devices. If you have an older harness, re-seat these two
 * jumpers. See fleet_ota.h. */
#define TGT_IO0         -1          /* -> target IO0 / BOOT  */
#define TGT_EN          -1          /* -> target EN  / RST   */
/* UART to the target reuses UART1: our RX(GPIO17) <- target TX,
 * our TX(GPIO18) -> target RX. Plus a common GND. 3.3V logic only. */

#define TOPBAR_H        24
#define LOG_Y           TOPBAR_H
#define LOG_H           (SCREEN_H - TOPBAR_H)
#define MAX_ROWS        32   /* enough rows for the 8 px mono font */

/* --- persisted settings (globals, shared by every header) ---------------- */
uint8_t  cfg_baud_idx    = 9;       /* index into BAUD_TABLE -> 115200 */
uint8_t  cfg_format_idx  = 0;       /* 8N1 */
uint8_t  cfg_eol_idx     = 2;       /* CR+LF */
uint8_t  cfg_font_idx    = 1;       /* montserrat 12 */
bool     cfg_timestamps  = false;
bool     cfg_hexview     = false;
bool     cfg_ansi        = true;
bool     cfg_autoscroll  = true;
bool     cfg_bridge      = false;
bool     cfg_hidenoise   = true;    /* drop 0x00/0x7F/0xFF idle-line garbage */
char     cfg_macro[MACRO_COUNT][MACRO_LEN] = {
           "help", "?", "AT", "AT+GMR", "version", "restart" };
char     cfg_filter[FILTER_LEN] = "";

/* --- runtime state ------------------------------------------------------- */
lv_display_t *disp        = nullptr;
Preferences   prefs;
bool          paused      = false;
uint32_t      rx_bytes    = 0;
uint32_t      tx_bytes    = 0;
uint32_t      rx_lines    = 0;
uint32_t      rx_noise    = 0;      /* bytes discarded as line noise */
bool          ui_dirty    = true;
int32_t       scroll_off  = 0;      /* 0 = pinned to newest line */
volatile bool g_flashing  = false;  /* true while the SD->target flasher owns UART1 */

/* forward declarations used across headers */
void monitor_mark_dirty();
void tools_refresh_macros();
void flash_refresh_softcmd();
void tools_set_autobaud_status(const char *s);

/* --- modules (single translation unit, Halo-style) ----------------------- */
#include "theme.h"
#include "logbuf.h"
#include "settings.h"
#include "touchscreen.h"
#include "serial_io.h"
#include "diag.h"
#include "sdcard.h"
#include "flasher.h"
#include "ota_protocol.h"
#include "fleet_ota.h"
#include "ui_common.h"
#include "ui_flash.h"
#include "ui_fleet.h"
#include "ui_monitor.h"
#include "ui_tools.h"

void setup() {
    Serial.begin(115200);
    delay(400);
    Serial.println("\n[BOOT] CYD543 Debug Tool " FW_VERSION);

    if (!log_init()) Serial.println("[ERROR] log buffer alloc failed");

    lv_init();
    lv_tick_set_cb([]() { return (uint32_t)(esp_timer_get_time() / 1000ULL); });

    disp = lv_bb_spi_lcd_create(DISPLAY_CYD_543);
    if (!disp) { Serial.println("[ERROR] Display init failed"); while (true) delay(100); }

    touch_setup();
    load_settings();
    uart_apply();

    ui_build_all();
    lv_screen_load(scr_monitor);

    log_push("CYD543 Debug Tool " FW_VERSION " ready", SEV_SYS);
    log_push("UART1  RX=GPIO17  TX=GPIO18  (3.3V, common GND)", SEV_SYS);
    char b[64];
    snprintf(b, sizeof(b), "Listening at %lu baud %s",
             (unsigned long)BAUD_TABLE[cfg_baud_idx], FORMAT_NAMES[cfg_format_idx]);
    log_push(b, SEV_SYS);

    /* forced initial render */
    for (int i = 0; i < 20; i++) { lv_timer_periodic_handler(); delay(5); }
}

void loop() {
    uart_poll();
    lv_timer_periodic_handler();
    delay(2);
}
