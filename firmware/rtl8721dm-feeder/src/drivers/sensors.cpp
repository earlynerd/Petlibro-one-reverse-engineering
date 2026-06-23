#include "drivers/sensors.h"
#include "drivers/aw9523b.h"
#include "app/registry.h"
extern "C" {
  #include "PinNames.h"
  #include "analogin_api.h"
  #include "gpio_api.h"
}

// analog current/voltage
static analogin_t g_lidI, g_feedI, g_batt;
static uint16_t   g_lidIzero = 0, g_feedIzero = 0;   // idle ADC offset (subtracted in mA)
// photoelectric detectors. lid_closed is on PB_3 (=SWD_CLK), which reads stuck on
// the GPIO input path, so it's read via ADC; the rest are plain GPIO.
static gpio_t     g_lidOpen, g_rotor, g_chute, g_hopper;
static analogin_t g_lidClosed;
static bool       g_up = false;

static void digIn(gpio_t* g, PinName p) {
    gpio_init(g, p);
    gpio_dir(g, PIN_INPUT);
    gpio_mode(g, PullNone);                 // high-Z, like the (unpulled) ADC reads
}

void Sensors::begin() {
    analogin_init(&g_lidI,  PB_2);             // lid <-> feed current swapped (bench-verified)
    analogin_init(&g_feedI, PB_1);
    analogin_init(&g_batt,  PB_7);
    digIn(&g_lidOpen,   PB_4);              // open-end detector
    analogin_init(&g_lidClosed, PB_3);      // closed-end detector; PB_3=SWD_CLK -> ADC (GPIO reads stuck)
    digIn(&g_rotor,     PB_5);
    digIn(&g_chute,     PA_17);             // chute <-> hopper swapped (bench-verified)
    digIn(&g_hopper,    PB_6);
    g_up = true;
}

// analogin_read_u16 returns the raw 12-bit value on this part (0..4095) — no shift.
uint16_t Sensors::lidCurrent()  { return analogin_read_u16(&g_lidI); }
uint16_t Sensors::feedCurrent() { return analogin_read_u16(&g_feedI); }
uint16_t Sensors::battery()     { return analogin_read_u16(&g_batt); }

// Estimate current in mA. ADC is 12-bit (0..4095); full-scale taken as 3.3 V
// (nominal — AmebaD ADC isn't perfectly linear, calibrate against a known load).
// Assumes the shunt voltage is read directly (no gain stage); if a current-sense
// amp is present, scale K accordingly. I = Vshunt / 0.25 ohm.
static constexpr uint32_t ADC_FS_MV  = 3300;
static constexpr uint32_t SHUNT_MOHM = 250;                       // 0.25 ohm
static constexpr uint32_t K_MA = ADC_FS_MV * 1000 / SHUNT_MOHM;   // mA-num per count·4095 = 13200
static uint32_t toMa(uint16_t raw, uint16_t zero) {
    return (raw > zero) ? (uint32_t)(raw - zero) * K_MA / 4095 : 0;
}
uint32_t Sensors::lidCurrentMa()  { return toMa(analogin_read_u16(&g_lidI),  g_lidIzero); }
uint32_t Sensors::feedCurrentMa() { return toMa(analogin_read_u16(&g_feedI), g_feedIzero); }

static uint16_t readAvg(analogin_t* a) {
    uint32_t s = 0;
    for (int i = 0; i < 16; i++) s += analogin_read_u16(a);
    return (uint16_t)(s / 16);
}
void     Sensors::zeroCurrent() { g_lidIzero = readAvg(&g_lidI); g_feedIzero = readAvg(&g_feedI); }
uint16_t Sensors::lidZero()     { return g_lidIzero; }
uint16_t Sensors::feedZero()    { return g_feedIzero; }

bool Sensors::lidOpen()   { return gpio_read(&g_lidOpen) != 0; }
// lid_closed is on PB_3 (SWD_CLK): read via ADC + threshold (states ~240/~3980; >2000 = HIGH).
bool Sensors::lidClosed() { return analogin_read_u16(&g_lidClosed) > 2000; }
bool Sensors::rotor()     { return gpio_read(&g_rotor)     != 0; }
bool Sensors::chute()     { return gpio_read(&g_chute)     != 0; }
bool Sensors::hopper()    { return gpio_read(&g_hopper)    != 0; }

static void buildJson(String& s) {
    s += "{\"lid_i_ma\":";   s += Sensors::lidCurrentMa();
    s += ",\"feed_i_ma\":";  s += Sensors::feedCurrentMa();
    s += ",\"batt\":";       s += Sensors::battery();
    s += ",\"lid_open\":";   s += Sensors::lidOpen()   ? 1 : 0;
    s += ",\"lid_closed\":"; s += Sensors::lidClosed() ? 1 : 0;
    s += ",\"rotor\":";      s += Sensors::rotor()     ? 1 : 0;
    s += ",\"chute\":";      s += Sensors::chute()     ? 1 : 0;
    s += ",\"hopper\":";     s += Sensors::hopper()    ? 1 : 0;
    s += '}';
}

static String cmdRead(const String&) { String s; buildJson(s); return s; }
static String cmdZero(const String&) {
    Sensors::zeroCurrent();
    String s = "{\"lid_zero\":"; s += Sensors::lidZero();
    s += ",\"feed_zero\":";      s += Sensors::feedZero(); s += "}";
    return s;
}
static void stateSensors(String& out) { out += "\"sensors\":"; buildJson(out); }

void sensorsInit() {
    Sensors::begin();
    // Keep all photoelectric emitters lit so the detectors read valid at all
    // times (lid endstops, rotor, chute, hopper), not only while a motor moves.
    Aw9523::emitter(0, true);   // P0_4 lid endstops
    Aw9523::emitter(1, true);   // P0_5 rotor encoder
    Aw9523::emitter(2, true);   // P0_7 hopper + chute
    Sensors::zeroCurrent();     // capture idle current offset (motors are stopped at boot)
    regAddState(stateSensors);
    regAddCommand("sensors.read", cmdRead, "", "current(mA)/battery + digital photo-detectors");
    regAddCommand("sensors.zero", cmdZero, "", "re-zero current offsets (run with motors stopped)");
}
