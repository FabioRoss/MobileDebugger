#pragma once
/* =========================================================================
 *  WRSoftSPI — bit-banged SPI master for the LoRa radio.
 *
 *  WHY THIS EXISTS
 *
 *  The JC4827W543 has exactly two usable SPI peripherals. The QSPI display
 *  takes SPI2 permanently (bb_spi_lcd calls spi_bus_initialize(SPI2_HOST,...)
 *  and never releases it), SPI0/SPI1 are flash and PSRAM. That leaves SPI3 as
 *  the only general-purpose master — and both the SD card and the LoRa radio
 *  wanted it.
 *
 *  Worse, both were constructed as `SPIClass x;`, and the Arduino-ESP32 core
 *  declares `SPIClass(uint8_t spi_bus = HSPI)` with HSPI == SPI3_HOST on the
 *  S3. Two default-constructed instances, same peripheral, different pins.
 *  SPIClass::begin() guards with `if (_spi) return true;` — per INSTANCE, not
 *  per bus — so the second begin() hard-resets SPI3 and re-matrixes the pins
 *  away from the first device. That is the real cause of the "board goes nuts
 *  when SD and LoRa are both up" symptom. It was worked around by making them
 *  mutually exclusive, which in turn meant the radio was dead for the whole
 *  duration of any card access.
 *
 *  Bit-banging LoRa removes it from the contest entirely: SPI3 belongs to the
 *  SD card, exclusively and permanently, and the radio is always available.
 *  The arbitration problem stops existing rather than being managed.
 *
 *  WHY BIT-BANGING IS SAFE HERE (this is the counter-intuitive part)
 *
 *  SPI is fully master-clocked. There is no timing requirement *between* bits
 *  — the slave only cares about edge order, not edge spacing. So an interrupt
 *  landing in the middle of a transfer is harmless: the slave just sees one
 *  stretched clock period. Unlike I2C or 1-Wire, preemption cannot corrupt a
 *  transfer, so nothing here disables interrupts. Doing so would hurt GPS and
 *  the display for no benefit.
 *
 *  The cost is CPU time, which is why this uses direct register writes rather
 *  than digitalWrite(). digitalWrite() is ~0.5-1 us per call; at four calls
 *  per bit that is ~24 us/byte, and the LoRa RX path polls registers every
 *  main-loop iteration. Register access brings it to well under 1 us/byte.
 *
 *  TUNING
 *
 *  The SX1276 accepts up to 10 MHz. WR_SOFTSPI_DELAY_NOPS sets the half-period
 *  padding; the default is deliberately conservative. Call benchmarkHz() on the
 *  bench to see what you are actually getting and lower the nop count if you
 *  want more speed. There is no reason to push it — a 69-byte race-control
 *  packet is ~70 bytes of FIFO traffic either way.
 * ========================================================================= */

#include <Arduino.h>
#include "soc/gpio_reg.h"
#include "soc/soc.h"
#include "esp_timer.h"

/* Half-period padding, in nop instructions. Larger = slower clock.
 * Override with -DWR_SOFTSPI_DELAY_NOPS=n if you want to retune globally. */
#ifndef WR_SOFTSPI_DELAY_NOPS
#define WR_SOFTSPI_DELAY_NOPS 6
#endif

class WRSoftSPI {
  public:
    /* Configure the four pins. Any GPIO works; pins >= 32 live in the second
     * register bank and are handled transparently. Safe to call twice. */
    void begin(int8_t sck, int8_t miso, int8_t mosi, int8_t ss);

    /* Release the pins back to inputs. The radio driver calls this from
     * end(); nothing else needs it. */
    void end();

    bool ready() const { return _ready; }

    /* Chip select. Kept in here rather than in the driver so it is a single
     * register write — a digitalWrite() pair would otherwise dominate the
     * cost of a 2-byte register access. */
    inline void select()   { *_ssClr = _ssMask; }
    inline void deselect() { *_ssSet = _ssMask; }

    /* One byte, SPI mode 0 (CPOL=0/CPHA=0), MSB first: MOSI is set while the
     * clock is low, the slave samples on the rising edge, and we sample MISO
     * there too — the slave has had it stable since the previous falling
     * edge. */
    inline uint8_t transfer(uint8_t out) {
        uint8_t in = 0;
        for (int8_t b = 7; b >= 0; b--) {
            if ((out >> b) & 1) *_mosiSet = _mosiMask;
            else                *_mosiClr = _mosiMask;
            delayHalf();
            *_sckSet = _sckMask;                       /* rising edge  */
            in <<= 1;
            if (*_misoIn & _misoMask) in |= 1;
            delayHalf();
            *_sckClr = _sckMask;                       /* falling edge */
        }
        return in;
    }

    void setDelayNops(uint8_t n) { _nops = n; }
    uint8_t delayNops() const { return _nops; }

    /* Measured bits/second, for bench work. Clocks a few thousand dummy bytes
     * with the radio DEselected, so it is safe to call any time after
     * begin(). */
    uint32_t benchmarkHz();

  private:
    inline void delayHalf() const {
        for (uint8_t i = 0; i < _nops; i++) __asm__ __volatile__("nop");
    }

    /* Resolve a pin to its set/clear registers and bit mask. GPIO 0-31 live
     * in GPIO_OUT_*, 32+ in GPIO_OUT1_*. */
    static void resolveOut(int8_t pin, volatile uint32_t *&set,
                           volatile uint32_t *&clr, uint32_t &mask);
    static void resolveIn(int8_t pin, volatile uint32_t *&reg, uint32_t &mask);

    int8_t _sck = -1, _miso = -1, _mosi = -1, _ss = -1;

    volatile uint32_t *_sckSet = nullptr,  *_sckClr = nullptr;
    volatile uint32_t *_mosiSet = nullptr, *_mosiClr = nullptr;
    volatile uint32_t *_ssSet = nullptr,   *_ssClr = nullptr;
    volatile uint32_t *_misoIn = nullptr;
    uint32_t _sckMask = 0, _mosiMask = 0, _ssMask = 0, _misoMask = 0;

    uint8_t _nops = WR_SOFTSPI_DELAY_NOPS;
    bool    _ready = false;
};
