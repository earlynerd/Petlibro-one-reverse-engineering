#include "app/config.h"
#include "app/registry.h"
#include "app/eventlog.h"
#include "web/http_server.h"
#include <Preferences.h>

// The namespaces each module persists into (see access_control / feeder / timekeeper).
static const char* MODULES[] = { "acl", "feeder", "time" };
static constexpr int N_MODULES = sizeof(MODULES) / sizeof(MODULES[0]);

static String cmdWipe(const String&) {
    int wiped = 0;
    for (int i = 0; i < N_MODULES; i++) {
        Preferences p;
        if (p.begin(MODULES[i], false)) { if (p.clear()) wiped++; p.end(); }
    }
    eventLogAppend("config", "wiped — reboot to apply");
    String s = "{\"wiped\":"; s += wiped; s += ",\"note\":\"reboot to apply (RAM still holds current values)\"}";
    return s;
}

static String cmdInfo(const String&) {
    String s = "{\"modules\":[";
    for (int i = 0; i < N_MODULES; i++) {
        Preferences p;
        if (i) s += ',';
        s += "{\"name\":\""; s += MODULES[i]; s += "\"";
        if (p.begin(MODULES[i], true)) { s += ",\"free\":"; s += p.freeEntries(); p.end(); }
        else                           { s += ",\"free\":-1"; }
        s += "}";
    }
    s += "]}";
    return s;
}

void configInit() {
    regAddCommand("config.wipe", cmdWipe, "", "erase all stored config (reboot to apply)");
    regAddCommand("config.info", cmdInfo, "", "DCT free-entry count per module");
}
