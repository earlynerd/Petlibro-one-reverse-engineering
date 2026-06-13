#include "config.h"
#include "rfid_snoop.h"
#include <stdio.h>
#include <string.h>

RfidSnoop Snoop;

// ---- Modbus / FDX-B decode helpers -----------------------------------------

// Standard Modbus RTU CRC16 (poly 0xA001, init 0xFFFF). Transmitted low byte
// first, so the frame's trailing two bytes are crc_lo, crc_hi.
static uint16_t modbusCrc(const uint8_t* p, size_t n) {
  uint16_t crc = 0xFFFF;
  for (size_t i = 0; i < n; i++) {
    crc ^= p[i];
    for (int b = 0; b < 8; b++)
      crc = (crc & 1) ? (uint16_t)((crc >> 1) ^ 0xA001) : (uint16_t)(crc >> 1);
  }
  return crc;
}

// Length of the FIRST Modbus PDU in the buffer, derived from the function code
// and (for variable-length frames) the embedded byte count. Lets us validate
// and decode the leading frame even when the idle-gap framer merged two PDUs.
static size_t frameLen(const uint8_t* b, size_t n, bool master) {
  if (n < 2) return n;
  uint8_t func = b[1];
  if (func & 0x80) return 5;                       // addr+fn+code+crc2 (exception)
  if (master) {
    switch (func) {
      case 0x03: case 0x04: case 0x06: return 8;   // addr+fn+addr2+val2+crc2
      case 0x10: return (n >= 7) ? (size_t)(9 + b[6]) : n; // +byteCount+data
      default:   return n;
    }
  } else {
    switch (func) {
      case 0x03: case 0x04: return (n >= 3) ? (size_t)(5 + b[2]) : n; // +byteCount+data
      case 0x06: case 0x10: return 8;
      default:   return n;
    }
  }
}

static bool crcOkFor(const uint8_t* b, size_t flen) {
  if (flen < 4) return false;
  return modbusCrc(b, flen - 2) == (uint16_t)(b[flen - 2] | (b[flen - 1] << 8));
}

// Format `v` as a zero-padded decimal string of exactly `digits` chars (+ NUL).
// Avoids 64-bit printf (%llu) which isn't reliably enabled on this toolchain.
static void fmtPadDec(uint64_t v, int digits, char* out) {
  out[digits] = '\0';
  for (int i = digits - 1; i >= 0; --i) { out[i] = (char)('0' + (int)(v % 10)); v /= 10; }
}

// Host channel may be disabled (one-wire link) via RFID_HOST_RX_PIN == -1.
static constexpr int  kHostPin     = RFID_HOST_RX_PIN;
static constexpr bool kHostPresent = (kHostPin >= 0);

RfidSnoop::RfidSnoop()
  : _readerUart(NOPIN, RFID_READER_RX_PIN),
    _hostUart(NOPIN, kHostPresent ? (pin_size_t)kHostPin : (pin_size_t)NOPIN),
    _hostEnabled(kHostPresent),
    _baud(RFID_SNOOP_BAUD),
    _readerFormat(RFID_READER_FORMAT),
    _hostFormat(RFID_HOST_FORMAT) {}

void RfidSnoop::begin() {
  _reader.uart     = &_readerUart;
  _reader.tag      = "RFID-R";
  _reader.isMaster = false;            // module TX -> slave replies
  _readerUart.begin(_baud, _readerFormat);

  if (_hostEnabled) {
    _host.uart     = &_hostUart;
    _host.tag      = "RFID-H";
    _host.isMaster = true;             // RTL TX -> master commands
    _hostUart.begin(_baud, _hostFormat);
  }
}

void RfidSnoop::restart() {
  _readerUart.end();
  _readerUart.begin(_baud, _readerFormat);
  if (_hostEnabled) {
    _hostUart.end();
    _hostUart.begin(_baud, _hostFormat);
  }
}

void RfidSnoop::setBaud(uint32_t baud) {
  if (!baud || baud == _baud) return;
  _baud = baud;
  restart();
}

void RfidSnoop::setFormat(int channel, uint16_t fmt) {
  if (channel == 0) {
    _readerFormat = fmt;
    _readerUart.end();
    _readerUart.begin(_baud, _readerFormat);
  } else {
    if (!_hostEnabled) return;
    _hostFormat = fmt;
    _hostUart.end();
    _hostUart.begin(_baud, _hostFormat);
  }
}

void RfidSnoop::service(Stream& out) {
  // Drain the master (host) tap first so a request is decoded before its reply
  // within the same pass -- the reply needs the request's register address.
  if (_hostEnabled) serviceChannel(_host, out);
  serviceChannel(_reader, out);
}

void RfidSnoop::serviceChannel(Channel& ch, Stream& out) {
  // Drain everything currently buffered by the PIO RX FIFO.
  while (ch.uart->available()) {
    int b = ch.uart->read();
    if (b < 0) break;
    ch.lastByteUs = micros();
    ch.bytes++;
    if (ch.len < kBufLen) ch.buf[ch.len++] = (uint8_t)b;
    if (ch.len == kBufLen && _enabled) emitFrame(ch, out);  // flush a full buffer
  }

  // Close the frame once the line has been idle long enough.
  if (ch.len && (micros() - ch.lastByteUs) > RFID_FRAME_GAP_US) {
    if (_enabled) emitFrame(ch, out);
    else ch.len = 0;   // still count, but drop the body when snoop is muted
  }
}

void RfidSnoop::emitFrame(Channel& ch, Stream& out) {
  ch.frames++;

  // Decode ALWAYS (before any TX-room check) so request/tag-presence state stays
  // correct even when a backed-up console makes us drop the printed line -- a
  // dropped reply must still reset the missed-poll counter, or we'd spuriously
  // report the tag as removed.
  char    desc[80];
  char    tagId[20] = {0};
  uint8_t tagQ      = 0;
  int     ev        = decodeFrame(ch, desc, sizeof(desc), tagId, &tagQ);

  // Non-blocking print: only emit the frame line if the control-port TX can take
  // it whole (hex dump + decoded annotation, ~160 bytes for a typical frame).
  if (out.availableForWrite() >= (int)(ch.len * 4 + 160)) {
    uint32_t ms = millis();
    out.printf("[%8lu.%03lu][%s] %2u:", ms / 1000UL, ms % 1000UL, ch.tag,
               (unsigned)ch.len);
    for (size_t i = 0; i < ch.len; i++) out.printf(" %02X", ch.buf[i]);
    out.print("  | ");
    for (size_t i = 0; i < ch.len; i++) {
      char c = (char)ch.buf[i];
      out.write((c >= 0x20 && c < 0x7F) ? c : '.');
    }
    out.print("  :: ");
    out.print(desc);
    out.println();
  }

  // Presence transitions stand out on their own line (guarded for TX room).
  if (ev != 0 && out.availableForWrite() >= 48) {
    if (ev > 0) out.printf("    *** TAG %s DETECTED (q=%u) ***\r\n", tagId, (unsigned)tagQ);
    else        out.println(F("    *** TAG REMOVED ***"));
  }
  ch.len = 0;
}

int RfidSnoop::decodeFrame(const Channel& ch, char* desc, size_t cap,
                           char* tagId, uint8_t* q) {
  const uint8_t* b = ch.buf;
  size_t         n = ch.len;
  int            ev = 0;

  if (n < 4) { snprintf(desc, cap, "runt (%u b)", (unsigned)n); return 0; }

  size_t flen  = frameLen(b, n, ch.isMaster);
  bool   crcOk = (n >= flen) && crcOkFor(b, flen);
  uint8_t func = b[1];
  const char* sfx = crcOk ? "" : " CRC?";

  if (func & 0x80) {                       // exception reply
    snprintf(desc, cap, "EXCEPTION fn0x%02X code0x%02X%s", func & 0x7F, b[2], sfx);
    return 0;
  }

  if (ch.isMaster) {
    // Request: addr func addrHi addrLo (qty|value)Hi Lo ...
    uint16_t a = (b[2] << 8) | b[3];
    uint16_t v = (b[4] << 8) | b[5];
    switch (func) {
      case 0x03: case 0x04:
        snprintf(desc, cap, "READ %u reg @0x%04X%s%s",
                 v, a, (a == kTagReg) ? " [TAG]" : "", sfx);
        if (a == kTagReg) {                // a tag poll; replies stopping => gone
          if (_missedTagPolls < 255) _missedTagPolls++;
          if (_tagPresent && _missedTagPolls >= kTagMissThresh) {
            _tagPresent = false; ev = -1;
          }
        }
        break;
      case 0x06:
        snprintf(desc, cap, "WRITE @0x%04X = 0x%04X%s", a, v, sfx);
        break;
      case 0x10:
        snprintf(desc, cap, "WRITE-MULTI %u reg @0x%04X%s", v, a, sfx);
        break;
      default:
        snprintf(desc, cap, "req fn0x%02X%s", func, sfx);
        break;
    }
    _lastReqFunc = func;
    _lastReqAddr = a;
  } else {
    // Reply: register address is implicit -> use the remembered request.
    switch (func) {
      case 0x03: case 0x04: {
        uint8_t bc = b[2];                 // byte count
        bool isTag = crcOk && (_lastReqAddr == kTagReg) && (bc >= 8) && (n >= 11);
        if (isTag) {
          uint16_t country = (b[3] << 8) | b[4];
          uint64_t nat = ((uint64_t)b[5] << 32) | ((uint64_t)b[6] << 24) |
                         ((uint64_t)b[7] << 16) | ((uint64_t)b[8] << 8) | b[9];
          char nat12[13];
          fmtPadDec(nat, 12, nat12);
          snprintf(tagId, 20, "%u%s", country, nat12);
          *q = b[10];
          snprintf(desc, cap, "READ reply: TAG %s  q=%u", tagId, (unsigned)b[10]);
          _missedTagPolls = 0;
          if (!_tagPresent) { _tagPresent = true; ev = +1; }
        } else {
          snprintf(desc, cap, "READ reply: %u reg%s", bc / 2, sfx);
        }
        break;
      }
      case 0x06:
        snprintf(desc, cap, "WRITE ack @0x%04X = 0x%04X%s",
                 (b[2] << 8) | b[3], (b[4] << 8) | b[5], sfx);
        break;
      case 0x10:
        snprintf(desc, cap, "WRITE-MULTI ack %u reg @0x%04X%s",
                 (b[4] << 8) | b[5], (b[2] << 8) | b[3], sfx);
        break;
      default:
        snprintf(desc, cap, "reply fn0x%02X%s", func, sfx);
        break;
    }
  }

  // Flag a frame that the idle-gap framer merged with the next PDU.
  if (n > flen && flen >= 4) {
    size_t used = strlen(desc);
    if (used < cap) snprintf(desc + used, cap - used, " (+%u b merged)", (unsigned)(n - flen));
  }
  return ev;
}

uint32_t RfidSnoop::framesOn(int channel) const {
  return channel == 0 ? _reader.frames : _host.frames;
}
uint32_t RfidSnoop::bytesOn(int channel) const {
  return channel == 0 ? _reader.bytes : _host.bytes;
}
