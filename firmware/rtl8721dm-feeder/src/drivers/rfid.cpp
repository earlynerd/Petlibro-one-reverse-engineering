#include "drivers/rfid.h"
#include "app/registry.h"
#include "app/eventlog.h"
#include "web/http_server.h"   // httpGetParam
#include <string.h>
#include <stdlib.h>
extern "C" {
  #include "PinNames.h"
  #include "serial_api.h"
  #include "gpio_api.h"
}

// ---- link parameters (from the proven bridge config) ---------------------
static constexpr unsigned long RFID_BAUD       = 19200UL;
static constexpr unsigned long RFID_GAP_US     = 3000UL;   // Modbus RTU inter-frame idle
static constexpr unsigned long RFID_TIMEOUT_MS = 50UL;     // wait for first reply byte
static constexpr uint8_t       RFID_SLAVE      = 0x03;
// readRegs status codes (alongside positive reg counts and negated exceptions)
static constexpr int ST_TIMEOUT  = 0;
static constexpr int ST_BADFRAME = -1000;

// ---- state ---------------------------------------------------------------
static serial_t g_uart;
static gpio_t   g_irq;
static bool     g_up        = false;
static char     g_lastId[16] = {0};
static uint8_t  g_lastQ      = 0;
static uint16_t g_lastCountry = 0;

// ---- Modbus CRC16 (poly 0xA001, init 0xFFFF, low byte first) -------------
static uint16_t modbusCrc(const uint8_t* p, size_t n) {
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < n; i++) {
        crc ^= p[i];
        for (int b = 0; b < 8; b++)
            crc = (crc & 1) ? (uint16_t)((crc >> 1) ^ 0xA001) : (uint16_t)(crc >> 1);
    }
    return crc;
}
static bool crcOk(const uint8_t* b, size_t flen) {
    if (flen < 4) return false;
    return modbusCrc(b, flen - 2) == (uint16_t)(b[flen - 2] | (b[flen - 1] << 8));
}

// ---- UART io -------------------------------------------------------------
static void drainRx()                       { while (serial_readable(&g_uart)) (void)serial_getc(&g_uart); }
static void sendBytes(const uint8_t* b, size_t n) { for (size_t i = 0; i < n; i++) serial_putc(&g_uart, b[i]); }

// Wait RFID_TIMEOUT_MS for the first byte; once bytes flow, close the frame
// after RFID_GAP_US of line idle (Modbus RTU framing).
static int recvFrame(uint8_t* buf, size_t cap) {
    uint32_t startMs = millis();
    uint32_t lastUs  = micros();
    size_t   len     = 0;
    bool     started = false;
    for (;;) {
        if (serial_readable(&g_uart)) {
            int c = serial_getc(&g_uart);
            started = true;
            if (len < cap) buf[len++] = (uint8_t)c;
            lastUs = micros();
            continue;
        }
        if (!started) {
            if (millis() - startMs >= RFID_TIMEOUT_MS) return 0;
        } else if ((uint32_t)(micros() - lastUs) >= RFID_GAP_US) {
            return (int)len;
        }
    }
}

static int transact(const uint8_t* pdu, size_t n, bool appendCrc, uint8_t* rsp, size_t cap) {
    if (!g_up) return 0;
    uint8_t frame[64];
    if (n + (appendCrc ? 2u : 0u) > sizeof(frame)) return 0;
    memcpy(frame, pdu, n);
    size_t flen = n;
    if (appendCrc) {
        uint16_t c = modbusCrc(frame, n);
        frame[flen++] = (uint8_t)(c & 0xFF);
        frame[flen++] = (uint8_t)(c >> 8);
    }
    drainRx();
    sendBytes(frame, flen);
    return recvFrame(rsp, cap);
}

// ---- public API ----------------------------------------------------------
void Rfid::begin() {
    serial_init(&g_uart, PA_26, PA_25);
    serial_baud(&g_uart, RFID_BAUD);
    serial_format(&g_uart, 8, ParityOdd, 1);     // 8O1 (see header note on RX parity)
    gpio_init(&g_irq, PA_16);
    gpio_dir(&g_irq, PIN_INPUT);
    gpio_mode(&g_irq, PullUp);                    // idle-HIGH; module pulls LOW for a tag
    g_up = true;
}

bool Rfid::tagPresentIRQ() { return gpio_read(&g_irq) == 0; }

int Rfid::readRegs(uint16_t addr, uint16_t qty, uint16_t* out, size_t cap) {
    uint8_t pdu[6] = { RFID_SLAVE, 0x03,
                       (uint8_t)(addr >> 8), (uint8_t)(addr & 0xFF),
                       (uint8_t)(qty  >> 8), (uint8_t)(qty  & 0xFF) };
    uint8_t rsp[96];
    int n = transact(pdu, sizeof(pdu), true, rsp, sizeof(rsp));
    if (n <= 0) return ST_TIMEOUT;
    if (n >= 3 && (rsp[1] & 0x80)) return -(int)rsp[2];          // Modbus exception
    if (n < 5 || rsp[0] != RFID_SLAVE || rsp[1] != 0x03) return ST_BADFRAME;
    uint8_t bc = rsp[2];
    size_t  need = (size_t)3 + bc + 2;
    if ((size_t)n < need || !crcOk(rsp, need)) return ST_BADFRAME;
    int regs = bc / 2;
    for (int i = 0; i < regs && (size_t)i < cap; i++)
        out[i] = (uint16_t)((rsp[3 + i * 2] << 8) | rsp[4 + i * 2]);
    return regs;
}

bool Rfid::writeReg(uint16_t addr, uint16_t value) {
    uint8_t pdu[6] = { RFID_SLAVE, 0x06,
                       (uint8_t)(addr  >> 8), (uint8_t)(addr  & 0xFF),
                       (uint8_t)(value >> 8), (uint8_t)(value & 0xFF) };
    uint8_t rsp[16];
    int n = transact(pdu, sizeof(pdu), true, rsp, sizeof(rsp));
    if (n < 8) return false;
    return rsp[0] == RFID_SLAVE && rsp[1] == 0x06 &&
           rsp[2] == pdu[2] && rsp[3] == pdu[3] &&
           rsp[4] == pdu[4] && rsp[5] == pdu[5] && crcOk(rsp, 8);
}

// "%03u" country + "%012llu" national, hand-rolled (no %llu on this libc).
static void fmtId(char* dst, uint16_t country, uint64_t national) {
    uint16_t c = country % 1000;
    dst[0] = '0' + (c / 100) % 10;
    dst[1] = '0' + (c / 10) % 10;
    dst[2] = '0' + c % 10;
    for (int i = 14; i >= 3; --i) { dst[i] = '0' + (int)(national % 10); national /= 10; }
    dst[15] = '\0';
}

bool Rfid::readTag(Tag& t) {
    t.valid = false;
    uint16_t r[4];
    int n = readRegs(0x000E, 4, r, 4);
    if (n < 4) return false;
    t.country  = r[0];
    t.national = ((uint64_t)r[1] << 24) | ((uint64_t)r[2] << 8) | (uint64_t)(r[3] >> 8);
    t.quality  = (uint8_t)(r[3] & 0xFF);
    fmtId(t.id, t.country, t.national);
    t.valid = true;
    return true;
}

// ===========================================================================
//  Harness commands + state
// ===========================================================================
static long qnum(const String& q, const char* k, long def) {
    String v = httpGetParam(q, k);
    return v.length() ? strtol(v.c_str(), nullptr, 0) : def;
}
static void hx8 (String& s, uint8_t v)  { static const char* H = "0123456789ABCDEF"; s += H[v >> 4]; s += H[v & 0xF]; }
static void hx16(String& s, uint16_t v) { hx8(s, (uint8_t)(v >> 8)); hx8(s, (uint8_t)v); }

static String cmdRead(const String&) {
    Rfid::Tag t;
    bool ok = Rfid::readTag(t);
    String s = "{\"ok\":"; s += ok ? "true" : "false";
    s += ",\"present\":"; s += Rfid::tagPresentIRQ() ? "true" : "false";
    if (ok && t.valid) {
        s += ",\"id\":\""; s += t.id; s += "\",\"country\":"; s += t.country;
        s += ",\"quality\":"; s += t.quality;
        strncpy(g_lastId, t.id, sizeof(g_lastId)); g_lastId[sizeof(g_lastId) - 1] = 0;
        g_lastQ = t.quality; g_lastCountry = t.country;
        eventLogAppend("read", String("tag ") + t.id + " q=" + t.quality);
    }
    s += "}";
    return s;
}

static String cmdRegs(const String& q) {
    uint16_t addr = (uint16_t)qnum(q, "addr", 0x000E);
    long     qty  = qnum(q, "qty", 4);
    if (qty < 1)  qty = 1;
    if (qty > 37) qty = 37;
    uint16_t out[37];
    int r = Rfid::readRegs(addr, (uint16_t)qty, out, 37);
    String s = "{\"addr\":"; s += addr; s += ",\"qty\":"; s += qty; s += ",\"status\":";
    if (r > 0) {
        s += r; s += ",\"regs\":[";
        for (int i = 0; i < r; i++) { if (i) s += ','; s += "\"0x"; hx16(s, out[i]); s += "\""; }
        s += "]";
    } else if (r == ST_TIMEOUT)  { s += "\"timeout\""; }
      else if (r == ST_BADFRAME) { s += "\"badframe\""; }
      else                       { s += "\"exception "; s += (-r); s += "\""; }
    s += "}";
    return s;
}

static String cmdWreg(const String& q) {
    uint16_t addr = (uint16_t)qnum(q, "addr", 0);
    uint16_t val  = (uint16_t)qnum(q, "val", 0);
    bool ok = Rfid::writeReg(addr, val);
    String s = "{\"ok\":"; s += ok ? "true" : "false"; s += ",\"addr\":"; s += addr; s += ",\"val\":"; s += val; s += "}";
    return s;
}

static String cmdTx(const String& q) {
    String bytes = httpGetParam(q, "bytes");
    uint8_t pdu[32]; size_t n = 0;
    int i = 0, L = bytes.length();
    while (i < L && n < sizeof(pdu)) {
        while (i < L && bytes[i] == ' ') i++;
        if (i >= L) break;
        int j = i; while (j < L && bytes[j] != ' ') j++;
        pdu[n++] = (uint8_t)strtoul(bytes.substring(i, j).c_str(), nullptr, 16);
        i = j;
    }
    if (n < 2) return "{\"error\":\"need >=2 hex bytes, e.g. 03 03 00 0E 00 04\"}";
    uint8_t rsp[96];
    int r = transact(pdu, n, true, rsp, sizeof(rsp));
    String s = "{\"sent\":"; s += n; s += ",\"len\":"; s += r; s += ",\"reply\":[";
    for (int k = 0; k < r; k++) { if (k) s += ','; s += "\"0x"; hx8(s, rsp[k]); s += "\""; }
    s += "]}";
    return s;
}

static String cmdIrq(const String&) {
    int lvl = gpio_read(&g_irq);
    String s = "{\"pa16\":"; s += lvl; s += ",\"present\":"; s += (lvl == 0) ? "true" : "false"; s += "}";
    return s;
}

static String cmdInit(const String&) {
    bool ok = Rfid::writeReg(0x0000, 0x0002);    // mode/enable, as stock does at boot
    eventLogAppend("rfid", ok ? "init 0x0000=0x0002 ack" : "init write FAILED");
    String s = "{\"ok\":"; s += ok ? "true" : "false"; s += "}";
    return s;
}

static void stateRfid(String& out) {
    out += "\"rfid\":{\"up\":"; out += g_up ? "true" : "false";
    if (g_up) {
        out += ",\"present\":"; out += Rfid::tagPresentIRQ() ? "true" : "false";
        if (g_lastId[0]) { out += ",\"last_tag\":\""; out += g_lastId; out += "\",\"q\":"; out += g_lastQ; }
    }
    out += "}";
}

void rfidInit() {
    Rfid::begin();
    regAddState(stateRfid);
    regAddCommand("rfid.read", cmdRead, "",                 "read FDX-B tag (4 regs @0x000E) + decode");
    regAddCommand("rfid.regs", cmdRegs, "addr:hex,qty:int", "read holding registers");
    regAddCommand("rfid.wreg", cmdWreg, "addr:hex,val:hex", "write single register (fn 0x06)");
    regAddCommand("rfid.tx",   cmdTx,   "bytes:hexlist",    "raw Modbus PDU (CRC appended) -> reply");
    regAddCommand("rfid.irq",  cmdIrq,  "",                 "read PA_16 tag-ready (LOW=present)");
    regAddCommand("rfid.init", cmdInit, "",                 "module mode/enable: write 0x0000=0x0002");
    eventLogAppend("rfid", "UART3 8O1 @19200 up (PA_26/PA_25), IRQ PA_16");
}
