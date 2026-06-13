# Decisions — Petlibro Pico bridge

Forward-facing log of structural/behavioural decisions. Newest at top.

## 2026-06-11 — Initial architecture

- **Pi Pico (RP2040) as a permanent in-housing ISP bridge** for the feeder's
  RTL8721DM, wired to the mainboard `ISP` header. Goal: reflash without
  disassembly.
- **arduino-pico (earlephilhower) core** via the maxgerhardt platform fork,
  chosen for `SerialPIO` (RX-only PIO UARTs), `Serial1` hardware UART, and
  Adafruit TinyUSB multi-CDC in one toolchain.
- **Two USB CDC ports** (user choice):
  - CDC0 `Serial` = transparent passthrough to the RTL UART; auto-tracks the
    host-requested baud so the Realtek Image Tool (1.5 Mbaud) and the ROM log
    (115200) both work without manual config.
  - CDC1 `USBControl` = command console (reset/boot macros, line control,
    status) + live RFID snoop output.
- **Multi-CDC enablement:** the core's `include/tusb_config.h` hard-pins
  `CFG_TUD_CDC` to 1 with an unguarded `#define`, so a `-DCFG_TUD_CDC=2` build
  flag is silently overridden. Fixed by forcing every TinyUSB translation unit
  to use `include/custom_tusb_config.h` (CFG_TUD_CDC=2) via
  `-DCFG_TUSB_CONFIG_FILE=\"custom_tusb_config.h\"`. Re-diff that file against
  the core's Adafruit rp2040 config on core version bumps.
- **RTL reset/boot lines are open-drain** (driven low to assert, hi-Z to
  release) so the Pico never fights the mainboard. Polarity is configurable.
- **`enterDownload()` is a configurable placeholder, not a verified sequence.**
  Ameba-D enters UART download from reset + the Image Tool handshake (dev
  boards strap via RTS/DTR); the exact strap pin/polarity/timing for this board
  is UNCONFIRMED and must be characterised on the bench.
- **RFID snoop is a protocol-agnostic capture layer** (idle-gap framed
  hex+ASCII per line). No JY-L601D/FDX-B decode yet — capture real reads first,
  then add a decoder keyed off the observed framing.

### Verified on hardware / datasheet
- 2026-06-11: flashed a Pico, **both USB CDC ports enumerate** as expected.
- 2026-06-11: the `ISP` header is **only RX / TX / GND** — no reset/strap pad.
- 2026-06-11: confirmed from UM0401 (RTL8721DM = **QFN68**):
  - **CHIP_EN = QFN68 pin 6** (1=enable, 0=shutdown) — dedicated reset, NOT on
    the ISP header. One wire from a Pico GPIO → pin 6 gives Pico-driven reset.
  - **UART_DOWNLOAD = PA[7] = QFN68 pin 7** (low at boot = UART download). PA[7]
    is also the LOGUART TX → it's the ISP "TX" data line, already wired. So the
    download strap needs **no extra wire**; firmware holds that data line low
    across a CHIP_EN reset (`enterDownload()`), then hands it back to the UART.
  - DO NOT pull **PA[27]/NORMAL_MODE_SEL (pin 33)** low at power-on (boot fails).
- Decision: implement esptool-style entry — CHIP_EN←reset, PA[7]←strap. The one
  wire worth running is Pico→CHIP_EN (pin 6); strap rides the existing TX line.

## 2026-06-12 — ROOT CAUSE: Adafruit CDC write drops data when DTR deasserted

- The real bug behind days of "responses never arrive / flasher times out 222s":
  `Adafruit_USBD_CDC::write()` loops `while (remain && tud_cdc_n_connected(_itf))`,
  and `tud_cdc_n_connected()` is *defined* as "host asserts DTR." SharpRTL872xTool's
  `Connect()` ends with `DtrEnable=false` and never re-asserts, so DTR stays LOW
  the whole session -> every `Serial.write()` of a chip response returned 0 and
  **dropped the data**. The chip answered correctly the whole time (proven on the
  tap), but the bridge threw the responses away.
- Why it fooled us: the tap is on CDC1, where the user's *terminal* asserts DTR,
  so the tap showed the responses fine — while the flasher on CDC0 (DTR low) got
  nothing. That asymmetry sent us chasing flow-control/baud/flood ghosts.
- Fix: send RTL->host via the low-level `tud_cdc_n_write(0,..)/tud_cdc_n_write_flush(0)`
  (gated only by `tud_cdc_n_write_available(0)`), which ignores DTR — like a real
  USB-UART adapter. Data stream left verbatim (no gating/dropping). Reading
  (host->RTL) was always fine: `tud_cdc_n_read` doesn't gate on DTR.
- Lesson: a transparent USB-serial bridge must NOT gate data on DTR. See
  [[arduino-pico-cdc-dtr-write-drop]].

## 2026-06-12 — non-blocking pump (fix full-duplex deadlock)

- Symptom: "almost every serial interaction hangs for almost forever." Root cause:
  `pump()` wrote whole chunks with `write()`, which BLOCKS when the destination
  FIFO is full. With the ROM streaming its idle/NAK flood while the flasher was
  mid-write (not draining device->host), `Serial.write()` blocked, stalling the
  host->RTL forward -> chip never ACKs -> flasher's 1000x200ms WaitResp = ~200s.
- Fix: pump now moves only `min(src.available(), dst.availableForWrite(), 64)`
  bytes per direction per loop, so neither `write()` ever blocks. A flood on one
  side can no longer wedge the other; bytes just pace through. (SerialPIO TX FIFO
  is 8 deep, so host->RTL moves <=8B/iteration — fine at these rates; keep the
  tap off for fast transfers as it adds per-byte latency.)

## 2026-06-12 — bridge proven bidirectional + traffic tap added

- Added a live bridge traffic monitor (`mon on|off`): tees both directions to the
  control port, run-length collapsed ('>' host->RTL, '<' RTL->host). For protocol
  debugging only — adds per-byte latency, keep off for fast transfers.
- The tap proved the SerialPIO bridge is **fully bidirectional**: SharpRTL872xTool
  `Floader()`→`ReadRegs(0x82000)` sent `> 31 00 20 08 00` and the chip replied
  `< 31 F6 79 19 89 15` (valid response code + status 0x15). Earlier TX-dead theory
  was wrong. `0x15` flood between commands = the ROM's idle/ready byte, not an error.
- Tool internals (from Tools/SharpRTL872xTool/.../Program.cs): always opens at
  115200; `-b` is only the fast transfer baud it negotiates to via `05 <x>`→ACK
  `06`. `Connect()` just toggles DTR/RTS and returns true (so "Connected" is
  meaningless). Flashloader uploaded to SRAM 0x82000 via XMODEM if not present.
- Next: validate end-to-end with a small `rf` at 115200, then full dump; if 1.5M
  drops bytes, harden throughput (FIFOs / pump on core1).

## 2026-06-12 — bridge UART moved from hardware UART0 to SerialPIO

- The strap shares the bridge RX pin (PA[7] net). With hardware `Serial1`
  (SerialUART), `begin()` saves the pin's prior function and `end()` restores it;
  GPIO-ing the pin in between (to drive the strap) desyncs that bookkeeping, so
  after one strap cycle the RX pin never re-mux back to UART — boot log died and
  the flasher never really handshook ("Connected" = port opened only).
- Fix (Matt's call): bridge now uses **`SerialPIO`** (TX=GP0, RX/strap=GP1, 256B
  FIFO). `SerialPIO::begin()` re-muxes the pin to PIO unconditionally every call
  and `end()` just disables/unclaims the SM — so end()→GPIO→begin() is clean and
  repeatable. PIO handles 1.5 Mbaud fine. `pump()` skips I/O while `_strapOn`.
- PIO budget: bridge TX+RX (2 SMs) + 2 RFID snoop RX (2 SMs) = 4 of 8 SMs.
- See [[arduino-pico-uart-pin-reuse]].

## 2026-06-12 — download-strap drive hardening

- Symptom: `download` resets the chip but it boots normally (strap not low at the
  ROM trap-sample). Found `SerialUART::end()` restores the pin to its pre-begin
  function (`GPIO_FUNC_NULL`), not SIO — so driving the strap relies on the
  follow-up `pinMode()`'s `gpio_init()`. Hardened `driveStrap()` to reclaim the
  pad explicitly (`gpio_init`/`gpio_set_dir`/`gpio_put`) and extended the
  post-CHIP_EN-release strap hold to `RTL_DL_STRAP_HOLD_MS` (40 ms) to cover a
  slow CHIP_EN rise + late trap sampling.
- Definitive bench arbiter = meter the ISP TX pad during `rst on`+`strap on`
  (~0 V = pin driven; ~3.3 V = pad not pulled → move bridge to `SerialPIO` for
  deterministic runtime pin reclaim, per Matt's suggestion). Pending hardware retest.

## 2026-06-12 — DTR/RTS auto-reset on the bridge port

- Enabled `RTL_DTR_RTS_AUTORESET` (default on) so stock AmebaD flashers work as a
  drop-in adapter. Polls `tud_cdc_n_get_line_state(0)` (bit0=DTR, bit1=RTS) each
  `pump()`. **Edge-triggered**, not level-mirrored: on the reset line's assert
  edge it runs the atomic `enterDownload()`/`reset()` macro, using the strap line
  as the mode selector. Rationale: mirroring levels would (a) drive PA[7] low
  while the chip drives it as UART TX → bus contention, and (b) leave the chip
  stuck in reset whenever a terminal holds DTR. Edge-trigger + atomic macro avoids
  both; strap is only ever driven inside the sequenced macro.
- Default mapping = Realtek convention DTR→reset, RTS→strap (`RTL_AR_RESET_ON_DTR`,
  `RTL_AR_INVERT`, `RTL_AR_DEBOUNCE_MS` configurable). Manual CDC1 `download`/`run`
  remains the deterministic fallback. Confirmed flash dump via `rf` is the safe
  first op (reads reliable on RTL8721DM); see README "Flashing / dumping".
- Trade-off noted: opening the bridge port in a terminal asserts DTR → one RTL
  reset (esptool-like). Set the flag to 0 for a fully passive log-watch bridge.

### Open / to verify on hardware
- Wire Pico GP2 → CHIP_EN (pin 6); confirm `reset`/`download`/`run` behave.
- ISP RX/TX orientation (which data pad is PA[7]); if swapped, set
  `RTL_DLSTRAP_PIN` to `RTL_UART_TX_PIN`. Tune reset/strap pulse widths.

## 2026-06-13 — RFID snoop: per-direction parity + corrected tap pins

- **The JY-L601D link uses ASYMMETRIC parity**, confirmed on the logic analyzer
  (`modbus_parity.png`, `modbus_parity_RFID.png`). It is Modbus RTU @ 19200,
  slave addr 0x03, but: **RTL→module (commands) = 8O1 (odd)**, **module→RTL
  (replies) = 8E1 (even)**. The 2026-06-12 "link is 19200 8O1" note was only
  half right — it characterised the command direction. The RTL master tolerates
  this because it doesn't validate RX parity.
- **Why it bit us:** `SerialPIO` validates parity and silently drops mismatched
  bytes, and `RfidSnoop` applied ONE shared format to both taps. At 8N1 (no
  validation) both taps captured; switching to 8O1 blinded the replies tap 100%.
  See [[arduino-pico-uart-pin-reuse]]; full breadcrumb in `DEBUG_LOG.md`.
- **Contract change:** snoop framing is now **per-channel**. `config.h` defines
  `RFID_READER_FORMAT` (module TX = 8E1) and `RFID_HOST_FORMAT` (RTL TX = 8O1)
  in place of the single `RFID_SNOOP_FORMAT`; `RfidSnoop::setFormat` now takes a
  channel arg; console `rfidparity` accepts an optional `r|h` tap selector.
- **Tap pins corrected:** the physical wiring is the opposite of the original
  guess — GP4 is on the RTL TX (commands), GP5 on the module TX (replies). So
  `RFID_READER_RX_PIN`/`RFID_HOST_RX_PIN` were swapped (now 5/4) so the `RFID-R`
  tag denotes the module and `RFID-H` the RTL. Verify on the bench after flash:
  with the collar read, `RFID-H` should show 8O1 commands and `RFID-R` the 8E1
  replies; if they come out swapped the tap wiring differs — swap the pins back
  or retune live with `rfidparity r|h …`.
- A passive snoop should not *validate* parity at all; per-direction formatting
  is the pragmatic fix, but a future hardening is a capture-only RX that records
  bytes regardless of the parity bit.

## 2026-06-13 — RFID protocol decoded; inline Modbus/FDX-B decoder added

- **The link is Modbus RTU (slave 0x03) carrying an FDX-B tag ID.** The animal
  tag is read as **4 registers @ 0x000E**: `0x000E` = country code, `0x000F`..
  `0x0011`[hi] = 38-bit national ID, `0x0011`[lo] = per-read signal/quality.
  Confirmed against a known tag: country 130 + national 023370514455 =
  `130023370514455`. Full register map + worked example in
  `Docs/RFID_PROTOCOL.md`.
- **Tag presence = whether the `0x000E` read is answered.** No tag → the module
  simply doesn't reply (no Modbus exception). The snoop infers "removed" after
  `kTagMissThresh` (3) consecutive unanswered tag polls, "detected" on the first
  valid reply.
- **Decoder lives in `rfid_snoop.cpp` (`RfidSnoop::decodeFrame`).** It annotates
  every frame with the Modbus action (READ/WRITE/WRITE-MULTI + reg addr), decodes
  the FDX-B id inline on `0x000E` replies, and prints `*** TAG … DETECTED/REMOVED
  ***` markers. Decode runs before the TX-room check so presence state stays
  correct even if a console-backpressure drop skips the printed line. A Modbus
  CRC16 check flags merged/garbled frames (`CRC?` / `(+N b merged)`); request→
  reply correlation spans both taps (replies carry no register address), so the
  host tap is now drained before the reader tap each `service()` pass.
- Still fuzzy: `reg 0x0000` mode value, the `0x0005`–`0x000D` diagnostic block,
  and exactly what the `0x0011` low byte measures. See doc "Open questions".
