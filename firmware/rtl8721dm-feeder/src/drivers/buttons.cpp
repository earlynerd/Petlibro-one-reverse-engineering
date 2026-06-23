#include "drivers/buttons.h"
#include "app/registry.h"
#include "app/eventlog.h"
extern "C" {
  #include "PinNames.h"
  #include "gpio_api.h"
}

static const PinName BTN_PIN[Buttons::COUNT]  = { PA_0, PA_2, PA_4, PB_26 };
// pin + tentative front-panel function (confirm by pressing — see header)
static const char*   BTN_NAME[Buttons::COUNT] = { "pa0_meal", "pa2_feed", "pa4_lid", "pb26_lock" };

static gpio_t   g_io[Buttons::COUNT];
static uint8_t  g_raw[Buttons::COUNT];
static uint8_t  g_stable[Buttons::COUNT];
static uint32_t g_since[Buttons::COUNT];
static uint16_t g_count[Buttons::COUNT];
static bool     g_up = false;

static constexpr uint32_t DEBOUNCE_MS = 25;

void Buttons::begin() {
    for (int i = 0; i < COUNT; i++) {
        gpio_init(&g_io[i], BTN_PIN[i]);
        gpio_dir(&g_io[i], PIN_INPUT);
        gpio_mode(&g_io[i], PullUp);         // idle HIGH; press pulls LOW
        g_raw[i] = g_stable[i] = 1;
        g_since[i] = 0;
        g_count[i] = 0;
    }
    g_up = true;
}

void Buttons::update() {
    if (!g_up) return;
    uint32_t now = millis();
    for (int i = 0; i < COUNT; i++) {
        uint8_t lv = gpio_read(&g_io[i]) ? 1 : 0;
        if (lv != g_raw[i]) {                                 // bouncing: restart timer
            g_raw[i] = lv; g_since[i] = now;
        } else if ((uint32_t)(now - g_since[i]) >= DEBOUNCE_MS && g_stable[i] != lv) {
            g_stable[i] = lv;
            if (lv == 0) {                                    // settled pressed (active-low)
                g_count[i]++;
                eventLogAppend("button", BTN_NAME[i]);
            }
        }
    }
}

bool Buttons::pressed(Id b) { return g_stable[b] == 0; }

// ---- harness ----
static void buildJson(String& s) {
    s += '{';
    for (int i = 0; i < Buttons::COUNT; i++) {
        if (i) s += ',';
        s += '"'; s += BTN_NAME[i]; s += "\":{\"pressed\":";
        s += (g_stable[i] == 0) ? "true" : "false";
        s += ",\"n\":"; s += g_count[i]; s += '}';
    }
    s += '}';
}
static String cmdRead(const String&) { String s; buildJson(s); return s; }
static void stateButtons(String& out) { out += "\"buttons\":"; buildJson(out); }

void buttonsInit() {
    Buttons::begin();
    regAddState(stateButtons);
    regAddCommand("buttons.read", cmdRead, "", "front-panel button states + press counts");
}
