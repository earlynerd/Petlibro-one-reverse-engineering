# Debug log — Petlibro Pico bridge

Bench-observed firmware/hardware bugs and their resolutions. Newest at bottom.
Read this before a debug session — the `Class:` tags are a cheap prior on what's
likely wrong.

## 2026-06-13 — RFID snoop replies channel went 100% silent after switch to odd parity

- **Observation:** After changing the RFID snoop from 9600 8N1 to 19200 8O1, one
  of the two RX taps went totally silent (zero bytes), while the other kept
  decoding clean Modbus. The "dead" direction's data was provably present — the
  feeder still opened the lid on a collar read, so the RTL was receiving it.
- **Root cause:** EXTERNAL device quirk, not our decode. The JY-L601D transmits
  its replies with **8E1 (even)** parity but receives commands as **8O1 (odd)** —
  verified on the logic analyzer (`modbus_parity.png` = master request decoded at
  8O1; `modbus_parity_RFID.png` = the matching slave response decoded at 8E1).
  The RTL master never noticed because it doesn't validate RX parity. Our snoop
  does: `SerialPIO::_handleIRQ` (framework `SerialPIO.cpp:105-112`) silently drops
  every parity-mismatched byte (bare `continue`, `// TODO - parity error`, no flag).
  Our only code-side defect was a latent design limitation — `RfidSnoop` forced a
  single shared `_format` onto both taps (`rfid_snoop.cpp`), so it could not match
  an asymmetric-parity link; whichever direction didn't match the one format went
  blind. The PIO decode + allocation were correct and symmetric (verified by
  tracing `SerialPIO.cpp` and `PIOProgram.cpp` — both taps fit on PIO0, 28/32
  instr, 4 SMs), which is what kept us from rewriting working code.
- **Fix:** Decoupled per-channel framing — `RfidSnoop::setFormat(channel, fmt)`,
  `RFID_READER_FORMAT=SERIAL_8E1` (module TX) / `RFID_HOST_FORMAT=SERIAL_8O1`
  (RTL TX) in `config.h`. Also corrected the tap pins (`RFID_READER_RX_PIN`/
  `RFID_HOST_RX_PIN` were swapped vs. the actual wiring) so the labels match
  reality, and extended the console to `rfidparity [r|h] n|e|o` for live per-tap
  tuning.
- **Class:** external-device-quirk (asymmetric per-direction parity) + latent
  shared-config-blinds-one-channel. Contributing: framework silently drops
  parity-mismatched bytes with no surfaced error.
- **Recently-touched?** No code-we-wrote bug. The 2026-06-12 switch to 8O1 turned
  ON parity validation, which exposed the module's asymmetric TX parity; the
  decode path itself was correct.
- **Time to fix:** ~1 session (most of it spent ruling out our own code first).
- **Lesson:** A passive snoop should never *validate* parity — capturing the wire
  is the whole job. When a channel goes silent right after a framing change,
  suspect the framing assumption vs. the actual line before suspecting the decode.
