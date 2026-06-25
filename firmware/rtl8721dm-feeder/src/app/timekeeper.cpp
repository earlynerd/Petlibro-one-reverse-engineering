#include "app/timekeeper.h"
#include "app/registry.h"
#include "app/eventlog.h"
#include "web/http_server.h"
#include "rtc_api.h"            // mbed RTC: rtc_init / rtc_read / rtc_write / rtc_isenabled
#include <Preferences.h>
#include <stdlib.h>

// SNTP (Realtek lwIP app, linked in lib_arduino.a). 32-bit time build, so the
// non-TIME64 signatures apply. Declared here to avoid depending on sntp.h's path.
extern "C" {
    void sntp_init(void);
    void sntp_stop(void);
    void sntp_get_lasttime(long *sec, long *usec, unsigned int *tick);
}

static bool         g_synced       = false;   // do we have a usable wall clock (from any source)?
static const char*  g_source       = "none";  // provenance: none / rtc / flash / ntp / manual
static unsigned int g_lastTick     = 0;        // SNTP update tick we last consumed
static int          g_tzOffsetMin  = 0;        // minutes from UTC (e.g. -480 = PST)
static uint32_t     g_lastSyncEpoch = 0;
static uint32_t     g_lastPersistMs = 0;       // last flash persist of the running clock

static constexpr uint32_t SANE_EPOCH = 1700000000UL;   // ~2023-11-14; below this the clock isn't really set
static constexpr uint32_t PERSIST_MS = 3600000UL;      // re-persist the running clock ~hourly

// DST without an on-device tz database: the control panel computes the upcoming
// offset transitions for the browser's zone and pushes them via `time.dst`. We
// store the FUTURE transitions and apply each when its UTC moment arrives —
// self-healing, since one missed while powered off is applied on the next check.
static constexpr int MAX_DST = 4;
struct DstTrans { uint32_t at; int16_t off; };   // at = UTC epoch of the change; off = new minutes-from-UTC
static DstTrans g_dst[MAX_DST];
static int      g_nDst = 0;
static bool applyDst();    // fwd decl: used by update(), defined with the persistence helpers below

// Persist/restore the wall clock through flash. The RTC keeps time across a warm
// reboot, but a cold power-down resets it — a saved epoch lets us re-seed an
// approximate clock on boot (stale by the downtime; NTP refines it when it lands).
static void     saveEpoch(uint32_t e) { Preferences p; if (p.begin("time", false)) { p.putUInt("epoch", e); p.end(); } }
static uint32_t loadEpoch()           { Preferences p; uint32_t e = 0; if (p.begin("time", true)) { e = p.getUInt("epoch", 0); p.end(); } return e; }
static void     persistNow()          { saveEpoch((uint32_t)rtc_read()); g_lastPersistMs = millis(); }

// Epoch (UTC) -> civil date/time. Hinnant's days-from-civil, inverted; no libc,
// no timezone database — pass an already tz-adjusted epoch to get local fields.
static void civil(uint32_t e, int& Y, int& Mo, int& D, int& h, int& mi, int& s) {
    uint32_t days = e / 86400UL, secs = e % 86400UL;
    h = (int)(secs / 3600); mi = (int)((secs % 3600) / 60); s = (int)(secs % 60);
    int z = (int)days + 719468;
    int era = (z >= 0 ? z : z - 146096) / 146097;
    unsigned doe = (unsigned)(z - era * 146097);
    unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int y = (int)yoe + era * 400;
    unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    unsigned mp = (5 * doy + 2) / 153;
    D  = (int)(doy - (153 * mp + 2) / 5 + 1);
    Mo = (int)(mp < 10 ? mp + 3 : mp - 9);
    Y  = y + (Mo <= 2);
}

static uint32_t localEpoch() { return (uint32_t)((int32_t)rtc_read() + g_tzOffsetMin * 60); }

bool     Timekeeper::synced()      { return g_synced; }
uint32_t Timekeeper::epoch()       { return (uint32_t)rtc_read(); }
uint32_t Timekeeper::epochOrZero() { return g_synced ? (uint32_t)rtc_read() : 0; }

int Timekeeper::minOfDayLocal() {
    if (!g_synced) return -1;
    uint32_t sod = localEpoch() % 86400UL;
    return (int)(sod / 60);
}

void Timekeeper::update() {
    long sec = 0, usec = 0; unsigned int tick = 0;
    sntp_get_lasttime(&sec, &usec, &tick);
    // A fresh, sane fix (after ~2001-09) we haven't consumed yet -> set the RTC.
    if (tick != 0 && tick != g_lastTick && sec > (long)SANE_EPOCH) {
        g_lastTick = tick;
        rtc_write((time_t)sec);              // RTC now holds UTC and free-runs
        bool first = !g_synced;
        g_synced = true; g_source = "ntp"; g_lastSyncEpoch = (uint32_t)sec;
        persistNow();                        // capture the accurate fix to flash
        eventLogAppend("time", first ? "ntp sync" : "ntp resync");
    }
    // Periodically persist the free-running clock so a cold boot can re-seed it.
    if (g_synced && (uint32_t)(millis() - g_lastPersistMs) >= PERSIST_MS) persistNow();
    if (g_synced) applyDst();   // roll the offset forward at a DST boundary (cheap: usually a no-op)
}

// ---- harness -------------------------------------------------------------
static long qn(const String& q, const char* k, long def) {
    String v = httpGetParam(q, k);
    return v.length() ? strtol(v.c_str(), nullptr, 0) : def;
}

static String isoLocal() {
    int Y, Mo, D, h, mi, s; civil(localEpoch(), Y, Mo, D, h, mi, s);
    char buf[24];
    snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d", Y, Mo, D, h, mi, s);
    return String(buf);
}

static String nowJson() {
    String s = "{\"synced\":";  s += g_synced ? "true" : "false";
    s += ",\"utc\":";           s += (uint32_t)rtc_read();
    s += ",\"local_iso\":\"";   s += isoLocal(); s += "\"";
    s += ",\"tz_min\":";        s += g_tzOffsetMin;
    s += ",\"dst_n\":";         s += g_nDst;
    s += ",\"dst_next\":";      s += (g_nDst ? g_dst[0].at : 0);
    s += ",\"source\":\"";      s += g_source; s += "\"";
    s += "}";
    return s;
}

static String cmdNow(const String&) { return nowJson(); }

static void saveTz() { Preferences p; if (p.begin("time", false)) { p.putInt("tz", g_tzOffsetMin); p.end(); } }
static void loadTz() { Preferences p; if (p.begin("time", true))  { g_tzOffsetMin = p.getInt("tz", g_tzOffsetMin); p.end(); } }

// DST schedule persistence (mirror feeder's int-count + bytes-blob pattern). 6 B/transition.
static void saveDst() {
    Preferences p; if (!p.begin("time", false)) return;
    p.putInt("dn", g_nDst);
    if (g_nDst > 0) {
        uint8_t buf[MAX_DST * 6];
        for (int i = 0; i < g_nDst; i++) {
            uint32_t a = g_dst[i].at; uint16_t o = (uint16_t)g_dst[i].off;
            buf[i*6+0]=a&0xFF; buf[i*6+1]=(a>>8)&0xFF; buf[i*6+2]=(a>>16)&0xFF; buf[i*6+3]=(a>>24)&0xFF;
            buf[i*6+4]=o&0xFF; buf[i*6+5]=(o>>8)&0xFF;
        }
        p.putBytes("dst", buf, g_nDst * 6);
    }
    p.end();
}
static void loadDst() {
    Preferences p; if (!p.begin("time", true)) return;
    int n = p.getInt("dn", 0); if (n < 0) n = 0; if (n > MAX_DST) n = MAX_DST;
    uint8_t buf[MAX_DST * 6];
    size_t got = (n > 0) ? p.getBytes("dst", buf, sizeof buf) : 0;
    p.end();
    g_nDst = 0;
    if (got >= (size_t)(n * 6)) {
        for (int i = 0; i < n; i++) {
            uint32_t a = (uint32_t)buf[i*6] | ((uint32_t)buf[i*6+1]<<8) | ((uint32_t)buf[i*6+2]<<16) | ((uint32_t)buf[i*6+3]<<24);
            uint16_t o = (uint16_t)buf[i*6+4] | ((uint16_t)buf[i*6+5]<<8);
            g_dst[g_nDst].at = a; g_dst[g_nDst].off = (int16_t)o; g_nDst++;
        }
    }
}
// Apply transitions whose UTC moment has arrived; pop them off the front.
static bool applyDst() {
    bool changed = false;
    while (g_nDst > 0 && (uint32_t)rtc_read() >= g_dst[0].at) {
        g_tzOffsetMin = g_dst[0].off;
        for (int i = 1; i < g_nDst; i++) g_dst[i-1] = g_dst[i];
        g_nDst--; changed = true;
    }
    if (changed) { saveTz(); saveDst(); eventLogAppend("time", "dst shift"); }
    return changed;
}

static String cmdTz(const String& q) {
    int prev = g_tzOffsetMin;
    if (httpGetParam(q, "min").length())      g_tzOffsetMin = (int)qn(q, "min", g_tzOffsetMin);
    else if (httpGetParam(q, "h").length())   g_tzOffsetMin = (int)qn(q, "h", 0) * 60;
    if (g_tzOffsetMin != prev) saveTz();       // spare flash: the panel may re-push an identical offset
    String s = "{\"tz_min\":"; s += g_tzOffsetMin; s += "}";
    return s;
}

static String cmdDst(const String& q) {
    // list=at:off,at:off,...  (at = UTC epoch s, off = minutes-from-UTC). FUTURE transitions only;
    // an empty/absent list clears the schedule. The panel derives these from the browser's tz rules.
    String list = httpGetParam(q, "list");
    g_nDst = 0;
    int i = 0;
    while (i < (int)list.length() && g_nDst < MAX_DST) {
        int comma = list.indexOf(',', i); if (comma < 0) comma = (int)list.length();
        int colon = list.indexOf(':', i);
        if (colon > i && colon < comma) {
            uint32_t at  = (uint32_t)strtoul(list.substring(i, colon).c_str(), nullptr, 10);
            int      off = (int)strtol(list.substring(colon + 1, comma).c_str(), nullptr, 10);
            if (at > 0) { g_dst[g_nDst].at = at; g_dst[g_nDst].off = (int16_t)off; g_nDst++; }
        }
        i = comma + 1;
    }
    saveDst();
    applyDst();   // any transition already in the past corrects the current offset immediately
    String s = "{\"dst_n\":"; s += g_nDst;
    s += ",\"tz_min\":";   s += g_tzOffsetMin;
    s += ",\"dst_next\":"; s += (g_nDst ? g_dst[0].at : 0);
    s += "}";
    return s;
}

static String cmdSet(const String& q) {       // manual set (bench / no network)
    long e = qn(q, "epoch", 0);
    if (e > (long)SANE_EPOCH) { rtc_write((time_t)e); g_synced = true; g_source = "manual"; persistNow(); eventLogAppend("time", "manual set"); }
    return nowJson();
}

static String cmdSync(const String&) {         // force a fresh NTP query
    sntp_stop(); g_lastTick = 0; sntp_init();
    return "{\"ok\":true}";
}

static void stateClock(String& out) { out += "\"clock\":"; out += nowJson(); }

void timeInit() {
    loadTz();                                  // restore timezone offset
    loadDst();                                 // restore the pushed DST transition schedule
    rtc_init();

    // Establish a clock immediately so events get real timestamps from the first
    // moment, not just after the NTP fix lands. Trust a retained RTC (survives a
    // warm reboot); else re-seed from the last epoch persisted to flash (survives
    // a cold power-down, stale by the downtime). NTP corrects either when it arrives.
    uint32_t rnow = (uint32_t)rtc_read();
    if (rnow >= SANE_EPOCH) {
        g_synced = true; g_source = "rtc";
        persistNow();                          // freshen flash with the accurate retained clock
        eventLogAppend("time", "rtc retained");
    } else {
        uint32_t saved = loadEpoch();
        if (saved >= SANE_EPOCH) {
            rtc_write((time_t)saved);
            g_synced = true; g_source = "flash";
            eventLogAppend("time", "restored from flash");
        }
    }
    g_lastPersistMs = millis();
    if (g_synced) applyDst();                  // a transition that elapsed while powered off applies at boot

    sntp_init();                               // async; WiFi must already be up
    regAddState(stateClock);
    regAddCommand("time.now",  cmdNow,  "",                 "current UTC + local time");
    regAddCommand("time.tz",   cmdTz,   "min:int|h:int",    "set timezone offset from UTC (minutes, or whole hours)");
    regAddCommand("time.dst",  cmdDst,  "list:csv",         "set future DST transitions epoch:offMin,... (panel-pushed; empty clears)");
    regAddCommand("time.set",  cmdSet,  "epoch:int",        "manually set the clock (unix UTC seconds)");
    regAddCommand("time.sync", cmdSync, "",                 "force a fresh NTP sync");
}
