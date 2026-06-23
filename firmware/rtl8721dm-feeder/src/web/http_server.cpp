#include "web/http_server.h"
#include "app/registry.h"
#include "app/eventlog.h"
#include "web/dashboard.h"

namespace {

int hexVal(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

String urlDecode(const String& s) {
    String out;
    out.reserve(s.length());
    for (unsigned i = 0; i < s.length(); ++i) {
        char c = s[i];
        if (c == '+') {
            out += ' ';
        } else if (c == '%' && i + 2 < s.length()) {
            int hi = hexVal(s[i + 1]), lo = hexVal(s[i + 2]);
            if (hi >= 0 && lo >= 0) { out += (char)((hi << 4) | lo); i += 2; }
            else out += c;
        } else {
            out += c;
        }
    }
    return out;
}

void sendJson(WiFiClient& client, const String& body) {
    client.print(F("HTTP/1.1 200 OK\r\n"
                   "Content-Type: application/json\r\n"
                   "Access-Control-Allow-Origin: *\r\n"
                   "Connection: close\r\n\r\n"));
    client.print(body);
}

void sendHtml(WiFiClient& client, const char* body) {
    client.print(F("HTTP/1.1 200 OK\r\n"
                   "Content-Type: text/html; charset=utf-8\r\n"
                   "Connection: close\r\n\r\n"));
    client.print(body);
}

void sendStatus(WiFiClient& client, const char* status, const String& body) {
    client.print("HTTP/1.1 ");
    client.print(status);
    client.print(F("\r\nContent-Type: application/json\r\n"
                   "Access-Control-Allow-Origin: *\r\n"
                   "Connection: close\r\n\r\n"));
    client.print(body);
}

} // namespace

String httpGetParam(const String& query, const char* key) {
    String pre = String(key) + "=";
    int from = 0;
    while (from <= (int)query.length()) {
        int idx = query.indexOf(pre, from);
        if (idx < 0) break;
        // must be at start or right after '&'
        if (idx == 0 || query[idx - 1] == '&') {
            int valStart = idx + pre.length();
            int amp = query.indexOf('&', valStart);
            String raw = (amp < 0) ? query.substring(valStart)
                                   : query.substring(valStart, amp);
            return urlDecode(raw);
        }
        from = idx + pre.length();
    }
    return String();
}

void httpHandleClient(WiFiClient& client) {
    // Read the request line: "METHOD /path?query HTTP/1.1"
    String reqLine = client.readStringUntil('\n');
    reqLine.trim();

    // Drain the rest of the headers (we don't need them for query-param cmds).
    while (client.connected()) {
        String h = client.readStringUntil('\n');
        if (h.length() <= 1) break;   // blank line ("\r") terminates headers
    }

    int sp1 = reqLine.indexOf(' ');
    int sp2 = reqLine.indexOf(' ', sp1 + 1);
    if (sp1 < 0 || sp2 < 0) { sendStatus(client, "400 Bad Request", "{\"error\":\"bad request\"}"); return; }

    String target = reqLine.substring(sp1 + 1, sp2);
    String path = target, query = "";
    int q = target.indexOf('?');
    if (q >= 0) { path = target.substring(0, q); query = target.substring(q + 1); }

    if (path == "/") {
        sendHtml(client, DASHBOARD_HTML);
    } else if (path == "/api/state") {
        String body; body.reserve(512);
        regBuildState(body);
        sendJson(client, body);
    } else if (path == "/api/commands") {
        String body; body.reserve(512);
        regBuildCommands(body);
        sendJson(client, body);
    } else if (path == "/api/cmd") {
        String name = httpGetParam(query, "cmd");
        if (name.length() == 0) { sendStatus(client, "400 Bad Request", "{\"error\":\"missing cmd\"}"); return; }
        String body; body.reserve(256);
        if (regDispatch(name, query, body)) sendJson(client, body);
        else sendStatus(client, "404 Not Found", "{\"error\":\"unknown command\"}");
    } else if (path == "/api/events") {
        uint32_t since = (uint32_t)strtoul(httpGetParam(query, "since").c_str(), nullptr, 10);
        String body; body.reserve(1024);
        eventLogBuildJson(since, body);
        sendJson(client, body);
    } else {
        sendStatus(client, "404 Not Found", "{\"error\":\"not found\"}");
    }
}
