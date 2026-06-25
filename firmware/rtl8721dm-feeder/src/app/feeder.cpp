#include "app/feeder.h"
#include "drivers/feed.h"
#include "drivers/sensors.h"
#include "app/registry.h"
#include "app/eventlog.h"
#include "app/timekeeper.h"
#include "app/alerts.h"
#include "web/http_server.h"
#include <Preferences.h>
#include <stdlib.h>

// One parcel = one auger chamber = 1/12 cup (the smallest unit in the stock app).
// Canonical analytics unit is the parcel; cups are derived for human/dashboard use.
static constexpr int PARCELS_PER_CUP = 12;

// hundredths-of-a-cup, rounded, formatted "C.cc" (e.g. 2 parcels -> "0.17").
static String cupsStr(int parcels) {
    int h = (parcels * 100 + PARCELS_PER_CUP / 2) / PARCELS_PER_CUP;
    String s = String(h / 100); s += '.';
    int f = h % 100; if (f < 10) s += '0'; s += f;
    return s;
}

// ---- config (in-RAM; Preferences-backed later) ---------------------------
static int      g_parcelsPerPortion = 1;     // chambers per "portion"; 1 = 1/12 cup (stock default)
static bool     g_dir               = true;   // auger forward direction (Feed dir convention)
static uint32_t g_jamMa             = 0;       // jam current guard, mA (0 = off until calibrated). CURRENT is the
                                               // primary jam signal AND lets the classifier tell a hard JAM from a BRIDGE.
static uint32_t g_msPerParcel       = 15000;   // dispense timeout budget per parcel — must exceed one rev (~8 s+/rev)
// Photoelectric polarities — bench-confirmed 2026-06-24 (both are active-low break-beams):
//   hopper: food blocking the beam reads LOW; CLEAR (out of food) reads HIGH -> empty = HIGH(1).
//   chute:  CLEAR/idle reads HIGH; a falling kibble (or a sustained blockage) interrupts it LOW.
// g_chuteClearLvl holds the CLEAR/resting level. The parcel counter keys its (beam-restored)
// edge on this level, so a real blockage is the OTHER level: blocked == (chute != g_chuteClearLvl).
static bool     g_hopperEmptyLvl    = true;
static bool     g_chuteClearLvl     = true;    // chute level when CLEAR (active-low: idle HIGH); feeds the parcel counter
static uint32_t g_chuteDebMs        = 1000;    // per-parcel chute refractory (de-noise the kibble burst)
// jam-recovery params (owned here for persistence; pushed into Feed::setRecovery)
static uint32_t g_stallMs           = 0;       // time-based stall jam OFF by default (slow rotor)
static uint32_t g_reverseMs         = 400;
static int      g_maxTries          = 3;

// ---- schedule (in-RAM; tick GATED until `time` lands) --------------------
static constexpr int MAX_SLOTS = 8;
struct Slot { uint16_t min; uint8_t portions; bool on; };
static Slot g_slots[MAX_SLOTS];
static int  g_nSlots = 0;
static bool g_auto   = false;                  // scheduler enable (off during bench)

static int  g_lastFireMin = -1;

// ---- in-flight feed tracking ---------------------------------------------
static bool        g_feeding      = false;
static const char* g_src          = "manual";
static int         g_wantPortions = 0;
static int         g_wantParcels  = 0;
// last-meal summary (for state)
static int         g_lastPortions = 0;
static int         g_lastDelivered = 0;
static int         g_lastWant     = 0;
static const char* g_lastResult   = "none";
static uint32_t    g_lastTs       = 0;

static long qn(const String& q, const char* k, long def) {
    String v = httpGetParam(q, k);
    return v.length() ? strtol(v.c_str(), nullptr, 0) : def;
}

// ---- persistence (Preferences/DCT) ---------------------------------------
// All tunables + the schedule (packed blob: 4 B/slot, well under the 132 B cap).
static void saveFeeder() {
    Preferences p;
    if (!p.begin("feeder", false)) return;
    p.putInt ("ppp",   g_parcelsPerPortion);
    p.putBool("dir",   g_dir);
    p.putUInt("jam",   g_jamMa);
    p.putUInt("msp",   g_msPerParcel);
    p.putBool("hopE",  g_hopperEmptyLvl);
    p.putBool("chuB",  g_chuteClearLvl);
    p.putUInt("chuMs", g_chuteDebMs);
    p.putUInt("stall", g_stallMs);
    p.putUInt("rev",   g_reverseMs);
    p.putInt ("tries", g_maxTries);
    p.putBool("auto",  g_auto);
    uint8_t buf[MAX_SLOTS * 4];
    for (int i = 0; i < g_nSlots; i++) {
        buf[i*4]   = (uint8_t)(g_slots[i].min & 0xFF);
        buf[i*4+1] = (uint8_t)(g_slots[i].min >> 8);
        buf[i*4+2] = g_slots[i].portions;
        buf[i*4+3] = g_slots[i].on ? 1 : 0;
    }
    p.putInt  ("sn", g_nSlots);
    if (g_nSlots > 0) p.putBytes("slots", buf, g_nSlots * 4);
    p.end();
}
static void loadFeeder() {
    Preferences p;
    if (!p.begin("feeder", true)) return;
    g_parcelsPerPortion = p.getInt ("ppp",   g_parcelsPerPortion); if (g_parcelsPerPortion < 1) g_parcelsPerPortion = 1;
    g_dir             = p.getBool("dir",   g_dir);
    g_jamMa           = p.getUInt("jam",   g_jamMa);
    g_msPerParcel     = p.getUInt("msp",   g_msPerParcel);
    g_hopperEmptyLvl  = p.getBool("hopE",  g_hopperEmptyLvl);
    g_chuteClearLvl = p.getBool("chuB",  g_chuteClearLvl);
    g_chuteDebMs      = p.getUInt("chuMs", g_chuteDebMs);
    g_stallMs         = p.getUInt("stall", g_stallMs);
    g_reverseMs       = p.getUInt("rev",   g_reverseMs);
    g_maxTries        = p.getInt ("tries", g_maxTries);
    g_auto            = p.getBool("auto",  g_auto);
    int n = p.getInt("sn", 0); if (n < 0) n = 0; if (n > MAX_SLOTS) n = MAX_SLOTS;
    uint8_t buf[MAX_SLOTS * 4];
    size_t got = (n > 0) ? p.getBytes("slots", buf, sizeof buf) : 0;
    g_nSlots = 0;
    if (got >= (size_t)(n * 4)) {
        for (int i = 0; i < n; i++) {
            g_slots[i].min      = (uint16_t)(buf[i*4] | (buf[i*4+1] << 8));
            g_slots[i].portions = buf[i*4+2];
            g_slots[i].on       = buf[i*4+3] != 0;
            g_nSlots++;
        }
    }
    p.end();
}

// Start a dispense of `portions` (false if invalid/busy/refused). Dispenses
// regardless of hopper state — a low hopper is a warning, not a blocker.
static bool startFeed(int portions, const char* src) {
    if (portions <= 0 || Feed::busy()) return false;
    // Hopper-low is surfaced continuously by the level poll in update(); we still
    // dispense regardless (a low reading may still have food in the auger path).

    int      parcels = portions * g_parcelsPerPortion;
    uint32_t ms      = (uint32_t)parcels * g_msPerParcel;
    g_src = src; g_wantPortions = portions; g_wantParcels = parcels;
    Feed::dispense(parcels, g_dir, ms, g_jamMa);
    if (!Feed::busy()) { Alerts::raise("feed", "HW!", Alerts::PRIO_FAULT); return false; }  // emitter power failed
    g_feeding = true;
    return true;
}

static const char* feedResultStr(Feed::Result r) {
    switch (r) {
        case Feed::RES_DONE:    return "done";
        case Feed::RES_TIMEOUT: return "timeout";
        case Feed::RES_JAM:     return "jam";
        case Feed::RES_ABORTED: return "aborted";
        default:                return "running";
    }
}

// Classify an under-delivery from the post-dispense signal set
// {rotor turned?, peak current, chute blocked?, hopper full?}. Returns a short
// code, and sets the display msg + alert priority. See DECISIONS for the table.
// NOTE: distinguishing a hard JAM from a soft BRIDGE needs a calibrated jam
// current (g_jamMa>0); uncalibrated, a not-turning/clear/full stall reads BRIDGE.
static const char* diagnose(int revs, uint8_t& prio, const char*& msg) {
    bool turned       = revs > 0;
    bool chuteBlocked = (Sensors::chute()  != g_chuteClearLvl);   // active-low: blocked = NOT at the clear/resting level
    bool hopperFull   = (Sensors::hopper() != g_hopperEmptyLvl);
    bool curKnown     = g_jamMa > 0;
    bool loaded       = curKnown && (uint32_t)Feed::peakCurrentMa() > g_jamMa;

    prio = Alerts::PRIO_FAULT;
    if (chuteBlocked) {
        if (!turned) { msg = "FULL";  return "overfill"; }   // bowl full, food backed up the chute
        msg = "CHUTE"; return "chute-block";                  // output obstructed, still turning
    }
    if (!turned) {                                            // auger not rotating
        if (loaded)      { msg = "JAM";    return "jam"; }    // hard obstruction (high current)
        if (hopperFull)  { msg = "BRIDGE"; return "bridge"; } // soft stall under full hopper = bridge/rathole
        prio = Alerts::PRIO_WARN; msg = "EMPTY"; return "empty";
    }
    // turned but did not deliver:
    if (hopperFull) { msg = "BRIDGE"; return "bridge"; }      // spinning in a void = bridge/rathole
    prio = Alerts::PRIO_WARN; msg = "EMPTY"; return "empty";  // out of food
}

void Feeder::update() {
    // Completion: we kicked off a dispense and the Feed driver just went idle.
    if (g_feeding && !Feed::busy()) {
        g_feeding = false;
        Feed::Result r = Feed::result();
        int delivered  = Feed::parcelCount();
        int revs       = Feed::revCount();

        g_lastPortions  = g_wantPortions;
        g_lastDelivered = delivered;
        g_lastWant      = g_wantParcels;
        g_lastResult    = feedResultStr(r);
        g_lastTs        = millis();

        // meal event: source, delivered/wanted parcels, volume in cups, result.
        eventLogAppend("meal", String(g_src) + " " + delivered + "/" + g_wantParcels + "p " + cupsStr(delivered) + "cup " + g_lastResult);

        if (delivered >= g_wantParcels) {
            Alerts::clear("feed");                 // clean delivery resolves the dispense fault
        } else {
            uint8_t prio; const char* msg;
            const char* code = diagnose(revs, prio, msg);
            Alerts::raise("feed", msg, prio);
            // diagnostic snapshot for bench analysis (peak current / rotor / hopper / chute).
            eventLogAppend("diag", String(code) + " cur=" + Feed::peakCurrentMa()
                           + " rev=" + revs + " d=" + delivered + "/" + g_wantParcels
                           + " hop=" + (Sensors::hopper() ? 1 : 0) + " ch=" + (Sensors::chute() ? 1 : 0));
        }
    }

    // Hopper food-level alert as a continuous condition (clears when refilled).
    if (Sensors::hopper() == g_hopperEmptyLvl) Alerts::raise("hopper", "LOW", Alerts::PRIO_WARN);
    else                                       Alerts::clear("hopper");

    // Scheduler tick — GATED until `time` provides a wall clock.
    if (g_auto && !g_feeding && !Feed::busy()) {
        int nowMin = Timekeeper::minOfDayLocal();
        if (nowMin >= 0 && nowMin != g_lastFireMin) {
            for (int i = 0; i < g_nSlots; i++) {
                if (g_slots[i].on && g_slots[i].min == nowMin) {
                    g_lastFireMin = nowMin;
                    startFeed(g_slots[i].portions, "sched");
                    break;
                }
            }
        }
    }
}

// ---- harness -------------------------------------------------------------
static String cfgJson() {
    String s = "{\"parcels_per_portion\":"; s += g_parcelsPerPortion;
    s += ",\"dir\":";            s += g_dir ? 1 : 0;
    s += ",\"jam_ma\":";         s += g_jamMa;
    s += ",\"ms_per_parcel\":";  s += g_msPerParcel;
    s += ",\"hopper_empty\":";   s += g_hopperEmptyLvl ? 1 : 0;
    s += ",\"chute_clear\":";    s += g_chuteClearLvl ? 1 : 0;
    s += ",\"chute_ms\":";       s += g_chuteDebMs;
    s += ",\"stall_ms\":";       s += g_stallMs;
    s += ",\"reverse_ms\":";     s += g_reverseMs;
    s += ",\"jam_tries\":";      s += g_maxTries;
    s += "}";
    return s;
}

static String cmdFeed(const String& q) {
    long p = qn(q, "portions", 1);
    bool ok = startFeed((int)p, "manual");
    String s = "{\"ok\":"; s += ok ? "true" : "false";
    s += ",\"portions\":"; s += p;
    s += ",\"busy\":";     s += Feed::busy() ? "true" : "false"; s += "}";
    return s;
}
static String cmdStop(const String&) { Feed::stop(); g_feeding = false; return "{\"ok\":true}"; }

static String cmdCfg(const String& q) {
    g_parcelsPerPortion = (int)qn(q, "parcels", g_parcelsPerPortion); if (g_parcelsPerPortion < 1) g_parcelsPerPortion = 1;
    g_dir            = qn(q, "dir", g_dir ? 1 : 0) != 0;
    g_jamMa          = (uint32_t)qn(q, "jam", g_jamMa);
    g_msPerParcel    = (uint32_t)qn(q, "ms", g_msPerParcel);
    g_hopperEmptyLvl = qn(q, "hopper_empty", g_hopperEmptyLvl ? 1 : 0) != 0;
    g_chuteClearLvl  = qn(q, "chute_clear", g_chuteClearLvl ? 1 : 0) != 0;   // chute CLEAR/idle level (active-low HW: 1)
    g_chuteDebMs     = (uint32_t)qn(q, "chute_ms", g_chuteDebMs);
    Feed::setChuteDebounce(g_chuteDebMs, g_chuteClearLvl);   // keep the driver's parcel counter in sync
    // recovery tuning (owned here, pushed to Feed). stall_ms default 0 (OFF) — the
    // slow rotor makes time-based stall unreliable; current is the primary jam signal.
    g_stallMs   = (uint32_t)qn(q, "stall_ms",   g_stallMs);
    g_reverseMs = (uint32_t)qn(q, "reverse_ms", g_reverseMs);
    g_maxTries  = (int)      qn(q, "jam_tries",  g_maxTries);
    Feed::setRecovery(g_stallMs, g_reverseMs, g_maxTries);
    saveFeeder();
    return cfgJson();
}

static String cmdAuto(const String& q) {
    g_auto = qn(q, "v", 1) != 0; g_lastFireMin = -1;
    saveFeeder();
    eventLogAppend("feeder", g_auto ? "auto on" : "auto off");
    String s = "{\"auto\":"; s += g_auto ? "true" : "false"; s += "}";
    return s;
}
static String cmdSchedAdd(const String& q) {
    long hh = qn(q, "h", 0), mm = qn(q, "m", 0), p = qn(q, "portions", 1);
    bool ok = false;
    if (g_nSlots < MAX_SLOTS && hh >= 0 && hh < 24 && mm >= 0 && mm < 60 && p > 0) {
        g_slots[g_nSlots].min      = (uint16_t)(hh * 60 + mm);
        g_slots[g_nSlots].portions = (uint8_t)p;
        g_slots[g_nSlots].on       = true;
        g_nSlots++; ok = true;
    }
    if (ok) saveFeeder();
    String s = "{\"ok\":"; s += ok ? "true" : "false"; s += ",\"n\":"; s += g_nSlots; s += "}";
    return s;
}
static String cmdSchedList(const String&) {
    String s = "{\"slots\":[";
    for (int i = 0; i < g_nSlots; i++) {
        if (i) s += ',';
        s += "{\"h\":";          s += g_slots[i].min / 60;
        s += ",\"m\":";          s += g_slots[i].min % 60;
        s += ",\"portions\":";   s += g_slots[i].portions;
        s += ",\"on\":";         s += g_slots[i].on ? "true" : "false"; s += "}";
    }
    s += "]}";
    return s;
}
static String cmdSchedClear(const String&) { g_nSlots = 0; saveFeeder(); return "{\"ok\":true,\"n\":0}"; }

static void stateFeeder(String& out) {
    out += "\"feeder\":{";
    out += "\"auto\":";                out += g_auto ? "true" : "false";
    out += ",\"feeding\":";            out += g_feeding ? "true" : "false";
    out += ",\"time_known\":";         out += (Timekeeper::minOfDayLocal() >= 0) ? "true" : "false";
    out += ",\"n_slots\":";            out += g_nSlots;
    out += ",\"parcels_per_cup\":";    out += PARCELS_PER_CUP;
    out += ",\"parcels_per_portion\":";out += g_parcelsPerPortion;
    out += ",\"dir\":";                out += g_dir ? 1 : 0;
    out += ",\"jam_ma\":";             out += g_jamMa;
    out += ",\"hopper_empty\":";       out += g_hopperEmptyLvl ? 1 : 0;
    out += ",\"hopper_low\":";         out += (Sensors::hopper() == g_hopperEmptyLvl) ? "true" : "false";
    out += ",\"chute_blocked\":";      out += (Sensors::chute()  != g_chuteClearLvl) ? "true" : "false";
    out += ",\"last_meal\":{\"portions\":"; out += g_lastPortions;
    out += ",\"delivered\":";          out += g_lastDelivered;
    out += ",\"wanted\":";             out += g_lastWant;
    out += ",\"cups\":";               out += cupsStr(g_lastDelivered);
    out += ",\"result\":\"";           out += g_lastResult; out += "\"";
    out += ",\"age_ms\":";             out += (g_lastTs ? (millis() - g_lastTs) : 0);
    out += "}}";
}

void feederInit() {
    loadFeeder();                                              // restore tunables + schedule + auto
    Feed::setChuteDebounce(g_chuteDebMs, g_chuteClearLvl);   // push loaded config into the driver
    Feed::setRecovery(g_stallMs, g_reverseMs, g_maxTries);
    regAddState(stateFeeder);
    regAddCommand("feeder.feed",        cmdFeed,       "portions:int",   "dispense N portions now (chute-confirmed)");
    regAddCommand("feeder.stop",        cmdStop,       "",               "abort the current dispense");
    regAddCommand("feeder.cfg",         cmdCfg,        "parcels:int,dir:0/1,jam:int,ms:int,hopper_empty:0/1,chute_clear:0/1,chute_ms:int,stall_ms:int,reverse_ms:int,jam_tries:int", "tune portion size / dir / guards / chute debounce / jam recovery");
    regAddCommand("feeder.auto",        cmdAuto,       "v:0/1",          "enable the (time-gated) schedule");
    regAddCommand("feeder.sched.add",   cmdSchedAdd,   "h:int,m:int,portions:int", "add a daily feed slot");
    regAddCommand("feeder.sched.list",  cmdSchedList,  "",               "list schedule slots");
    regAddCommand("feeder.sched.clear", cmdSchedClear, "",               "clear all schedule slots");
}
