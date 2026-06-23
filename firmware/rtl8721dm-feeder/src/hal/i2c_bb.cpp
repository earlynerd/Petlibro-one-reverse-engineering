#include "hal/i2c_bb.h"

void I2cBitBang::d() { delayMicroseconds(6); }           // ~80 kHz

void I2cBitBang::sdaHi() { gpio_dir(&_sda, PIN_INPUT); }  // release -> pull-up HIGH
void I2cBitBang::sdaLo() { gpio_dir(&_sda, PIN_OUTPUT); } // drive preset-0 -> LOW
void I2cBitBang::sclHi() { gpio_dir(&_scl, PIN_INPUT); }
void I2cBitBang::sclLo() { gpio_dir(&_scl, PIN_OUTPUT); }

void I2cBitBang::begin(PinName sda, PinName scl) {
    gpio_init(&_sda, sda);   gpio_init(&_scl, scl);
    gpio_write(&_sda, 0);    gpio_write(&_scl, 0);        // preset 0: OUTPUT == drive LOW
    gpio_mode(&_sda, PullUp); gpio_mode(&_scl, PullUp);
    gpio_dir(&_sda, PIN_INPUT); gpio_dir(&_scl, PIN_INPUT);  // idle released (HIGH)
}

void I2cBitBang::start() { sdaHi(); sclHi(); d(); sdaLo(); d(); sclLo(); d(); }
void I2cBitBang::stop()  { sdaLo(); d(); sclHi(); d(); sdaHi(); d(); }

int I2cBitBang::wr(uint8_t v) {
    for (int i = 0; i < 8; i++) {
        if (v & 0x80) sdaHi(); else sdaLo();
        v <<= 1; d(); sclHi(); d(); sclLo();
    }
    sdaHi(); d(); sclHi(); d();
    int ack = (gpio_read(&_sda) == 0);
    sclLo(); d();
    return ack;
}

uint8_t I2cBitBang::rd(bool sendAck) {
    uint8_t v = 0; sdaHi();
    for (int i = 0; i < 8; i++) {
        d(); sclHi(); d();
        v = (v << 1) | (gpio_read(&_sda) & 1);
        sclLo();
    }
    if (sendAck) sdaLo(); else sdaHi();
    d(); sclHi(); d(); sclLo(); d(); sdaHi();
    return v;
}

bool I2cBitBang::probe(uint8_t addr7) {
    start(); int ack = wr((uint8_t)(addr7 << 1)); stop();
    return ack != 0;
}

bool I2cBitBang::writeReg(uint8_t addr7, uint8_t reg, uint8_t val) {
    start();
    int ok = wr((uint8_t)(addr7 << 1)); ok &= wr(reg); ok &= wr(val);
    stop();
    return ok != 0;
}

int I2cBitBang::readReg(uint8_t addr7, uint8_t reg, uint8_t* buf, int n) {
    start();
    int ok = wr((uint8_t)(addr7 << 1)); ok &= wr(reg);
    start();
    ok &= wr((uint8_t)((addr7 << 1) | 1));
    if (!ok) { stop(); return -1; }
    for (int i = 0; i < n; i++) buf[i] = rd(i < n - 1);
    stop();
    return n;
}
