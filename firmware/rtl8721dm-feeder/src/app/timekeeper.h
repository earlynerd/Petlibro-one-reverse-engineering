#pragma once
#include <Arduino.h>

// ===========================================================================
//  Timekeeper — wall clock for the feeder (NTP -> RTC).
//
//  The on-chip RTC is the canonical, free-running clock: SNTP (async, hourly)
//  fetches UTC from pool.ntp.org and writes it to the RTC on each fix; the rest
//  of the firmware reads epoch/local time from the RTC. Until the first fix,
//  synced() is false — minOfDayLocal() returns -1 (so the feeder schedule stays
//  gated) and event timestamps fall back to millis().
//
//  Timezone is a config offset in minutes from UTC (e.g. -480 = PST, -300 = EST,
//  330 = IST), set via `time.tz`. No DST handling — set the offset you want.
// ===========================================================================
namespace Timekeeper {
    void     update();          // pump from loop(): pick up new SNTP fixes -> RTC
    bool     synced();          // have we ever gotten a valid time?
    uint32_t epoch();           // current UTC unix seconds (only meaningful if synced)
    uint32_t epochOrZero();     // epoch() if synced, else 0 (for eventLogSetClock)
    int      minOfDayLocal();   // local minutes since midnight, or -1 if not synced
}
void timeInit();                // rtc_init + sntp_init (WiFi must be up) + harness
