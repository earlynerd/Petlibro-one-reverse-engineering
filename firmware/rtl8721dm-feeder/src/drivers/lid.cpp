#include "drivers/lid.h"
#include "drivers/sensors.h"
#include "drivers/aw9523b.h"
#include "app/registry.h"
#include "app/eventlog.h"
#include "web/http_server.h"
#include <stdlib.h>
extern "C" {
  #include "PinNames.h"
  #include "pwmout_api.h"
}

static pwmout_t      g_a, g_b;                 // PA_28 OPEN, PA_30 CLOSE
static bool          g_up       = false;
static int           g_drive    = 0;           // -1 close, 0 stop, +1 open
static uint32_t      g_deadline = 0;
static bool          g_closed     = false;     // closed-loop (stop at endstop)?
static bool          g_targetOpen = true;      // which end we're driving toward
static bool          g_endLevel   = false;     // digital level meaning "at end" (LOW=blocked beam; tune)
static uint32_t      g_stallMa    = 0;         // stall guard in mA (0 = disabled until calibrated)

static constexpr uint32_t PWM_PERIOD_US = 50;  // 20 kHz
static constexpr uint32_t MAX_RUN_MS    = 8000;

void Lid::begin() {
    pwmout_init(&g_a, PA_28); pwmout_init(&g_b, PA_30);
    pwmout_period_us(&g_a, PWM_PERIOD_US); pwmout_period_us(&g_b, PWM_PERIOD_US);
    pwmout_write(&g_a, 0.0f); pwmout_write(&g_b, 0.0f);
    g_up = true; g_drive = 0; g_closed = false;
}
void Lid::stop()  { pwmout_write(&g_a, 0.0f); pwmout_write(&g_b, 0.0f); g_drive = 0; g_closed = false; }
// Invariant: never drive the lid without its endstop emitter (P0_4) powered.
// If the emitter can't be powered (expander down), refuse to move.
void Lid::open(float d) {
    if (!Aw9523::emitter(0, true)) { eventLogAppend("lid", "refuse open: emitter power failed"); Lid::stop(); return; }
    if (d < 0) d = 0; if (d > 1) d = 1; pwmout_write(&g_b, 0.0f); pwmout_write(&g_a, d); g_drive = 1;
}
void Lid::close(float d) {
    if (!Aw9523::emitter(0, true)) { eventLogAppend("lid", "refuse close: emitter power failed"); Lid::stop(); return; }
    if (d < 0) d = 0; if (d > 1) d = 1; pwmout_write(&g_a, 0.0f); pwmout_write(&g_b, d); g_drive = -1;
}

// App-level closed-loop move with sensible defaults (used by access_control/feeder).
void Lid::moveTo(bool open) {
    g_targetOpen = open;
    g_endLevel   = false;                 // endstop triggers LOW
    g_closed     = true;
    g_deadline   = millis() + 6000;
    if (open) Lid::open(0.45f); else Lid::close(0.45f);
}

void Lid::update() {
    if (g_drive == 0) return;
    if ((int32_t)(millis() - g_deadline) >= 0) { eventLogAppend("lid", "stop: timeout"); Lid::stop(); return; }
    if (g_stallMa && Sensors::lidCurrentMa() > g_stallMa) {
        eventLogAppend("lid", String("stop: stall ") + Sensors::lidCurrentMa() + "mA"); Lid::stop(); return;
    }
    if (g_closed) {
        // This lid's endstops read their triggered level (LOW) only at the OPPOSITE
        // end: lid_closed drops at the open end, lid_open drops at the closed end.
        // So watch the opposite detector for the target direction.
        bool watched = g_targetOpen ? Sensors::lidClosed() : Sensors::lidOpen();
        if (watched == g_endLevel) { eventLogAppend("lid", String(g_targetOpen ? "open" : "close") + " endstop hit"); Lid::stop(); }
    }
}

// ---- harness ----
static float qf(const String& q, const char* k, float def) { String v = httpGetParam(q, k); return v.length() ? (float)atof(v.c_str()) : def; }
static long  qn(const String& q, const char* k, long def)  { String v = httpGetParam(q, k); return v.length() ? strtol(v.c_str(), nullptr, 0) : def; }

static String stateJson() {
    String s = "{\"drive\":"; s += g_drive;
    s += ",\"open_end\":";   s += Sensors::lidOpen()   ? 1 : 0;
    s += ",\"closed_end\":"; s += Sensors::lidClosed() ? 1 : 0;
    s += ",\"current_ma\":"; s += Sensors::lidCurrentMa();
    s += "}";
    return s;
}

static String cmdOpen(const String& q) {
    float d = qf(q, "duty", 0.5f); long ms = qn(q, "ms", 1200);
    if (ms < 0) ms = 0; if ((uint32_t)ms > MAX_RUN_MS) ms = MAX_RUN_MS;
    g_closed = false; g_deadline = millis() + ms; Lid::open(d);
    eventLogAppend("lid", String("open duty=") + d); return stateJson();
}
static String cmdClose(const String& q) {
    float d = qf(q, "duty", 0.5f); long ms = qn(q, "ms", 1200);
    if (ms < 0) ms = 0; if ((uint32_t)ms > MAX_RUN_MS) ms = MAX_RUN_MS;
    g_closed = false; g_deadline = millis() + ms; Lid::close(d);
    eventLogAppend("lid", String("close duty=") + d); return stateJson();
}
static String cmdStop(const String&) { Lid::stop(); return stateJson(); }

static String cmdGoto(const String& q) {
    String t = httpGetParam(q, "target");
    bool toOpen = !(t == "close" || t == "closed" || t == "c");
    float d = qf(q, "duty", 0.45f);
    g_endLevel = qn(q, "level", 0) != 0;     // digital level that means "at end" (default LOW)
    g_stallMa  = (uint32_t)qn(q, "stall", 0);    // mA; 0 = disabled
    long ms    = qn(q, "ms", 6000); if ((uint32_t)ms > MAX_RUN_MS) ms = MAX_RUN_MS;
    g_targetOpen = toOpen;
    g_closed = true; g_deadline = millis() + ms;
    if (toOpen) Lid::open(d); else Lid::close(d);
    eventLogAppend("lid", String("goto ") + (toOpen ? "open" : "close"));
    return stateJson();
}

static void stateLid(String& out) { out += "\"lid\":"; out += stateJson(); }

void lidInit() {
    Lid::begin();
    regAddState(stateLid);
    regAddCommand("lid.open",  cmdOpen,  "duty:0-1,ms:int", "drive lid OPEN (PA_28), auto-stop after ms");
    regAddCommand("lid.close", cmdClose, "duty:0-1,ms:int", "drive lid CLOSE (PA_30), auto-stop after ms");
    regAddCommand("lid.stop",  cmdStop,  "",                "stop the lid motor");
    regAddCommand("lid.goto",  cmdGoto,  "target:open/close,duty:0-1,level:0/1,ms:int,stall:int",
                  "closed-loop: drive until target endstop reads `level` (digital)");
}
