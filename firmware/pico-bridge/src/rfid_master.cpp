#include "config.h"
#include "rfid_master.h"
#include <string.h>

RfidMaster Master;

// The TX pin doubles as the snoop's host tap (RFID_HOST_RX_PIN); if that is
// disabled (-1, one-wire link) there is no command line to drive, so master
// mode is unavailable. Guard the SerialPIO construction against a negative pin.
static constexpr int  kTxPin     = RFID_MASTER_TX_PIN;
static constexpr bool kTxPresent = (kTxPin >= 0);

// Standard Modbus RTU CRC16 (poly 0xA001, init 0xFFFF), low byte transmitted
// first -- same as rfid_snoop.cpp's, kept local so the two modules stay
// decoupled (it's a small, fixed, well-known function).
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

RfidMaster::RfidMaster()
  : _tx(kTxPresent ? (pin_size_t)kTxPin : (pin_size_t)NOPIN, NOPIN),  // TX-only
    _rx(NOPIN, RFID_MASTER_RX_PIN),                                   // RX-only
    _timeoutMs(RFID_MASTER_TIMEOUT_MS) {}

bool RfidMaster::begin() {
  if (_active) return true;
  if (!kTxPresent) return false;            // no command line to drive
  _tx.begin(RFID_SNOOP_BAUD, RFID_MASTER_TX_FORMAT);   // 8O1 commands out (GP4 -> OUTPUT)
  _rx.begin(RFID_SNOOP_BAUD, RFID_MASTER_RX_FORMAT);   // 8E1 replies in
  _active = true;
  return true;
}

void RfidMaster::end() {
  if (!_active) return;
  _rx.end();
  _tx.end();
  // SerialPIO::end() leaves the TX pin as a driven OUTPUT (it only clears the
  // SM + outover); force it back to high-Z so the command net is released
  // before the host comes out of reset.
  if (kTxPresent) pinMode(kTxPin, INPUT);
  _active = false;
}

void RfidMaster::drainRx() {
  while (_rx.read() >= 0) { /* discard */ }
}

// Read one byte at a time off the RX queue. Wait up to _timeoutMs for the FIRST
// reply byte; once bytes start arriving, close the frame after RFID_FRAME_GAP_US
// of line idle (Modbus RTU inter-frame gap). yield() only while idle so USB/CDC
// stays serviced without starving a fast burst.
int RfidMaster::recvFrame(uint8_t* buf, size_t cap) {
  uint32_t startMs = millis();
  uint32_t lastUs  = micros();
  size_t   len     = 0;
  bool     started = false;
  for (;;) {
    int b = _rx.read();
    if (b >= 0) {
      started = true;
      if (len < cap) buf[len++] = (uint8_t)b;
      lastUs = micros();
      continue;
    }
    if (!started) {
      if (millis() - startMs >= _timeoutMs) return 0;          // no reply at all
    } else if ((micros() - lastUs) >= RFID_FRAME_GAP_US) {
      return (int)len;                                          // frame complete
    }
    yield();
  }
}

void RfidMaster::sendBytes(const uint8_t* b, size_t n) {
  for (size_t i = 0; i < n; i++) _tx.write(b[i]);
  _tx.flush();   // block until the whole frame is on the wire, so the reply
                 // timing and the trailing idle gap are measured correctly
}

int RfidMaster::transact(const uint8_t* pdu, size_t n, bool appendCrc,
                         uint8_t* rsp, size_t rspCap) {
  if (!_active) return 0;
  uint8_t frame[64];
  if (n + (appendCrc ? 2 : 0) > sizeof(frame)) return 0;
  memcpy(frame, pdu, n);
  size_t flen = n;
  if (appendCrc) {
    uint16_t c = modbusCrc(frame, n);
    frame[flen++] = (uint8_t)(c & 0xFF);
    frame[flen++] = (uint8_t)(c >> 8);
  }
  drainRx();                 // clear any line noise before the exchange
  sendBytes(frame, flen);
  return recvFrame(rsp, rspCap);
}

int RfidMaster::readRegs(uint16_t addr, uint16_t qty, uint16_t* out, size_t outCap) {
  uint8_t pdu[6] = { RFID_MB_SLAVE, 0x03,
                     (uint8_t)(addr >> 8), (uint8_t)(addr & 0xFF),
                     (uint8_t)(qty  >> 8), (uint8_t)(qty  & 0xFF) };
  uint8_t rsp[96];   // up to 37 regs (3 + 2*37 + 2 = 79 B); covers kMaxBlockRegs
  int n = transact(pdu, sizeof(pdu), true, rsp, sizeof(rsp));
  if (n <= 0) return kTimeout;                       // silent (reg absent/unreadable)
  if (n >= 3 && (rsp[1] & 0x80)) {                   // Modbus exception
    return -(int)rsp[2];
  }
  if (n < 5 || rsp[0] != RFID_MB_SLAVE || rsp[1] != 0x03) return kBadFrame;
  uint8_t bc = rsp[2];                               // byte count
  size_t  need = (size_t)3 + bc + 2;                 // hdr + data + crc
  if ((size_t)n < need || !crcOk(rsp, need))         return kBadFrame;
  int regs = bc / 2;
  for (int i = 0; i < regs && (size_t)i < outCap; i++)
    out[i] = (uint16_t)((rsp[3 + i * 2] << 8) | rsp[4 + i * 2]);
  return regs;
}

bool RfidMaster::writeReg(uint16_t addr, uint16_t value) {
  uint8_t pdu[6] = { RFID_MB_SLAVE, 0x06,
                     (uint8_t)(addr  >> 8), (uint8_t)(addr  & 0xFF),
                     (uint8_t)(value >> 8), (uint8_t)(value & 0xFF) };
  uint8_t rsp[16];
  int n = transact(pdu, sizeof(pdu), true, rsp, sizeof(rsp));
  if (n < 8) return false;
  // A 0x06 ack echoes the request verbatim.
  return rsp[0] == RFID_MB_SLAVE && rsp[1] == 0x06 &&
         rsp[2] == pdu[2] && rsp[3] == pdu[3] &&
         rsp[4] == pdu[4] && rsp[5] == pdu[5] && crcOk(rsp, 8);
}

// One-line note for a base address we already understand (Docs/RFID_PROTOCOL.md).
static const char* knownReg(uint16_t a) {
  switch (a) {
    case 0x0000: return "config/mode (RTL wrote 0x0002 at boot)";
    case 0x000E: return "FDX-B tag block: country, 38-bit national ID, read-quality";
    default:     return nullptr;
  }
}

int RfidMaster::readBlock(uint16_t addr, uint16_t maxQty, uint16_t* out, size_t cap) {
  int n = readRegs(addr, 1, out, cap);          // discovery: probe the start address
  if (n <= 0) return n;                          // silent (not a base) / exception / bad
  if (n > 1)  return n;                          // already got >1 reg (ignores qty)
  if (maxQty <= 1) return n;

  // The module honours qty: a recognised base returns however many consecutive
  // registers we ask for. Pull the whole window in ONE read.
  int g = readRegs(addr, maxQty, out, cap);
  if (g > 1) return g;                            // got the block

  // maxQty overshot the readable range and was refused -> grow up to find the
  // largest qty it will serve. (readRegs only writes out[] on success, so a
  // refused read leaves the last good payload intact.)
  int best = readRegs(addr, 1, out, cap);         // restore the single-register payload
  if (best < 1) best = 1;
  for (uint16_t q = 2; q <= maxQty; q++) {
    int gg = readRegs(addr, q, out, cap);
    if (gg >= (int)q) { best = gg; continue; }    // full read -> keep growing
    if (gg > best) best = gg;                      // clamp returned a longer block
    break;                                         // refusal/clamp -> ceiling found
  }
  return best;
}

void RfidMaster::dump(Stream& io, uint16_t first, uint16_t last, uint16_t blockQty) {
  if (!_active) { io.println(F("master: not active -- run 'master on' first")); return; }
  if (last < first) { uint16_t t = first; first = last; last = t; }
  if (blockQty < 1) blockQty = 1;
  if (blockQty > kMaxBlockRegs) blockQty = (uint16_t)kMaxBlockRegs;

  io.printf("[master] sweep base addrs 0x%04X..0x%04X (qty=1); block-read each hit up to %u reg; %lu ms timeout\r\n",
            first, last, (unsigned)blockQty, (unsigned long)_timeoutMs);
  io.println(F("[master] answered base addresses only; press any key to abort"));

  // Per-address exception/silent lines are pure noise on a real sweep (a single
  // run can throw hundreds), so they're tallied, not printed. We keep a code
  // histogram for exceptions and capture the addresses of whichever non-answer
  // category is RARE (<=kListCap) so anomalies still surface -- e.g. a handful
  // of silent addresses amid a sea of illegal-address exceptions.
  static constexpr uint32_t kListCap = 32;
  uint32_t answered = 0, exceptions = 0, bad = 0, scanned = 0, silent = 0;
  uint32_t excHist[8] = {0};                      // [code] for codes 1..6, [7]=other
  uint16_t silentAddr[kListCap]; uint16_t excAddr[kListCap]; uint8_t excAddrCode[kListCap];

  uint16_t blk[kMaxBlockRegs];
  for (uint32_t a = first; a <= last; a++) {
    if (io.available()) {                       // user abort
      while (io.available()) io.read();
      io.println(F("[master] aborted"));
      break;
    }
    int r = readBlock((uint16_t)a, blockQty, blk, kMaxBlockRegs);
    scanned++;
    if (r == kTimeout) {                          // silent: no reply at all
      if (silent < kListCap) silentAddr[silent] = (uint16_t)a;
      silent++;
      continue;
    }
    if (r == kBadFrame) { bad++; continue; }      // reply failed CRC/length
    if (r < 0) {                                  // Modbus exception (negated code)
      uint8_t code = (uint8_t)(-r);
      if (exceptions < kListCap) { excAddr[exceptions] = (uint16_t)a; excAddrCode[exceptions] = code; }
      excHist[(code >= 1 && code <= 6) ? code : 7]++;
      exceptions++;
      continue;
    }
    answered++;
    int shown = (r < (int)kMaxBlockRegs) ? r : (int)kMaxBlockRegs;
    io.printf("  0x%04X (%d reg):", (unsigned)a, r);
    for (int i = 0; i < shown; i++) io.printf(" %04X", blk[i]);
    const char* note = knownReg((uint16_t)a);
    if (note) io.printf("   <- %s", note);
    io.println();
  }

  io.printf("[master] done: %lu scanned, %lu base(s), %lu exception(s), %lu bad, %lu silent\r\n",
            (unsigned long)scanned, (unsigned long)answered, (unsigned long)exceptions,
            (unsigned long)bad, (unsigned long)silent);

  if (exceptions) {                               // exception-code breakdown
    io.print(F("  exception codes:"));
    for (int c = 1; c <= 6; c++) if (excHist[c]) io.printf(" 0x%02X x%lu", c, (unsigned long)excHist[c]);
    if (excHist[7]) io.printf(" other x%lu", (unsigned long)excHist[7]);
    io.println();
  }
  if (silent && silent <= kListCap) {             // rare category: list it
    io.print(F("  silent addrs:"));
    for (uint32_t i = 0; i < silent; i++) io.printf(" 0x%04X", silentAddr[i]);
    io.println();
  }
  if (exceptions && exceptions <= kListCap) {
    io.print(F("  exception addrs:"));
    for (uint32_t i = 0; i < exceptions; i++) io.printf(" 0x%04X(%02X)", excAddr[i], excAddrCode[i]);
    io.println();
  }
}
