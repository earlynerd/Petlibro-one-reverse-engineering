#include "app/access_control.h"
#include "drivers/rfid.h"
#include "drivers/lid.h"
#include "drivers/aw9523b.h"
#include "app/registry.h"
#include "app/eventlog.h"
#include "web/http_server.h"
#include <Preferences.h>
#include <string.h>
#include <stdlib.h>

// ---- whitelist (in-RAM; Preferences-backed later) ------------------------
static constexpr int MAX_TAGS = 16;
static char g_tags[MAX_TAGS][16];
static int  g_nTags = 0;

static bool hasTag(const char* id) {
    for (int i = 0; i < g_nTags; i++) if (strcmp(g_tags[i], id) == 0) return true;
    return false;
}
static bool addTag(const char* id) {
    if (g_nTags >= MAX_TAGS || !id || id[0] == 0 || hasTag(id)) return false;
    strncpy(g_tags[g_nTags], id, 15); g_tags[g_nTags][15] = 0; g_nTags++;
    return true;
}
static bool removeTag(const char* id) {
    for (int i = 0; i < g_nTags; i++) if (strcmp(g_tags[i], id) == 0) {
        for (int j = i; j < g_nTags - 1; j++) memcpy(g_tags[j], g_tags[j + 1], 16);
        g_nTags--; return true;
    }
    return false;
}

// ---- state machine -------------------------------------------------------
static bool     g_enabled    = false;
static bool     g_present     = false;
static bool     g_authorized  = false;
static char     g_lastTag[16] = {0};
static uint32_t g_visitStart  = 0;
static uint32_t g_holdUntil    = 0;
static uint32_t g_holdMs       = 5000;     // grace window after the tag leaves
static bool     g_lidCmdOpen   = false;

// ---- persistence (Preferences/DCT) ---------------------------------------
// Whitelist can't fit one DCT value (16 tags x 16 B > 132 B cap), so each tag
// is its own "tN" string key; enable+hold are scalars. Saved on every mutation.
static void saveAcl() {
    Preferences p;
    if (!p.begin("acl", false)) return;
    p.putBool("en",   g_enabled);
    p.putUInt("hold", g_holdMs);
    p.putInt ("n",    g_nTags);
    for (int i = 0; i < g_nTags; i++)  p.putString((String("t") + i).c_str(), g_tags[i]);
    for (int i = g_nTags; i < MAX_TAGS; i++) p.remove((String("t") + i).c_str());   // drop stale slots
    p.end();
}
static void loadAcl() {
    Preferences p;
    if (!p.begin("acl", true)) return;
    g_enabled = p.getBool("en", false);
    g_holdMs  = p.getUInt("hold", g_holdMs);
    int n = p.getInt("n", 0);
    if (n < 0) n = 0; if (n > MAX_TAGS) n = MAX_TAGS;
    g_nTags = 0;
    for (int i = 0; i < n; i++) {
        String v = p.getString((String("t") + i).c_str(), "");
        if (v.length()) { strncpy(g_tags[g_nTags], v.c_str(), 15); g_tags[g_nTags][15] = 0; g_nTags++; }
    }
    p.end();
}

// Read the tag (a few tries) and report whether it's whitelisted.
static bool readAndCheck() {
    Rfid::Tag t;
    for (int i = 0; i < 3; i++) {
        if (Rfid::readTag(t) && t.valid) {
            strncpy(g_lastTag, t.id, sizeof(g_lastTag)); g_lastTag[sizeof(g_lastTag) - 1] = 0;
            return hasTag(g_lastTag);
        }
    }
    g_lastTag[0] = 0;
    return false;
}

void AccessControl::update() {
    if (!g_enabled) return;
    uint32_t now = millis();
    bool present = Rfid::tagPresentIRQ();          // PA_16 LOW == tag in field

    if (present && !g_present) {                   // tag arrived
        g_authorized = readAndCheck();
        g_visitStart = now;
        eventLogAppend("visit", String(g_lastTag[0] ? g_lastTag : "unknown") + (g_authorized ? " auth" : " deny"));
    } else if (!present && g_present) {            // tag left
        eventLogAppend("visit", String("end ") + (g_lastTag[0] ? g_lastTag : "unknown")
                                 + " " + ((now - g_visitStart) / 1000) + "s");
        g_authorized = false;
    }
    g_present = present;

    if (present && g_authorized) g_holdUntil = now + g_holdMs;   // refresh grace while eating
    bool wantOpen = (int32_t)(now - g_holdUntil) < 0;            // inside the grace window
    if (wantOpen != g_lidCmdOpen) {
        g_lidCmdOpen = wantOpen;
        Lid::moveTo(wantOpen);
        eventLogAppend("lid", wantOpen ? "acl: open" : "acl: close");
    }
}

// ---- harness -------------------------------------------------------------
static long qn(const String& q, const char* k, long def) {
    String v = httpGetParam(q, k);
    return v.length() ? strtol(v.c_str(), nullptr, 0) : def;
}

static String cmdEnable(const String& q) {
    g_enabled = qn(q, "v", 1) != 0;
    if (g_enabled) {
        Aw9523::rfidPower(true);                   // RFID must be powered to assert the IRQ
        g_present = false; g_holdUntil = 0; g_lidCmdOpen = false;
    }
    saveAcl();
    eventLogAppend("acl", g_enabled ? "enabled" : "disabled");
    String s = "{\"enabled\":"; s += g_enabled ? "true" : "false"; s += "}";
    return s;
}
static String cmdAdd(const String& q) {
    String id = httpGetParam(q, "tag");
    bool ok = addTag(id.c_str());
    if (ok) saveAcl();
    String s = "{\"ok\":"; s += ok ? "true" : "false"; s += ",\"n\":"; s += g_nTags; s += "}";
    return s;
}
static String cmdRemove(const String& q) {
    String id = httpGetParam(q, "tag");
    bool ok = removeTag(id.c_str());
    if (ok) saveAcl();
    String s = "{\"ok\":"; s += ok ? "true" : "false"; s += ",\"n\":"; s += g_nTags; s += "}";
    return s;
}
static String cmdList(const String&) {
    String s = "{\"tags\":[";
    for (int i = 0; i < g_nTags; i++) { if (i) s += ','; s += '"'; s += g_tags[i]; s += '"'; }
    s += "]}";
    return s;
}
static String cmdClear(const String&) { g_nTags = 0; saveAcl(); return "{\"ok\":true,\"n\":0}"; }
static String cmdHold(const String& q) {
    long s = qn(q, "s", 5); if (s < 0) s = 0;
    g_holdMs = (uint32_t)s * 1000;
    saveAcl();
    String r = "{\"hold_s\":"; r += s; r += "}";
    return r;
}

static void stateAcl(String& out) {
    out += "\"acl\":{\"enabled\":"; out += g_enabled ? "true" : "false";
    out += ",\"present\":";        out += g_present ? "true" : "false";
    out += ",\"authorized\":";     out += g_authorized ? "true" : "false";
    out += ",\"lid_open\":";       out += g_lidCmdOpen ? "true" : "false";
    out += ",\"hold_s\":";         out += (g_holdMs / 1000);
    out += ",\"n_tags\":";         out += g_nTags;
    if (g_lastTag[0]) { out += ",\"last_tag\":\""; out += g_lastTag; out += "\""; }
    out += "}";
}

void aclInit() {
    loadAcl();                              // restore whitelist + hold + enable
    if (g_enabled) {                        // resume gated lid across reboots (mirror cmdEnable)
        Aw9523::rfidPower(true);
        g_present = false; g_holdUntil = 0; g_lidCmdOpen = false;
    }
    regAddState(stateAcl);
    regAddCommand("acl.enable", cmdEnable, "v:0/1",  "enable RFID-gated lid (also powers RFID)");
    regAddCommand("acl.add",    cmdAdd,    "tag:id", "add a tag id to the whitelist");
    regAddCommand("acl.remove", cmdRemove, "tag:id", "remove a tag id from the whitelist");
    regAddCommand("acl.list",   cmdList,   "",       "list whitelisted tag ids");
    regAddCommand("acl.clear",  cmdClear,  "",       "clear the whitelist");
    regAddCommand("acl.hold",   cmdHold,   "s:int",  "grace seconds the lid stays open after a tag leaves");
}
