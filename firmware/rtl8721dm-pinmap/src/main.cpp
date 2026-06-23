/*
 * RTL8721DM GPIO explorer — board-mapping tool for the Petlibro feeder.
 *
 * Runs on the feeder's own RTL8721DM (flashed via the Pico bridge) and probes
 * every GPIO over the LOGUART console to discover what Petlibro wired where:
 * the RFID-module UART, the lid motor driver, buttons, sensors, the tag-ready
 * IRQ, power-enable lines, etc.
 *
 * It talks the raw mbed gpio_t HAL with PinName values directly, NOT the Arduino
 * digitalRead/pinMode (those index the SparkFun variant's fixed table:
 * digitalRead(5) -> PA_26, not PA_5 — useless for scanning arbitrary pins).
 * PinName = (port<<5)|num : PA_0..31 = 0..31, PB_0..31 = 32..63.
 *
 * ONLY the 34 GPIOs actually bonded on the RTL8721DM QFN68 are touched (datasheet
 * UM0401 Fig 2-3). The unbonded enum entries are skipped — some of PB_8..PB_21
 * are the internal MCM SPI-flash pads, and re-muxing one HARD-HANGS the chip
 * (kills XIP). Pin alt-functions below are from the datasheet pin table.
 *
 * SAFETY
 *   * PA_7 / PA_8 = LOGUART (this console) -> hard-blocked.
 *   * PA_27 (SWD_DATA / NORMAL_MODE_SEL strap), PA_30 (SPS_SEL = 1.1V reg mode),
 *     PB_3 (SWD_CLK by default) -> driving needs `force`.
 *   * Reading is safe (high-Z input can't drive the board). Driving may move a
 *     motor/rail; reset undoes it. Recover from any hang with `reset` on CDC1.
 */
#include <Arduino.h>
#include <Wire.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
extern "C" {
  #include "PinNames.h"
  #include "gpio_api.h"
  // ROM helper: reads a pin's input level WITHOUT re-muxing the pad or touching
  // pad-control regs. Lets us read the HAL-unsupported pins (PB_13/14/16/17) and
  // even unbonded/flash pads safely (no re-mux -> can't kill XIP). Meaningful
  // only for pads left at their GPIO-input default (datasheet Table 2-3).
  uint32_t GPIO_ReadDataBit(uint32_t GPIO_Pin);
  // Write-side twins: these act on the GPIO IP registers (port/pin arithmetic,
  // no pad/pinmux lookup), so they're safe on PB_13/14/16/17 too — unlike
  // gpio_init/GPIO_Init, whose pinmux+PAD-pull config traps on those pads.
  void GPIO_WriteBit(uint32_t GPIO_Pin, uint32_t BitVal);
  void GPIO_Direction(uint32_t GPIO_Pin, uint32_t dir);   // 0 = input, 1 = output
  #include "i2c_api.h"            // i2c_t + i2c_init / i2c_frequency (mbed HAL)
  #include "analogin_api.h"       // analogin_t + analogin_init / analogin_read_u16 (ADC)
  #include "pwmout_api.h"         // pwmout_t + pwmout_init / pwmout_write (PA_28=PWM6, PA_30=PWM7)
  // Ameba timeout variants (not in the mbed header) so an I2C scan can't hang:
  int i2c_read_timeout(i2c_t *o, int addr, char *d, int len, int stop, int ms);
  int i2c_write_timeout(i2c_t *o, int addr, char *d, int len, int stop, int ms);
}

// The framework's i2c_api.o references usi_i2c_reset() in its USI-I2C branch, but
// the USI source isn't linked in this build. We only ever use I2C0 (PB_6/PB_5),
// never the USI path, so a no-op stub resolves the link; it is never called.
extern "C" void usi_i2c_reset(void) {}

// --------------------------------------------------------------------------
// The 34 GPIOs bonded on the RTL8721DM QFN68 (UM0401 Figure 2-3 + pin tables).
// --------------------------------------------------------------------------
struct Pin { PinName pin; const char* alt; bool block; bool caution; };

static const Pin PINS[] = {
  // ---- Port A ----
  { PA_0,  "MIC_BIAS / GPIO",            false, false },
  { PA_2,  "GPIO",                       false, false },
  { PA_4,  "GPIO",                       false, false },
  { PA_7,  "LOGUART_TX (console)",       true,  false },
  { PA_8,  "LOGUART_RX (console)",       true,  false },
  { PA_12, "KEY_ROW0 / ICFG0",           false, false },
  { PA_13, "KEY_ROW1 / ICFG1",           false, false },
  { PA_14, "KEY_ROW2 / RTC_OUT / ICFG2", false, false },
  { PA_15, "KEY_ROW3 / EXT32K / ICFG3",  false, false },
  { PA_16, "KEY_ROW4",                   false, false },
  { PA_17, "KEY_ROW6",                   false, false },
  { PA_18, "KEY_ROW5 / RTC_OUT",         false, false },
  { PA_19, "KEY_COL2",                   false, false },
  { PA_25, "KEY_COL1",                   false, false },
  { PA_26, "KEY_COL0",                   false, false },
  { PA_27, "SWD_DATA / NORMAL_MODE_SEL", false, true  },
  { PA_28, "PWM6",                       false, false },
  { PA_30, "PWM7 / SPS_SEL (reg mode)",  false, true  },
  // ---- Port B ----
  { PB_1,  "ADC_CH4",                    false, false },
  { PB_2,  "ADC_CH5",                    false, false },
  { PB_3,  "ADC_CH6 / SWD_CLK",          false, true  },
  { PB_4,  "TOUCH_KEY0 / ADC_CH0",       false, false },
  { PB_5,  "TOUCH_KEY1 / ADC_CH1",       false, false },
  { PB_6,  "TOUCH_KEY2 / ADC_CH2",       false, false },
  { PB_7,  "TOUCH_KEY3 / ADC_CH3",       false, false },
  // PB_13/14/16/17 are bonded GPIOs. gpio_init/GPIO_Init traps on their PAD
  // config (secure-domain pads), so they're driven/read via the raw GPIO IP ROM
  // calls instead (see rawOnly()/rawDrive()/readPin) — no internal pull control.
  { PB_13, "GPIO (raw IP, no pull)",     false, false },
  { PB_14, "GPIO (raw IP, no pull)",     false, false },
  { PB_16, "GPIO (raw IP, no pull)",     false, false },
  { PB_17, "GPIO (raw IP, no pull)",     false, false },
  { PB_22, "GPIO / SD_D0",               false, false },
  { PB_23, "GPIO / SD_D1",               false, false },
  { PB_26, "GPIO",                       false, false },
  { PB_29, "GPIO",                       false, false },
  { PB_31, "GPIO",                       false, false },
};
static const int NPINS = (int)(sizeof(PINS) / sizeof(PINS[0]));

static int findPin(PinName p) {
  for (int i = 0; i < NPINS; i++) if (PINS[i].pin == p) return i;
  return -1;   // not a QFN68-bonded GPIO
}

static void pinLabel(PinName pn, char* out, size_t cap) {
  int port = ((int)pn >> 5) & 1, num = (int)pn & 31;
  snprintf(out, cap, "P%c_%d", port ? 'B' : 'A', num);
}

// Parse "PA5" / "PA_5" / "pb10" / raw 0..63 into a PinName; NC on error.
static PinName parsePin(const char* s) {
  if (!s || !*s) return NC;
  const char* p = s;
  if (*p == 'P' || *p == 'p') p++;
  if (*p == 'A' || *p == 'a' || *p == 'B' || *p == 'b') {
    int port = (*p == 'B' || *p == 'b') ? 1 : 0;
    p++;
    if (*p == '_') p++;
    if (*p < '0' || *p > '9') return NC;
    int num = atoi(p);
    if (num < 0 || num > 31) return NC;
    return (PinName)((port << 5) | num);
  }
  if (*s >= '0' && *s <= '9') {
    int v = atoi(s);
    if (v >= 0 && v < 64) return (PinName)v;
  }
  return NC;
}

static PinMode parsePull(const char* s, PinMode dflt) {
  if (!s) return dflt;
  if (!strcmp(s, "up"))   return PullUp;
  if (!strcmp(s, "down")) return PullDown;
  if (!strcmp(s, "none") || !strcmp(s, "hiz") || !strcmp(s, "z")) return PullNone;
  return dflt;
}

// --------------------------------------------------------------------------
// Raw GPIO helpers (config persists in hardware after the gpio_t goes away)
// --------------------------------------------------------------------------
static const uint32_t SETTLE_US = 200;

// PB_13/14/16/17 are bonded GPIOs, but gpio_init/GPIO_Init traps on their PAD
// config (secure-domain pads). They're still reachable through the raw GPIO IP
// ROM calls (dir/read/write), so we route them there and skip pull control
// (PAD_PullCtrl would trap). Driving only takes effect if the pad is GPIO-muxed.
static bool rawOnly(PinName pn) {
  return pn == PB_13 || pn == PB_14 || pn == PB_16 || pn == PB_17;
}
static void rawDrive(PinName pn, int val) {        // GPIO IP only — no pad/pinmux write
  GPIO_Direction((uint32_t)pn, 1 /*OUT*/);
  GPIO_WriteBit((uint32_t)pn, val ? 1 : 0);
}

static int readPin(PinName pn, PinMode pull) {
  if (rawOnly(pn)) {                               // no PAD pull available -> input + raw read
    GPIO_Direction((uint32_t)pn, 0 /*IN*/);
    delayMicroseconds(SETTLE_US);
    return GPIO_ReadDataBit((uint32_t)pn) ? 1 : 0;
  }
  gpio_t g; gpio_init(&g, pn);
  gpio_dir(&g, PIN_INPUT);
  gpio_mode(&g, pull);
  delayMicroseconds(SETTLE_US);
  return gpio_read(&g);
}
static void drivePin(PinName pn, int val) {
  if (rawOnly(pn)) { rawDrive(pn, val); return; }
  gpio_t g; gpio_init(&g, pn);
  gpio_dir(&g, PIN_OUTPUT);
  gpio_mode(&g, PullNone);
  gpio_write(&g, val ? 1 : 0);
}
// Raw input read via the ROM function — no gpio_init, no pad-control writes, no
// re-mux. Safe on any pin (incl. PB_13/14/16/17 and unbonded pads); meaningful
// for pads at their GPIO-input default.
static int rawRead(PinName pn) { return GPIO_ReadDataBit((uint32_t)pn) ? 1 : 0; }

// PU/PD test: 1/1 -> driven HIGH, 0/0 -> driven LOW, 1/0 -> floating, else odd.
static char classifyPin(PinName pn) {
  if (rawOnly(pn)) return readPin(pn, PullNone) ? 'H' : 'L';   // no PU/PD -> can't see float
  gpio_t g; gpio_init(&g, pn);
  gpio_dir(&g, PIN_INPUT);
  gpio_mode(&g, PullUp);   delayMicroseconds(SETTLE_US); int a = gpio_read(&g);
  gpio_mode(&g, PullDown); delayMicroseconds(SETTLE_US); int b = gpio_read(&g);
  if (a && b)   return 'H';
  if (!a && !b) return 'L';
  if (a && !b)  return '.';
  return '?';
}

// --------------------------------------------------------------------------
// Commands
// --------------------------------------------------------------------------
static void printHelp() {
  Serial.println(F("RTL8721DM GPIO explorer (34 bonded QFN68 pins; `list` to see them)"));
  Serial.println(F("  classify              PU/PD per pin: H=driven-hi L=driven-lo .=floating"));
  Serial.println(F("  scan [up|down|none]   read every bonded pin once (default pull=down)"));
  Serial.println(F("  rawscan               raw-read ALL bonded pins incl. PB_13-17 (no pull, no fault)"));
  Serial.println(F("  gread <pin>           raw-read one pin (works on blocked/unbonded too)"));
  Serial.println(F("  watch [up|down|none] [ms]  print only pins that CHANGE (default down,40ms)"));
  Serial.println(F("                            -> actuate a button/sensor/tag, see which pin moves"));
  Serial.println(F("  read <pin> [up|down|none]  read one pin"));
  Serial.println(F("  out <pin> 0|1 [force]      drive a pin (DANGER: may move a motor/rail)"));
  Serial.println(F("  in  <pin> [up|down|none]   set a pin back to input"));
  Serial.println(F("  aread <pin>           ADC read (PB_1..PB_7) — analog detectors / motor current"));
  Serial.println(F("  ascan                 ADC read all of PB_1..PB_7 at once"));
  Serial.println(F("  pwm PA_28|PA_30 <0-100>|off  PWM a lid-motor pin (PWM6/7) for slow drive"));
  Serial.println(F("  i2c scan              HW I2C0 scan (SDA=PB_6,SCL=PB_5) — find AW9523B"));
  Serial.println(F("  i2c id                read AW9523B ID reg 0x10 at 0x58-0x5B (expect 0x23)"));
  Serial.println(F("  i2c r <addr> <reg> [n]     read n regs (hex)"));
  Serial.println(F("  i2c w <addr> <reg> <val>   write reg (hex)"));
  Serial.println(F("  bb scan <sda> <scl>        bit-bang I2C scan on ANY pins (e.g. PA_25 PA_26)"));
  Serial.println(F("  bb r/w <sda> <scl> <addr> <reg> [n/val]   bit-bang read/write (hex)"));
  Serial.println(F("  disp on [bri]|off|clear   dot-matrix (SCL=PA_13 SDAL=PA_12 SDAR=PA_15)"));
  Serial.println(F("  disp px  L|R|B <grid 0-23> <bit 0-7>   light ONE dot (clears rest)"));
  Serial.println(F("  disp col L|R|B <grid>     whole grid=0xFF | disp bit L|R|B <bit>  one bit in all grids"));
  Serial.println(F("  disp raw L|R|B <grid> <hex> | disp fill L|R|B <hex>"));
  Serial.println(F("  disp pins <scl> <sdaL> <sdaR>   re-map the 3 lines live (def PA_13 PA_14 PA_15)"));
  Serial.println(F("  disp idx L|R|B <plane 0-7>   binary map scan: photo planes 0-7, code->grid/bit"));
  Serial.println(F("  disp cmd <hex> [L|R|B]   fire a bare command byte (probe TM1680 COM/mode: A0/A4/A8/AC)"));
  Serial.println(F("  disp xy <x> <y> | hline <y> | vline <x> | border   drive by physical (x,y) via the map"));
  Serial.println(F("  disp text <string>   scroll text across the panel (5x7 font; any key aborts)"));
  Serial.println(F("  list                  the bonded pin table + alt functions"));
  Serial.println(F("  help                  this list"));
  Serial.println(F("pins: PA0..PA31 / PB0..PB31 (or PA_5, pb10, raw 0..63). Off-table pins refused."));
  Serial.println(F("Hang? type `reset` on the bridge CDC1 to reboot this firmware."));
}

static void cmdList() {
  Serial.println(F("-- RTL8721DM QFN68 bonded GPIOs (datasheet UM0401 Fig 2-3) --"));
  char lbl[8], b[64];
  for (int i = 0; i < NPINS; i++) {
    pinLabel(PINS[i].pin, lbl, sizeof lbl);
    snprintf(b, sizeof b, "  %-6s %-26s%s", lbl, PINS[i].alt,
             PINS[i].block ? "  [BLOCKED]" : PINS[i].caution ? "  [caution: force to drive]" : "");
    Serial.println(b);
  }
  Serial.println(F("Unbonded enum pins (PB_8..21 etc.) are skipped — some are the SPI-flash"));
  Serial.println(F("pads and re-muxing them hangs XIP. Recover with `reset` on CDC1."));
}

static void cmdScan(PinMode pull) {
  const char* pn = (pull == PullUp) ? "up" : (pull == PullDown) ? "down" : "none";
  Serial.print(F("-- scan (pull=")); Serial.print(pn); Serial.println(F(") --"));
  char lbl[8], b[24];
  int lastPort = -1, col = 0;
  for (int i = 0; i < NPINS; i++) {
    int port = ((int)PINS[i].pin >> 5) & 1;
    if (port != lastPort) { if (lastPort >= 0) Serial.println(); Serial.print(port ? F("PB  ") : F("PA  ")); lastPort = port; col = 0; }
    pinLabel(PINS[i].pin, lbl, sizeof lbl);
    if (PINS[i].block) snprintf(b, sizeof b, "%-7s=--  ", lbl);
    else               snprintf(b, sizeof b, "%-7s=%d   ", lbl, readPin(PINS[i].pin, pull));
    Serial.print(b);
    if (++col == 6) { Serial.println(); Serial.print(F("    ")); col = 0; }
  }
  Serial.println();
}

static void cmdClassify(const char* startArg) {
  int start = 0;
  if (startArg) {
    PinName sp = parsePin(startArg);
    int si = (sp == NC) ? -1 : findPin(sp);
    if (si >= 0) start = si;
    else Serial.println(F("(start pin not found — starting from the beginning)"));
  }
  Serial.println(F("-- classify (PU/PD): HIGH/LOW=driven, float=unconnected --"));
  Serial.println(F("   if it FAULTS, note the dangling pin, `reset` on CDC1,"));
  Serial.println(F("   then `classify <next-pin>` to resume past it."));
  char lbl[8], b[64];
  int driven = 0;
  for (int i = start; i < NPINS; i++) {
    pinLabel(PINS[i].pin, lbl, sizeof lbl);
    snprintf(b, sizeof b, "  %-6s ", lbl);
    Serial.print(b);
    Serial.flush();                      // label out BEFORE the (possibly faulting) probe
    if (PINS[i].block) { Serial.print(F("-- skipped: ")); Serial.println(PINS[i].alt); continue; }
    char c = classifyPin(PINS[i].pin);
    snprintf(b, sizeof b, "%-5s %s",
             c == 'H' ? "HIGH" : c == 'L' ? "LOW" : c == '.' ? "float" : "ODD", PINS[i].alt);
    Serial.println(b);
    if (c == 'H' || c == 'L' || c == '?') driven++;
  }
  snprintf(b, sizeof b, "-- classify complete: %d driven --", driven);
  Serial.println(b);
}

// Raw-read every bonded pin (INCLUDING the HAL-blocked PB_13/14/16/17) via the
// ROM read — full coverage, no faults. Levels reflect whatever's driving the pad
// right now (no pull applied). Flushes the label before each read so a fault (if
// any) is attributable.
static void cmdRawScan() {
  Serial.println(F("-- rawscan: GPIO_ReadDataBit on every bonded pin (incl. PB_13-17) --"));
  char lbl[8], b[20];
  int lastPort = -1, col = 0;
  for (int i = 0; i < NPINS; i++) {
    int port = ((int)PINS[i].pin >> 5) & 1;
    if (port != lastPort) { if (lastPort >= 0) Serial.println(); Serial.print(port ? F("PB  ") : F("PA  ")); lastPort = port; col = 0; }
    pinLabel(PINS[i].pin, lbl, sizeof lbl);
    Serial.flush();
    int v = rawRead(PINS[i].pin);
    snprintf(b, sizeof b, "%-7s=%d   ", lbl, v);
    Serial.print(b);
    if (++col == 6) { Serial.println(); Serial.print(F("    ")); col = 0; }
  }
  Serial.println();
  Serial.println(F("(raw level only — no pull, so a floating pin reads arbitrarily)"));
}

static void cmdGread(const char* arg) {
  PinName p = parsePin(arg);
  if (p == NC) { Serial.println(F("usage: gread <pin>  (raw read; works on any pin incl. blocked)")); return; }
  char lbl[8], b[40]; pinLabel(p, lbl, sizeof lbl);
  Serial.print(F("  ")); Serial.print(lbl); Serial.print(F(" rawread "));
  Serial.flush();
  int v = rawRead(p);
  snprintf(b, sizeof b, "= %d", v);
  Serial.println(b);
}

static void cmdWatch(PinMode pull, uint32_t periodMs) {
  static int last[64];
  const char* pn = (pull == PullUp) ? "up" : (pull == PullDown) ? "down" : "none";
  Serial.print(F("-- watch (pull=")); Serial.print(pn);
  Serial.print(F(", ")); Serial.print(periodMs); Serial.println(F("ms) — press Enter to stop --"));
  // LOGUART pins (PA_7/8) toggle with our own console TX → skip. Other blocked
  // pins (PB_13-17) can't take a pull, so read them raw (no-pull) instead.
  for (int i = 0; i < NPINS; i++) {
    bool console = (PINS[i].pin == PA_7 || PINS[i].pin == PA_8);
    last[i] = console ? -1 : PINS[i].block ? rawRead(PINS[i].pin) : readPin(PINS[i].pin, pull);
  }
  Serial.println(F("baseline captured; now actuate something... (PB_13-17 read raw/no-pull)"));

  char b[48], lbl[8];
  while (!Serial.available()) {
    for (int i = 0; i < NPINS; i++) {
      if (PINS[i].pin == PA_7 || PINS[i].pin == PA_8) continue;
      int v = PINS[i].block ? rawRead(PINS[i].pin) : readPin(PINS[i].pin, pull);
      if (v != last[i]) {
        pinLabel(PINS[i].pin, lbl, sizeof lbl);
        snprintf(b, sizeof b, "[%8lu] %-6s %d -> %d  %s",
                 (unsigned long)millis(), lbl, last[i], v, PINS[i].alt);
        Serial.println(b);
        last[i] = v;
      }
    }
    delay(periodMs);
  }
  while (Serial.available()) Serial.read();
  Serial.println(F("-- watch stopped --"));
}

// Resolve + guard a pin argument. Returns table index, or -1 (message printed).
static int resolvePin(const char* arg) {
  PinName p = parsePin(arg);
  if (p == NC) { Serial.println(F("bad pin (e.g. PA_5, pb10, or raw 0..63)")); return -1; }
  int i = findPin(p);
  if (i < 0) { Serial.println(F("refused: not a QFN68-bonded GPIO (try 'list')")); return -1; }
  if (PINS[i].block) { Serial.println(F("refused: LOGUART console pin")); return -1; }
  return i;
}

static void cmdRead(const char* arg, const char* pullArg) {
  int i = resolvePin(arg); if (i < 0) return;
  PinMode pull = parsePull(pullArg, PullNone);
  char lbl[8], b[64]; pinLabel(PINS[i].pin, lbl, sizeof lbl);
  snprintf(b, sizeof b, "%s = %d  (pull=%s)  %s", lbl, readPin(PINS[i].pin, pull),
           pull == PullUp ? "up" : pull == PullDown ? "down" : "none", PINS[i].alt);
  Serial.println(b);
}

static void cmdOut(const char* arg, const char* valArg, const char* forceArg) {
  if (!valArg) { Serial.println(F("usage: out <pin> 0|1 [force]")); return; }
  int i = resolvePin(arg); if (i < 0) return;
  bool force = forceArg && !strcmp(forceArg, "force");
  if (PINS[i].caution && !force) {
    char b[80]; snprintf(b, sizeof b, "refused: %s is a strap/SWD pin — append 'force' if intended", PINS[i].alt);
    Serial.println(b);
    return;
  }
  int v = (atoi(valArg) != 0) ? 1 : 0;
  char lbl[8], b[72]; pinLabel(PINS[i].pin, lbl, sizeof lbl);
  snprintf(b, sizeof b, "DRIVING %s = %d  (%s; reset to undo)", lbl, v, PINS[i].alt);
  Serial.println(b);
  drivePin(PINS[i].pin, v);
}

static void cmdIn(const char* arg, const char* pullArg) {
  int i = resolvePin(arg); if (i < 0) return;
  PinMode pull = parsePull(pullArg, PullNone);
  char lbl[8], b[48]; pinLabel(PINS[i].pin, lbl, sizeof lbl);
  snprintf(b, sizeof b, "%s -> input, reads %d", lbl, readPin(PINS[i].pin, pull));
  Serial.println(b);
}

// --------------------------------------------------------------------------
// Analog (ADC) reads. The photoelectric DETECTORS are phototransistors (analog)
// and the motor current shunts are analog, so digital reads miss them. ADC pins
// (PinMap_ADC): PB_1=CH4 PB_2=CH5 PB_3=CH6 PB_4=CH0 PB_5=CH1 PB_6=CH2 PB_7=CH3.
// analogin_init asserts on non-ADC pins, so we guard to those seven.
// --------------------------------------------------------------------------
static bool isADC(PinName p) {
  return p == PB_1 || p == PB_2 || p == PB_3 || p == PB_4 ||
         p == PB_5 || p == PB_6 || p == PB_7;
}
static uint16_t areadRaw(PinName p) {
  analogin_t a; analogin_init(&a, p);
  return analogin_read_u16(&a);
}
static void cmdARead(const char* arg) {
  PinName p = parsePin(arg);
  if (p == NC || !isADC(p)) { Serial.println(F("aread: ADC pins only: PB_1 PB_2 PB_3 PB_4 PB_5 PB_6 PB_7")); return; }
  char lbl[8], b[48]; pinLabel(p, lbl, sizeof lbl);
  uint16_t v = areadRaw(p);
  snprintf(b, sizeof b, "%s = %u  (%lu%% fs)", lbl, v, (unsigned long)((uint32_t)v * 100 / 65535));
  Serial.println(b);
}
static void cmdAScan() {
  static const PinName adc[] = { PB_1, PB_2, PB_3, PB_4, PB_5, PB_6, PB_7 };
  Serial.println(F("-- ADC scan (raw u16 0..65535) --"));
  char lbl[8], b[48];
  for (unsigned i = 0; i < sizeof(adc) / sizeof(adc[0]); i++) {
    uint16_t v = areadRaw(adc[i]);
    pinLabel(adc[i], lbl, sizeof lbl);
    snprintf(b, sizeof b, "  %-6s = %5u  (%lu%% fs)", lbl, v, (unsigned long)((uint32_t)v * 100 / 65535));
    Serial.println(b);
  }
}

// --------------------------------------------------------------------------
// PWM the lid-motor pins (PA_28=PWM6, PA_30=PWM7) for SLOW, controlled drive so
// the lid eases into its optical endstop instead of slamming the hardstop at full
// speed. `pwm PA_28 35` = 35% duty @1kHz; `pwm PA_28 off` = free PWM + drive LOW.
// Motor: PWM the active-direction pin, hold the other LOW; STOP = both LOW.
// --------------------------------------------------------------------------
static pwmout_t pwm28, pwm30;
static bool pwm28up = false, pwm30up = false;
static void pwmStop(PinName p) {
  if (p == PA_28 && pwm28up) { pwmout_free(&pwm28); pwm28up = false; }
  if (p == PA_30 && pwm30up) { pwmout_free(&pwm30); pwm30up = false; }
  drivePin(p, 0);                                   // GPIO LOW after freeing PWM
}
static void cmdPwm(const char* arg, const char* dutyArg) {
  PinName p = parsePin(arg);
  if (p != PA_28 && p != PA_30) { Serial.println(F("pwm: PA_28 (PWM6) or PA_30 (PWM7) only")); return; }
  if (dutyArg && !strcmp(dutyArg, "off")) { pwmStop(p); Serial.println(F("pwm off (pin driven LOW)")); return; }
  if (!dutyArg) { Serial.println(F("usage: pwm PA_28|PA_30 <0-100> | off")); return; }
  int duty = atoi(dutyArg); if (duty < 0) duty = 0; if (duty > 100) duty = 100;
  pwmout_t* o = (p == PA_28) ? &pwm28 : &pwm30;
  bool* up = (p == PA_28) ? &pwm28up : &pwm30up;
  if (!*up) { pwmout_init(o, p); pwmout_period_us(o, 1000); *up = true; }   // 1 kHz
  pwmout_write(o, duty / 100.0f);
  char b[40]; snprintf(b, sizeof b, "pwm %s = %d%% @1kHz", arg, duty); Serial.println(b);
}

// --------------------------------------------------------------------------
// Hardware I2C0 master on PB_6 (SDA) / PB_5 (SCL) — the AW9523B expander's bus
// (the only bonded I2C0 pin pair besides the RFID-UART pins PA_25/26). Uses the
// mbed i2c HAL directly: real ACK/NACK return codes, and timeout variants so a
// scan can't hang on a dead address. AW9523B lives at 0x58..0x5B; its ID
// register 0x10 reads 0x23.
//   i2c scan | i2c id | i2c r <addr> <reg> [n] | i2c w <addr> <reg> <val>  (hex)
// --------------------------------------------------------------------------
static i2c_t i2cExp;
static bool  i2cExpUp = false;
static void i2cExpEnsure() {
  if (!i2cExpUp) { i2c_init(&i2cExp, PB_6, PB_5); i2c_frequency(&i2cExp, 100000); i2cExpUp = true; }
}
static bool i2cAck(int addr7) {                          // address-only probe
  return i2c_write_timeout(&i2cExp, addr7, nullptr, 0, 1, 4) == 0;
}
static int i2cReadReg(int addr7, uint8_t reg, uint8_t* buf, int n) {
  char r = (char)reg;
  if (i2c_write_timeout(&i2cExp, addr7, &r, 1, 0, 8) != 1) return -1;   // reg pointer, repeated start
  return i2c_read_timeout(&i2cExp, addr7, (char*)buf, n, 1, 20);
}

static void handleI2c(const char* sub, const char* aA, const char* aR) {
  if (!sub) { Serial.println(F("usage: i2c scan | i2c id | i2c r <addr> <reg> [n] | i2c w <addr> <reg> <val>  (hex; I2C0 SDA=PB_6 SCL=PB_5)")); return; }
  i2cExpEnsure();
  char b[48];

  if (!strcmp(sub, "scan")) {
    Serial.println(F("i2c scan (HW I2C0: SDA=PB_6, SCL=PB_5):"));
    int n = 0;
    for (int a = 0x08; a <= 0x77; a++)
      if (i2cAck(a)) { snprintf(b, sizeof b, " 0x%02X", a); Serial.print(b); n++; }
    snprintf(b, sizeof b, "  (%d found)", n);
    Serial.println(b);
  } else if (!strcmp(sub, "id")) {
    for (int a = 0x58; a <= 0x5B; a++) {
      uint8_t id = 0; int r = i2cReadReg(a, 0x10, &id, 1);
      if (r == 1) snprintf(b, sizeof b, "  0x%02X reg0x10 = 0x%02X%s", a, id, id == 0x23 ? "   <-- AW9523B" : "");
      else        snprintf(b, sizeof b, "  0x%02X reg0x10 = (no ack)", a);
      Serial.println(b);
    }
  } else if (!strcmp(sub, "r")) {
    if (!aA || !aR) { Serial.println(F("usage: i2c r <addr> <reg> [n]")); return; }
    char* aN = strtok(NULL, " \t");
    int addr = (int)strtoul(aA, 0, 16); uint8_t reg = (uint8_t)strtoul(aR, 0, 16);
    int n = aN ? atoi(aN) : 1; if (n < 1) n = 1; if (n > 16) n = 16;
    uint8_t buf[16]; int got = i2cReadReg(addr, reg, buf, n);
    if (got < 1) { Serial.println(F("i2c r: no ACK")); return; }
    Serial.print(F("rx:"));
    for (int i = 0; i < got; i++) { snprintf(b, sizeof b, " %02X", buf[i]); Serial.print(b); }
    Serial.println();
  } else if (!strcmp(sub, "w")) {
    if (!aA || !aR) { Serial.println(F("usage: i2c w <addr> <reg> <val>")); return; }
    char* aV = strtok(NULL, " \t");
    if (!aV) { Serial.println(F("usage: i2c w <addr> <reg> <val>")); return; }
    int addr = (int)strtoul(aA, 0, 16);
    char wbuf[2] = { (char)strtoul(aR, 0, 16), (char)strtoul(aV, 0, 16) };
    int r = i2c_write_timeout(&i2cExp, addr, wbuf, 2, 1, 20);
    snprintf(b, sizeof b, "i2c w: %s (r=%d)", r == 2 ? "ACK" : "no-ACK", r);
    Serial.println(b);
  } else {
    Serial.println(F("usage: i2c scan | i2c id | i2c r <addr> <reg> [n] | i2c w <addr> <reg> <val>"));
  }
}

// --------------------------------------------------------------------------
// Bit-bang I2C on ARBITRARY pins (not constrained by the HW I2C pinmux like
// Wire). Reliable selective ACK detection (a real device pulls SDA low). Use
// when the bus isn't on Wire's fixed SDA=PA_26/SCL=PA_25 assignment.
//   bb scan <sda> <scl> | bb r <sda> <scl> <addr> <reg> [n] | bb w <sda> <scl> <addr> <reg> <val>
// --------------------------------------------------------------------------
static gpio_t bbSda, bbScl;
static void bbInit(PinName sda, PinName scl) {
  gpio_init(&bbSda, sda);  gpio_init(&bbScl, scl);
  gpio_write(&bbSda, 0);   gpio_write(&bbScl, 0);
  gpio_mode(&bbSda, PullUp); gpio_mode(&bbScl, PullUp);
  gpio_dir(&bbSda, PIN_INPUT); gpio_dir(&bbScl, PIN_INPUT);
}
static inline void bbd() { delayMicroseconds(6); }
#define BSDA_HI() gpio_dir(&bbSda, PIN_INPUT)
#define BSDA_LO() gpio_dir(&bbSda, PIN_OUTPUT)
#define BSCL_HI() gpio_dir(&bbScl, PIN_INPUT)
#define BSCL_LO() gpio_dir(&bbScl, PIN_OUTPUT)
static void bbStart() { BSDA_HI(); BSCL_HI(); bbd(); BSDA_LO(); bbd(); BSCL_LO(); bbd(); }
static void bbStop()  { BSDA_LO(); bbd(); BSCL_HI(); bbd(); BSDA_HI(); bbd(); }
static int bbWr(uint8_t v) {
  for (int i = 0; i < 8; i++) { if (v & 0x80) BSDA_HI(); else BSDA_LO(); v <<= 1; bbd(); BSCL_HI(); bbd(); BSCL_LO(); }
  BSDA_HI(); bbd(); BSCL_HI(); bbd();
  int ack = (gpio_read(&bbSda) == 0);
  BSCL_LO(); bbd();
  return ack;
}
static uint8_t bbRd(int sendAck) {
  uint8_t v = 0; BSDA_HI();
  for (int i = 0; i < 8; i++) { bbd(); BSCL_HI(); bbd(); v = (v << 1) | (gpio_read(&bbSda) & 1); BSCL_LO(); }
  if (sendAck) BSDA_LO(); else BSDA_HI();
  bbd(); BSCL_HI(); bbd(); BSCL_LO(); bbd(); BSDA_HI();
  return v;
}

static void handleBitbang(const char* sub, const char* sdaS, const char* sclS) {
  if (!sub) { Serial.println(F("usage: bb scan|r|w <sda> <scl> [addr reg n/val]  (hex)")); return; }
  PinName sda = parsePin(sdaS), scl = parsePin(sclS);
  if (sda == NC || scl == NC || findPin(sda) < 0 || findPin(scl) < 0 ||
      PINS[findPin(sda)].block || PINS[findPin(scl)].block) {
    Serial.println(F("bb: sda/scl must be bonded, non-blocked GPIO")); return;
  }
  bbInit(sda, scl);
  char b[16];
  if (!strcmp(sub, "scan")) {
    char sl[8], cl[8]; pinLabel(sda, sl, sizeof sl); pinLabel(scl, cl, sizeof cl);
    Serial.print(F("bb scan SDA=")); Serial.print(sl); Serial.print(F(" SCL=")); Serial.print(cl); Serial.println(F(":"));
    int n = 0;
    for (int a = 0x08; a <= 0x77; a++) { bbStart(); int ack = bbWr((uint8_t)(a << 1)); bbStop(); if (ack) { snprintf(b, sizeof b, " 0x%02X", a); Serial.print(b); n++; } }
    snprintf(b, sizeof b, "  (%d found)", n); Serial.println(b);
  } else if (!strcmp(sub, "r")) {
    char* aA = strtok(NULL, " \t"); char* aR = strtok(NULL, " \t"); char* aN = strtok(NULL, " \t");
    if (!aA || !aR) { Serial.println(F("usage: bb r <sda> <scl> <addr> <reg> [n]")); return; }
    uint8_t addr = (uint8_t)strtoul(aA, 0, 16), reg = (uint8_t)strtoul(aR, 0, 16);
    int n = aN ? atoi(aN) : 1; if (n < 1) n = 1; if (n > 16) n = 16;
    bbStart(); int ok = bbWr((uint8_t)(addr << 1)); ok &= bbWr(reg);
    bbStart(); ok &= bbWr((uint8_t)((addr << 1) | 1));
    if (!ok) { bbStop(); Serial.println(F("bb r: no ACK")); return; }
    Serial.print(F("rx:"));
    for (int i = 0; i < n; i++) { snprintf(b, sizeof b, " %02X", bbRd(i < n - 1)); Serial.print(b); }
    bbStop(); Serial.println();
  } else if (!strcmp(sub, "w")) {
    char* aA = strtok(NULL, " \t"); char* aR = strtok(NULL, " \t"); char* aV = strtok(NULL, " \t");
    if (!aA || !aR || !aV) { Serial.println(F("usage: bb w <sda> <scl> <addr> <reg> <val>")); return; }
    uint8_t addr = (uint8_t)strtoul(aA, 0, 16), reg = (uint8_t)strtoul(aR, 0, 16), val = (uint8_t)strtoul(aV, 0, 16);
    bbStart(); int ok = bbWr((uint8_t)(addr << 1)); ok &= bbWr(reg); ok &= bbWr(val); bbStop();
    Serial.println(ok ? F("bb w: ACKed") : F("bb w: no ACK"));
  } else Serial.println(F("usage: bb scan|r|w <sda> <scl> ..."));
}

// --------------------------------------------------------------------------
// Dot-matrix display driver (TM16xx-style pseudo-I2C; 2 chips share one SCL,
// one data line each). Bench-confirmed pins:
//   SCL = PA_13 (shared)   SDA_LEFT = PA_14   SDA_RIGHT = PA_15
//   (PA_12 = connector "PWM", not a data line)
// This MIRRORS the stock sequence captured in display_i2c.csv: fixed-address
// mode, per grid -> START, cmd(0xC0+grid), data, STOP, then a display-control
// byte (0x88|bri). No leading data-command byte (the stock firmware sends none
// and it works). MSB-first; the 9th (ACK) clock is pulsed but ignored.
//
// Open-drain emulation, exactly like `bb` above: releasing a line (dir=INPUT)
// lets the bus pull-up take it HIGH; driving (dir=OUTPUT, preset value 0) pulls
// it LOW. The two chips share SCL but each only acts on START conditions seen
// on ITS OWN data line, so clocking line L's transaction is ignored by chip R
// as long as R's SDA stays idle-high -> we can drive either half in isolation.
//
// Purpose: ground-truth the data->dot mapping by lighting known cells and
// having a human read off the physical (row,col). Primitives:
//   disp on [bri] | off | clear
//   disp px  L|R|B <grid 0-23> <bit 0-7>   one dot only (clears the rest)
//   disp col L|R|B <grid 0-23>             whole grid byte = 0xFF (one column?)
//   disp bit L|R|B <bit 0-7>               that bit set in ALL 24 grids (one row?)
//   disp raw L|R|B <grid 0-23> <hex>       set one grid byte (no clear)
//   disp fill L|R|B <hex>                  set every grid to <hex>
// --------------------------------------------------------------------------
// Runtime-settable; override live with `disp pins <scl> <sdaL> <sdaR>`.
// Bench-confirmed 2026-06-17: data lines are PA_14 & PA_15 (PA_12 = the
// connector "PWM", NOT a data line — stock never drives it).
static PinName D_SCL = PA_13, D_SDAL = PA_14, D_SDAR = PA_15;
static gpio_t dScl, dSdaL, dSdaR;
static bool   dispUp = false;
static uint8_t dRam[2][24];                 // shadow: [0]=LEFT(PA_14) [1]=RIGHT(PA_15)
static char    g_dispText[96];              // raw string for `disp text` (set in dispatch)

static inline void dDelay() { delayMicroseconds(6); }   // ~80 kHz, very relaxed
static void dInit() {
  if (dispUp) return;
  gpio_init(&dScl, D_SCL); gpio_init(&dSdaL, D_SDAL); gpio_init(&dSdaR, D_SDAR);
  gpio_write(&dScl, 0); gpio_write(&dSdaL, 0); gpio_write(&dSdaR, 0);   // preset-low for OD
  gpio_mode(&dScl, PullUp); gpio_mode(&dSdaL, PullUp); gpio_mode(&dSdaR, PullUp);
  gpio_dir(&dScl, PIN_INPUT); gpio_dir(&dSdaL, PIN_INPUT); gpio_dir(&dSdaR, PIN_INPUT); // idle high
  dispUp = true;
}
// mask bit0 = LEFT (PA_12), bit1 = RIGHT (PA_15)
static inline void DSCL_HI() { gpio_dir(&dScl, PIN_INPUT);  }
static inline void DSCL_LO() { gpio_dir(&dScl, PIN_OUTPUT); }
static inline void DSDA(uint8_t m, int hi) {
  if (m & 1) gpio_dir(&dSdaL, hi ? PIN_INPUT : PIN_OUTPUT);
  if (m & 2) gpio_dir(&dSdaR, hi ? PIN_INPUT : PIN_OUTPUT);
}
static void dStart(uint8_t m) { DSDA(m, 1); DSCL_HI(); dDelay(); DSDA(m, 0); dDelay(); DSCL_LO(); dDelay(); }
static void dStop(uint8_t m)  { DSDA(m, 0); dDelay(); DSCL_HI(); dDelay(); DSDA(m, 1); dDelay(); }
static void dByte(uint8_t m, uint8_t v) {
  for (int i = 0; i < 8; i++) { DSDA(m, (v & 0x80) ? 1 : 0); v <<= 1; dDelay(); DSCL_HI(); dDelay(); DSCL_LO(); }
  DSDA(m, 1); dDelay(); DSCL_HI(); dDelay(); DSCL_LO(); dDelay();   // 9th = ACK clock, ignored
}
static void dCtrl(uint8_t m, uint8_t bri) { dStart(m); dByte(m, 0x88 | (bri & 0x07)); dStop(m); }  // ON
static void dOff(uint8_t m)               { dStart(m); dByte(m, 0x80); dStop(m); }                 // OFF
static void dGrid(uint8_t m, uint8_t g, uint8_t data) { dStart(m); dByte(m, 0xC0 | (g & 0x1F)); dByte(m, data); dStop(m); }
static void dFlush(uint8_t m) {                                   // push shadow RAM, each line separately
  for (int g = 0; g < 24; g++) { if (m & 1) dGrid(1, g, dRam[0][g]); if (m & 2) dGrid(2, g, dRam[1][g]); }
}
static void dClearRam(uint8_t m) { for (int g = 0; g < 24; g++) { if (m & 1) dRam[0][g] = 0; if (m & 2) dRam[1][g] = 0; } }

static int parseLineMask(const char* s) {
  if (!s) return 0;
  char c = s[0] | 0x20;
  if (c == 'l') return 1; if (c == 'r') return 2; if (c == 'b') return 3;
  return 0;
}

// Physical (x,y) -> driver cell. DIRECT lookup from the 8-plane `disp idx L`
// scan (Photos/pixel_idx_L_*), trusting the transcription exactly — no formula.
// MAP_L[y][x] = (grid<<3)|bit, 0xFF=unmapped. y 0..6 = bottom..top; x 0..27 =
// left..right (x0,x1 red). Line L covers x0-16; line R (x17-27) = TBD scan.
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
  // line R (x17-27): HYPOTHESIS — mirror of line L (localX = 27-x), driven on
  // PA_15 with the same grid/bit numbers. Validate by walking the right half.
  // rightmost column x27: a split vertical (bit = 6-y) — grid15 top 3 (y4-6),
  // grid14 bottom 4 (y0-3). Its block-mirror would land off-panel, so special-case.
  if (x == 27) { *mask = 2; *grid = (y >= 4) ? 15 : 14; *bit = 6 - y; return true; }
  if (x >= 17 && x <= 26) {
    uint8_t vr = MAP_L[y][28 - x];                 // axis 28: x17->col11 ... x26->col2
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

// --- 5x7 text rendering (font from Tools/gen_font.py; col-major, bit0=top) ---
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
  if (c >= 'a' && c <= 'z') c -= 32;               // fold to upper
  for (int i = 0; FONT_CHARS[i]; i++) if (FONT_CHARS[i] == c) return i;
  return 0;                                        // unknown -> space
}
// Scroll `s` right-to-left across the 28-wide panel. Any serial byte aborts.
static void dScrollText(const char* s) {
  static uint8_t buf[300];                         // glyph columns (bit0=top)
  int w = 0;
  for (const char* p = s; *p && w < (int)sizeof(buf) - 6; p++) {
    const uint8_t* g = FONT5x7[fontIndex(*p)];
    for (int c = 0; c < 5; c++) buf[w++] = g[c];
    buf[w++] = 0;                                  // 1-col gap
  }
  for (int off = -28; off <= w; off++) {
    dClearRam(3);
    for (int sx = 0; sx < 28; sx++) {
      int src = off + sx;
      if (src < 0 || src >= w) continue;
      uint8_t col = buf[src];
      for (int r = 0; r < 7; r++) if ((col >> r) & 1) dSetXY(sx, 6 - r);  // bit0=top -> y6
    }
    dFlush(3); dCtrl(3, 7);
    if (Serial.available()) { while (Serial.available()) Serial.read(); break; }
    delay(55);
  }
  dClearRam(3); dFlush(3);
}

// ---- raw register peek/poke + PADCTR decode -------------------------------
// PINMUX->PADCTR[pin] @ 0x48000400 + pin*4 (PinName index; PA_n=n, PB_n=32+n).
// Bitfields: [4:0]=func [8]=PU [9]=PD [10:11]=drive-strength [12]=schmitt
//            [13]=pull-sel [15]=shutdown.  GPIO IP @ 0x48014000 (port A).
static void cmdPeek(const char* aA, const char* aN) {
  if (!aA) { Serial.println(F("usage: peek <hexaddr> [count]")); return; }
  uint32_t addr = (uint32_t)strtoul(aA, 0, 16);
  int n = aN ? atoi(aN) : 1; if (n < 1) n = 1; if (n > 32) n = 32;
  char b[48];
  for (int i = 0; i < n; i++) {
    uint32_t a = addr + (uint32_t)i * 4, v = *(volatile uint32_t*)a;
    snprintf(b, sizeof b, "  [0x%08lX] = 0x%08lX", (unsigned long)a, (unsigned long)v);
    Serial.println(b);
  }
}
static void cmdPoke(const char* aA, const char* aV) {
  if (!aA || !aV) { Serial.println(F("usage: poke <hexaddr> <hexval>  (32-bit)")); return; }
  uint32_t addr = (uint32_t)strtoul(aA, 0, 16), val = (uint32_t)strtoul(aV, 0, 16);
  *(volatile uint32_t*)addr = val;
  uint32_t rb = *(volatile uint32_t*)addr;
  char b[64];
  snprintf(b, sizeof b, "poke [0x%08lX] <- 0x%08lX (rb 0x%08lX)", (unsigned long)addr, (unsigned long)val, (unsigned long)rb);
  Serial.println(b);
}
static void cmdPad(const char* aPin, const char* aVal) {
  if (!aPin) { Serial.println(F("usage: pad <PinName> [hexval]   read+decode (or write) PADCTR (e.g. pad PA_13)")); return; }
  PinName p = parsePin(aPin);
  if (p == NC) { Serial.println(F("pad: bad pin name")); return; }
  uint32_t addr = 0x48000400UL + (uint32_t)p * 4;
  if (aVal) { *(volatile uint32_t*)addr = (uint32_t)strtoul(aVal, 0, 16); }
  uint32_t v = *(volatile uint32_t*)addr;
  char b[96];
  snprintf(b, sizeof b, "PADCTR[%d] @0x%08lX = 0x%08lX", (int)p, (unsigned long)addr, (unsigned long)v);
  Serial.println(b);
  snprintf(b, sizeof b, "  func=0x%02lX drv=%lu pu=%lu pd=%lu smt=%lu shutdown=%lu",
    (unsigned long)(v & 0x1F), (unsigned long)((v >> 10) & 3), (unsigned long)((v >> 8) & 1),
    (unsigned long)((v >> 9) & 1), (unsigned long)((v >> 12) & 1), (unsigned long)((v >> 15) & 1));
  Serial.println(b);
}

// Dump PADCTR for ALL 64 pads (PA_0..31, PB_0..31) — cast the net wide so any
// pad stock configures differently shows up, not just the display lines. Decodes
// func/drv/pulls/shutdown inline so a stock-vs-cold diff is readable at a glance.
static void cmdPadAll() {
  Serial.println(F("idx pin    PADCTR     func drv pu pd smt shdn"));
  char b[80];
  for (int i = 0; i < 64; i++) {
    uint32_t v = *(volatile uint32_t*)(0x48000400UL + (uint32_t)i * 4);
    char pl[8]; pinLabel((PinName)i, pl, sizeof pl);
    snprintf(b, sizeof b, "%2d  %-6s 0x%08lX  %02lX   %lu   %lu  %lu  %lu   %lu",
      i, pl, (unsigned long)v, (unsigned long)(v & 0x1F), (unsigned long)((v >> 10) & 3),
      (unsigned long)((v >> 8) & 1), (unsigned long)((v >> 9) & 1),
      (unsigned long)((v >> 12) & 1), (unsigned long)((v >> 15) & 1));
    Serial.println(b);
  }
}

static void handleDisp(const char* sub, const char* a2, const char* a3) {
  if (!sub) { Serial.println(F("usage: disp on|off|clear|px|col|bit|raw|fill|text ...  (see help)")); return; }
  dInit();
  char b[48];

  if (!strcmp(sub, "text")) {                      // raw string captured in dispatch()
    if (!g_dispText[0]) { Serial.println(F("usage: disp text <string>")); return; }
    Serial.print(F("scrolling: ")); Serial.println(g_dispText);
    dScrollText(g_dispText);
    Serial.println(F("(done)"));
    return;
  }

  if (!strcmp(sub, "on"))  { uint8_t bri = a2 ? (uint8_t)atoi(a2) : 7; if (bri > 7) bri = 7; dCtrl(3, bri); snprintf(b, sizeof b, "disp ON bri=%u (both lines)", bri); Serial.println(b); return; }
  if (!strcmp(sub, "off")) { dOff(3); Serial.println(F("disp OFF (both lines)")); return; }
  if (!strcmp(sub, "clear")){ dClearRam(3); dFlush(3); dCtrl(3, 7); Serial.println(F("disp cleared + ON")); return; }
  if (!strcmp(sub, "pins")) {
    char* a4 = strtok(NULL, " \t");
    if (!a2 || !a3 || !a4) { Serial.println(F("usage: disp pins <scl> <sdaL> <sdaR>  (e.g. disp pins PA_13 PA_14 PA_15)")); return; }
    PinName sc = parsePin(a2), sl = parsePin(a3), sr = parsePin(a4);
    if (sc == NC || sl == NC || sr == NC) { Serial.println(F("disp pins: bad pin name")); return; }
    D_SCL = sc; D_SDAL = sl; D_SDAR = sr; dispUp = false; dInit();
    char sb[8], lb[8], rb[8]; pinLabel(D_SCL, sb, sizeof sb); pinLabel(D_SDAL, lb, sizeof lb); pinLabel(D_SDAR, rb, sizeof rb);
    snprintf(b, sizeof b, "disp pins set: SCL=%s L=%s R=%s", sb, lb, rb); Serial.println(b);
    return;
  }
  if (!strcmp(sub, "cmd")) {
    // Fire a bare command byte (START, byte, STOP) — for probing TM16xx/TM1680
    // command/mode bytes (e.g. 0xA0/0xA4/0xA8/0xAC COM-option, 0x80-0x8F ctrl).
    // Does NOT touch RAM; follow with `disp col L 0` to see if the map changed.
    if (!a2) { Serial.println(F("usage: disp cmd <hex> [L|R|B]  (bare START,byte,STOP)")); return; }
    uint8_t v = (uint8_t)strtoul(a2, 0, 16);
    int cm = a3 ? parseLineMask(a3) : 3; if (!cm) cm = 3;
    dStart(cm); dByte(cm, v); dStop(cm);
    snprintf(b, sizeof b, "disp cmd 0x%02X -> %s", v, cm == 1 ? "L" : cm == 2 ? "R" : "B"); Serial.println(b);
    return;
  }
  // ---- framebuffer commands (drive by physical x,y via xy2cell map) ----
  if (!strcmp(sub, "xy") || !strcmp(sub, "hline") || !strcmp(sub, "vline") || !strcmp(sub, "border")) {
    dClearRam(3);
    int hit = 0, miss = 0;
    auto put = [&](int x, int y){ if (dSetXY(x,y)) hit++; else miss++; };
    if (!strcmp(sub, "xy")) {
      char* a4 = strtok(NULL, " \t");
      if (!a2 || !a3) { Serial.println(F("usage: disp xy <x 0-27> <y 0-6>")); return; }
      put(atoi(a2), atoi(a3)); (void)a4;
    } else if (!strcmp(sub, "hline")) {
      if (!a2) { Serial.println(F("usage: disp hline <y 0-6>")); return; }
      int y=atoi(a2); for (int x=0;x<28;x++) put(x,y);
    } else if (!strcmp(sub, "vline")) {
      if (!a2) { Serial.println(F("usage: disp vline <x 0-27>")); return; }
      int x=atoi(a2); for (int y=0;y<7;y++) put(x,y);
    } else { // border
      for (int x=0;x<28;x++){ put(x,0); put(x,6); }
      for (int y=0;y<7;y++){ put(0,y); put(27,y); }
    }
    dFlush(3); dCtrl(3,7);
    snprintf(b, sizeof b, "disp %s: %d dots lit, %d unmapped", sub, hit, miss); Serial.println(b);
    return;
  }

  int m = parseLineMask(a2);
  if (!m) { Serial.println(F("disp: need line L|R|B")); return; }
  const char* lbl = (m == 1) ? "LEFT" : (m == 2) ? "RIGHT" : "BOTH";

  if (!strcmp(sub, "px")) {
    char* a4 = strtok(NULL, " \t");
    if (!a3 || !a4) { Serial.println(F("usage: disp px L|R|B <grid 0-23> <bit 0-7>")); return; }
    int g = atoi(a3), bit = atoi(a4);
    if (g < 0 || g > 23 || bit < 0 || bit > 7) { Serial.println(F("disp px: grid 0-23, bit 0-7")); return; }
    dClearRam(m); if (m & 1) dRam[0][g] = (1 << bit); if (m & 2) dRam[1][g] = (1 << bit);
    dFlush(m); dCtrl(m, 7);
    snprintf(b, sizeof b, "disp px %s grid=%d bit=%d (0x%02X)", lbl, g, bit, 1 << bit); Serial.println(b);
  } else if (!strcmp(sub, "col")) {
    if (!a3) { Serial.println(F("usage: disp col L|R|B <grid 0-23>")); return; }
    int g = atoi(a3); if (g < 0 || g > 23) { Serial.println(F("disp col: grid 0-23")); return; }
    dClearRam(m); if (m & 1) dRam[0][g] = 0xFF; if (m & 2) dRam[1][g] = 0xFF;
    dFlush(m); dCtrl(m, 7);
    snprintf(b, sizeof b, "disp col %s grid=%d = 0xFF", lbl, g); Serial.println(b);
  } else if (!strcmp(sub, "bit")) {
    if (!a3) { Serial.println(F("usage: disp bit L|R|B <bit 0-7>")); return; }
    int bit = atoi(a3); if (bit < 0 || bit > 7) { Serial.println(F("disp bit: bit 0-7")); return; }
    dClearRam(m); for (int g = 0; g < 24; g++) { if (m & 1) dRam[0][g] = (1 << bit); if (m & 2) dRam[1][g] = (1 << bit); }
    dFlush(m); dCtrl(m, 7);
    snprintf(b, sizeof b, "disp bit %s bit=%d in all 24 grids", lbl, bit); Serial.println(b);
  } else if (!strcmp(sub, "raw")) {
    char* a4 = strtok(NULL, " \t");
    if (!a3 || !a4) { Serial.println(F("usage: disp raw L|R|B <grid 0-23> <hex>")); return; }
    int g = atoi(a3); uint8_t v = (uint8_t)strtoul(a4, 0, 16);
    if (g < 0 || g > 23) { Serial.println(F("disp raw: grid 0-23")); return; }
    if (m & 1) dRam[0][g] = v; if (m & 2) dRam[1][g] = v;
    dFlush(m); dCtrl(m, 7);
    snprintf(b, sizeof b, "disp raw %s grid=%d = 0x%02X", lbl, g, v); Serial.println(b);
  } else if (!strcmp(sub, "fill")) {
    if (!a3) { Serial.println(F("usage: disp fill L|R|B <hex>")); return; }
    uint8_t v = (uint8_t)strtoul(a3, 0, 16);
    for (int g = 0; g < 24; g++) { if (m & 1) dRam[0][g] = v; if (m & 2) dRam[1][g] = v; }
    dFlush(m); dCtrl(m, 7);
    snprintf(b, sizeof b, "disp fill %s = 0x%02X", lbl, v); Serial.println(b);
  } else if (!strcmp(sub, "idx")) {
    // Binary-coded scan plane: light cell (grid,bit) iff bit `plane` of its
    // linear index (= grid*8 + bit) is set. Photograph planes 0..7; each dot's
    // 8-bit on/off code decodes to: bit = code & 7, grid = code >> 3.
    if (!a3) { Serial.println(F("usage: disp idx L|R|B <plane 0-7>  (binary map scan)")); return; }
    int plane = atoi(a3); if (plane < 0 || plane > 7) { Serial.println(F("disp idx: plane 0-7")); return; }
    dClearRam(m);
    for (int g = 0; g < 24; g++) for (int bit = 0; bit < 8; bit++) {
      int index = g * 8 + bit;
      if ((index >> plane) & 1) { if (m & 1) dRam[0][g] |= (1 << bit); if (m & 2) dRam[1][g] |= (1 << bit); }
    }
    dFlush(m); dCtrl(m, 7);
    snprintf(b, sizeof b, "disp idx %s plane=%d  (decode: bit=code&7 grid=code>>3)", lbl, plane); Serial.println(b);
  } else {
    Serial.println(F("usage: disp on|off|clear|pins|px|col|bit|raw|fill|idx ..."));
  }
}

// --------------------------------------------------------------------------
// Console
// --------------------------------------------------------------------------
static char line[80];
static size_t lineLen = 0;

static void dispatch(char* s) {
  // Capture the raw argument of `disp text <string>` before strtok mangles `s`.
  g_dispText[0] = 0;
  {
    const char* p = s; while (*p == ' ' || *p == '\t') p++;
    if (!strncmp(p, "disp", 4)) {
      p += 4; while (*p == ' ' || *p == '\t') p++;
      if (!strncmp(p, "text", 4)) {
        p += 4; while (*p == ' ' || *p == '\t') p++;
        strncpy(g_dispText, p, sizeof(g_dispText) - 1);
        g_dispText[sizeof(g_dispText) - 1] = 0;
        for (char* q = g_dispText; *q; q++) if (*q == '\r' || *q == '\n') { *q = 0; break; }
      }
    }
  }
  char* verb = strtok(s, " \t");
  char* a1 = strtok(NULL, " \t");
  char* a2 = strtok(NULL, " \t");
  char* a3 = strtok(NULL, " \t");
  if (!verb) return;

  if (!strcmp(verb, "help") || !strcmp(verb, "?")) printHelp();
  else if (!strcmp(verb, "list")) cmdList();
  else if (!strcmp(verb, "scan")) cmdScan(parsePull(a1, PullDown));
  else if (!strcmp(verb, "classify")) cmdClassify(a1);
  else if (!strcmp(verb, "rawscan")) cmdRawScan();
  else if (!strcmp(verb, "gread")) cmdGread(a1);
  else if (!strcmp(verb, "i2c")) handleI2c(a1, a2, a3);
  else if (!strcmp(verb, "bb")) handleBitbang(a1, a2, a3);
  else if (!strcmp(verb, "disp")) handleDisp(a1, a2, a3);
  else if (!strcmp(verb, "peek")) cmdPeek(a1, a2);
  else if (!strcmp(verb, "poke")) cmdPoke(a1, a2);
  else if (!strcmp(verb, "pad")) cmdPad(a1, a2);
  else if (!strcmp(verb, "padall")) cmdPadAll();
  else if (!strcmp(verb, "watch")) {
    PinMode pull = PullDown; uint32_t ms = 40;
    const char* args[2] = { a1, a2 };
    for (int k = 0; k < 2; k++) {
      if (!args[k]) break;
      if (args[k][0] >= '0' && args[k][0] <= '9') ms = (uint32_t)atoi(args[k]);
      else pull = parsePull(args[k], PullDown);
    }
    if (ms < 5) ms = 5;
    cmdWatch(pull, ms);
  }
  else if (!strcmp(verb, "read")) cmdRead(a1, a2);
  else if (!strcmp(verb, "out") || !strcmp(verb, "write") || !strcmp(verb, "drive")) cmdOut(a1, a2, a3);
  else if (!strcmp(verb, "in") || !strcmp(verb, "mode")) cmdIn(a1, a2);
  else if (!strcmp(verb, "aread") || !strcmp(verb, "ar")) cmdARead(a1);
  else if (!strcmp(verb, "ascan") || !strcmp(verb, "as")) cmdAScan();
  else if (!strcmp(verb, "pwm")) cmdPwm(a1, a2);
  else { Serial.print(F("? unknown '")); Serial.print(verb); Serial.println(F("' (try help)")); }
}

void setup() {
  // Hold the lid motor STOPPED from power-up: its driver reads hi-Z as ON, so
  // leaving PA_28/PA_30 as inputs lets the geared motor creep. Both LOW = stop.
  drivePin(PA_28, 0);
  drivePin(PA_30, 0);

  Serial.begin(115200);
  delay(50);
  Serial.println();
  Serial.println(F("============ RTL8721DM GPIO explorer (QFN68) ============"));
  Serial.println(F("34 bonded GPIOs. type 'help'. Safe first moves: 'classify' then 'watch'."));
  Serial.print(F("> "));
}

void loop() {
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\r') continue;
    if (c == '\n') {
      line[lineLen] = '\0';
      if (lineLen) dispatch(line);
      lineLen = 0;
      Serial.print(F("> "));
    } else if (lineLen < sizeof(line) - 1) {
      line[lineLen++] = c;
    } else lineLen = 0;
  }
}
