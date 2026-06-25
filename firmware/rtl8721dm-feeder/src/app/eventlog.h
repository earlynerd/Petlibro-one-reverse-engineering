#pragma once
#include <Arduino.h>

// ===========================================================================
//  Event log — the data spine the dashboard's analytics are computed from.
// ===========================================================================
//  Every meaningful action emits a timestamped event here: pet visits,
//  dispenses, lid cycles, faults. The web layer serves these raw via
//  /api/events?since=<seq> and the dashboard derives all metrics client-side.
//
//  BACKING STORE: a LittleFS journal in a carved flash region (wear-leveled,
//  power-loss safe) — durable across reboots, the source for analytics. If the
//  flash region can't be validated/mounted, it falls back to a small in-RAM
//  ring (volatile) so the API still works. Same append()/since() API either way.
//
//  `ts` is the RTC epoch once NTP/RTC has synced (see eventLogSetClock), else
//  millis() (small values < 1e9, distinguishable from epochs).

struct Event {
    uint32_t seq;
    uint32_t ts;            // millis() for now; RTC epoch later
    char     type[16];      // e.g. "visit","dispense","lid","alert","boot"
    char     detail[48];    // short free-form payload (JSON-escaped on output)
};

void eventLogInit();
// Install a clock returning the current unix epoch (UTC seconds), or 0 if time
// is not yet known. While unset or returning 0, timestamps fall back to millis()
// (so pre-sync events carry small boot-relative values; epochs are >1e9).
void eventLogSetClock(uint32_t (*fn)());
// Append an event. Returns its sequence number.
uint32_t eventLogAppend(const char* type, const String& detail);
// Latest assigned sequence number (0 == nothing logged yet).
uint32_t eventLogHeadSeq();
// Build a JSON array of events with seq > since (oldest-first), capped to the
// most-recent slice so the response stays bounded.
void eventLogBuildJson(uint32_t since, String& out);

// Backing-store status + housekeeping (for the events.* harness commands).
void eventLogStatsJson(String& out);   // {fs, records, head_seq, ...}
void eventLogClear();                  // drop stored events (seq counter keeps advancing)
