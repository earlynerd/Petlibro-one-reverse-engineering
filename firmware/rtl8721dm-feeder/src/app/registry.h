#pragma once
#include <Arduino.h>

// ===========================================================================
//  Registry — the spine of the web-first bench harness.
// ===========================================================================
//  Every subsystem (driver or app module) plugs into the harness through two
//  tables instead of touching the web layer directly:
//
//    * STATE contributors  -> each appends its own "name":{...} object to the
//      /api/state snapshot. The dashboard polls /api/state and renders it.
//
//    * COMMAND handlers     -> each is a named action (e.g. "lid.open",
//      "feed.dispense"). They are listed in /api/commands (so the dashboard
//      can auto-render a control per command) and invoked via /api/cmd.
//
//  Phase 1 adds a driver == implement it + register a state fn + some commands;
//  its telemetry and controls then appear in the browser with no UI edits.

// Append a JSON object fragment of the form:  "name":{ ... }
// (no leading/trailing comma — the registry joins fragments itself)
typedef void (*StateFn)(String& out);

// Receives the raw URL query string (e.g. "cmd=feed.dispense&revs=3").
// Returns a JSON value/object string that becomes the command result.
typedef String (*CmdFn)(const String& query);

void regAddState(StateFn fn);
void regAddCommand(const char* name, CmdFn fn, const char* argspec, const char* help);

// Build "{...}" of all state contributors.
void regBuildState(String& out);
// Build the command manifest array: [{"name":..,"args":..,"help":..}, ...]
void regBuildCommands(String& out);
// Dispatch a command by name; returns false if no such command.
bool regDispatch(const String& name, const String& query, String& out);
