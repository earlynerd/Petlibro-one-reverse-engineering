#include "drivers/display.h"
#include "app/registry.h"
#include "web/http_server.h"
#include <string.h>
#include <stdlib.h>
extern "C" {
  #include "PinNames.h"
  #include "gpio_api.h"
}
#include "drivers/repl_data.h"

// ---- pins (confirmed 2026-06-17) -----------------------------------------
static const PinName D_SCL  = PA_13;   // shared clock
static const PinName D_SDAL = PA_14;   // line L  (mask bit0)
static const PinName D_SDAR = PA_15;   // line R  (mask bit1)

static gpio_t  dScl, dSdaL, dSdaR;
static bool    dispUp = false;
static uint8_t dRam[2][24];            // shadow RAM: [0]=line L, [1]=line R
static uint8_t  g_bri = 7;
static bool     g_on  = false;
static bool     g_pushpull = false;    // open-drain default (confirmed-working state). Bench-verified the
                                       // display works in EITHER mode once the corrected init sequence ran,
                                       // so drive mode was NOT the fix. Toggle via disp.drive if ever needed.
static uint32_t g_halfUs   = 6;        // bit half-period (us); raise to give a weak pull-up more rise time

// ---- bit-bang primitives (ported verbatim from the pinmap explorer) ------
static inline void dDelay() { delayMicroseconds(g_halfUs); }
// Configure the pins ONCE. Re-running gpio_init before every transaction (an earlier
// mis-fix) glitched the pads right before each command's START/STOP — pixel writes
// (no dInit) framed cleanly while commands did not. DRIVE MODE is runtime-selectable
// (`disp.drive`); changing it clears dispUp to force a single clean re-init.
static void dInit() {
    if (dispUp) return;
    gpio_init(&dScl, D_SCL); gpio_init(&dSdaL, D_SDAL); gpio_init(&dSdaR, D_SDAR);
    if (g_pushpull) {
        gpio_write(&dScl, 1);  gpio_dir(&dScl,  PIN_OUTPUT);   // idle driven HIGH
        gpio_write(&dSdaL, 1); gpio_dir(&dSdaL, PIN_OUTPUT);
        gpio_write(&dSdaR, 1); gpio_dir(&dSdaR, PIN_OUTPUT);
    } else {
        gpio_write(&dScl, 0); gpio_write(&dSdaL, 0); gpio_write(&dSdaR, 0);   // preset-low for open-drain
        gpio_mode(&dScl, PullUp); gpio_mode(&dSdaL, PullUp); gpio_mode(&dSdaR, PullUp);
        gpio_dir(&dScl, PIN_INPUT); gpio_dir(&dSdaL, PIN_INPUT); gpio_dir(&dSdaR, PIN_INPUT);   // idle high
    }
    dispUp = true;
}
static inline void DSCL_HI() { if (g_pushpull) gpio_write(&dScl, 1); else gpio_dir(&dScl, PIN_INPUT);  }
static inline void DSCL_LO() { if (g_pushpull) gpio_write(&dScl, 0); else gpio_dir(&dScl, PIN_OUTPUT); }
static inline void DSDA(uint8_t m, int hi) {
    if (g_pushpull) {
        if (m & 1) { gpio_write(&dSdaL, hi); gpio_dir(&dSdaL, PIN_OUTPUT); }
        if (m & 2) { gpio_write(&dSdaR, hi); gpio_dir(&dSdaR, PIN_OUTPUT); }
    } else {
        if (m & 1) gpio_dir(&dSdaL, hi ? PIN_INPUT : PIN_OUTPUT);
        if (m & 2) gpio_dir(&dSdaR, hi ? PIN_INPUT : PIN_OUTPUT);
    }
}
// Release SDA (input). Open-drain: this is the HIGH state. Push-pull: frees the line
// for the chip's ACK on the 9th clock (avoids contention).
static inline void DSDA_REL(uint8_t m) {
    if (m & 1) gpio_dir(&dSdaL, PIN_INPUT);
    if (m & 2) gpio_dir(&dSdaR, PIN_INPUT);
}
static void dStart(uint8_t m) { DSDA(m, 1); DSCL_HI(); dDelay(); DSDA(m, 0); dDelay(); DSCL_LO(); dDelay(); }
static void dStop(uint8_t m)  { DSDA(m, 0); dDelay(); DSCL_HI(); dDelay(); DSDA(m, 1); dDelay(); }
static void dByte(uint8_t m, uint8_t v) {
    for (int i = 0; i < 8; i++) { DSDA(m, (v & 0x80) ? 1 : 0); v <<= 1; dDelay(); DSCL_HI(); dDelay(); DSCL_LO(); }
    DSDA_REL(m); dDelay(); DSCL_HI(); dDelay(); DSCL_LO(); dDelay();      // 9th = ACK clock (SDA released)
}
// Every command leads with the 0x48 data-mode byte, matching stock's init pattern:
//   ON  = [0x48, 0x88|bri]   OFF = [0x48, 0x80]   grid = [0xC0|g, data]
// NOTE: an earlier claim that "the chip ignores lone bytes / needs >=2 bytes" was WRONG —
// lone bytes DO appear in the capture. The cold-init fix was sending the 0x48 data-mode
// command at all (we'd omitted it) + correct framing, NOT avoiding single-byte frames.
// We keep the [0x48,...] bundling because it's the proven-working sequence, not because
// lone bytes are forbidden.
static void dCtrl(uint8_t m, uint8_t bri) { dStart(m); dByte(m, 0x48); dByte(m, 0x88 | (bri & 0x07)); dStop(m); }  // data-mode + ON
static void dOff(uint8_t m)               { dStart(m); dByte(m, 0x48); dByte(m, 0x80); dStop(m); }                 // data-mode + OFF
static void dGrid(uint8_t m, uint8_t g, uint8_t data) { dStart(m); dByte(m, 0xC0 | (g & 0x1F)); dByte(m, data); dStop(m); }
static void dFlush(uint8_t m) {
    // Steady-state refresh = just the 24 grid frames [addr,data]. The TM1629 scans RAM
    // to the LEDs continuously, so no per-update commit is needed — stock's later updates
    // are pure grid writes. The [0x48,0x88] wake and the one-time lone 0x48 both live in
    // begin() (stock sends the lone 0x48 only after the FIRST image, never again).
    for (int g = 0; g < 24; g++) { if (m & 1) dGrid(1, g, dRam[0][g]); if (m & 2) dGrid(2, g, dRam[1][g]); }
}
static void dClearRam(uint8_t m) { for (int g = 0; g < 24; g++) { if (m & 1) dRam[0][g] = 0; if (m & 2) dRam[1][g] = 0; } }

// Exact stock cold-boot replay (diagnostic: match stock byte-for-byte, then bisect what's
// essential). Boot image decoded from RAW digital capture (digital.csv) with a from-scratch
// edge/START/STOP/ACK decoder — all 24 grids per chip verified, incl. grid 0 = 0xFE (the
// first transaction of each burst carries one spurious extra clock after START; dropping it
// yields C0 FE / 48 88, matching the walking-bit pattern). dRam[0]=line L=PA_14; dRam[1]=
// line R=PA_15. (Capture ch0->chipA->BOOT_L, ch1->chipB->BOOT_R; confirm L/R pin mapping
// against the displayed image — swap the two arrays if the halves are mirrored.)
static const uint8_t BOOT_L[24] = {
    0xFE,0x07,0xFD,0x07,0xFB,0x07,0xF7,0x07,0xEF,0x07,0xDF,0x07,
    0xBF,0x07,0x78,0x07,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00 };
static const uint8_t BOOT_R[24] = {
    0xFE,0x0F,0xFD,0x0F,0xFB,0x0F,0xF7,0x0F,0xEF,0x0F,0xDF,0x0F,
    0xBF,0x0F,0x7F,0x07,0xFF,0x06,0xFF,0x05,0xE0,0x03,0x7F,0x00 };
static void dStockBoot() {
    dInit();
    dStart(3); dByte(3, 0x48); dByte(3, 0x88); dStop(3);          // init: data-mode + display ON (dim, bri 0)
    for (int g = 0; g < 24; g++) { dRam[0][g] = BOOT_L[g]; dRam[1][g] = BOOT_R[g]; }
    for (int g = 0; g < 24; g++) { dGrid(1, g, dRam[0][g]); dGrid(2, g, dRam[1][g]); }
    dStart(3); dByte(3, 0x48); dByte(3, 0x9F); dStop(3);          // reveal: data-mode + display ON max bri (0x9F as captured)
}

// ---- (x,y) -> driver cell : VERBATIM from pinmap xy2cell() (verified map) --
static bool xy2cell(int x, int y, uint8_t* mask, int* grid, int* bit) {
    if (x < 0 || x > 27 || y < 0 || y > 6) return false;
    static const uint8_t MAP_L[7][28] = {
        /* y0 */ { 0xB6,0x6B,0x60,0x61,0x62,0x63,0x64,0x65,0x67,0x68,0x69,0x6A,0x76,0x86,0x96,0xA7,0xA5,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF },
        /* y1 */ { 0xB5,0x5B,0x50,0x51,0x52,0x53,0x54,0x56,0x57,0x58,0x59,0x5A,0x75,0x85,0x95,0x97,0xA6,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF },
        /* y2 */ { 0xB4,0x4B,0x40,0x41,0x42,0x43,0x45,0x46,0x47,0x48,0x49,0x4A,0x74,0x84,0x94,0xA9,0xA8,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF },
        /* y3 */ { 0xB3,0x3B,0x30,0x31,0x32,0x34,0x35,0x36,0x37,0x38,0x39,0x3A,0x73,0x83,0x93,0x9A,0x98,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF },
        /* y4 */ { 0xB2,0x2B,0x20,0x21,0x23,0x24,0x25,0x26,0x27,0x28,0x29,0x2A,0x72,0x82,0x92,0x7A,0x8A,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF },
        /* y5 */ { 0xB1,0x1B,0x10,0x12,0x13,0x14,0x15,0x16,0x17,0x18,0x19,0x1A,0x71,0x81,0x91,0x89,0x87,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF },
        /* y6 */ { 0xB0,0x0B,0x01,0x02,0x03,0x04,0x05,0x06,0x07,0x08,0x09,0x0A,0x70,0x80,0x90,0x79,0x78,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF },
    };
    uint8_t v = MAP_L[y][x];
    if (v != 0xFF) { *mask = 1; *grid = v >> 3; *bit = v & 7; return true; }
    // line R (x17-27): mirror of line L (localX = 28-x) on PA_15, same grid/bit.
    if (x == 27) { *mask = 2; *grid = (y >= 4) ? 15 : 14; *bit = 6 - y; return true; }   // split vertical
    if (x >= 17 && x <= 26) {
        uint8_t vr = MAP_L[y][28 - x];
        if (vr != 0xFF) { *mask = 2; *grid = vr >> 3; *bit = vr & 7; return true; }
    }
    return false;
}
static bool dSetXY(int x, int y) {
    uint8_t m; int g, b;
    if (!xy2cell(x, y, &m, &g, &b)) return false;
    if (m & 1) dRam[0][g] |= (1 << b);
    if (m & 2) dRam[1][g] |= (1 << b);
    return true;
}

// ---- 5x7 font (verbatim; col-major, bit0=top) ----------------------------
static const char FONT_CHARS[] = " ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789.-:!";
static const uint8_t FONT5x7[][5] = {
    { 0x00,0x00,0x00,0x00,0x00 },{ 0x7E,0x09,0x09,0x09,0x7E },{ 0x7F,0x49,0x49,0x49,0x36 },
    { 0x3E,0x41,0x41,0x41,0x22 },{ 0x7F,0x41,0x41,0x22,0x1C },{ 0x7F,0x49,0x49,0x49,0x41 },
    { 0x7F,0x09,0x09,0x09,0x01 },{ 0x3E,0x41,0x49,0x49,0x3A },{ 0x7F,0x08,0x08,0x08,0x7F },
    { 0x00,0x41,0x7F,0x41,0x00 },{ 0x20,0x40,0x41,0x3F,0x01 },{ 0x7F,0x08,0x14,0x22,0x41 },
    { 0x7F,0x40,0x40,0x40,0x40 },{ 0x7F,0x02,0x04,0x02,0x7F },{ 0x7F,0x02,0x04,0x08,0x7F },
    { 0x3E,0x41,0x41,0x41,0x3E },{ 0x7F,0x09,0x09,0x09,0x06 },{ 0x3E,0x41,0x51,0x21,0x5E },
    { 0x7F,0x09,0x19,0x29,0x46 },{ 0x46,0x49,0x49,0x49,0x31 },{ 0x01,0x01,0x7F,0x01,0x01 },
    { 0x3F,0x40,0x40,0x40,0x3F },{ 0x1F,0x20,0x40,0x20,0x1F },{ 0x7F,0x20,0x10,0x20,0x7F },
    { 0x63,0x14,0x08,0x14,0x63 },{ 0x03,0x04,0x78,0x04,0x03 },{ 0x61,0x51,0x49,0x45,0x43 },
    { 0x3E,0x51,0x49,0x45,0x3E },{ 0x00,0x42,0x7F,0x40,0x00 },{ 0x42,0x61,0x51,0x49,0x46 },
    { 0x21,0x41,0x45,0x4B,0x31 },{ 0x18,0x14,0x12,0x7F,0x10 },{ 0x27,0x45,0x45,0x45,0x39 },
    { 0x3E,0x49,0x49,0x49,0x30 },{ 0x01,0x71,0x09,0x05,0x03 },{ 0x36,0x49,0x49,0x49,0x36 },
    { 0x06,0x49,0x49,0x49,0x3E },{ 0x00,0x00,0x60,0x60,0x00 },{ 0x00,0x08,0x08,0x08,0x00 },
    { 0x00,0x00,0x36,0x36,0x00 },{ 0x00,0x00,0x5F,0x00,0x00 },
};
static int fontIndex(char c) {
    if (c >= 'a' && c <= 'z') c -= 32;
    for (int i = 0; FONT_CHARS[i]; i++) if (FONT_CHARS[i] == c) return i;
    return 0;
}

// ---- scroll state (non-blocking marquee) ---------------------------------
static uint8_t  g_cols[256];
static int      g_colW   = 0;
static int      g_off    = 0;
static uint32_t g_next   = 0;
static bool     g_scroll = false;
static char     g_msg[40] = {0};

static void buildCols(const char* s) {
    g_colW = 0;
    for (const char* p = s; *p && g_colW < (int)sizeof(g_cols) - 6; p++) {
        const uint8_t* g = FONT5x7[fontIndex(*p)];
        for (int c = 0; c < 5; c++) g_cols[g_colW++] = g[c];
        g_cols[g_colW++] = 0;                       // 1-col gap
    }
}

// ---- public API ----------------------------------------------------------
// Init/wake is dCtrl = [0x48,0x88|bri] (data-mode + display ON); steady-state refresh is
// dFlush = grid [addr,data] writes only (chip scans RAM continuously, no per-frame
// preamble needed — matches stock). "Blank" = zero RAM (chip stays on, all segments off),
// not display-off.
void Display::begin() {
    // Stock's cold-boot sequence byte-for-byte (init + image + 0x9F reveal). This is the
    // sequence that finally lights the panel from cold — DO NOT alter it. (Bisect later to
    // find the minimal essential step, one change at a time, never losing the working state.)
    dStockBoot();
    // Then blank the boot image so we come up dark (alert surface). dStockBoot already
    // initialised + turned the chips on, so a grids-only dFlush of zero RAM shows nothing.
    dClearRam(3); dFlush(3);
    g_on = true;
}
void Display::on(uint8_t bri)  { if (bri > 7) bri = 7; g_bri = bri; dCtrl(3, g_bri); dFlush(3); g_on = true; }
void Display::off()            { dOff(3); g_on = false; }
void Display::setPixel(int x, int y) { dSetXY(x, y); }
void Display::flush()          { dFlush(3); }

void Display::clear() {
    g_scroll = false; g_msg[0] = 0;
    dClearRam(3); dFlush(3); g_on = true;
}

void Display::showText(const char* s) {
    g_scroll = false;
    strncpy(g_msg, s, sizeof(g_msg) - 1); g_msg[sizeof(g_msg) - 1] = 0;
    dClearRam(3);
    int x = 0;
    for (const char* p = s; *p && x < 28; p++) {
        const uint8_t* g = FONT5x7[fontIndex(*p)];
        for (int c = 0; c < 5 && x < 28; c++, x++) {
            uint8_t col = g[c];
            for (int r = 0; r < 7; r++) if ((col >> r) & 1) dSetXY(x, 6 - r);
        }
        x++;                                        // gap between chars
    }
    dFlush(3); g_on = true;
}

void Display::scroll(const char* s) {
    strncpy(g_msg, s, sizeof(g_msg) - 1); g_msg[sizeof(g_msg) - 1] = 0;
    buildCols(s);
    g_off = -28; g_next = millis(); g_scroll = (g_colW > 0); g_on = true;
}

void Display::update() {
    if (!g_scroll) return;
    if ((int32_t)(millis() - g_next) < 0) return;
    g_next = millis() + 55;
    dClearRam(3);
    for (int sx = 0; sx < 28; sx++) {
        int src = g_off + sx;
        if (src < 0 || src >= g_colW) continue;
        uint8_t col = g_cols[src];
        for (int r = 0; r < 7; r++) if ((col >> r) & 1) dSetXY(sx, 6 - r);
    }
    dFlush(3);
    if (++g_off > g_colW) g_off = -28;              // loop the marquee
}

// ---- harness -------------------------------------------------------------
static long qn(const String& q, const char* k, long def) { String v = httpGetParam(q, k); return v.length() ? strtol(v.c_str(), nullptr, 0) : def; }

static String cmdOn(const String& q)   { Display::on((uint8_t)qn(q, "bri", 7)); return "{\"on\":true}"; }
static String cmdOff(const String&)    { Display::off(); return "{\"on\":false}"; }
static String cmdClear(const String&)  { Display::clear(); return "{\"ok\":true}"; }
static String cmdBright(const String& q){ Display::on((uint8_t)qn(q, "b", g_bri)); String s="{\"bri\":"; s+=g_bri; s+="}"; return s; }

static String cmdText(const String& q) {
    String s = httpGetParam(q, "s");
    if (s.length() == 0) return "{\"error\":\"need s=\"}";
    Display::showText(s.c_str());
    return String("{\"ok\":true,\"text\":\"") + s + "\"}";
}
static String cmdScroll(const String& q) {
    String s = httpGetParam(q, "s");
    if (s.length() == 0) return "{\"error\":\"need s=\"}";
    Display::scroll(s.c_str());
    return String("{\"ok\":true,\"scroll\":\"") + s + "\"}";
}
static String cmdPx(const String& q) {
    int x = (int)qn(q, "x", 0), y = (int)qn(q, "y", 0);
    g_scroll = false; dClearRam(3);
    bool ok = dSetXY(x, y);
    dFlush(3); g_on = true;
    String s = "{\"ok\":"; s += ok ? "true" : "false"; s += ",\"x\":"; s += x; s += ",\"y\":"; s += y; s += "}";
    return s;
}
static String cmdBorder(const String&) {
    g_scroll = false; dClearRam(3);
    int n = 0;
    for (int x = 0; x < 28; x++) { if (dSetXY(x, 0)) n++; if (dSetXY(x, 6)) n++; }
    for (int y = 0; y < 7; y++)  { if (dSetXY(0, y)) n++; if (dSetXY(27, y)) n++; }
    dFlush(3); g_on = true;
    String s = "{\"ok\":true,\"lit\":"; s += n; s += "}";
    return s;
}
static String cmdFill(const String&) {
    g_scroll = false;
    for (int g = 0; g < 24; g++) { dRam[0][g] = 0xFF; dRam[1][g] = 0xFF; }   // RAW: bypasses the xy map
    dFlush(3); g_on = true;
    return "{\"ok\":true,\"mode\":\"raw all-grids 0xFF\"}";
}
static String cmdDrive(const String& q) {
    g_pushpull = qn(q, "pp", g_pushpull ? 1 : 0) != 0;
    dispUp = false; dInit();        // reconfigure the pads ONCE for the new drive mode (not per-transaction)
    String s = "{\"drive\":\""; s += g_pushpull ? "push-pull" : "open-drain"; s += "\"}";
    return s;
}
static String cmdRate(const String& q) {
    long us = qn(q, "us", g_halfUs); if (us < 1) us = 1; if (us > 500) us = 500;
    g_halfUs = (uint32_t)us;
    String s = "{\"rate_us\":"; s += g_halfUs; s += "}";
    return s;
}
// Sample-accurate replay of stock's captured cold-boot init (timing + content + levels).
// Drives PA_13/14/15 push-pull to reproduce the exact waveform from digital.csv frame 0.
// Diagnostic: if this lights a cold panel, the PA_13/14/15 signal is sufficient and we
// bisect; if even this fails cold, the signal alone isn't enough (power/sequencing/other).
static String cmdReplay(const String&) {
    g_scroll = false;
    gpio_init(&dScl, D_SCL); gpio_init(&dSdaL, D_SDAL); gpio_init(&dSdaR, D_SDAR);
    gpio_dir(&dScl, PIN_OUTPUT); gpio_dir(&dSdaL, PIN_OUTPUT); gpio_dir(&dSdaR, PIN_OUTPUT);  // push-pull
    for (uint32_t i = 0; i < REPL_N; i++) {
        uint8_t s = REPL_STATE[i];
        gpio_write(&dScl,  s & 1);
        gpio_write(&dSdaL, (s >> 1) & 1);
        gpio_write(&dSdaR, (s >> 2) & 1);
        uint16_t d = REPL_DT[i];
        if (d) delayMicroseconds(d);
    }
    dispUp = false;          // next normal op re-inits the pins
    g_on = true;
    return String("{\"ok\":true,\"transitions\":") + REPL_N + "}";
}
static String cmdCmd(const String& q) {     // send a command frame to both chips: v, optional v2 (second byte)
    long v  = qn(q, "v",  -1);
    long v2 = qn(q, "v2", -1);
    if (v < 0 || v > 255) return "{\"error\":\"need v=<hex>; optional v2=<hex> for a 2-byte frame\"}";
    dStart(3); dByte(3, (uint8_t)v); if (v2 >= 0 && v2 <= 255) dByte(3, (uint8_t)v2); dStop(3);
    String s = "{\"v\":"; s += (int)v; s += ",\"v2\":"; s += (int)v2; s += "}";
    return s;
}

static void stateDisplay(String& out) {
    out += "\"display\":{\"on\":"; out += g_on ? "true" : "false";
    out += ",\"bri\":"; out += g_bri;
    out += ",\"drive\":\""; out += (g_pushpull ? "push-pull" : "open-drain");
    out += "\",\"rate_us\":"; out += g_halfUs;
    out += ",\"scrolling\":"; out += g_scroll ? "true" : "false";
    out += ",\"msg\":\""; out += g_msg; out += "\"}";
}

void displayInit() {
    Display::begin();
    regAddState(stateDisplay);
    regAddCommand("disp.on",     cmdOn,     "bri:0-7", "display ON at brightness");
    regAddCommand("disp.off",    cmdOff,    "",        "display OFF");
    regAddCommand("disp.clear",  cmdClear,  "",        "blank the panel");
    regAddCommand("disp.bright", cmdBright, "b:0-7",   "set brightness");
    regAddCommand("disp.text",   cmdText,   "s:str",   "static text from left (~4 chars)");
    regAddCommand("disp.scroll", cmdScroll, "s:str",   "scrolling marquee (loops)");
    regAddCommand("disp.px",     cmdPx,     "x:0-27,y:0-6", "light one (x,y) via verified map");
    regAddCommand("disp.border", cmdBorder, "",        "draw the panel border (map check)");
    regAddCommand("disp.fill",   cmdFill,   "",        "light every dot (raw, bypasses map)");
    regAddCommand("disp.drive",  cmdDrive,  "pp:0/1",  "drive mode: 0=open-drain, 1=push-pull");
    regAddCommand("disp.rate",   cmdRate,   "us:int",  "bit half-period us (default 6; raise to slow the bus)");
    regAddCommand("disp.cmd",    cmdCmd,    "v:hex,v2:hex", "send a 2-byte command frame to both chips");
    regAddCommand("disp.replay", cmdReplay, "",        "replay stock's exact captured cold-boot init (timing+content+levels)");
}
