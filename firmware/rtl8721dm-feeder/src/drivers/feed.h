#pragma once
#include <Arduino.h>

// ===========================================================================
//  Feed/dispense motor — H-bridge on AW9523B expander outputs P0_2 / P0_3
//  (external pulldown; both LOW = STOP). Current shunt PB_2; dispense-rotor
//  encoder PB_5 (1 pulse/rev, emitter P0_5).
//
//  NON-BLOCKING like the lid: run()/dispense() start and return; update() (from
//  loop()) counts encoder pulses and auto-stops on revs reached, hard timeout,
//  or jam (current spike). Characterize the rotor encoder high/low levels and
//  jam current on the bench before trusting dispense().
// ===========================================================================
namespace Feed {
    void begin();
    void update();             // call every loop()
    void stop();
    void run(bool dir);        // dir=true -> P0_2 high; false -> P0_3 high
    int  revCount();
}
void feedInit();
