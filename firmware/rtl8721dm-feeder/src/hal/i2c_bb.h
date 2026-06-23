#pragma once
#include <Arduino.h>
extern "C" {
  #include "PinNames.h"
  #include "gpio_api.h"
}

// ===========================================================================
//  Bit-bang I2C on arbitrary GPIO — ported verbatim from the proven pinmap
//  explorer (firmware/rtl8721dm-pinmap). Open-drain emulation: releasing a line
//  (dir=INPUT) lets the pull-up take it HIGH; driving (dir=OUTPUT, preset 0)
//  pulls it LOW. ~80 kHz, very relaxed timing.
//
//  Needed because the AW9523B sits on PA_18/PA_19 — pins the hardware I2C0
//  pinmux cannot reach (Wire is locked to PA_25/PA_26, the RFID UART). Uses the
//  raw mbed gpio_t HAL with PinName values, not Arduino digitalRead/pinMode
//  (which index the SparkFun variant's fixed pin table).
// ===========================================================================
class I2cBitBang {
public:
    void begin(PinName sda, PinName scl);
    bool probe(uint8_t addr7);                                  // address-only ACK
    bool writeReg(uint8_t addr7, uint8_t reg, uint8_t val);
    int  readReg(uint8_t addr7, uint8_t reg, uint8_t* buf, int n);  // bytes read, or -1

private:
    gpio_t _sda, _scl;
    void d();
    void sdaHi(); void sdaLo();
    void sclHi(); void sclLo();
    void start();  void stop();
    int     wr(uint8_t v);
    uint8_t rd(bool sendAck);
};
