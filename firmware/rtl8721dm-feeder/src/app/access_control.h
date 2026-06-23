#pragma once
#include <Arduino.h>

// ===========================================================================
//  Access control — the headline behavior. Watches the RFID tag-ready IRQ
//  (PA_16); on a tag, reads its FDX-B id, checks the whitelist, and drives the
//  lid open for authorized tags. Tag leaves -> grace period -> lid closes.
//  Emits `visit` events (the first real analytics data) on every arrival and
//  departure.
//
//  DISABLED by default (so it never grabs the lid during bench work). Enable
//  with `acl.enable v=1`, which also powers the RFID rail. Whitelist is in-RAM
//  for now (Preferences-backed config arrives with the config layer).
// ===========================================================================
namespace AccessControl {
    void update();     // pump from loop()
}
void aclInit();        // register harness commands + state
