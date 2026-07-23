#pragma once
#include <esp_system.h>

/* ------------------------------------------------------------------ *
 *  SYSTEM REPORT — run this firmware on a suspect board to confirm
 *  chip revision, flash size and, above all, whether PSRAM is present
 *  and working (the usual culprit behind "my CYD reboots randomly").
 * ------------------------------------------------------------------ */
static const char *reset_reason_str() {
    switch (esp_reset_reason()) {
        case ESP_RST_POWERON:  return "power-on";
        case ESP_RST_EXT:      return "external";
        case ESP_RST_SW:       return "software";
        case ESP_RST_PANIC:    return "PANIC / exception";
        case ESP_RST_INT_WDT:  return "interrupt WDT";
        case ESP_RST_TASK_WDT: return "task WDT";
        case ESP_RST_WDT:      return "other WDT";
        case ESP_RST_BROWNOUT: return "BROWNOUT (power!)";
        case ESP_RST_SDIO:     return "sdio";
        default:               return "unknown";
    }
}

void diag_sysinfo(char *b, size_t n) {
    uint64_t mac = ESP.getEfuseMac();
    uint32_t up  = millis() / 1000;
    snprintf(b, n,
        "Chip      %s rev %d, %d core(s) @ %lu MHz\n"
        "MAC       %02X:%02X:%02X:%02X:%02X:%02X\n"
        "Flash     %lu KB @ %lu MHz\n"
        "PSRAM     %lu KB total / %lu KB free\n"
        "Heap      %lu KB free / %lu KB min / %lu KB total\n"
        "Sketch    %lu KB used, %lu KB free\n"
        "IDF       %s\n"
        "Temp      %.1f C\n"
        "Reset     %s\n"
        "Uptime    %lu h %lu m %lu s\n"
        "Log buf   %u lines (%s)\n"
        "Tool FW   " FW_VERSION,
        ESP.getChipModel(), ESP.getChipRevision(), ESP.getChipCores(),
        (unsigned long)ESP.getCpuFreqMHz(),
        (uint8_t)(mac >> 40), (uint8_t)(mac >> 32), (uint8_t)(mac >> 24),
        (uint8_t)(mac >> 16), (uint8_t)(mac >> 8),  (uint8_t)mac,
        (unsigned long)(ESP.getFlashChipSize() / 1024),
        (unsigned long)(ESP.getFlashChipSpeed() / 1000000),
        (unsigned long)(ESP.getPsramSize() / 1024),
        (unsigned long)(ESP.getFreePsram() / 1024),
        (unsigned long)(ESP.getFreeHeap() / 1024),
        (unsigned long)(ESP.getMinFreeHeap() / 1024),
        (unsigned long)(ESP.getHeapSize() / 1024),
        (unsigned long)(ESP.getSketchSize() / 1024),
        (unsigned long)(ESP.getFreeSketchSpace() / 1024),
        esp_get_idf_version(),
        temperatureRead(),
        reset_reason_str(),
        (unsigned long)(up / 3600), (unsigned long)((up / 60) % 60), (unsigned long)(up % 60),
        (unsigned)s_cap, s_cap == LOG_LINES_PSRAM ? "PSRAM" : "internal RAM");
}

/* Write/verify a 1 MB pattern through PSRAM and report throughput. */
void diag_psram_test(char *out, size_t n) {
    const size_t SZ = 1024 * 1024;
    uint32_t *p = (uint32_t *)heap_caps_malloc(SZ, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!p) { snprintf(out, n, "PSRAM test: allocation of 1 MB FAILED"); return; }

    uint32_t t0 = millis();
    for (size_t i = 0; i < SZ / 4; i++) p[i] = (uint32_t)(i * 2654435761UL);
    uint32_t t1 = millis();
    size_t errors = 0;
    for (size_t i = 0; i < SZ / 4; i++)
        if (p[i] != (uint32_t)(i * 2654435761UL)) errors++;
    uint32_t t2 = millis();
    heap_caps_free(p);

    uint32_t wms = (t1 - t0) ? (t1 - t0) : 1;
    uint32_t rms = (t2 - t1) ? (t2 - t1) : 1;
    snprintf(out, n, "PSRAM 1 MB: %s\nwrite %lu ms (%.1f MB/s)\nread  %lu ms (%.1f MB/s)\nerrors %u",
             errors ? "FAILED" : "PASS",
             (unsigned long)(t1 - t0), 1000.0f / (float)wms,
             (unsigned long)(t2 - t1), 1000.0f / (float)rms,
             (unsigned)errors);
}

/* ------------------------------------------------------------------ *
 *  I2C SCANNER — mostly used to find out which touch controller a
 *  board actually carries when touch "doesn't work".
 * ------------------------------------------------------------------ */
static const char *i2c_guess(uint8_t a) {
    switch (a) {
        case 0x14: case 0x5D: return "GT911 touch";
        case 0x15:            return "CST816/CST820 touch";
        case 0x38:            return "FT6236/FT6336 touch";
        case 0x24:            return "CST226 touch";
        case 0x3C: case 0x3D: return "SSD1306/SH1106 OLED";
        case 0x40:            return "INA219 / SHT / PCA9685";
        case 0x48:            return "ADS1115 / TMP102";
        case 0x51:            return "PCF8563 RTC";
        case 0x57:            return "AT24C EEPROM";
        case 0x68:            return "DS3231 / MPU6050";
        case 0x76: case 0x77: return "BMP280/BME280";
        case 0x27: case 0x3F: return "PCF8574 LCD backpack";
        default:              return "unknown";
    }
}

int diag_i2c_scan(int sda, int scl, char *out, size_t n) {
    int found = 0, p = 0;
    out[0] = 0;
    Wire.end();
    Wire.begin(sda, scl, 100000);
    for (uint8_t a = 1; a < 127; a++) {
        Wire.beginTransmission(a);
        if (Wire.endTransmission() == 0) {
            found++;
            p += snprintf(out + p, n - p, "0x%02X  %s\n", a, i2c_guess(a));
            if ((size_t)p >= n - 40) break;
        }
        delay(1);
    }
    if (!found) snprintf(out, n, "No devices responded on SDA=%d SCL=%d.\n"
                                 "Check pull-ups, wiring and 3.3V.", sda, scl);
    Wire.end();
#if TOUCH_CAPACITIVE
    /* the touch controller shares this bus — bring it back up */
    if (sda == TOUCH_SDA && scl == TOUCH_SCL) bbct.init(TOUCH_SDA, TOUCH_SCL, TOUCH_RST, TOUCH_INT);
#endif
    return found;
}

/* ------------------------------------------------------------------ *
 *  GPIO PROBE — level, pull config, edge frequency, manual drive.
 *  3.3V logic only.
 * ------------------------------------------------------------------ */
volatile uint32_t probe_edges = 0;
int   probe_pin  = -1;
uint8_t probe_mode = 0;   /* 0 input, 1 input_pullup, 2 input_pulldown, 3 output */

static void IRAM_ATTR probe_isr() { probe_edges++; }

void probe_detach() {
    if (probe_pin >= 0) { detachInterrupt(digitalPinToInterrupt(probe_pin)); probe_pin = -1; }
}

void probe_attach(int pin, uint8_t mode) {
    probe_detach();
    probe_pin  = pin;
    probe_mode = mode;
    switch (mode) {
        case 0: pinMode(pin, INPUT);          break;
        case 1: pinMode(pin, INPUT_PULLUP);   break;
        case 2: pinMode(pin, INPUT_PULLDOWN); break;
        case 3: pinMode(pin, OUTPUT);         break;
    }
    if (mode != 3) {
        probe_edges = 0;
        attachInterrupt(digitalPinToInterrupt(pin), probe_isr, CHANGE);
    }
}
