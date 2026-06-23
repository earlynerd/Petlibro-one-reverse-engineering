#pragma once
#include <Arduino.h>
#include <WiFi.h>

// ===========================================================================
//  Minimal HTTP layer over WiFiServer/WiFiClient.
// ===========================================================================
//  The AmebaD WiFiServer gives us raw TCP, so we parse the request line and
//  route ourselves. Routes:
//     GET  /                       -> dashboard page
//     GET  /api/state              -> live snapshot (registry state contributors)
//     GET  /api/commands           -> command manifest (auto-renders controls)
//     GET|POST /api/cmd?cmd=NAME... -> dispatch a command, return JSON result
//     GET  /api/events?since=N      -> event-log tail (oldest-first, seq > N)
//
//  Phase 0 services clients synchronously from loop(). Phase 2 moves this to a
//  low-priority FreeRTOS task so it can never stall the actuation loop.

// Handle one already-accepted client connection, then close it.
void httpHandleClient(WiFiClient& client);

// Extract a URL query parameter value (minimal x-www-form-urlencoded decode).
// Returns "" if absent. e.g. httpGetParam("cmd=echo&msg=hi%20there", "msg").
String httpGetParam(const String& query, const char* key);
