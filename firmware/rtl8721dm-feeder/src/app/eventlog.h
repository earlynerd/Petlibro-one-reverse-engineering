#pragma once
#include <Arduino.h>

// ===========================================================================
//  Event log — the data spine the dashboard's analytics are computed from.
// ===========================================================================
//  Every meaningful action emits a timestamped event here: pet visits,
//  dispenses, lid cycles, faults. The web layer serves these raw via
//  /api/events?since=<seq> and the dashboard derives all metrics client-side.
//
//  PHASE 0: backed by a small in-RAM ring (volatile, survives nothing). This
//  proves the API + dashboard tail end-to-end.
//  PHASE 0+: swap the backing store for a LittleFS journal in a carved upper-
//  flash region (wear-leveled, power-loss safe) — same append()/since() API.
//
//  `ts` is millis() for now; becomes a real RTC epoch once NTP/RTC lands.

struct Event {
    uint32_t seq;
    uint32_t ts;            // millis() for now; RTC epoch later
    char     type[16];      // e.g. "visit","dispense","lid","alert","boot"
    char     detail[48];    // short free-form payload (JSON-escaped on output)
};

void eventLogInit();
// Append an event. Returns its sequence number.
uint32_t eventLogAppend(const char* type, const String& detail);
// Latest assigned sequence number (0 == nothing logged yet).
uint32_t eventLogHeadSeq();
// Build a JSON array of events with seq > since (oldest-first).
void eventLogBuildJson(uint32_t since, String& out);
