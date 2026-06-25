#include "drivers/feed.h"
#include "drivers/sensors.h"
#include "drivers/aw9523b.h"
#include "app/registry.h"
#include "app/eventlog.h"
#include "web/http_server.h"
#include <stdlib.h>

// ---- running state -------------------------------------------------------
static bool     g_up        = false;
static int      g_dir       = 0;        // H-bridge: 0 stop, +1/-1 running
static uint8_t  g_mode      = 0;        // 0 idle, 1 timed run, 2 dispense
static uint8_t  g_phase     = 0;        // dispense sub-phase: 0 forward, 1 reverse-recovery
static bool     g_fwdDir    = true;     // requested forward direction for this op
static uint32_t g_deadline  = 0;        // overall timeout
static uint32_t g_phaseUntil = 0;       // reverse-recovery window end

// ---- counters ------------------------------------------------------------
static int      g_target    = 0;        // parcels to deliver (dispense)
static int      g_parcels   = 0;        // chute-confirmed parcels delivered
static int      g_count     = 0;        // rotor revolutions (chambers turned)
static bool     g_encHigh   = false;    // rotor edge baseline
static bool     g_chuteHigh = false;    // chute edge baseline (last raw level)
static uint32_t g_lastAdv   = 0;        // last time the rotor advanced (stall ref)

// ---- chute parcel debounce -----------------------------------------------
static uint32_t g_chuteDebMs = 1000;    // refractory after a counted parcel (absorbs the kibble burst)
static bool     g_chuteActive = true;   // chute level meaning "beam broken" (a kibble is crossing)
static uint32_t g_chuteLock  = 0;       // ignore chute until this time

// ---- guards / recovery (bench-tunable) -----------------------------------
static uint32_t g_jamMa     = 0;        // PRIMARY jam guard: current, mA (0 = disabled until calibrated)
static uint32_t g_stallMs   = 0;        // backstop: no rotor advance this long => jam. 0 = OFF (default):
                                        // this rotor pulses ~8 s+/rev, so a short window false-trips.
static uint32_t g_reverseMs = 400;      // back off this long to clear a jam
static int      g_maxTries  = 3;        // reverse attempts before giving up
static int      g_jamTries  = 0;
static uint32_t g_peakMa    = 0;        // highest current seen this attempt (for the app classifier)

static Feed::Result g_result = Feed::RES_NONE;

static constexpr uint32_t MAX_RUN_MS = 60000;   // hard safety backstop

// --------------------------------------------------------------------------
void Feed::begin() { Aw9523::setOutput(0, 2, false); Aw9523::setOutput(0, 3, false); g_up = true; g_dir = 0; g_mode = 0; }
void Feed::stop()  { Aw9523::setOutput(0, 2, false); Aw9523::setOutput(0, 3, false); g_dir = 0; g_mode = 0; g_phase = 0; }

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

void Feed::runFor(bool dir, uint32_t ms, uint32_t jamMa) {
    if (ms > MAX_RUN_MS) ms = MAX_RUN_MS;
    uint32_t now = millis();
    g_mode = 1; g_phase = 0; g_fwdDir = dir;
    g_target = 0; g_parcels = 0; g_count = 0; g_jamTries = 0; g_peakMa = 0; g_chuteLock = 0;
    g_jamMa = jamMa; g_result = RES_RUNNING;
    g_deadline = now + ms; g_lastAdv = now;
    Feed::run(dir);
    g_encHigh = Sensors::rotor(); g_chuteHigh = Sensors::chute();   // baselines AFTER run() lit the emitters
}

void Feed::dispense(int parcels, bool dir, uint32_t ms, uint32_t jamMa) {
    if (ms > MAX_RUN_MS) ms = MAX_RUN_MS;
    uint32_t now = millis();
    g_mode = 2; g_phase = 0; g_fwdDir = dir;
    g_target = parcels; g_parcels = 0; g_count = 0; g_jamTries = 0; g_peakMa = 0; g_chuteLock = 0;
    g_jamMa = jamMa; g_result = RES_RUNNING;
    g_deadline = now + ms; g_lastAdv = now;
    Feed::run(dir);
    g_encHigh = Sensors::rotor(); g_chuteHigh = Sensors::chute();
}

void Feed::setRecovery(uint32_t stallMs, uint32_t reverseMs, int maxTries) {
    g_stallMs = stallMs; g_reverseMs = reverseMs; g_maxTries = maxTries;
}

void Feed::setChuteDebounce(uint32_t refractoryMs, bool activeLevel) {
    g_chuteDebMs = refractoryMs; g_chuteActive = activeLevel;
}

bool        Feed::busy()          { return g_dir != 0; }
Feed::Result Feed::result()       { return g_result; }
int         Feed::parcelCount()   { return g_parcels; }
int         Feed::revCount()      { return g_count; }
int         Feed::peakCurrentMa() { return (int)g_peakMa; }

void Feed::update() {
    if (g_dir == 0) return;
    uint32_t now = millis();
    uint32_t ma = Sensors::feedCurrentMa();
    if (ma > g_peakMa) g_peakMa = ma;             // track peak for the app classifier

    // Overall timeout backstop (both modes). A timed run reaching its window is a
    // normal completion; a dispense reaching it without the parcel count is a timeout.
    if ((int32_t)(now - g_deadline) >= 0) {
        if (g_mode == 2) { g_result = RES_TIMEOUT; eventLogAppend("dispense", String("timeout parcels=") + g_parcels + "/" + g_target + " revs=" + g_count); }
        else             { g_result = RES_DONE;    eventLogAppend("dispense", String("run end revs=") + g_count); }
        Feed::stop(); return;
    }

    // Rotor revolutions = chambers turned (volumetric + stall reference).
    bool r = Sensors::rotor();
    if (r && !g_encHigh) { g_count++; g_lastAdv = now; }
    g_encHigh = r;

    // Reverse-recovery sub-phase: back off, don't count or re-trip, then resume forward.
    if (g_phase == 1) {
        if ((int32_t)(now - g_phaseUntil) >= 0) {
            g_phase = 0; Feed::run(g_fwdDir);
            g_encHigh = Sensors::rotor(); g_chuteHigh = Sensors::chute();
            g_lastAdv = now;
        }
        return;
    }

    // Chute-confirmed delivery. One parcel = the leading beam-break edge; the
    // refractory window then absorbs the rest of that parcel's noisy kibble burst
    // (and real parcels are seconds apart, so it never merges two).
    bool c = Sensors::chute();
    if (g_mode == 2 && c == g_chuteActive && g_chuteHigh != g_chuteActive    // idle -> beam broken
        && (int32_t)(now - g_chuteLock) >= 0) {                              // outside the refractory
        g_parcels++;
        g_chuteLock = now + g_chuteDebMs;
        if (g_parcels >= g_target) { g_result = RES_DONE; eventLogAppend("dispense", String("done parcels=") + g_parcels + " revs=" + g_count); Feed::stop(); return; }
    }
    g_chuteHigh = c;

    // Jam handling. Current is primary; the time-based stall is an optional
    // backstop (g_stallMs==0 disables it — default, since this rotor is slow).
    if (g_mode == 2) {
        bool jamI  = g_jamMa && ma > g_jamMa;
        bool stall = g_stallMs && (int32_t)(now - g_lastAdv) > (int32_t)g_stallMs;
        if (jamI || stall) {
            if (g_jamTries >= g_maxTries) {
                g_result = RES_JAM;
                eventLogAppend("dispense", String("JAM give-up ") + (jamI ? "cur" : "stall") + " parcels=" + g_parcels + " revs=" + g_count);
                Feed::stop(); return;
            }
            g_jamTries++;
            eventLogAppend("dispense", String("jam recover #") + g_jamTries + " " + (jamI ? "cur" : "stall"));
            g_phase = 1; g_phaseUntil = now + g_reverseMs;
            Feed::run(!g_fwdDir);                 // reverse to clear the chamber
            g_encHigh = Sensors::rotor();
            g_lastAdv = now;                      // reset stall clock for the reverse move
        }
    } else {  // timed run: current-jam stop only (no reverse-recovery)
        if (g_jamMa && ma > g_jamMa) {
            g_result = RES_JAM;
            eventLogAppend("dispense", String("JAM ") + ma + "mA revs=" + g_count);
            Feed::stop();
        }
    }
}

// ---- harness -------------------------------------------------------------
static long qn(const String& q, const char* k, long def) { String v = httpGetParam(q, k); return v.length() ? strtol(v.c_str(), nullptr, 0) : def; }

static const char* resultStr(Feed::Result r) {
    switch (r) {
        case Feed::RES_RUNNING: return "running";
        case Feed::RES_DONE:    return "done";
        case Feed::RES_TIMEOUT: return "timeout";
        case Feed::RES_JAM:     return "jam";
        case Feed::RES_ABORTED: return "aborted";
        default:                return "none";
    }
}

static String stateJson() {
    String s = "{\"dir\":";  s += g_dir;
    s += ",\"mode\":";       s += g_mode;
    s += ",\"parcels\":";    s += g_parcels;
    s += ",\"target\":";     s += g_target;
    s += ",\"revs\":";       s += g_count;
    s += ",\"jam_tries\":";  s += g_jamTries;
    s += ",\"result\":\"";   s += resultStr(g_result); s += "\"";
    s += ",\"current_ma\":"; s += Sensors::feedCurrentMa();
    s += ",\"peak_ma\":";    s += g_peakMa;
    s += ",\"rotor\":";      s += Sensors::rotor() ? 1 : 0;
    s += ",\"chute\":";      s += Sensors::chute() ? 1 : 0;
    s += "}";
    return s;
}

static String cmdRun(const String& q) {
    long dir = qn(q, "dir", 1); long ms = qn(q, "ms", 1000); if (ms < 0) ms = 0;
    Feed::runFor(dir != 0, (uint32_t)ms, (uint32_t)qn(q, "jam", 0));
    eventLogAppend("dispense", String("run dir=") + dir);
    return stateJson();
}
static String cmdStop(const String&) { Feed::stop(); g_result = Feed::RES_ABORTED; return stateJson(); }

static String cmdDispense(const String& q) {
    long parcels = qn(q, "parcels", qn(q, "revs", 1));   // accept legacy revs= alias
    long dir = qn(q, "dir", 1); long ms = qn(q, "ms", 30000); if (ms < 0) ms = 0;
    Feed::dispense((int)parcels, dir != 0, (uint32_t)ms, (uint32_t)qn(q, "jam", 0));
    eventLogAppend("dispense", String("dispense parcels=") + parcels + " dir=" + dir);
    return stateJson();
}

static void stateFeed(String& out) { out += "\"feed\":"; out += stateJson(); }

void feedInit() {
    Feed::begin();
    regAddState(stateFeed);
    regAddCommand("feed.run",      cmdRun,      "dir:0/1,ms:int,jam:int",            "run feed motor, auto-stop after ms");
    regAddCommand("feed.stop",     cmdStop,     "",                                  "stop the feed motor");
    regAddCommand("feed.dispense", cmdDispense, "parcels:int,dir:0/1,ms:int,jam:int","deliver N chute-confirmed parcels (jam reverse-recovery)");
}
