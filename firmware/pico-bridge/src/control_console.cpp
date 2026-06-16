#include <string.h>
#include <stdlib.h>
#include <hardware/gpio.h>
#include "config.h"
#include "control_console.h"

ControlConsole Console;

void ControlConsole::begin(Stream* io, RtlBridge* rtl, RfidSnoop* snoop, RfidMaster* master) {
  _io = io;
  _rtl = rtl;
  _snoop = snoop;
  _master = master;
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
  _io->println(F("  rfinject 4|5 0|1 drive GP4(RTL-TX net)/GP5(module-TX net) to find the RTL RFID-UART pins"));
  _io->println(F("  rfinject off     release GP4/GP5, resume snoop"));
  _io->println(F("  pwen 1|0|read|off  GP6 = module PWEN: 1=power module ON, read/scan to trace its RTL pin"));
  _io->println(F("  --- RFID master mode (Pico drives the module; holds RTL in reset) ---"));
  _io->println(F("  master on|off    enter/leave master mode (reset RTL + suspend snoop)"));
  _io->println(F("  master init      write reg 0x0000 = 0x0002 (the RTL's boot enable)"));
  _io->println(F("  master read <addr> [qty]   read holding register(s) (hex addr)"));
  _io->println(F("  master write <addr> <val>  write single register (hex)"));
  _io->println(F("  master dump [first] [last] [blkqty] find base addrs + block-read each (def 0x0000..0x00FF, 16)"));
  _io->println(F("  master tx <hex...>         send PDU + auto-CRC, print raw reply"));
  _io->println(F("  master txraw <hex...>      send bytes verbatim (you supply CRC)"));
  _io->println(F("  master timeout <ms>        per-transaction reply timeout"));
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
  _io->printf("  RFID snoop    : %s%s\r\n", _snoop->enabled() ? "on" : "off",
              _snoop->suspended() ? " (SUSPENDED — master mode owns the pins)" : "");
  _io->printf("  RFID reader (module TX, 8E1): %lu frames / %lu bytes\r\n",
              (unsigned long)_snoop->framesOn(0), (unsigned long)_snoop->bytesOn(0));
  _io->printf("  RFID host   (RTL TX,    8O1): %lu frames / %lu bytes\r\n",
              (unsigned long)_snoop->framesOn(1), (unsigned long)_snoop->bytesOn(1));
  _io->printf("  RFID master   : %s (timeout %lu ms)\r\n",
              _master->active() ? "ACTIVE — Pico is the Modbus master" : "off",
              (unsigned long)_master->timeout());
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
    if (masterGuard()) return;
    _rtl->reset();
    _io->println(F("ok: reset pulsed (application)"));
  } else if (is(verb, "download") || is(verb, "boot") || is(verb, "dl")) {
    if (masterGuard()) return;
    _rtl->enterDownload();
    _io->println(F("ok: download-mode macro run (verify on bench!)"));
  } else if (is(verb, "run")) {
    if (masterGuard()) return;
    _rtl->run();
    _io->println(F("ok: released strap, reset into application"));
  } else if (is(verb, "rst")) {
    if (masterGuard()) return;
    if (arg && is(arg, "on"))  { _rtl->assertReset(true);  _io->println(F("ok: RST held")); }
    else if (arg && is(arg, "off")) { _rtl->assertReset(false); _io->println(F("ok: RST released")); }
    else _io->println(F("usage: rst on|off"));
  } else if (is(verb, "strap")) {
    if (masterGuard()) return;
    if (arg && is(arg, "on"))  { _rtl->driveStrap(true);  _io->println(F("ok: strap held low (UART offline)")); }
    else if (arg && is(arg, "off")) { _rtl->driveStrap(false); _io->println(F("ok: strap released (UART online)")); }
    else _io->println(F("usage: strap on|off"));
  } else if (is(verb, "master") || is(verb, "m")) {
    handleMaster(arg);
  } else if (is(verb, "rfinject") || is(verb, "rfi")) {
    // Inject a static level onto an RFID UART net via the Pico's tap, so the
    // RTL pin on that same net can be found by scanning the RTL (no module power
    // needed). GP4 = RTL->module command net (find RFID-TX); GP5 = module->RTL
    // reply net (find RFID-RX). Suspends the snoop; never resets the RTL.
    if (masterGuard()) return;
    char* arg2 = strtok(nullptr, " \t");
    if (arg && is(arg, "off")) {
      gpio_init(RFID_HOST_RX_PIN);   gpio_set_dir(RFID_HOST_RX_PIN, GPIO_IN);
      gpio_init(RFID_READER_RX_PIN); gpio_set_dir(RFID_READER_RX_PIN, GPIO_IN);
      _snoop->resume();
      _io->println(F("ok: rfinject off — GP4/GP5 released, snoop resumed"));
    } else if (arg && arg2 && (is(arg, "4") || is(arg, "5"))) {
      int pin = is(arg, "4") ? RFID_HOST_RX_PIN : RFID_READER_RX_PIN;
      int lvl = (atoi(arg2) != 0) ? 1 : 0;
      _snoop->suspend();   // free GP4/GP5 from the snoop PIO UARTs
      gpio_init(RFID_HOST_RX_PIN);   gpio_set_dir(RFID_HOST_RX_PIN, GPIO_IN);   // both to input,
      gpio_init(RFID_READER_RX_PIN); gpio_set_dir(RFID_READER_RX_PIN, GPIO_IN); // then drive one
      gpio_init(pin); gpio_set_dir(pin, GPIO_OUT); gpio_put(pin, lvl);
      _io->printf("ok: GP%d = %d  (%s net) — now scan the RTL for the pin that follows\r\n",
                  pin, lvl, is(arg, "4") ? "RTL->module TX/cmd" : "module->RTL RX/reply");
    } else {
      _io->println(F("usage: rfinject 4|5 0|1  (GP4=RTL-TX net, GP5=module-TX net)  |  rfinject off"));
    }
  } else if (is(verb, "pwen") || is(verb, "pw")) {
    // GP6 is wired to the RFID module's PWEN (power-enable) pin. Drive it to
    // force the module on/off (independent of the RTL); `read` to sense what the
    // board does to it; drive it + scan the RTL to find the pin that natively
    // drives PWEN. Independent of GP4/GP5 -> no snoop/master interaction.
    if (arg && is(arg, "off")) {
      gpio_init(RFID_PWEN_PIN); gpio_set_dir(RFID_PWEN_PIN, GPIO_IN);
      _io->println(F("ok: PWEN (GP6) released to input (hi-Z)"));
    } else if (arg && (is(arg, "read") || is(arg, "r") || is(arg, "?"))) {
      gpio_init(RFID_PWEN_PIN); gpio_set_dir(RFID_PWEN_PIN, GPIO_IN);
      _io->printf("PWEN (GP6) input = %d\r\n", gpio_get(RFID_PWEN_PIN) ? 1 : 0);
    } else if (arg && (is(arg, "0") || is(arg, "1"))) {
      int lvl = is(arg, "1") ? 1 : 0;
      gpio_init(RFID_PWEN_PIN); gpio_set_dir(RFID_PWEN_PIN, GPIO_OUT); gpio_put(RFID_PWEN_PIN, lvl);
      _io->printf("ok: PWEN (GP6) = %d — module power %s (driving)\r\n",
                  lvl, lvl ? "ENABLED" : "disabled");
    } else {
      _io->println(F("usage: pwen 1|0 (drive on/off) | pwen read | pwen off   [GP6 -> module PWEN]"));
    }
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

// Block host reset/strap verbs while master mode owns GP4: releasing the RTL
// from reset would make it drive the command net against our TX, and toggling
// the strap tears down the bridge UART mid-session. Make the user exit first.
bool ControlConsole::masterGuard() {
  if (_master->active()) {
    _io->println(F("blocked: RFID master mode is active — 'master off' first"));
    return true;
  }
  return false;
}

// Print n bytes as space-separated hex.
static void printHex(Stream* io, const uint8_t* b, size_t n) {
  for (size_t i = 0; i < n; i++) io->printf(" %02X", b[i]);
}

void ControlConsole::handleMaster(char* sub) {
  if (!sub) {
    _io->printf("master: %s. Subcommands: on off init read write dump tx txraw timeout\r\n",
                _master->active() ? "ACTIVE" : "off");
    return;
  }

  // --- mode entry / exit ---------------------------------------------------
  if (is(sub, "on") || is(sub, "enter")) {
    if (_master->active()) { _io->println(F("master: already active")); return; }
    _rtl->assertReset(true);          // hold the RTL off -> it releases the command net
    _snoop->suspend();                // free GP4/GP5 for us to claim
    if (!_master->begin()) {          // claim GP4(TX,8O1) + GP5(RX,8E1)
      _snoop->resume();
      _rtl->assertReset(false);
      _io->println(F("master: begin failed (host/command pin disabled in config?)"));
      return;
    }
    _io->println(F("ok: MASTER MODE — RTL held in reset, snoop suspended, Pico drives the module"));
    _io->println(F("    (verify on the bench the RTL releases the command line; 'master off' to restore)"));
    return;
  }
  if (is(sub, "off") || is(sub, "exit")) {
    if (!_master->active()) { _io->println(F("master: not active")); return; }
    _master->end();                   // release GP4 (back to hi-Z input)
    _snoop->resume();                 // snoop reclaims GP4/GP5 (RX-only)
    _rtl->assertReset(false);         // let the host boot again
    _io->println(F("ok: master off — snoop resumed, RTL released (booting application)"));
    return;
  }
  if (is(sub, "timeout")) {
    char* a = strtok(nullptr, " \t");
    if (a) { _master->setTimeout(strtoul(a, nullptr, 10));
             _io->printf("ok: master timeout = %lu ms\r\n", (unsigned long)_master->timeout()); }
    else _io->println(F("usage: master timeout <ms>"));
    return;
  }

  // --- everything below needs the bus --------------------------------------
  if (!_master->active()) {
    _io->println(F("master: not active — run 'master on' first"));
    return;
  }

  if (is(sub, "init")) {
    bool ok = _master->writeReg(0x0000, 0x0002);
    _io->printf("master init: write 0x0000 = 0x0002 -> %s\r\n", ok ? "ACK" : "no/!ack");
    return;
  }
  if (is(sub, "read") || is(sub, "rd")) {
    char* aA = strtok(nullptr, " \t");
    char* aQ = strtok(nullptr, " \t");
    if (!aA) { _io->println(F("usage: master read <addr-hex> [qty]")); return; }
    uint16_t addr = (uint16_t)strtoul(aA, nullptr, 16);
    uint16_t qty  = aQ ? (uint16_t)strtoul(aQ, nullptr, 10) : 1;
    if (qty < 1) qty = 1;
    if (qty > RfidMaster::kMaxBlockRegs) qty = (uint16_t)RfidMaster::kMaxBlockRegs;  // reply must fit our buffers
    uint16_t v[RfidMaster::kMaxBlockRegs];
    int r = _master->readRegs(addr, qty, v, RfidMaster::kMaxBlockRegs);
    if (r > 0) {
      for (int i = 0; i < r; i++)
        _io->printf("  0x%04X = 0x%04X  (%u)\r\n", (unsigned)(addr + i), v[i], v[i]);
    } else if (r == RfidMaster::kTimeout) {
      _io->printf("  0x%04X : no reply (silent — register absent or no tag)\r\n", addr);
    } else if (r == RfidMaster::kBadFrame) {
      _io->printf("  0x%04X : reply failed CRC/length check\r\n", addr);
    } else {
      _io->printf("  0x%04X : EXCEPTION 0x%02X\r\n", addr, (unsigned)(-r));
    }
    return;
  }
  if (is(sub, "write") || is(sub, "wr")) {
    char* aA = strtok(nullptr, " \t");
    char* aV = strtok(nullptr, " \t");
    if (!aA || !aV) { _io->println(F("usage: master write <addr-hex> <val-hex>")); return; }
    uint16_t addr = (uint16_t)strtoul(aA, nullptr, 16);
    uint16_t val  = (uint16_t)strtoul(aV, nullptr, 16);
    bool ok = _master->writeReg(addr, val);
    _io->printf("master write 0x%04X = 0x%04X -> %s\r\n", addr, val, ok ? "ACK" : "no/!ack");
    return;
  }
  if (is(sub, "dump")) {
    char* aF = strtok(nullptr, " \t");
    char* aL = strtok(nullptr, " \t");
    char* aB = strtok(nullptr, " \t");
    uint16_t first = aF ? (uint16_t)strtoul(aF, nullptr, 16) : RFID_MASTER_DUMP_FIRST;
    uint16_t last  = aL ? (uint16_t)strtoul(aL, nullptr, 16) : RFID_MASTER_DUMP_LAST;
    uint16_t blk   = aB ? (uint16_t)strtoul(aB, nullptr, 10) : RFID_MASTER_BLOCK_QTY;
    _master->dump(*_io, first, last, blk);
    return;
  }
  if (is(sub, "tx") || is(sub, "txraw")) {
    bool appendCrc = is(sub, "tx");
    uint8_t pdu[32];
    size_t  n = 0;
    char*   t;
    while ((t = strtok(nullptr, " \t")) != nullptr && n < sizeof(pdu))
      pdu[n++] = (uint8_t)strtoul(t, nullptr, 16);
    if (n < (appendCrc ? 2u : 4u)) {
      _io->printf("usage: master %s <hex bytes...>%s\r\n", sub,
                  appendCrc ? "  (CRC appended for you)" : "  (include the 2 CRC bytes)");
      return;
    }
    uint8_t rsp[64];
    _io->print(F("  tx:"));   printHex(_io, pdu, n);
    if (appendCrc) _io->print(F(" +crc"));
    _io->println();
    int r = _master->transact(pdu, n, appendCrc, rsp, sizeof(rsp));
    if (r > 0) { _io->printf("  rx %d:", r); printHex(_io, rsp, (size_t)r); _io->println(); }
    else       _io->println(F("  rx: no reply (timeout)"));
    return;
  }

  _io->printf("? unknown master subcommand '%s' (try 'help')\r\n", sub);
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
