#pragma once
#include <Arduino.h>

// ===========================================================================
//  Config — management commands for the persisted settings.
//
//  Each module owns its own Preferences/DCT namespace (acl, feeder, time) and
//  loads on boot / saves on change. This module just adds cross-cutting
//  housekeeping: a factory wipe and a free-entry report. The DCT region self-
//  places at the top of flash (SDK-managed, CRC'd + backed-up) — distinct from
//  the LittleFS event-log region that gets pinned above the KM4 image later.
// ===========================================================================
void configInit();   // register config.wipe / config.info
