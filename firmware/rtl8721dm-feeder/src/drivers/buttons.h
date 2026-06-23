#pragma once
#include <Arduino.h>

// ===========================================================================
//  Front-panel buttons — capacitive-touch keys presented as active-low GPIO
//  inputs (idle HIGH, press LOW) on the con1 ribbon (Docs/RTL8721DM_module_
//  pinout.txt): PA_0, PA_2, PA_4, PB_26. The icon<->pin mapping below is
//  TENTATIVE (the pinout doc marks these con1 lines "?"); press each physical
//  key and watch the emitted `button` events to confirm which pin is which.
//
//  Debounced + edge-detected in update() (call from loop()); each press emits a
//  `button` event and bumps a counter.
// ===========================================================================
namespace Buttons {
    enum Id { MEAL = 0, FEED, LID, LOCK, COUNT };   // PA_0, PA_2, PA_4, PB_26 (tentative)
    void begin();
    void update();
    bool pressed(Id b);     // current debounced state (true = pressed / LOW)
}
void buttonsInit();
