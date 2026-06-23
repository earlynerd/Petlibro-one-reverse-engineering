#pragma once
#include <Arduino.h>

// ===========================================================================
//  AW9523B 16-port GPIO/LED expander (addr 0x58-0x5B, ID reg 0x10 == 0x23).
//  Bit-bang I2C on PA_18/PA_19. Gates most of the feeder's peripherals:
//
//    P0_2 / P0_3  feed-motor H-bridge (external pulldown)  -- raw only, careful
//    P0_4         lid photo-emitter power
//    P0_5         dispense/rotor-encoder emitter power
//    P0_6         RFID module power enable (PWEN)
//    P0_7         hopper + chute emitter power
//    P1_7         piezo beeper
//    P0_0 / P0_1  HW-revision strap inputs (read-only)
//
//  Per-pin assert is read-modify-write (other pins untouched): GCR push-pull,
//  LEDM GPIO-mode, CFG output, OUT level. Proven by tools/aw9523_pwen.py.
// ===========================================================================
namespace Aw9523 {
    bool        begin();                 // find chip on PA_18/PA_19 (both orderings)
    bool        present();
    uint8_t     address();
    const char* sdaName();
    const char* sclName();

    bool readReg(uint8_t reg, uint8_t& out);
    bool writeReg(uint8_t reg, uint8_t val);

    // Drive a pin as a push-pull GPIO output (full init). port 0|1, bit 0..7.
    bool setOutput(uint8_t port, uint8_t bit, bool level);
    // Read a pin's logic level from the input register (no reconfigure).
    bool getInput(uint8_t port, uint8_t bit, bool& level);

    // Named conveniences used by later drivers.
    bool rfidPower(bool on);             // P0_6
    bool beep(uint16_t ms);              // P1_7 (DC pulse; silent if passive piezo)
    bool emitter(uint8_t which, bool on);// 0=lid(P0_4) 1=dispense(P0_5) 2=hopper(P0_7)
}

// Register harness commands + state contributor, and attempt begin().
void aw9523Init();
