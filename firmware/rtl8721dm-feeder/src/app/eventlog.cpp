#include "app/eventlog.h"
#include <string.h>
extern "C" {
  #include "flash_api.h"     // flash_read_id / flash_stream_read / flash_stream_write / flash_erase_sector
  #include "device_lock.h"   // device_mutex_lock, RT_DEV_LOCK_FLASH
}

// ===========================================================================
//  Durable event journal on RAW FLASH (no filesystem).
//
//  The SDK's prebuilt littlefs faulted on directory ops in our build, but raw
//  flash R/W/erase is proven solid + cache-coherent. Our log is just fixed-size
//  append-only records, so we don't need a filesystem: a sector-ring keyed by
//  the record's own seq.
//
//   * Region: JBASE, NSECT x 4 KB sectors, in the dump-verified free window.
//   * Each on-flash record = Event (72 B) + a MAGIC trailer (4 B), written in
//     one flash op. The magic is LAST, so a torn write (power loss mid-record)
//     leaves no magic -> the record is ignored. The magic also distinguishes
//     our records from leftover stock-firmware bytes on first use.
//   * seq IS the record index: record `s` lives at slot (s-1) % TOTAL. So
//     seq<->address is O(1) — no scan except a one-time boot pass to recover
//     the head seq. Entering a fresh sector erases it (FIFO-drops the oldest).
//
//  If the region can't be validated against the JEDEC flash capacity, the log
//  falls back to a volatile in-RAM ring (same API).
// ===========================================================================

static constexpr uint32_t JBASE   = 0x500000;          // verified-free (dump: 0x500000-0x7CB000)
static constexpr uint32_t SECT    = 4096;
static constexpr uint32_t NSECT   = 64;                // 256 KB region
static constexpr uint32_t JSIZE   = NSECT * SECT;
static constexpr uint32_t MAGIC   = 0xFEED1234u;

namespace {
constexpr uint32_t REC   = sizeof(Event);              // 72
constexpr uint32_t RECF  = REC + 4;                    // 76: record + magic trailer
constexpr uint32_t RPS   = SECT / RECF;                // 53 records per sector
constexpr uint32_t TOTAL = NSECT * RPS;                // 3392 record slots
constexpr uint32_t RETAINED = TOTAL - RPS;             // slots guaranteed valid (one sector is mid-fill)
constexpr uint32_t MAX_EMIT = 200;                     // max records returned by one buildJson

bool     g_fsUp = false;
uint32_t g_seq  = 0;                                   // last assigned seq (monotonic)
uint32_t (*g_clockFn)() = nullptr;                     // epoch source (0 => fall back to millis)

// RAM ring fallback (used only when the flash region is unavailable)
constexpr int CAP = 64;
Event g_ring[CAP];
int   g_rcount = 0, g_head = 0;

uint8_t g_secbuf[SECT];                                // boot-scan buffer (BSS)

void copyTrunc(char* dst, size_t dstsz, const char* src) {
    size_t i = 0;
    for (; src[i] && i < dstsz - 1; ++i) dst[i] = src[i];
    dst[i] = '\0';
}
void appendEscaped(String& out, const char* s) {
    for (const char* p = s; *p; ++p) {
        char c = *p;
        if (c == '"' || c == '\\') { out += '\\'; out += c; }
        else if (c == '\n')        { out += "\\n"; }
        else                       { out += c; }
    }
}
void emitRecord(const Event& e, String& out) {
    out += "{\"seq\":";  out += e.seq;
    out += ",\"ts\":";   out += e.ts;
    out += ",\"type\":\""; appendEscaped(out, e.type);
    out += "\",\"detail\":\""; appendEscaped(out, e.detail);
    out += "\"}";
}

// ---- raw flash helpers (device-locked) ------------------------------------
void fRead(uint32_t addr, void* buf, uint32_t n) {
    flash_t f; device_mutex_lock(RT_DEV_LOCK_FLASH);
    flash_stream_read(&f, addr, n, (uint8_t*)buf);
    device_mutex_unlock(RT_DEV_LOCK_FLASH);
}
void fWrite(uint32_t addr, const void* buf, uint32_t n) {
    flash_t f; device_mutex_lock(RT_DEV_LOCK_FLASH);
    flash_stream_write(&f, addr, n, (uint8_t*)buf);
    device_mutex_unlock(RT_DEV_LOCK_FLASH);
}
void fErase(uint32_t sec) {
    flash_t f; device_mutex_lock(RT_DEV_LOCK_FLASH);
    flash_erase_sector(&f, JBASE + sec * SECT);
    device_mutex_unlock(RT_DEV_LOCK_FLASH);
}
uint32_t slotAddr(uint32_t slot) {
    return JBASE + (slot / RPS) * SECT + (slot % RPS) * RECF;
}

uint32_t flashCapacity() {
    flash_t f; uint8_t id[4] = {0};
    device_mutex_lock(RT_DEV_LOCK_FLASH);
    int r = flash_read_id(&f, id, 3);
    device_mutex_unlock(RT_DEV_LOCK_FLASH);
    if (r < 0) return 0;
    uint8_t code = id[2];                       // Winbond/most: 3rd ID byte = log2(bytes)
    if (code < 0x14 || code > 0x20) return 0;
    return (uint32_t)1u << code;
}

// Write one record: [Event | MAGIC]. Erase the sector first if we're entering it.
void appendFlash(const Event& e) {
    uint32_t slot = (e.seq - 1) % TOTAL;
    if (slot % RPS == 0) fErase(slot / RPS);    // entering a sector -> wipe the previous lap (FIFO drop)
    uint8_t buf[RECF];
    memcpy(buf, &e, REC);
    uint32_t m = MAGIC; memcpy(buf + REC, &m, 4);
    fWrite(slotAddr(slot), buf, RECF);          // magic written last -> torn write is self-detecting
}

// Read slot; true + fills e only if the magic trailer is intact.
bool readSlot(uint32_t slot, Event& e) {
    uint8_t buf[RECF];
    fRead(slotAddr(slot), buf, RECF);
    uint32_t m; memcpy(&m, buf + REC, 4);
    if (m != MAGIC) return false;
    memcpy(&e, buf, REC);
    return true;
}

void ringPush(const Event& e) {
    g_ring[g_head] = e;
    g_head = (g_head + 1) % CAP;
    if (g_rcount < CAP) g_rcount++;
}
void ringBuildJson(uint32_t since, String& out) {
    int start = (g_rcount < CAP) ? 0 : g_head;
    out += '[';
    bool first = true;
    for (int n = 0; n < g_rcount; ++n) {
        const Event& e = g_ring[(start + n) % CAP];
        if (e.seq <= since) continue;
        if (!first) out += ','; first = false;
        emitRecord(e, out);
    }
    out += ']';
}
} // namespace

void eventLogSetClock(uint32_t (*fn)()) { g_clockFn = fn; }

void eventLogInit() {
    g_rcount = 0; g_head = 0; g_seq = 0; g_fsUp = false;

    uint32_t cap = flashCapacity();
    if (cap == 0 || (uint64_t)JBASE + JSIZE > cap) {     // region not backed by real flash -> RAM ring
        Serial.println("[evlog] flash region unavailable -> RAM fallback"); Serial.flush();
        return;
    }
    g_fsUp = true;

    // One-time boot scan: recover the head seq from the highest valid record.
    uint32_t maxSeq = 0;
    for (uint32_t sec = 0; sec < NSECT; ++sec) {
        fRead(JBASE + sec * SECT, g_secbuf, SECT);
        for (uint32_t i = 0; i < RPS; ++i) {
            const uint8_t* r = g_secbuf + i * RECF;
            uint32_t m; memcpy(&m, r + REC, 4);
            if (m != MAGIC) continue;
            uint32_t sq; memcpy(&sq, r, 4);              // seq = first 4 bytes of Event
            if (sq > maxSeq) maxSeq = sq;
        }
    }
    g_seq = maxSeq;
    Serial.print("[evlog] flash journal up @0x"); Serial.print(JBASE, HEX);
    Serial.print(" cap=0x"); Serial.print(cap, HEX);
    Serial.print(" slots="); Serial.print(TOTAL);
    Serial.print(" seq="); Serial.println(g_seq); Serial.flush();
}

uint32_t eventLogAppend(const char* type, const String& detail) {
    Event e;
    e.seq = ++g_seq;
    uint32_t ep = g_clockFn ? g_clockFn() : 0;
    e.ts = ep ? ep : millis();
    copyTrunc(e.type,   sizeof(e.type),   type);
    copyTrunc(e.detail, sizeof(e.detail), detail.c_str());
    if (g_fsUp) appendFlash(e);
    else        ringPush(e);
    return e.seq;
}

uint32_t eventLogHeadSeq() { return g_seq; }

void eventLogBuildJson(uint32_t since, String& out) {
    if (!g_fsUp) { ringBuildJson(since, out); return; }
    out += '[';
    if (g_seq > 0) {
        uint32_t retained = (g_seq < RETAINED) ? g_seq : RETAINED;
        uint32_t oldest   = g_seq - retained + 1;
        uint32_t start    = since + 1;
        if (start < oldest) start = oldest;
        if (g_seq >= MAX_EMIT && start < g_seq - MAX_EMIT + 1) start = g_seq - MAX_EMIT + 1;
        bool first = true;
        for (uint32_t s = start; s <= g_seq; ++s) {
            Event e;
            if (!readSlot((s - 1) % TOTAL, e) || e.seq != s) continue;   // erased / torn / stale slot
            if (!first) out += ','; first = false;
            emitRecord(e, out);
        }
    }
    out += ']';
}

void eventLogStatsJson(String& out) {
    uint32_t retained = g_fsUp ? ((g_seq < RETAINED) ? g_seq : RETAINED) : (uint32_t)g_rcount;
    out += "{\"fs\":";        out += g_fsUp ? "true" : "false";
    out += ",\"records\":";   out += retained;
    out += ",\"head_seq\":";  out += g_seq;
    out += ",\"capacity\":";  out += g_fsUp ? RETAINED : (uint32_t)CAP;
    out += ",\"base\":";      out += JBASE;
    out += "}";
}

void eventLogClear() {
    if (g_fsUp) for (uint32_t s = 0; s < NSECT; ++s) fErase(s);   // wipe all; g_seq keeps advancing
    g_rcount = 0; g_head = 0;
}
