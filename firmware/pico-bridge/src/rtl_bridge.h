#pragma once
#include <Arduino.h>
#include <SerialPIO.h>

// Transparent USB<->UART bridge to the RTL8721DM, plus reset / download-strap
// control. The bridge half shuttles bytes between USB CDC0 (`Serial`) and the
// RTL UART, auto-tracking the host-requested baud rate so flashing tools and
// serial terminals "just work".
//
// The RTL UART is a PIO UART (SerialPIO), not hardware UART0: its RX pin doubles
// as the UART_DOWNLOAD strap, and SerialPIO lets us reclaim that pin as GPIO and
// hand it back to the UART deterministically on every strap cycle (hardware
// UART0's begin/end pin-function save/restore desyncs when GPIO'd in between).
class RtlBridge {
public:
  RtlBridge();
  void begin();

  // Shuttle bytes host<->RTL and keep the UART baud matched to the host.
  // Call as often as possible from loop().
  void pump();

  // Force the bridge UART baud (overrides host auto-tracking until the host
  // sets a new line coding).
  void setBaud(uint32_t baud);
  uint32_t baud() const { return _baud; }

  // Raw, level-held line control (open-drain: asserted = driven, released = hi-Z).
  void assertReset(bool on);    // CHIP_EN: on = held in reset/shutdown
  void driveStrap(bool on);     // UART_DOWNLOAD(PA7): on = hold the line low
                                // (takes the bridge UART offline while held)

  void reset();          // pulse CHIP_EN -> boot the application normally
  void enterDownload();  // strap-low + CHIP_EN reset -> UART download mode
  void run();            // release strap + reset into the application

  uint32_t bytesToRtl()   const { return _toRtl; }
  uint32_t bytesFromRtl() const { return _fromRtl; }
  bool resetAsserted()    const { return _rstOn; }
  bool strapAsserted()    const { return _strapOn; }

  // Live traffic tap: tee bridge bytes to a debug stream, run-length collapsed.
  // '>' = host->RTL (commands we send the chip), '<' = RTL->host (chip replies).
  // For protocol debugging only -- adds latency, keep off for fast transfers.
  void setMonitor(Stream* s) { _mon = s; }
  void setMonitorEnabled(bool e);
  bool monitorEnabled() const { return _monOn; }

private:
  static void driveLine(uint8_t pin, bool activeLow, bool assert);
  void serviceAutoReset();      // map bridge-port DTR/RTS -> reset/download
  void monByte(char dir, uint8_t b);
  void monEmit();

  SerialPIO _uart;              // PIO UART to the RTL (TX=GP0, RX/strap=GP1)
  uint32_t _baud    = 0;
  uint32_t _toRtl   = 0;
  uint32_t _fromRtl = 0;
  bool     _rstOn   = false;
  bool     _strapOn = false;

  // DTR/RTS auto-reset edge state
  bool     _arInit     = false;
  bool     _arPrevReset = false;
  uint32_t _arLastMs   = 0;

  // Live traffic monitor state
  Stream*  _mon      = nullptr;
  bool     _monOn    = false;
  char     _monDir   = 0;
  int      _monByte  = -1;
  uint32_t _monRun   = 0;
  uint32_t _monLastUs = 0;
};

extern RtlBridge Rtl;
