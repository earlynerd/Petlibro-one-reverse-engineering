/*
 * RTL8721DM Feeder — de-clouded replacement firmware.  PHASE 0: foundation.
 *
 * Stands up the web-first bench harness:
 *   WiFi STA + mDNS  ->  HTTP server  ->  registry (state + commands)  ->  event log
 *
 * Open http://feeder.local/ (or the printed IP) to drive and observe the
 * device. In Phase 1, each peripheral driver registers a state contributor and
 * a few commands here, and its telemetry + controls appear in the dashboard
 * automatically. Right now only demo commands exist to prove the loop.
 *
 * Serial = LOGUART (PA[7]/PA[8]) -> comes back out the Pico bridge CDC0.
 */
#include <Arduino.h>
#include <WiFi.h>
#include "secrets.h"
#include "app/registry.h"
#include "app/eventlog.h"
#include "web/http_server.h"
#include "drivers/aw9523b.h"
#include "drivers/rfid.h"
#include "drivers/sensors.h"
#include "drivers/lid.h"
#include "drivers/feed.h"
#include "drivers/buttons.h"
#include "drivers/display.h"
#include "app/access_control.h"
#include "app/feeder.h"
#include "app/timekeeper.h"
#include "app/alerts.h"
#include "app/config.h"

static WiFiServer server(80);
static bool        g_wifiUp = false;

// --------------------------------------------------------------------------
//  Demo state + commands (replaced by real drivers in Phase 1)
// --------------------------------------------------------------------------
static String ipStr() {
    IPAddress ip = WiFi.localIP();
    return String(ip[0]) + "." + ip[1] + "." + ip[2] + "." + ip[3];
}

static void state_sys(String& out) {
    out += "\"sys\":{";
    out += "\"uptime_ms\":"; out += millis();
    out += ",\"wifi\":";     out += (g_wifiUp ? "true" : "false");
    out += ",\"rssi\":";     out += (g_wifiUp ? WiFi.RSSI() : 0);
    out += ",\"ssid\":\"";   out += (g_wifiUp ? WiFi.SSID() : "");
    out += "\",\"ip\":\"";   out += (g_wifiUp ? ipStr() : String("0.0.0.0"));
    out += "\"}";
}

static String cmd_ping(const String&) {
    return String("{\"pong\":true,\"millis\":") + millis() + "}";
}
static String cmd_echo(const String& q) {
    String msg = httpGetParam(q, "msg");
    String out = "{\"echo\":\""; out += msg; out += "\"}";
    return out;
}
static String cmd_logtest(const String& q) {
    String msg = httpGetParam(q, "msg");
    if (msg.length() == 0) msg = "manual log.test";
    uint32_t seq = eventLogAppend("test", msg);
    return String("{\"ok\":true,\"seq\":") + seq + "}";
}
static String cmd_evstats(const String&) { String s; eventLogStatsJson(s); return s; }
static String cmd_evclear(const String&) { eventLogClear(); String s; eventLogStatsJson(s); return s; }

// --------------------------------------------------------------------------
static void connectWifi() {
    Serial.print("WiFi: connecting to ");
    Serial.println(WIFI_SSID);
    uint32_t start = millis();
    while (WiFi.status() != WL_CONNECTED) {
        WiFi.begin((char*)WIFI_SSID, (char*)WIFI_PASS);
        // WiFi.begin blocks ~ until associated or times out internally;
        // re-loop until connected.
        if (WiFi.status() == WL_CONNECTED) break;
        if (millis() - start > 60000UL) { Serial.println("WiFi: still trying…"); start = millis(); }
        delay(500);
    }
    g_wifiUp = true;
    Serial.print("WiFi: connected, ip ");
    Serial.println(ipStr());
}

void setup() {
    Serial.begin(115200);
    delay(50);
    Serial.println();
    Serial.println("=== RTL8721DM Feeder — Phase 0 harness ===");

    eventLogInit();
    eventLogAppend("boot", "phase0 harness up");

    regAddState(state_sys);
    regAddCommand("ping",     cmd_ping,    "",         "liveness check");
    regAddCommand("echo",     cmd_echo,    "msg:str",  "echo a string back");
    regAddCommand("log.test", cmd_logtest, "msg:str",  "append a test event");
    regAddCommand("events.stats", cmd_evstats, "", "event-log backing store status (fs/records/seq)");
    regAddCommand("events.clear", cmd_evclear, "", "drop stored events (seq keeps advancing)");
    configInit();   // config.wipe / config.info (persisted settings live in each module)

#ifndef DISPLAY_ONLY
    aw9523Init();   // AW9523B expander: bit-bang I2C on PA_18/PA_19, harness cmds
    rfidInit();     // JY-L601D RFID: UART3 Modbus master on PA_26/PA_25, IRQ PA_16
    sensorsInit();  // ADC cluster (PB_1..7) + hopper digital (PA_17)
    lidInit();      // lid motor (PA_28/PA_30 PWM) + endstops
    feedInit();     // feed motor (AW9523 P0_2/P0_3) + rotor encoder
    buttonsInit();  // front-panel buttons (PA_0/PA_2/PA_4/PB_26), active-low
    aclInit();      // access control: RFID -> whitelist -> lid (disabled until acl.enable)
    feederInit();   // feeder: manual/scheduled dispensing -> meal events (sched gated on time)
    alertsInit();   // fault conditions -> dot-matrix panel (alert surface)
#endif
    displayInit();  // WS1625 dot-matrix (PA_13/14/15), alert surface

    connectWifi();

    eventLogSetClock(Timekeeper::epochOrZero); // wire clock first so timeInit's own events get real ts
    timeInit();                              // RTC (+flash re-seed) + SNTP (WiFi up) -> wall clock

    server.begin();
    Serial.print("HTTP: serving on http://");
    Serial.print(ipStr());
    Serial.println("/  (mDNS feeder.local added in next increment)");
}

void loop() {
    if (WiFi.status() != WL_CONNECTED) { g_wifiUp = false; connectWifi(); }

    Timekeeper::update();   // pick up NTP fixes -> RTC (net/clock only)

#ifndef DISPLAY_ONLY
    Lid::update();    // non-blocking motor safety/closed-loop pumps
    Feed::update();
    Buttons::update();
    AccessControl::update();   // RFID-gated lid state machine
    Feeder::update();          // dispense completion + (gated) schedule
    Alerts::update();          // arbitrate fault conditions -> display
#endif
    Display::update();  // advances the marquee when scrolling

    WiFiClient client = server.available();
    if (client) {
        httpHandleClient(client);
        client.stop();
    }
}
