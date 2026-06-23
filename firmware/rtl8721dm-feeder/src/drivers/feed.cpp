#include "drivers/feed.h"
#include "drivers/sensors.h"
#include "drivers/aw9523b.h"
#include "app/registry.h"
#include "app/eventlog.h"
#include "web/http_server.h"
#include <stdlib.h>

static bool     g_up        = false;
static int      g_dir       = 0;        // 0 stop, +1/-1 running
static uint32_t g_deadline  = 0;
static bool     g_counting  = false;
static int      g_target    = 0;
static int      g_count     = 0;
static bool     g_encHigh   = false;           // last rotor level (for digital edge detect)
static uint32_t g_jamMa     = 0;               // jam guard in mA (0 = disabled until calibrated)

static constexpr uint32_t MAX_RUN_MS = 60000;   // dispense can be long; 60 s safety backstop

void Feed::begin() { Aw9523::setOutput(0, 2, false); Aw9523::setOutput(0, 3, false); g_up = true; g_dir = 0; }
void Feed::stop()  { Aw9523::setOutput(0, 2, false); Aw9523::setOutput(0, 3, false); g_dir = 0; g_counting = false; }
void Feed::run(bool dir) {
    // Invariant: never run the auger without its sensor emitters powered —
    // dispense/rotor encoder (P0_5) and hopper+chute (P0_7). Refuse if power fails.
    bool e1 = Aw9523::emitter(1, true);   // P0_5 dispense / rotor encoder
    bool e2 = Aw9523::emitter(2, true);   // P0_7 hopper + chute
    if (!(e1 && e2)) { eventLogAppend("dispense", "refuse: sensor emitter power failed"); Feed::stop(); return; }
    if (dir) { Aw9523::setOutput(0, 3, false); Aw9523::setOutput(0, 2, true); }
    else     { Aw9523::setOutput(0, 2, false); Aw9523::setOutput(0, 3, true); }
    g_dir = dir ? 1 : -1;
}
int Feed::revCount() { return g_count; }

void Feed::update() {
    if (g_dir == 0) return;
    if ((int32_t)(millis() - g_deadline) >= 0) { eventLogAppend("dispense", String("stop: timeout revs=") + g_count); Feed::stop(); return; }
    if (g_jamMa && Sensors::feedCurrentMa() > g_jamMa) {
        eventLogAppend("dispense", String("JAM ") + Sensors::feedCurrentMa() + "mA revs=" + g_count); Feed::stop(); return;
    }
    // Count rotor edges whenever the auger is turning (run OR dispense). Only
    // dispense mode auto-stops at the revolution target.
    bool r = Sensors::rotor();
    if (r && !g_encHigh) {                           // rising edge = one revolution
        g_count++;
        if (g_counting && g_count >= g_target) { eventLogAppend("dispense", String("done revs=") + g_count); Feed::stop(); return; }
    }
    g_encHigh = r;
}

// ---- harness ----
static long qn(const String& q, const char* k, long def) { String v = httpGetParam(q, k); return v.length() ? strtol(v.c_str(), nullptr, 0) : def; }

static String stateJson() {
    String s = "{\"dir\":"; s += g_dir;
    s += ",\"count\":";      s += g_count;
    s += ",\"current_ma\":"; s += Sensors::feedCurrentMa();
    s += ",\"rotor\":";      s += Sensors::rotor() ? 1 : 0;
    s += "}";
    return s;
}

static String cmdRun(const String& q) {
    long dir = qn(q, "dir", 1); long ms = qn(q, "ms", 1000);
    if (ms < 0) ms = 0; if ((uint32_t)ms > MAX_RUN_MS) ms = MAX_RUN_MS;
    g_counting = false; g_count = 0; g_jamMa = (uint32_t)qn(q, "jam", 0);
    g_deadline = millis() + ms; Feed::run(dir != 0);
    g_encHigh = Sensors::rotor();               // baseline AFTER run() lit the emitter
    eventLogAppend("dispense", String("run dir=") + dir); return stateJson();
}
static String cmdStop(const String&) { Feed::stop(); return stateJson(); }

static String cmdDispense(const String& q) {
    long revs = qn(q, "revs", 1); long dir = qn(q, "dir", 1);
    long ms   = qn(q, "ms", 30000); if ((uint32_t)ms > MAX_RUN_MS) ms = MAX_RUN_MS;
    g_jamMa   = (uint32_t)qn(q, "jam", 0);
    g_target  = (int)revs; g_count = 0; g_counting = true;
    g_deadline = millis() + ms; Feed::run(dir != 0);
    g_encHigh = Sensors::rotor();               // baseline AFTER run() lit the emitter
    eventLogAppend("dispense", String("dispense revs=") + revs + " dir=" + dir);
    return stateJson();
}

static void stateFeed(String& out) { out += "\"feed\":"; out += stateJson(); }

void feedInit() {
    Feed::begin();
    regAddState(stateFeed);
    regAddCommand("feed.run",      cmdRun,      "dir:0/1,ms:int,jam:int",        "run feed motor, auto-stop after ms");
    regAddCommand("feed.stop",     cmdStop,     "",                              "stop the feed motor");
    regAddCommand("feed.dispense", cmdDispense, "revs:int,dir:0/1,ms:int,jam:int","count rotor pulses, stop at revs/jam/timeout");
}
