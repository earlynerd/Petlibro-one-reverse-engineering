#include "app/registry.h"

namespace {
constexpr int MAX_STATE = 24;   // 12 contributors today; headroom for growth
constexpr int MAX_CMD   = 96;   // 65 commands today; was 48 -> silently dropped time.* (registered last)

StateFn  g_state[MAX_STATE];
int      g_stateN = 0;

struct CmdEntry {
    const char* name;
    CmdFn       fn;
    const char* argspec;   // e.g. "revs:int" or "" for none
    const char* help;
};
CmdEntry g_cmd[MAX_CMD];
int      g_cmdN = 0;

// Minimal JSON string escaper for the small, controlled strings we emit here.
void appendEscaped(String& out, const char* s) {
    for (const char* p = s; *p; ++p) {
        char c = *p;
        if (c == '"' || c == '\\') { out += '\\'; out += c; }
        else if (c == '\n')        { out += "\\n"; }
        else                       { out += c; }
    }
}
} // namespace

void regAddState(StateFn fn) {
    if (g_stateN < MAX_STATE) g_state[g_stateN++] = fn;
    else Serial.println("[registry] MAX_STATE exceeded — state contributor DROPPED (raise MAX_STATE)");
}

void regAddCommand(const char* name, CmdFn fn, const char* argspec, const char* help) {
    if (g_cmdN < MAX_CMD) g_cmd[g_cmdN++] = { name, fn, argspec ? argspec : "", help ? help : "" };
    else { Serial.print("[registry] MAX_CMD exceeded — command DROPPED (raise MAX_CMD): "); Serial.println(name); }
}

void regBuildState(String& out) {
    out += '{';
    for (int i = 0; i < g_stateN; ++i) {
        if (i) out += ',';
        g_state[i](out);
    }
    out += '}';
}

void regBuildCommands(String& out) {
    out += '[';
    for (int i = 0; i < g_cmdN; ++i) {
        if (i) out += ',';
        out += "{\"name\":\"";  appendEscaped(out, g_cmd[i].name);
        out += "\",\"args\":\""; appendEscaped(out, g_cmd[i].argspec);
        out += "\",\"help\":\""; appendEscaped(out, g_cmd[i].help);
        out += "\"}";
    }
    out += ']';
}

bool regDispatch(const String& name, const String& query, String& out) {
    for (int i = 0; i < g_cmdN; ++i) {
        if (name == g_cmd[i].name) {
            out += g_cmd[i].fn(query);
            return true;
        }
    }
    return false;
}
