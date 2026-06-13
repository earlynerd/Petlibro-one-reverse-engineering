#include <string.h>
#include <stdlib.h>
#include "config.h"
#include "control_console.h"

ControlConsole Console;

void ControlConsole::begin(Stream* io, RtlBridge* rtl, RfidSnoop* snoop) {
  _io = io;
  _rtl = rtl;
  _snoop = snoop;
  _len = 0;
}

void ControlConsole::printHelp() {
  _io->println(F("Petlibro RTL8721DM bridge — control console"));
  _io->println(F("  reset            pulse RTL reset (run application)"));
  _io->println(F("  download | boot  enter UART download mode (strap+reset macro)"));
  _io->println(F("  run              release strap and reset into application"));
  _io->println(F("  rst on|off       hold/release CHIP_EN (reset)"));
  _io->println(F("  strap on|off     hold/release UART_DOWNLOAD (PA7, low=dl)"));
  _io->println(F("  baud <n>         force bridge UART baud (else host-tracked)"));
  _io->println(F("  rfidbaud <n>     set RFID snoop baud"));
  _io->println(F("  rfidparity [r|h] n|e|o  RFID parity per tap (r=module/8E1, h=RTL/8O1; omit=both)"));
  _io->println(F("  snoop on|off     enable/disable RFID frame printing"));
  _io->println(F("  mon on|off       tap bridge traffic (> host->RTL, < RTL->host)"));
  _io->println(F("  status           show line states, bauds, counters"));
  _io->println(F("  help             this list"));
}

void ControlConsole::printStatus() {
  _io->println(F("--- status ---"));
  _io->printf("  bridge baud   : %lu\r\n", (unsigned long)_rtl->baud());
  _io->printf("  CHIP_EN reset : %s\r\n", _rtl->resetAsserted() ? "ASSERTED" : "released");
  _io->printf("  DL strap (PA7): %s\r\n", _rtl->strapAsserted() ? "ASSERTED (low)" : "released");
  _io->printf("  host->RTL     : %lu bytes\r\n", (unsigned long)_rtl->bytesToRtl());
  _io->printf("  RTL->host     : %lu bytes\r\n", (unsigned long)_rtl->bytesFromRtl());
  _io->printf("  RFID snoop    : %s\r\n", _snoop->enabled() ? "on" : "off");
  _io->printf("  RFID reader (module TX, 8E1): %lu frames / %lu bytes\r\n",
              (unsigned long)_snoop->framesOn(0), (unsigned long)_snoop->bytesOn(0));
  _io->printf("  RFID host   (RTL TX,    8O1): %lu frames / %lu bytes\r\n",
              (unsigned long)_snoop->framesOn(1), (unsigned long)_snoop->bytesOn(1));
}

// Returns true if `s` equals `kw` (case-sensitive, whole token).
static bool is(const char* s, const char* kw) { return strcmp(s, kw) == 0; }

void ControlConsole::handleLine(char* line) {
  // Tokenize: verb + optional argument.
  char* verb = strtok(line, " \t");
  char* arg  = strtok(nullptr, " \t");
  if (!verb) return;

  if (is(verb, "help") || is(verb, "?")) {
    printHelp();
  } else if (is(verb, "status") || is(verb, "st")) {
    printStatus();
  } else if (is(verb, "reset")) {
    _rtl->reset();
    _io->println(F("ok: reset pulsed (application)"));
  } else if (is(verb, "download") || is(verb, "boot") || is(verb, "dl")) {
    _rtl->enterDownload();
    _io->println(F("ok: download-mode macro run (verify on bench!)"));
  } else if (is(verb, "run")) {
    _rtl->run();
    _io->println(F("ok: released strap, reset into application"));
  } else if (is(verb, "rst")) {
    if (arg && is(arg, "on"))  { _rtl->assertReset(true);  _io->println(F("ok: RST held")); }
    else if (arg && is(arg, "off")) { _rtl->assertReset(false); _io->println(F("ok: RST released")); }
    else _io->println(F("usage: rst on|off"));
  } else if (is(verb, "strap")) {
    if (arg && is(arg, "on"))  { _rtl->driveStrap(true);  _io->println(F("ok: strap held low (UART offline)")); }
    else if (arg && is(arg, "off")) { _rtl->driveStrap(false); _io->println(F("ok: strap released (UART online)")); }
    else _io->println(F("usage: strap on|off"));
  } else if (is(verb, "baud")) {
    if (arg) { uint32_t b = strtoul(arg, nullptr, 10); _rtl->setBaud(b);
               _io->printf("ok: bridge baud = %lu\r\n", (unsigned long)b); }
    else _io->println(F("usage: baud <n>"));
  } else if (is(verb, "rfidbaud")) {
    if (arg) { uint32_t b = strtoul(arg, nullptr, 10); _snoop->setBaud(b);
               _io->printf("ok: RFID snoop baud = %lu\r\n", (unsigned long)b); }
    else _io->println(F("usage: rfidbaud <n>"));
  } else if (is(verb, "rfidparity")) {
    // Forms: "rfidparity <r|h> <n|e|o>" (one tap) or "rfidparity <n|e|o>" (both).
    // The JY-L601D talks 8O1 toward the module and 8E1 back, so the taps differ.
    char* arg2 = strtok(nullptr, " \t");
    int   ch   = -1;            // -1 = apply to both taps
    const char* pstr = arg;
    if (arg && is(arg, "r"))      { ch = 0; pstr = arg2; }
    else if (arg && is(arg, "h")) { ch = 1; pstr = arg2; }

    uint16_t fmt; bool ok = true;
    if      (pstr && is(pstr, "n")) fmt = SERIAL_8N1;
    else if (pstr && is(pstr, "e")) fmt = SERIAL_8E1;
    else if (pstr && is(pstr, "o")) fmt = SERIAL_8O1;
    else ok = false;

    if (!ok) {
      _io->println(F("usage: rfidparity [r|h] n|e|o  (r=reader/module, h=host/RTL)"));
    } else {
      const char* fn = (fmt == SERIAL_8N1) ? "8N1" : (fmt == SERIAL_8E1) ? "8E1" : "8O1";
      if (ch < 0) {
        _snoop->setFormat(0, fmt); _snoop->setFormat(1, fmt);
        _io->printf("ok: RFID reader+host parity = %s\r\n", fn);
      } else {
        _snoop->setFormat(ch, fmt);
        _io->printf("ok: RFID %s parity = %s\r\n", ch == 0 ? "reader(module)" : "host(RTL)", fn);
      }
    }
  } else if (is(verb, "snoop")) {
    if (arg && is(arg, "on"))  { _snoop->setEnabled(true);  _io->println(F("ok: snoop on")); }
    else if (arg && is(arg, "off")) { _snoop->setEnabled(false); _io->println(F("ok: snoop off")); }
    else _io->println(F("usage: snoop on|off"));
  } else if (is(verb, "mon")) {
    if (arg && is(arg, "on"))  { _rtl->setMonitorEnabled(true);  _io->println(F("ok: bridge tap on (> host->RTL, < RTL->host)")); }
    else if (arg && is(arg, "off")) { _rtl->setMonitorEnabled(false); _io->println(F("ok: bridge tap off")); }
    else _io->println(F("usage: mon on|off"));
  } else {
    _io->printf("? unknown command '%s' (try 'help')\r\n", verb);
  }
}

void ControlConsole::service() {
  if (!_io) return;

  // One-time greeting once the host opens the control port.
  // Require enough TX room for the whole banner so it can't block mid-print.
  if (!_greeted && _io->availableForWrite() >= 128) {
    _io->println();
    _io->println(F("== Petlibro RTL8721DM bridge — control + RFID snoop =="));
    _io->println(F("type 'help' for commands"));
    _greeted = true;
  }

  // Live-log bridge baud changes so we can see what a flashing host requests.
  // Non-blocking: only emit when there's TX room; retry next loop otherwise.
  uint32_t b = _rtl->baud();
  if (b != _lastBaud && _io->availableForWrite() >= 40) {
    _io->printf("[bridge] RTL UART baud -> %lu\r\n", (unsigned long)b);
    _lastBaud = b;
  }

  // Relay RFID frames to this port.
  _snoop->service(*_io);

  // Line-buffered command input.
  while (_io->available()) {
    char c = (char)_io->read();
    if (c == '\r') continue;
    if (c == '\n') {
      _buf[_len] = '\0';
      if (_len) handleLine(_buf);
      _len = 0;
    } else if (_len < sizeof(_buf) - 1) {
      _buf[_len++] = c;
    } else {
      _len = 0;   // overflow: drop the line
    }
  }
}
