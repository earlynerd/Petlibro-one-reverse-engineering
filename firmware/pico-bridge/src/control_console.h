#pragma once
#include <Arduino.h>
#include "rtl_bridge.h"
#include "rfid_snoop.h"

// Interactive command console on USB CDC1. Line-buffered, simple verb parser.
// Also relays the RFID snoop output to the same port so you can flash on CDC0
// while watching tag traffic and issuing reset/boot commands here.
class ControlConsole {
public:
  void begin(Stream* io, RtlBridge* rtl, RfidSnoop* snoop);
  void service();   // read/dispatch input lines + pump snoop output

private:
  void handleLine(char* line);
  void printHelp();
  void printStatus();

  Stream*     _io    = nullptr;
  RtlBridge*  _rtl   = nullptr;
  RfidSnoop*  _snoop = nullptr;
  char        _buf[80];
  size_t      _len   = 0;
  bool        _greeted = false;
  uint32_t    _lastBaud = 0;    // for live "host changed baud" logging
};

extern ControlConsole Console;
