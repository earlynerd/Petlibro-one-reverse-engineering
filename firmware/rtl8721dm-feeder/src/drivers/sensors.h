#pragma once
#include <Arduino.h>

// ===========================================================================
//  Sensor cluster. Two kinds of channel:
//   * ANALOG (12-bit, 0..4095) — motor current shunts + battery divider.
//   * DIGITAL photoelectric DETECTORS — beam present/absent, two clean states
//     (~0.2 V / ~3.2 V on the bench), read as GPIO. Their EMITTERS are powered
//     via the AW9523B (auto-powered by the motor drivers; or `emit`).
//
//  Channels (Docs/RTL8721DM_module_pinout.txt; open/closed SWAPPED per bench
//  verification 2026-06-20):
//    lidCurrent   PB_2  ADC  lid-motor current shunt  [swapped vs doc, bench]
//    feedCurrent  PB_1  ADC  feed-motor current shunt [swapped vs doc, bench]
//    battery      PB_7  ADC  battery divider
//    lidOpen      PB_4  dig   lid OPEN-end detector   (emitter P0_4)
//    lidClosed    PB_3  dig   lid CLOSED-end detector (emitter P0_4)
//    rotor        PB_5  dig   dispense-rotor encoder, 1 pulse/rev (emitter P0_5)
//    chute        PA_17 dig   dispense-chute detector (emitter P0_7)  [swapped vs doc, bench]
//    hopper       PB_6  dig   food-level detector (emitter P0_7)      [swapped vs doc, bench]
// ===========================================================================
namespace Sensors {
    void begin();
    // analog (0..4095)
    uint16_t lidCurrent();
    uint16_t feedCurrent();
    uint16_t battery();
    // current in mA via the 0.25 ohm shunt, idle-offset subtracted (see .cpp)
    uint32_t lidCurrentMa();
    uint32_t feedCurrentMa();
    void     zeroCurrent();      // capture the idle ADC offset (motors must be stopped)
    uint16_t lidZero();
    uint16_t feedZero();
    // digital photoelectric (raw pin level: true = HIGH)
    bool lidOpen();
    bool lidClosed();
    bool rotor();
    bool chute();
    bool hopper();
}
void sensorsInit();
