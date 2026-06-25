#pragma once
#include <Arduino.h>

// ===========================================================================
//  Alerts — the dot-matrix panel as a fault surface.
//
//  Producers (feeder, lid, …) push named conditions; this module arbitrates by
//  priority and renders the most urgent one to the display (static text for
//  short messages, a marquee for longer ones), clearing the panel to dark when
//  nothing is active. The producer owns resolution: it calls clear() when its
//  condition goes away (or sets a ttl for transient ones). raise() also writes
//  the event log, so a fault is one call — live surface + history together.
//
//  Messages render through the 5x7 font (uppercase A-Z, 0-9, . - : !); keep
//  them short and in that charset. <=4 chars show static, longer ones scroll.
// ===========================================================================
namespace Alerts {
    enum { PRIO_INFO = 1, PRIO_WARN = 2, PRIO_FAULT = 3 };

    // Raise or refresh a condition. Re-raising the same key updates it in place
    // (no duplicate, no log spam). ttlMs=0 => sticky until clear(); else expires.
    void raise(const char* key, const char* msg, uint8_t prio, uint32_t ttlMs = 0);
    void clear(const char* key);
    void update();     // pump from loop(): expire, arbitrate, render
}
void alertsInit();     // register harness commands + state
