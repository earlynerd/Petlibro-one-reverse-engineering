#pragma once
#include <Arduino.h>
#include <SerialPIO.h>

// Passive listener for the JY-L601D RFID module's UART link. Each of the two
// data lines is sampled by an RX-only PIO UART (the RP2040 has only two
// hardware UARTs, one of which is the RTL bridge, so PIO gives us the extra
// receivers). Bytes are reassembled into frames by inter-byte idle gap and
// emitted as tagged, timestamped hex+ASCII records.
//
// This is a protocol-agnostic capture layer: it does not yet decode the
// JY-L601D / FDX-B frame structure. Capture real traffic first, then add a
// decoder keyed off the observed framing.
class RfidSnoop {
public:
  RfidSnoop();

  void begin();
  void setBaud(uint32_t baud);
  // Set framing for ONE tap independently. channel: 0 = reader (module TX),
  // 1 = host (RTL TX). The JY-L601D uses different parity per direction, so the
  // two taps are configured separately. Re-inits just that PIO UART.
  void setFormat(int channel, uint16_t fmt);   // SERIAL_8N1 / 8E1 / 8O1 etc.

  // Poll both PIO receivers and emit any completed frames to `out`.
  void service(Stream& out);

  void setEnabled(bool en) { _enabled = en; }
  bool enabled() const { return _enabled; }

  // Release / reclaim the two PIO RX UARTs so another consumer (RFID master
  // mode) can drive the same GP4/GP5 pins. suspend() ends both UARTs and makes
  // service() a no-op; resume() re-inits them at the stored baud/formats and
  // clears any half-assembled frame. Idempotent.
  void suspend();
  void resume();
  bool suspended() const { return _suspended; }

  uint32_t framesOn(int channel) const;   // 0 = reader, 1 = host
  uint32_t bytesOn(int channel) const;

private:
  static constexpr size_t   kBufLen        = 64;
  static constexpr uint16_t kTagReg        = 0x000E; // tag-data block start register
  static constexpr uint8_t  kTagMissThresh = 3;      // unanswered tag polls -> "removed"
                                                     // (>=3 tolerates an odd dropped reply)

  struct Channel {
    SerialPIO*  uart   = nullptr;
    const char* tag    = "";
    bool        isMaster = false;  // host(RTL)=command side; reader(module)=reply side
    uint8_t     buf[kBufLen];
    size_t      len    = 0;
    uint32_t    lastByteUs = 0;
    uint32_t    frames = 0;
    uint32_t    bytes  = 0;
  };

  void serviceChannel(Channel& ch, Stream& out);
  void emitFrame(Channel& ch, Stream& out);
  // Decode one closed Modbus frame: write a human description into `desc`, and
  // if it is a tag-data read reply, decode the FDX-B id into `tagId` + quality.
  // Updates request/tag-presence state. Returns +1 if the tag just appeared,
  // -1 if it just disappeared, 0 otherwise.
  int  decodeFrame(const Channel& ch, char* desc, size_t cap, char* tagId, uint8_t* q);
  void restart();               // re-init both PIO UARTs at current _baud + per-channel formats

  SerialPIO _readerUart;
  SerialPIO _hostUart;
  Channel   _reader;
  Channel   _host;
  bool      _hostEnabled;
  bool      _enabled = false;   // default OFF: floating taps would spam/block CDC1
  bool      _suspended = false; // true while master mode owns the GP4/GP5 pins
  uint32_t  _baud;              // shared: link runs one baud both directions
  uint16_t  _readerFormat;      // reader tap (module TX) framing -- see config.h
  uint16_t  _hostFormat;        // host tap (RTL TX) framing

  // Modbus/FDX-B decode state. A Modbus reply carries no register address, so we
  // remember the master's last request (seen on the host tap) to interpret the
  // slave's reply (seen on the reader tap). Tag presence is inferred from whether
  // the tag-region read is being answered.
  uint8_t   _lastReqFunc    = 0;
  uint16_t  _lastReqAddr    = 0;
  bool      _tagPresent     = false;
  uint8_t   _missedTagPolls = 0;
};

extern RfidSnoop Snoop;
