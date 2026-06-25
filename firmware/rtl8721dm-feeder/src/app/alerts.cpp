#include "app/alerts.h"
#include "drivers/display.h"
#include "app/registry.h"
#include "app/eventlog.h"
#include "web/http_server.h"
#include <string.h>
#include <stdlib.h>

namespace {
struct Alert {
    char     key[12];
    char     msg[24];
    uint8_t  prio;
    uint32_t expire;     // 0 = sticky; else millis deadline
    uint32_t raised;     // last (re)raise time, for tie-break
    bool     active;
};
constexpr int MAX = 8;
Alert g_a[MAX];
char  g_shownKey[12] = {0};   // what the panel currently shows (empty = dark)
char  g_shownMsg[24] = {0};

void copyTrunc(char* d, size_t n, const char* s) { size_t i = 0; for (; s[i] && i < n - 1; i++) d[i] = s[i]; d[i] = 0; }

int findKey(const char* key) {
    for (int i = 0; i < MAX; i++) if (g_a[i].active && strcmp(g_a[i].key, key) == 0) return i;
    return -1;
}
int freeSlot() {
    for (int i = 0; i < MAX; i++) if (!g_a[i].active) return i;
    int lo = 0; for (int i = 1; i < MAX; i++) if (g_a[i].prio < g_a[lo].prio) lo = i;  // evict lowest prio
    return lo;
}
} // namespace

void Alerts::raise(const char* key, const char* msg, uint8_t prio, uint32_t ttlMs) {
    int i = findKey(key);
    bool isNew = (i < 0);
    if (isNew) { i = freeSlot(); copyTrunc(g_a[i].key, sizeof(g_a[i].key), key); }
    bool changed = isNew || strcmp(g_a[i].msg, msg) != 0;
    copyTrunc(g_a[i].msg, sizeof(g_a[i].msg), msg);
    g_a[i].prio   = prio;
    g_a[i].raised = millis();
    g_a[i].expire = ttlMs ? (millis() + ttlMs) : 0;
    g_a[i].active = true;
    if (changed) eventLogAppend("alert", String(key) + " " + msg);   // log only on new/changed
}

void Alerts::clear(const char* key) {
    int i = findKey(key);
    if (i >= 0) { g_a[i].active = false; eventLogAppend("alert", String(key) + " clear"); }
}

void Alerts::update() {
    uint32_t now = millis();
    for (int i = 0; i < MAX; i++)                       // expire TTL alerts
        if (g_a[i].active && g_a[i].expire && (int32_t)(now - g_a[i].expire) >= 0) {
            g_a[i].active = false;
            eventLogAppend("alert", String(g_a[i].key) + " expire");
        }

    int best = -1;                                      // highest prio, tie -> most recent
    for (int i = 0; i < MAX; i++) {
        if (!g_a[i].active) continue;
        if (best < 0 || g_a[i].prio > g_a[best].prio ||
            (g_a[i].prio == g_a[best].prio && (int32_t)(g_a[i].raised - g_a[best].raised) > 0))
            best = i;
    }

    if (best < 0) {                                     // nothing active -> dark
        if (g_shownKey[0]) { Display::clear(); g_shownKey[0] = 0; g_shownMsg[0] = 0; }
        return;
    }
    // Render only when the shown alert changes — re-issuing scroll() each loop
    // would reset the marquee and it would never advance.
    if (strcmp(g_shownKey, g_a[best].key) != 0 || strcmp(g_shownMsg, g_a[best].msg) != 0) {
        copyTrunc(g_shownKey, sizeof(g_shownKey), g_a[best].key);
        copyTrunc(g_shownMsg, sizeof(g_shownMsg), g_a[best].msg);
        if (strlen(g_a[best].msg) <= 4) Display::showText(g_a[best].msg);
        else                            Display::scroll(g_a[best].msg);
    }
}

// ---- harness -------------------------------------------------------------
static long qn(const String& q, const char* k, long def) { String v = httpGetParam(q, k); return v.length() ? strtol(v.c_str(), nullptr, 0) : def; }

static String listJson() {
    String s = "{\"shown\":\""; s += g_shownKey; s += "\",\"active\":[";
    bool first = true;
    for (int i = 0; i < MAX; i++) {
        if (!g_a[i].active) continue;
        if (!first) s += ','; first = false;
        s += "{\"key\":\""; s += g_a[i].key;
        s += "\",\"msg\":\""; s += g_a[i].msg;
        s += "\",\"prio\":"; s += g_a[i].prio; s += "}";
    }
    s += "]}";
    return s;
}

static String cmdTest(const String& q) {
    String k = httpGetParam(q, "key"); if (!k.length()) k = "test";
    String m = httpGetParam(q, "msg"); if (!m.length()) m = "TEST";
    Alerts::raise(k.c_str(), m.c_str(), (uint8_t)qn(q, "prio", Alerts::PRIO_WARN), (uint32_t)qn(q, "ttl", 0));
    return listJson();
}
static String cmdClear(const String& q) {
    String k = httpGetParam(q, "key");
    if (k.length()) Alerts::clear(k.c_str());
    else for (int i = 0; i < MAX; i++) g_a[i].active = false;   // clear all
    return listJson();
}

static void stateAlerts(String& out) { out += "\"alerts\":"; out += listJson(); }

void alertsInit() {
    regAddState(stateAlerts);
    regAddCommand("alerts.test",  cmdTest,  "key:str,msg:str,prio:1-3,ttl:int", "raise a test alert on the panel");
    regAddCommand("alerts.clear", cmdClear, "key:str", "clear one alert (key=) or all (no key)");
}
