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
  Serial.println(F("  i2c scan              HW I2C0 scan (SDA=PB_6,SCL=PB_5) — find AW9523B"));
  Serial.println(F("  i2c id                read AW9523B ID reg 0x10 at 0x58-0x5B (expect 0x23)"));
  Serial.println(F("  i2c r <addr> <reg> [n]     read n regs (hex)"));
  Serial.println(F("  i2c w <addr> <reg> <val>   write reg (hex)"));
  Serial.println(F("  bb scan <sda> <scl>        bit-bang I2C scan on ANY pins (e.g. PA_25 PA_26)"));
  Serial.println(F("  bb r/w <sda> <scl> <addr> <reg> [n/val]   bit-bang read/write (hex)"));
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
// Console
// --------------------------------------------------------------------------
static char line[80];
static size_t lineLen = 0;

static void dispatch(char* s) {
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
  else { Serial.print(F("? unknown '")); Serial.print(verb); Serial.println(F("' (try help)")); }
}

void setup() {
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
