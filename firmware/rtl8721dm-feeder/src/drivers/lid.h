#pragma once
#include <Arduino.h>

// ===========================================================================
//  Lid/cover motor — H-bridge on RTL PWM pins:
//     PA_28 = PWM6 = A = OPEN ;  PA_30 = PWM7 = B = CLOSE
//  Both LOW = STOP (never leave hi-Z — it creeps). Current shunt PB_1; endstop
//  photoelectric detectors PB_3 (open) / PB_4 (closed), emitter P0_4.
//
//  NON-BLOCKING: open()/close() start driving and return; update() (called from
//  loop()) auto-stops on hard timeout, stall current, or — in closed-loop goto —
//  the target endstop crossing its threshold. Lets you watch live ADCs while it
//  moves and characterize endstop polarity/threshold before trusting goto.
// ===========================================================================
namespace Lid {
    void begin();
    void update();             // call every loop(): enforces stop conditions
    void stop();
    void open(float duty);     // duty 0..1 on PA_28
    void close(float duty);    // duty 0..1 on PA_30
    void moveTo(bool open);    // closed-loop drive to an endstop (app-level, default duty/timeout)
}
void lidInit();
