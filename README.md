# CYD543 Debug Tool

A handheld UART monitor + board doctor for the JC4827W543C (ESP32-S3, 480×272, LVGL 9).

## Files

```
CYD543_DebugTool/
├── CYD543_DebugTool.ino   globals, setup(), loop()
├── theme.h                palette + severity colours
├── logbuf.h               PSRAM ring buffer, ANSI/keyword classification
├── settings.h             NVS persistence
├── touchscreen.h          touch init + indev callback (+ raw access for tests)
├── serial_io.h            UART1, line assembly, TX, auto-baud, USB bridge
├── diag.h                 sysinfo, PSRAM test, I2C scan, GPIO probe
├── ui_common.h            widget builders + global keyboard overlay
├── ui_monitor.h           the serial monitor screen
├── ui_tools.h             tools tabs + LCD test + touch test screens
├── lv_conf.h              ← yours (see below)
├── lv_bb_spi_lcd.h/.cpp   ← copy verbatim from an existing working project
```

`lv_bb_spi_lcd.h/.cpp` are the static driver files — copy them from one of your
existing CYD543 sketches, don't regenerate them.

## Libraries

lvgl ≥ 9.3, bb_spi_lcd, bb_captouch (capacitive variant only), **ArduinoJson ≥ 7.0**
(for manifest parsing — the sketch uses the v7 `JsonDocument` API). SD and FS come
with the ESP32 Arduino core.

## lv_conf.h

Start from your existing one and make sure these are set:

```c
#define LV_COLOR_DEPTH        16
#define LV_MEM_SIZE           (128 * 1024U)
#define LV_USE_CANVAS         1
#define LV_USE_TABVIEW        1
#define LV_USE_KEYBOARD       1
#define LV_USE_TEXTAREA       1
#define LV_USE_DROPDOWN       1
#define LV_USE_SWITCH         1
#define LV_USE_LIST           1
#define LV_USE_BAR            1
#define LV_FONT_MONTSERRAT_10 1
#define LV_FONT_MONTSERRAT_12 1
#define LV_FONT_MONTSERRAT_14 1
#define LV_FONT_MONTSERRAT_16 1
```

Board settings: ESP32S3 Dev Module, **PSRAM enabled** (the log buffer and the
touch-test canvas both live there; without it the log falls back to 150 lines).

Touch variant: `#define TOUCH_CAPACITIVE 1` at the top of the `.ino` (set to 0
and fill in the four SPI pins for the resistive board).

## Wiring to the board under test

| Debug tool | Target |
|---|---|
| GPIO17 (RX) | target TX |
| GPIO18 (TX) | target RX |
| GND | GND — mandatory |

3.3 V logic only. Never feed a 5 V TX line into GPIO17 without a divider.

## Monitor screen

Status bar: link LED, current baud/format, RX/TX byte counters, line count,
active filter. Buttons, left to right:

- **⏸ / ▶** freeze capture (bytes still counted, nothing is parsed)
- **🗑** clear buffer
- **⌨** open the keyboard and send a command
- **⇩** jump back to the newest line
- **⚙** tools

Drag anywhere on the log to scroll back through history; the violet bar on the
right shows where you are. New lines never yank the view while you're scrolled back.

Long lines are wrapped across as many screen rows as they need, and every
wrapped row keeps the colour of the line it came from. Break points come from
measuring real glyph advances, so wrapping is correct for the proportional
fonts; the wrapped height of each line is cached per ring slot so the
scrollbar does not re-measure the whole buffer every frame.

**Hide noise** (UART tab, on by default) drops every byte that is not
printable ASCII, CR, LF or TAB, and counts them as `N` in the status bar.
This covers the whole class of line-noise framings (`0xFF`, `0xFE`, `0xFC`,
`0x00`, `0x7F`) rather than a fixed list. Turn it off to see the raw bytes
escaped as `\FE`; hex view is never filtered.

### Colour coding

| Colour | Meaning |
|---|---|
| red | error, fail, panic, assert, backtrace, exception, timeout, `E (…)` |
| amber | warn, retry, missing, unknown, `W (…)` |
| green | ok, ready, connected, done, started, `success` |
| blue | info, `I (…)` |
| cyan | debug/verbose, `D (…)`, `V (…)` |
| violet | lines you sent (`>> cmd`) |
| grey | the tool's own messages |

If the target emits ANSI colours (ESP-IDF default), those win and the escape
codes are stripped instead of littering the log. Toggle in UART tab.

Non-printable bytes render as `\4F` — a screenful of those means wrong baud.

## Tools

**UART** — baud, framing, line ending, log font size, timestamps, hex view,
ANSI colours, USB bridge, substring filter, and **auto-baud sweep**: it walks 15
rates scoring ~350 ms of traffic each on how much of it looks like text, then
locks on. Keep the target talking while it runs (~5 s).

**MACRO** — six commands persisted in NVS, one tap to send, EDIT to change.

**SYS** — chip/revision/cores/clock, MAC, flash, PSRAM, heap, sketch size, IDF
version, die temperature, **reset reason** (brownout vs panic vs WDT is usually
the whole diagnosis) and uptime. PSRAM TEST writes and verifies 1 MB and reports
throughput.

**LCD/TOUCH** — full-screen test patterns (R/G/B/white/black, colour bars, 1 px
grid, gradient; tap to cycle, EXIT top right) for dead pixels, stuck rows and
wrong colour order. Touch test paints where you press over a 40 px reference
grid — gaps are dead zones, consistent offset is calibration drift. Live raw vs
mapped coordinates and the running min/max calibration values are on the tab.

**I2C** — scan any pin pair, defaults to the touch bus (SDA 8 / SCL 4), with
guesses for common addresses (GT911, CST816, FT6336, RTCs, sensors). Answers
"is the touch controller even alive, and which one is it?". Touch is
re-initialised automatically after scanning its own bus.

**GPIO** — attach any broken-out pin as input / pullup / pulldown / output,
read level, rough edge frequency (500 ms window), drive high/low or fire a
200 µs pulse.

## FLASH tab — program a second board from SD

The tool can flash a precompiled firmware onto another ESP board over UART using
the ESP ROM bootloader protocol (the same one esptool speaks). No stub is
uploaded, so writes are uncompressed (ROM speed) but every part is MD5-verified
after writing. Targets are auto-detected: ESP32, S2, S3, C3, C6, ESP8266.

### Extra wiring (on top of the monitor's TX/RX/GND)

| Debug tool | Target | Purpose |
|---|---|---|
| GPIO17 (RX) | target TX (U0TXD) | serial |
| GPIO18 (TX) | target RX (U0RXD) | serial |
| **GPIO5** | target **IO0 / BOOT** | enter download mode (optional) |
| **GPIO6** | target **EN / RST** | reset the target (optional) |
| GND | GND | mandatory |

The two control pins are `TGT_IO0` and `TGT_EN` at the top of the `.ino` — change
them to any free GPIO (5/6/7/9/14/15/16/46 are broken out and unused on this
board), or set either to `-1` if you are not using them. They are optional; see
"Getting the target into download mode" below. 3.3 V logic only.

The serial pair must reach the target's **UART0** (`IO43`/`IO44` on an S3) —
the ROM bootloader only listens there, no matter what the application did with
USB CDC. On the debug tool side any pins work, since it uses the UART1
peripheral; set `UART_RX_PIN` / `UART_TX_PIN` accordingly. If you point them at
the tool's own `IO43`/`IO44`, enable **USB CDC On Boot** on the debug tool so
its console moves to native USB and stops contending for those pins.

### SD card

Pins are `SD_CS 10 / SD_MOSI 11 / SD_CLK 12 / SD_MISO 13` on a dedicated
`SPIClass` so the TF slot never fights the QSPI display. On the **resistive**
variant this bus is shared with the XPT2046 touch controller, so SD and
resistive touch can't both be live; the **capacitive** variant (I²C touch) is
unaffected.

### Two ways to supply firmware

**1. Merged binary.** Produce a single image with
`esptool merge-bin -o merged.bin ...` (or your build system's merged output),
drop it on the SD card, select it in the browser — it's flashed at offset `0x0`.

**2. Manifest** (esp-web-tools style). A `manifest.json` next to its `.bin`
files, part paths relative to the manifest:

```json
{
  "name": "My Firmware",
  "parts": [
    { "path": "bootloader.bin",  "offset": "0x0"    },
    { "path": "partitions.bin",  "offset": "0x8000" },
    { "path": "boot_app0.bin",   "offset": "0xe000" },
    { "path": "firmware.bin",    "offset": "0x10000"}
  ]
}
```

`offset` accepts a hex string (`"0x10000"`) or a number. The esp-web-tools
`{"builds":[{"parts":[...]}]}` shape is also accepted (first build is used).
Up to 8 parts.

### Getting the target into download mode

A PC does this with DTR/RTS, which plain TX/RX does not give you. Pick a
strategy in **Enter bootloader**:

- **auto (IO0+EN)** — the tool drives the two extra wires itself. Fully hands
  off. Set `TGT_IO0` / `TGT_EN` to `-1` in the `.ino` if you have not wired them.
- **soft command** — the tool sends a string over UART and the *target's own
  firmware* reboots itself into download mode. Only two data wires needed, but
  the target must implement it, and it cannot recover a crashed or bricked
  board. Edit the string with **CMD**. On the target:

  ```cpp
  #include "soc/rtc_cntl_reg.h"
  // call this when you receive your chosen command over serial
  void enterDownloadMode() {
    Serial0.flush();
    REG_WRITE(RTC_CNTL_OPTION1_REG, RTC_CNTL_FORCE_DOWNLOAD_BOOT);
    esp_restart();
  }
  ```
  (Valid for ESP32/S2/S3/C3; the register moved on some newer chips.)
- **manual BOOT+RST** — hold BOOT on the target, tap RST, release. The tool
  retries the sync for 25 s, so there is no rush. Zero extra wires, always
  works, including on a bricked board.

After flashing, auto mode reboots the target for you; the other two modes ask
you to tap RST.

### Using it

1. Insert the SD card, open **Tools → FLASH**, tap **MOUNT**.
2. Browse (tap folders to enter, `..` to go up) and tap a `.bin` or
   `manifest.json` to select it — the selection line shows the parts and offsets.
3. Pick a **Speed** (default 460800; drop to 115200 if a target is flaky over
   long/hand wiring) and leave **verify (MD5)** on.
4. Tap **FLASH**. The view switches to the monitor so you can watch progress,
   chip detection and per-part verification scroll by in colour. The progress
   bar and percent live on the FLASH tab; **ABORT** stops mid-write.

On success the target is reset into the new firmware automatically. Roughly:
1 MB ≈ 30–60 s at 460800.

### Notes / limits

- The flash runs in its own FreeRTOS task on core 0; the UI (core 1) only polls
  progress, so the screen stays responsive and LVGL is never touched from the
  flash task.
- Encrypted-flash and Secure Boot targets are not handled (plaintext only).
- Power the target from its own supply if it has WiFi/BT bursts; share a common
  ground. Only very small targets should be run off the tool's 3.3 V rail.

## Notes

- The log buffer is 1000 lines × 120 chars in PSRAM (~125 KB).
- Lines longer than 119 chars wrap into a new entry; a partial line with no
  newline is flushed after 300 ms so prompts like `>` still appear.
- `\r`, `\n` and `\r\n` are all handled as line breaks.
- USB bridge mode mirrors UART1 to the USB serial port and vice versa, so you
  can drop the tool inline between a PC and a target.
