#include "WRSoftSPI.h"

void WRSoftSPI::resolveOut(int8_t pin, volatile uint32_t *&set,
                           volatile uint32_t *&clr, uint32_t &mask) {
    if (pin < 32) {
        set  = (volatile uint32_t *)GPIO_OUT_W1TS_REG;
        clr  = (volatile uint32_t *)GPIO_OUT_W1TC_REG;
        mask = 1u << pin;
    } else {
        set  = (volatile uint32_t *)GPIO_OUT1_W1TS_REG;
        clr  = (volatile uint32_t *)GPIO_OUT1_W1TC_REG;
        mask = 1u << (pin - 32);
    }
}

void WRSoftSPI::resolveIn(int8_t pin, volatile uint32_t *&reg, uint32_t &mask) {
    if (pin < 32) {
        reg  = (volatile uint32_t *)GPIO_IN_REG;
        mask = 1u << pin;
    } else {
        reg  = (volatile uint32_t *)GPIO_IN1_REG;
        mask = 1u << (pin - 32);
    }
}

void WRSoftSPI::begin(int8_t sck, int8_t miso, int8_t mosi, int8_t ss) {
    _sck = sck; _miso = miso; _mosi = mosi; _ss = ss;

    /* pinMode() rather than raw register setup: it also detaches whatever the
     * GPIO matrix had routed to these pins, which matters because SPI3 may
     * have had them matrixed in an earlier build. */
    pinMode(_sck, OUTPUT);
    pinMode(_mosi, OUTPUT);
    pinMode(_miso, INPUT);
    pinMode(_ss, OUTPUT);

    resolveOut(_sck,  _sckSet,  _sckClr,  _sckMask);
    resolveOut(_mosi, _mosiSet, _mosiClr, _mosiMask);
    resolveOut(_ss,   _ssSet,   _ssClr,   _ssMask);
    resolveIn (_miso, _misoIn,  _misoMask);

    /* Mode 0 idle state: clock low, chip deselected. */
    *_sckClr = _sckMask;
    *_ssSet  = _ssMask;

    _ready = true;
}

void WRSoftSPI::end() {
    if (!_ready) return;
    /* Leave the bus benign: deselect first so the radio does not see a
     * dangling transaction, then float the driven lines. */
    *_ssSet  = _ssMask;
    *_sckClr = _sckMask;
    pinMode(_sck, INPUT);
    pinMode(_mosi, INPUT);
    pinMode(_ss, INPUT_PULLUP);
    _ready = false;
}

uint32_t WRSoftSPI::benchmarkHz() {
    if (!_ready) return 0;

    const uint32_t kBytes = 4096;
    /* Deliberately does NOT select the radio — this clocks the bus with the
     * slave ignoring it, so it can be run at any time without disturbing
     * radio state. */
    const int64_t t0 = esp_timer_get_time();
    for (uint32_t i = 0; i < kBytes; i++) transfer(0x00);
    const int64_t t1 = esp_timer_get_time();

    const int64_t us = t1 - t0;
    if (us <= 0) return 0;
    return (uint32_t)(((uint64_t)kBytes * 8ULL * 1000000ULL) / (uint64_t)us);
}
