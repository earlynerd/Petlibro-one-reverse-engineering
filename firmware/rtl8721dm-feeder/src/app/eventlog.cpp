#include "app/eventlog.h"

namespace {
constexpr int CAP = 64;            // ring capacity (Phase 0, in-RAM)
Event    g_ring[CAP];
int      g_count = 0;              // entries written (saturates at CAP)
int      g_head  = 0;             // next write slot
uint32_t g_seq   = 0;              // monotonic sequence counter

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
} // namespace

void eventLogInit() {
    g_count = 0;
    g_head  = 0;
    g_seq   = 0;
}

uint32_t eventLogAppend(const char* type, const String& detail) {
    Event& e = g_ring[g_head];
    e.seq = ++g_seq;
    e.ts  = millis();
    copyTrunc(e.type,   sizeof(e.type),   type);
    copyTrunc(e.detail, sizeof(e.detail), detail.c_str());

    g_head = (g_head + 1) % CAP;
    if (g_count < CAP) g_count++;
    return e.seq;
}

uint32_t eventLogHeadSeq() { return g_seq; }

void eventLogBuildJson(uint32_t since, String& out) {
    // Walk the ring oldest-first.
    int start = (g_count < CAP) ? 0 : g_head;
    out += '[';
    bool first = true;
    for (int n = 0; n < g_count; ++n) {
        const Event& e = g_ring[(start + n) % CAP];
        if (e.seq <= since) continue;
        if (!first) out += ',';
        first = false;
        out += "{\"seq\":";  out += e.seq;
        out += ",\"ts\":";   out += e.ts;
        out += ",\"type\":\""; appendEscaped(out, e.type);
        out += "\",\"detail\":\""; appendEscaped(out, e.detail);
        out += "\"}";
    }
    out += ']';
}
