#pragma once
#include <Arduino.h>

// ===========================================================================
//  Feeder — scheduled + manual dispensing, the second headline behavior.
//
//  A "portion" is a configurable number of PARCELS (the auger's fixed-volume
//  chambers); manual `feeder.feed portions=K` and each schedule slot ask the
//  Feed driver for that many chute-confirmed parcels. The driver owns the
//  closed loop (chute confirmation + jam reverse-recovery); this module owns
//  the product policy: portion sizing, hopper warnings, the daily schedule,
//  and the `meal` analytics events that feed the dashboard.
//
//  Manual feed always works. The schedule is in-RAM for now (Preferences later)
//  and its tick is GATED until the `time` module supplies a wall clock — until
//  then timeNowMin() returns -1 and no slot fires. Enable with `feeder.auto v=1`.
// ===========================================================================
namespace Feeder {
    void update();     // pump from loop(): dispense completion + (gated) scheduler
}
void feederInit();     // register harness commands + state
