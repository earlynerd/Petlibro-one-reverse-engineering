#include "drivers/aw9523b.h"
#include "hal/i2c_bb.h"
#include "app/registry.h"
#include "app/eventlog.h"
#include "web/http_server.h"   // httpGetParam
#include <stdlib.h>
#include <string.h>

// ---- register map --------------------------------------------------------
//  port 0 : IN 0x00  OUT 0x02  CFG 0x04  LEDM 0x12
//  port 1 : IN 0x01  OUT 0x03  CFG 0x05  LEDM 0x13
//  GCR 0x11 bit4 = Port-0 push-pull(1)/open-drain(0); ID 0x10 == 0x23
static constexpr uint8_t REG_ID  = 0x10;
static constexpr uint8_t REG_GCR = 0x11;
static uint8_t REG_IN  (uint8_t p) { return p ? 0x01 : 0x00; }
static uint8_t REG_OUT (uint8_t p) { return p ? 0x03 : 0x02; }
static uint8_t REG_CFG (uint8_t p) { return p ? 0x05 : 0x04; }
static uint8_t REG_LEDM(uint8_t p) { return p ? 0x13 : 0x12; }

// ---- state ---------------------------------------------------------------
static I2cBitBang  g_bus;
static bool        g_present = false;
static uint8_t     g_addr    = 0;
static const char* g_sda     = "-";
static const char* g_scl     = "-";

// ---- low-level wrappers --------------------------------------------------
bool Aw9523::readReg(uint8_t reg, uint8_t& out) {
    if (!g_present) return false;
    uint8_t b;
    if (g_bus.readReg(g_addr, reg, &b, 1) != 1) return false;
    out = b; return true;
}
bool Aw9523::writeReg(uint8_t reg, uint8_t val) {
    if (!g_present) return false;
    return g_bus.writeReg(g_addr, reg, val);
}

static bool rmw(uint8_t reg, uint8_t setMask, uint8_t clrMask) {
    uint8_t cur;
    if (!Aw9523::readReg(reg, cur)) return false;
    uint8_t nw = (uint8_t)((cur | setMask) & ~clrMask);
    if (!Aw9523::writeReg(reg, nw)) return false;
    uint8_t chk;
    if (!Aw9523::readReg(reg, chk)) return false;
    return chk == nw;
}

// ---- public API ----------------------------------------------------------
bool Aw9523::present()      { return g_present; }
uint8_t Aw9523::address()   { return g_addr; }
const char* Aw9523::sdaName(){ return g_sda; }
const char* Aw9523::sclName(){ return g_scl; }

bool Aw9523::begin() {
    struct Ord { PinName sda, scl; const char* sn; const char* cn; };
    static const Ord ords[2] = {
        { PA_18, PA_19, "PA_18", "PA_19" },
        { PA_19, PA_18, "PA_19", "PA_18" },
    };
    for (int i = 0; i < 2; ++i) {
        g_bus.begin(ords[i].sda, ords[i].scl);
        for (uint8_t a = 0x58; a <= 0x5B; ++a) {
            uint8_t id;
            if (g_bus.readReg(a, REG_ID, &id, 1) == 1 && id == 0x23) {
                g_present = true; g_addr = a;
                g_sda = ords[i].sn; g_scl = ords[i].cn;
                return true;
            }
        }
    }
    g_present = false;
    return false;
}

bool Aw9523::setOutput(uint8_t port, uint8_t bit, bool level) {
    if (!g_present || port > 1 || bit > 7) return false;
    uint8_t mask = (uint8_t)(1u << bit);
    if (port == 0 && !rmw(REG_GCR, 0x10, 0x00)) return false;       // Port-0 push-pull
    if (!rmw(REG_LEDM(port), mask, 0x00)) return false;             // GPIO mode
    if (!rmw(REG_CFG(port),  0x00, mask)) return false;             // output (0=out)
    return rmw(REG_OUT(port), level ? mask : 0x00, level ? 0x00 : mask);
}

bool Aw9523::getInput(uint8_t port, uint8_t bit, bool& level) {
    if (port > 1 || bit > 7) return false;
    uint8_t v;
    if (!readReg(REG_IN(port), v)) return false;
    level = (v >> bit) & 1;
    return true;
}

bool Aw9523::rfidPower(bool on)          { return setOutput(0, 6, on); }
bool Aw9523::emitter(uint8_t which, bool on) {
    const uint8_t bits[3] = { 4, 5, 7 };                 // lid, dispense, hopper
    if (which > 2) return false;
    return setOutput(0, bits[which], on);
}
bool Aw9523::beep(uint16_t ms) {
    if (ms > 2000) ms = 2000;
    if (!setOutput(1, 7, true)) return false;
    delay(ms);
    return setOutput(1, 7, false);
}

// ===========================================================================
//  Harness commands + state
// ===========================================================================
static long qnum(const String& q, const char* k, long def) {
    String v = httpGetParam(q, k);
    if (v.length() == 0) return def;
    return strtol(v.c_str(), nullptr, 0);               // accepts 0x.. and decimal
}
static void hex2(String& s, uint8_t v) {
    static const char* H = "0123456789ABCDEF";
    s += "\"0x"; s += H[v >> 4]; s += H[v & 0xF]; s += "\"";
}

static String cmdScan(const String&) {
    bool ok = Aw9523::begin();
    String s = "{\"present\":"; s += ok ? "true" : "false";
    if (ok) {
        s += ",\"addr\":"; s += Aw9523::address();
        s += ",\"sda\":\""; s += Aw9523::sdaName();
        s += "\",\"scl\":\""; s += Aw9523::sclName();
        s += "\",\"id\":\"0x23\"";
    }
    s += "}";
    return s;
}

static String cmdDump(const String&) {
    if (!Aw9523::present()) return "{\"error\":\"not present (run aw9523.scan)\"}";
    const uint8_t regs[] = { 0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x10, 0x11, 0x12, 0x13 };
    String s = "{";
    for (unsigned i = 0; i < sizeof(regs); ++i) {
        uint8_t v;
        if (i) s += ',';
        s += "\"r"; { char b[3]; b[0]="0123456789ABCDEF"[regs[i]>>4]; b[1]="0123456789ABCDEF"[regs[i]&0xF]; b[2]=0; s += b; }
        s += "\":";
        if (Aw9523::readReg(regs[i], v)) hex2(s, v); else s += "null";
    }
    s += "}";
    return s;
}

static String cmdRreg(const String& q) {
    uint8_t reg = (uint8_t)qnum(q, "reg", -1);
    uint8_t v;
    if (!Aw9523::readReg(reg, v)) return "{\"ok\":false}";
    String s = "{\"ok\":true,\"reg\":"; s += reg; s += ",\"val\":"; hex2(s, v); s += "}";
    return s;
}

static String cmdWreg(const String& q) {
    uint8_t reg = (uint8_t)qnum(q, "reg", -1);
    uint8_t val = (uint8_t)qnum(q, "val", 0);
    bool ok = Aw9523::writeReg(reg, val);
    uint8_t chk = 0; Aw9523::readReg(reg, chk);
    String s = "{\"ok\":"; s += ok ? "true" : "false"; s += ",\"readback\":"; hex2(s, chk); s += "}";
    return s;
}

static String cmdOut(const String& q) {
    uint8_t port = (uint8_t)qnum(q, "port", 0);
    uint8_t bit  = (uint8_t)qnum(q, "bit", 0);
    bool    v    = qnum(q, "v", 0) != 0;
    bool ok = Aw9523::setOutput(port, bit, v);
    String s = "{\"ok\":"; s += ok ? "true" : "false";
    s += ",\"port\":"; s += port; s += ",\"bit\":"; s += bit; s += ",\"v\":"; s += v ? 1 : 0; s += "}";
    return s;
}

static String cmdIn(const String& q) {
    uint8_t port = (uint8_t)qnum(q, "port", 0);
    uint8_t bit  = (uint8_t)qnum(q, "bit", 0);
    bool lvl;
    if (!Aw9523::getInput(port, bit, lvl)) return "{\"ok\":false}";
    String s = "{\"ok\":true,\"port\":"; s += port; s += ",\"bit\":"; s += bit; s += ",\"level\":"; s += lvl ? 1 : 0; s += "}";
    return s;
}

static String cmdRfidPower(const String& q) {
    bool on = qnum(q, "v", 1) != 0;
    bool ok = Aw9523::rfidPower(on);
    eventLogAppend("rfid", String("power ") + (on ? "on" : "off") + (ok ? "" : " FAIL"));
    String s = "{\"ok\":"; s += ok ? "true" : "false"; s += ",\"on\":"; s += on ? 1 : 0; s += "}";
    return s;
}

static String cmdBeep(const String& q) {
    uint16_t ms = (uint16_t)qnum(q, "ms", 120);
    bool ok = Aw9523::beep(ms);
    eventLogAppend("beep", String(ms) + "ms");
    String s = "{\"ok\":"; s += ok ? "true" : "false"; s += ",\"ms\":"; s += ms; s += "}";
    return s;
}

static String cmdEmit(const String& q) {
    String which = httpGetParam(q, "which");
    bool on = qnum(q, "v", 1) != 0;
    uint8_t idx = which == "dispense" ? 1 : which == "hopper" ? 2 : 0;  // default lid
    bool ok = Aw9523::emitter(idx, on);
    String s = "{\"ok\":"; s += ok ? "true" : "false"; s += ",\"which\":\""; s += which; s += "\",\"on\":"; s += on ? 1 : 0; s += "}";
    return s;
}

static void stateAw(String& out) {
    out += "\"aw9523\":{\"present\":";
    if (!Aw9523::present()) { out += "false}"; return; }
    out += "true,\"addr\":"; out += Aw9523::address();
    out += ",\"sda\":\""; out += Aw9523::sdaName();
    out += "\",\"scl\":\""; out += Aw9523::sclName(); out += "\"";
    uint8_t v;
    if (Aw9523::readReg(0x02, v)) { out += ",\"p0_out\":"; out += v; }
    if (Aw9523::readReg(0x03, v)) { out += ",\"p1_out\":"; out += v; }
    out += "}";
}

void aw9523Init() {
    regAddState(stateAw);
    regAddCommand("aw9523.scan", cmdScan, "",                       "find AW9523B (ID 0x23) on PA_18/PA_19");
    regAddCommand("aw9523.dump", cmdDump, "",                       "dump key registers");
    regAddCommand("aw9523.rreg", cmdRreg, "reg:hex",                "read one register");
    regAddCommand("aw9523.wreg", cmdWreg, "reg:hex,val:hex",        "write one register");
    regAddCommand("aw9523.out",  cmdOut,  "port:0/1,bit:0-7,v:0/1", "drive a pin (push-pull GPIO, full init)");
    regAddCommand("aw9523.in",   cmdIn,   "port:0/1,bit:0-7",       "read a pin level");
    regAddCommand("rfid.power",  cmdRfidPower, "v:0/1",             "RFID module power (P0_6 PWEN)");
    regAddCommand("beeper.beep", cmdBeep, "ms:int",                 "pulse piezo P1_7 (DC; may be silent if passive)");
    regAddCommand("emit",        cmdEmit, "which:lid/dispense/hopper,v:0/1", "photo-emitter power");

    bool ok = Aw9523::begin();
    eventLogAppend("aw9523", ok ? String("found 0x") + String(Aw9523::address(), HEX) + " ID 0x23"
                                : "NOT found on PA_18/PA_19");
}
