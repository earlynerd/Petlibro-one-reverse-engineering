#pragma once
#include <Arduino.h>

// ===========================================================================
//  Feed/dispense motor — H-bridge on AW9523B expander outputs P0_2 / P0_3
//  (external pulldown; both LOW = STOP). Current shunt PB_2; dispense-rotor
//  encoder PB_5 (1 pulse/rev, emitter P0_5); chute drop detector PA_17
//  (emitter P0_7).
//
//  The auger is a chambered, positive-displacement mechanism: each rotor
//  revolution ejects one fixed-volume PARCEL. So dispense() targets a number
//  of CHUTE-CONFIRMED parcels — food is only counted as delivered when the
//  chute beam sees it fall. The rotor encoder is the volumetric/jam reference.
//
//  NON-BLOCKING like the lid: runFor()/dispense() start and return; update()
//  (from loop()) does the closed loop and auto-stops. When it ends, busy()
//  goes false and result() reports the outcome — that's how app/feeder learns
//  a dispense finished (and whether it jammed) without duplicating this logic.
//
//  Robustness baked into dispense():
//    * Chute-confirmed counting — turning an empty chamber (hopper low/empty)
//      never counts as delivered; it just runs to the timeout. dispense() is
//      attempted regardless of hopper state.
//    * Jam handling — CURRENT is the primary jam signal (set jamMa). A
//      time-based rotor stall is a secondary backstop and is OFF by default,
//      because this rotor pulses slowly (~8 s+/rev) so a short stall window
//      would false-trip a healthy dispense; only enable it (setRecovery
//      stallMs) with a window well above the real inter-pulse time. On a jam:
//      brief reverse to clear, resume forward; after maxTries -> RES_JAM.
//    * peakCurrentMa() records the highest current seen during the attempt —
//      the app's jam/overfill-vs-bridge classifier reads it after completion.
//  Characterize the rotor/chute levels and jam current on the bench before
//  trusting dispense().
// ===========================================================================
namespace Feed {
    enum Result : uint8_t { RES_NONE = 0, RES_RUNNING, RES_DONE, RES_TIMEOUT, RES_JAM, RES_ABORTED };

    void begin();
    void update();             // call every loop()
    void stop();
    void run(bool dir);        // raw: dir=true -> P0_2 high; false -> P0_3 high (no auto-stop)

    // Open-loop timed run: drive for `ms`, auto-stop on timeout/current-jam. Bench use.
    void runFor(bool dir, uint32_t ms, uint32_t jamMa);
    // Closed-loop dispense: deliver `parcels` chute-confirmed drops, or jam/timeout.
    // Non-blocking — kicks off and returns; poll busy()/result() (pumped by update()).
    void dispense(int parcels, bool dir, uint32_t ms, uint32_t jamMa);

    // Tune jam recovery: rotor-stall window, reverse-pulse length, max retries.
    void setRecovery(uint32_t stallMs, uint32_t reverseMs, int maxTries);

    // Chute parcel detection: count one parcel on the leading beam-break edge
    // (chute == activeLevel), then ignore the chute for `refractoryMs` — kibbles
    // flicker the beam many times as one parcel falls, and real parcels are
    // seconds apart, so a ~1 s refractory de-noises without merging parcels.
    void setChuteDebounce(uint32_t refractoryMs, bool activeLevel);

    bool   busy();             // true while the motor is running
    Result result();           // outcome of the last completed run/dispense
    int    parcelCount();      // chute-confirmed parcels delivered in the last dispense
    int    revCount();         // rotor pulses (chambers turned) since the last start
    int    peakCurrentMa();    // highest feed-motor current seen during the last run/dispense
}
void feedInit();
