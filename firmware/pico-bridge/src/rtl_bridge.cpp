#include "config.h"
#include "rtl_bridge.h"
#include <hardware/gpio.h>

#ifdef USE_TINYUSB
#include <Adafruit_TinyUSB.h>
#endif

RtlBridge Rtl;

// PIO UART to the RTL: TX=GP0, RX/strap=GP1, 256-byte FIFO for 1.5 Mbaud bursts.
RtlBridge::RtlBridge()
  : _uart(RTL_UART_TX_PIN, RTL_UART_RX_PIN, 256) {}

// Open-drain style line driver: when asserted we actively drive the pin to its
// active level; when released we go hi-Z (INPUT) and let the mainboard's own
// pull resistor define the level. Used for CHIP_EN so the Pico never fights the
// RTL board.
void RtlBridge::driveLine(uint8_t pin, bool activeLow, bool assert) {
  if (assert) {
    pinMode(pin, OUTPUT);
    digitalWrite(pin, activeLow ? LOW : HIGH);
  } else {
    pinMode(pin, INPUT);   // hi-Z; board pull decides the idle level
  }
}

void RtlBridge::begin() {
  // CHIP_EN released (RTL free to run) before anything else.
  driveLine(RTL_RST_PIN, RTL_RST_ACTIVE_LOW, false);
  _rstOn = _strapOn = false;

  _uart.begin(RTL_UART_DEFAULT_BAUD, SERIAL_8N1);
  _uart.setTimeout(2);
  _baud = RTL_UART_DEFAULT_BAUD;

  Serial.setTimeout(2);               // keep readBytes() non-blocking-ish
}

void RtlBridge::setBaud(uint32_t baud) {
  if (!baud || baud == _baud) return;
  _uart.end();
  _uart.begin(baud, SERIAL_8N1);      // PIO re-inits cleanly at the new divider
  _uart.setTimeout(2);
  _baud = baud;
}

void RtlBridge::assertReset(bool on) {
  driveLine(RTL_RST_PIN, RTL_RST_ACTIVE_LOW, on);
  _rstOn = on;
}

// The UART_DOWNLOAD strap (PA[7]) shares the net with the chip's LOGUART TX,
// which the Pico reads on its PIO-UART RX pin. To hold the strap we stop the PIO
// UART (which frees the pins), drive that pin low as plain GPIO, then restart
// the PIO UART -- begin() re-muxes the pin back to PIO every time, so this is
// clean and repeatable. Only drive it low while CHIP_EN is asserted (chip not
// driving the line).
void RtlBridge::driveStrap(bool on) {
  if (on) {
    _uart.end();                                   // stop PIO SMs, free the pins
    gpio_init(RTL_DLSTRAP_PIN);                     // SIO, input
    gpio_set_dir(RTL_DLSTRAP_PIN, GPIO_OUT);
    gpio_put(RTL_DLSTRAP_PIN, RTL_DLSTRAP_ACTIVE_LOW ? 0 : 1);
  } else {
    _uart.begin(_baud, SERIAL_8N1);                // begin() re-muxes RX to PIO
    _uart.setTimeout(2);
  }
  _strapOn = on;
}

void RtlBridge::reset() {
  assertReset(true);
  delay(RTL_RST_PULSE_MS);
  assertReset(false);
}

// esptool-style entry, mapped to RTL872xD straps (UM0401):
//   CHIP_EN (pin 6) = reset, UART_DOWNLOAD/PA[7] (pin 7) = boot strap, low to
//   download from UART. Hold the strap low across a CHIP_EN reset; the boot ROM
//   samples PA[7] on release and enters UART download mode. Requires a wire
//   from RTL_RST_PIN to CHIP_EN; without it, power-cycle while strap is held.
void RtlBridge::enterDownload() {
  assertReset(true);              // CHIP_EN low: chip off, PA[7] not driven
  delay(RTL_RST_PULSE_MS);
  driveStrap(true);               // hold UART_DOWNLOAD (PA[7] net) low
  delay(RTL_BOOT_SETTLE_MS);
  assertReset(false);             // CHIP_EN high: ROM samples strap = low
  delay(RTL_DL_STRAP_HOLD_MS);    // hold past CHIP_EN rise + trap latch
  driveStrap(false);             // release -> line resumes as LOGUART
}

void RtlBridge::run() {
  driveStrap(false);              // strap high (internal pull-up) -> boot flash
  reset();
}

void RtlBridge::serviceAutoReset() {
#if defined(USE_TINYUSB) && RTL_DTR_RTS_AUTORESET
  uint8_t ls = tud_cdc_n_get_line_state(0);   // bit0 = DTR, bit1 = RTS
  bool dtr = ls & 0x01;
  bool rts = ls & 0x02;
#if RTL_AR_INVERT
  dtr = !dtr;
  rts = !rts;
#endif
#if RTL_AR_RESET_ON_DTR
  bool resetReq = dtr, strapReq = rts;
#else
  bool resetReq = rts, strapReq = dtr;
#endif

  // Prime on the first call so a line that's already asserted at boot doesn't
  // look like an edge and reset the RTL spuriously.
  if (!_arInit) { _arPrevReset = resetReq; _arInit = true; return; }

  if (resetReq && !_arPrevReset) {              // assert edge on the reset line
    uint32_t now = millis();
    if (now - _arLastMs >= RTL_AR_DEBOUNCE_MS) {
      _arLastMs = now;
      if (strapReq) enterDownload();            // strap held -> UART download
      else          reset();                    // plain reset -> run app
    }
  }
  _arPrevReset = resetReq;
#endif
}

void RtlBridge::setMonitorEnabled(bool e) {
  monEmit();
  _monDir = 0;
  _monByte = -1;
  _monRun = 0;
  _monOn = e;
}

// Emit the pending run-length token (e.g. " 03*512" or " 06").
void RtlBridge::monEmit() {
  if (!_mon || !_monRun) return;
  // Non-blocking: skip the write if the control-port TX FIFO is backed up,
  // rather than block the whole bridge loop on it.
  if (_mon->availableForWrite() >= 16) {
    if (_monRun == 1) _mon->printf(" %02X", _monByte);
    else              _mon->printf(" %02X*%lu", _monByte, (unsigned long)_monRun);
  }
  _monRun = 0;
}

// Feed one byte to the tap. Coalesces runs of the same byte and starts a fresh
// line ('>' or '<') whenever the direction flips, so command/reply bytes stand
// out from the ETX sync flood.
void RtlBridge::monByte(char dir, uint8_t b) {
  if (!_monOn || !_mon) return;
  _monLastUs = micros();
  // Coalesce a run of the same byte without an upper bound, so an idle/NAK
  // flood collapses to a single "15*48000"-style token instead of drowning the
  // trace (and the terminal). 60000 cap only guards the counter.
  if (_monRun && dir == _monDir && (int)b == _monByte && _monRun < 60000) {
    _monRun++;
    return;
  }
  monEmit();
  if (dir != _monDir) {
    if (_mon->availableForWrite() >= 24)   // non-blocking guard
      _mon->printf("\r\n[%lu] %c", (unsigned long)millis(), dir);  // timestamped line per direction-run
    _monDir = dir;
  }
  _monByte = b;
  _monRun = 1;
}

void RtlBridge::pump() {
#ifdef USE_TINYUSB
  // CDC interface 0 is `Serial` (the bridge port). Track the host's baud.
  cdc_line_coding_t lc;
  tud_cdc_n_get_line_coding(0, &lc);
  if (lc.bit_rate && lc.bit_rate != _baud) {
    setBaud(lc.bit_rate);
  }
  serviceAutoReset();
#endif

  // While the strap is held, the PIO UART is torn down -- don't touch it.
  if (_strapOn) return;

  uint8_t buf[64];

  // NON-BLOCKING both directions: only move as many bytes as the destination
  // FIFO can accept *right now*. Never call write() with more than there's room
  // for, so neither write can block. This prevents a full-duplex deadlock where
  // one side flooding (e.g. the ROM's idle/NAK stream) wedges a blocked write
  // and stops us forwarding the other side's data.

  // host -> RTL (USB CDC -> PIO UART TX; PIO TX FIFO is only 8 deep)
  {
    int space = _uart.availableForWrite();
    int avail = Serial.available();
    int n = avail < space ? avail : space;
    if (n > (int)sizeof(buf)) n = (int)sizeof(buf);
    if (n > 0) {
      size_t got = Serial.readBytes(buf, n);
      if (got) {
        _uart.write(buf, got);
        _toRtl += got;
        if (_monOn) for (size_t i = 0; i < got; i++) monByte('>', buf[i]);
      }
    }
  }

  // RTL -> host (PIO UART RX -> USB CDC TX), verbatim (never alter the stream).
  // Use the low-level tud_cdc_n_write API rather than Serial.write(): Adafruit's
  // CDC wrapper gates writes on tud_cdc_n_connected(), which is true only while
  // the host asserts DTR. SharpRTL872xTool leaves DTR deasserted all session, so
  // Serial.write() silently dropped every response. A real USB-UART adapter
  // ignores DTR for data, so we send regardless of it.
  {
#ifdef USE_TINYUSB
    int space = (int)tud_cdc_n_write_available(0);
#else
    int space = Serial.availableForWrite();
#endif
    int avail = _uart.available();
    int n = avail < space ? avail : space;
    if (n > (int)sizeof(buf)) n = (int)sizeof(buf);
    if (n > 0) {
      size_t got = _uart.readBytes(buf, n);
      if (got) {
#ifdef USE_TINYUSB
        tud_cdc_n_write(0, buf, got);
        tud_cdc_n_write_flush(0);
#else
        Serial.write(buf, got);
#endif
        _fromRtl += got;
        if (_monOn) for (size_t i = 0; i < got; i++) monByte('<', buf[i]);
      }
    }
  }

  // Flush a pending monitor token once the line has gone idle.
  if (_monOn && _monRun && (micros() - _monLastUs) > 5000) monEmit();
}
