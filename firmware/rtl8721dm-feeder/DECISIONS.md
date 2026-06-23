# Feeder firmware — decisions log

Forward-facing breadcrumbs for the de-clouded replacement firmware. Newest first.

## 2026-06-21 — Phase 2 begins: access_control (RFID-gated lid)

- **`app/access_control`** — first app-layer module. Watches the PA_16 tag IRQ; on a
  tag, reads FDX-B id, checks an in-RAM **whitelist**, drives the lid open via the new
  `Lid::moveTo(open)` for authorized tags; tag leaves → `hold` grace → lid closes.
  Emits **`visit`** events (arrival auth/deny + departure with duration) — the first
  real analytics into the spine.
- **Disabled by default** (`acl.enable v=1`, which also powers the RFID rail) so it
  never grabs the lid during bench work. While enabled it owns the lid — disable for
  manual lid control. Commands: `acl.enable/add/remove/list/clear/hold`; state `acl{...}`.
- Whitelist is **in-RAM** for now; Preferences-backed persistence comes with the config
  layer. Uses the corrected opposite-end endstop logic in `Lid::moveTo`.
- Next: `feeder` (schedule + manual dispense), `alerts` (faults → display), `time`
  (RTC/NTP), then config persistence + the real dashboard.

## 2026-06-21 — lid_closed (PB_3) must be read via ADC, not GPIO

- **PB_3 doubles as SWD_CLK.** As an analog (ADC) input it reads the pad voltage fine
  (how the pinmap always read it); switching it to a digital `gpio_read` broke it —
  the GPIO *input* path is owned by the serial-wire-debug peripheral, so it reads
  stuck. Only PB_3 was affected because it's the lone debug pin in the sensor set
  (SWD_DAT=PA_27, SWD_CLK=PB_3). Symptom matched exactly: lid_closed died the moment
  we switched analog→digital.
- Fix: `lid_closed` reads PB_3 via **ADC + threshold (>2000 on 12-bit)**, presenting a
  clean bool. The other detectors stay GPIO (not debug pins, confirmed working).
- General rule for this board: **read SWD/JTAG-muxed pads via ADC, never plain GPIO.**

**Endstop trigger logic (bench-verified, 2026-06-21):** both endstops read 1 in
mid-travel; each reads **0 only at the OPPOSITE end** — `lid_closed`→0 at the OPEN
end, `lid_open`→0 at the CLOSED end. So `lid.goto` watches the *opposite* detector for
0 (open: wait `lid_closed`==0; close: wait `lid_open`==0). Pin labels are correct and
NOT swapped (`lid_open`=PB_4, `lid_closed`=PB_3) — my attempt to re-swap was wrong.
**Dispense timeout** raised: default 30 s, hard cap 60 s (long dispenses were hitting
the old 8 s backstop before the rev target).

## 2026-06-22 — Display root cause #3 (the real one): cold-init ORDER

- Eliminated by measurement, in sequence: expander power (zeroed, stayed lit), display
  3.3V (present), and — decisively — **RTL pad config (`padall` dump identical warm-reset
  vs cold-boot)**. So the persistent-through-reset-not-power-cycle state is NOT in the
  RTL; it's the **WS1625 chips' own internal state** (independently powered; an RTL warm
  reset doesn't touch them, a power cycle does).
- **Decoded stock's cold-boot init (`display_init.csv` + `display_init.png` waveform,
  read by the user): `0x48` then `0x88` in a SINGLE transaction** (`START,0x48,0x88,STOP`,
  ACK on each byte's 9th edge), per chip, before any grid RAM. We never sent `0x48` at
  all — `disp on` sends only `0x88`, which is why "display on" never woke a cold chip.
  A power-on WS1625 needs the `0x48` data-mode command to accept writes; after a warm
  reset from stock the chip is already configured, so our firmware appeared to work.
- **Fix:** `Display::begin()` sends `dStart(3); dByte(0x48); dByte(0x88); dStop(3)` — both
  bytes in ONE frame (a STOP between them would make the chip read `0x88` as a fresh
  command instead of completing the `0x48` setup). `dFlush` prepends `0x48` before each
  grid batch (matches stock refresh). Confirmable zero-reflash on pinmap cold boot:
  `disp cmd 48 B; disp cmd 88 B; disp fill B 7F`.
- Correction to my earlier notes: stock does NOT "send no data command" (I mis-decoded),
  and it is NOT two separate frames. It is `0x48`+`0x88` in one frame. Trust the user's
  waveform reading over the crude CSV parallel re-decode (which mis-framed the start).

## 2026-06-23 — DISPLAY LIGHTS FROM COLD BOOT (resolved, hardware-confirmed)

- **First time our firmware lit the panel from a true cold power-up.** Confirmed on the
  bench: `dStockBoot()` + clear comes up working every cold boot. It briefly flashes the
  stock boot image (~5ms) before the clear blanks it — cosmetic, accepted, not worth fixing. The fix was the
  **corrected init sequence**, NOT the drive mode. Bench-verified: works in BOTH open-drain
  and push-pull once the corrected sequence runs — so push-pull was a red herring (earlier
  push-pull tests failed only because the framing was still broken at the time).
- What "corrected init sequence" means (the accumulated framing fixes, all required):
  the `0x48` data-mode command (we'd been omitting it entirely), bundling it with the
  display-control byte to match stock (`[0x48,0x88|bri]`), run-once `dInit` (the per-op
  `dInit` was glitching command framing), and `dStockBoot()` replaying stock's exact
  `[0x48,0x88]` + 24 grids + `[0x48,0x9F]`.
  - **Correction:** an earlier note claimed "the chip ignores lone bytes / needs ≥2 bytes."
    That was WRONG — lone bytes DO appear in the capture. The fix was sending `0x48` at all +
    correct framing, not avoiding single-byte frames.
- `begin()` = `dStockBoot()` (the working sequence — DO NOT alter) then `dClearRam(3); dFlush(3)`
  to blank the boot image so we come up dark (alert surface). `g_pushpull=false` (open-drain)
  default restored — the confirmed-working state.
- **Next (carefully):** bisect `dStockBoot()` one change at a time to find the minimal
  essential init, never losing the working state. Candidates to test-remove: the exact boot
  image (vs blank), the `0x48,0x9F` reveal, the dim-load. Likely-essential: sending the
  `0x48` data-mode command + correct framing.

### FINAL verified decode from raw digital capture (2026-06-22)

- Re-exported as **raw digital transitions** (`digital.csv`: time + 3 channel levels;
  SCL = channel 2). Wrote a from-scratch decoder: detect START/STOP (SDA edge while SCL
  high), sample SDA on SCL **rising** edges, 8 data + 1 ACK per byte. Sixteen sub-0.5µs
  SCL pulses are export artifacts (real clocks are ~1.5µs high / 3µs low) — filtered by
  rejecting rising edges whose preceding low was <0.5µs. With that filter every
  transaction is clean.
- **Verified stock boot, per chip:** `[0x48,0x88]` (one transaction) → 24 grid frames
  `[0xC0|g, data]` → `[0x48,0x9F]` reveal. Grid 0 = `C0 FE` (the runt had made it read
  `E0 7F`). The boot image (a walking-bit diagonal) decoded for both chips and **matches
  the firmware `BOOT_L`/`BOOT_R` arrays exactly**. `dStockBoot()` replay is byte-accurate.

### Decoder framing + full stock update structure (2026-06-22, properly decoded)

- **Bus is 9 clocks/byte: 8 data (MSB-first) + 1 ACK**, then a spurious extra sample at
  the STOP. My CSV decoder originally read 8 samples/byte (no ACK skip), so every byte
  after the first absorbed the prior byte's ACK bit → garbage (the bogus `0xE0` address).
  Fixed by stepping 9 samples/byte and skipping the ACK. Grid addresses then decode as a
  clean `C0..D7` sequence — validates the framing.
- **Stock's per-chip cold sequence:** `[0x48,0x88]` (2-byte, display ON) → **24 grid
  frames** `[0xC0|g, data]` (the first transfer after the init IS image data) → `[0x48]`
  lone-byte `0x48` transaction immediately after. (Lone bytes are valid — the chip does
  NOT require ≥2-byte frames; an earlier note to the contrary was wrong.)
- **The lone `0x48` after the first image is a boot-LOGO reveal, not a terminator.**
  Timing tells it: stock does `[0x48,0x88]` (display on, brightness **0** = dim) → load 24
  grid frames *while dim* → `[0x48,0x8F]` (max brightness, the `0x9F` in the decode is
  `0x8F`+phantom-bit4) to **reveal** the loaded logo → ~1.74 s hold → clear (next update is
  all-zero grids). Subsequent updates are pure 24-grid rewrites (brightness already set).
  So it's cosmetic dim-load-then-reveal, NOT a functional requirement.
- **Driver (final):** `begin()` = `dInit()` + `dCtrl(3,g_bri)` (`[0x48,0x88|bri]`, on at our
  brightness) + blank `dFlush()`. `dFlush()` = 24 grid frames only (steady-state refresh;
  TM1629 scans RAM continuously, no commit needed). `dCtrl`/`dOff` = 2-byte
  `[0x48,0x88|bri]`/`[0x48,0x80]`. We skip the dim-load-reveal (init straight at full
  brightness). (We bundle `0x48` with command bytes to match stock — not because lone
  bytes are forbidden; they aren't.) Pending cold-boot hardware confirmation.
- New pinmap tools added for this hunt: `peek`/`poke`/`pad`/`padall` (raw reg + PADCTR
  decode). Kept — generally useful.

## 2026-06-21 — Display root cause #2 (firmware): per-op dInit glitched commands

- Logic-analyzer capture of OUR firmware showed **malformed START/STOP specifically on
  the both-chip COMMANDS** (`0x40` data, `0x88` display-control), while per-chip PIXEL
  writes framed cleanly. (Credit: bench observation.)
- Cause: the per-transaction `dInit()` I'd added (chasing an unconfirmed "WiFi re-muxes
  the pads" theory) re-ran `gpio_init()` on PA_13/14/15 **right before each command's
  START**, glitching the pads mid-stream. `dGrid` (pixels) never calls `dInit`, so it
  framed clean — exactly the asymmetry seen. **Fix: restored run-once `dInit` (guard).**
- This plausibly explains the whole post-power-fix darkness AND why *both* open-drain and
  push-pull failed: the display-control/data commands were corrupted regardless of drive
  mode, so the chips never got a clean "display ON". Drive mode stays runtime-selectable;
  switching it clears `dispUp` to force one clean re-init.
- **Confirmed by decoding the stock cold-boot capture (`display_init.csv`):** stock's init
  is just `0x88` (display ON) to each chip, then per-grid `0xC0..0xD7`+data (chips written
  sequentially, idle line held HIGH). **No data command, no reset, no mode-set** — so there
  was never a "missing init"; the panel had correct RAM but our glitched `0x88` never turned
  it on. Dropped the speculative `0x40` data-command I'd added, to match stock exactly.
  Process lesson: when something breaks right after a change, audit THAT change first.

## 2026-06-21 — Display blackout root cause: POWER, not firmware

- The multi-session "display dark" saga was a **swapped power connector** — DC was
  feeding the **battery input**, which doesn't power the display rail. Continuity and
  the 3.3 V measured on the signal-side pins were all fine; the panel supply was not.
  Display (and the verified pixel map) were correct the whole time. Fixed by correcting
  the power connector.
- While chasing a wrong "weak internal pull-up" theory, the display bit-bang was
  briefly switched from open-drain to push-pull. **Reverted to open-drain** (the proven
  pinmap drive: release-to-pull-up HIGH, drive LOW) on 2026-06-21 after the display
  failed again — the push-pull change never had a basis (the dark display was the power
  connector). The per-transaction pin re-assert (`dInit()` in dFlush/dCtrl/dOff) is kept
  as harmless robustness.
- Toolchain bug fixed in passing: `platform-amebad/builder/main.py` staged the flash
  image only on relink (post-action), so uploading a cached project flashed whatever
  built last. Added a pre-upload action to stage the current project's image.

## 2026-06-20 — Bench corrections round 2

- **lid/feed current shunts swapped:** `lidCurrent`=PB_2, `feedCurrent`=PB_1.
- **chute/hopper swapped:** `chute`=PA_17, `hopper`=PB_6.
- **Current now reported in mA** via the **0.25 Ω** shunt: `mA = (raw-zero)·3300·1000/(4095·250)`
  (first-order; ADC FS taken as 3.3 V, no gain stage assumed — calibrate against a
  known load). `lid_i_ma`/`feed_i_ma` in state. **Idle offset (~254 counts ≈ 818 mA
  unzeroed) auto-captured at boot** and subtracted; `sensors.zero` re-zeros on demand.
- **Emitters lit at boot** (sensorsInit powers P0_4/P0_5/P0_7) so detectors read valid
  at all times, not only while a motor moves — the move-coupling alone left endstops
  dark during/after manual inspection. Motor-coupling kept as a guarantee.
- **Feed rotor counter decoupled from dispense mode:** counts edges whenever the auger
  turns (run *or* dispense); only dispense auto-stops at the rev target. Fixes "count
  never goes up."
- **Stall/jam auto-stop disabled by default (0 = off):** they were unproven thresholds
  on the newly-unclamped current and could stop a motor before anything registered.
  Set `stall`/`jam` (mA) once calibrated; until then the per-move timeout is the guard.
- **Display: re-assert PA_13/14/15 on every transaction** (dropped the one-time-init
  guard; `dInit()` now runs at each `dFlush`/`dCtrl`/`dOff`). Hypothesis for the total
  blackout: WiFi/peripheral bring-up re-muxed the pads after the one-time init (the
  pinmap probe never ran WiFi, so once sufficed there).

## 2026-06-20 — Bench corrections: endstops swapped + photo-sensors are digital

- **Lid endstops were swapped.** `LID_OPEN` is **PB_4**, `LID_CLOSED` is **PB_3**
  (opposite of `RTL8721DM_module_pinout.txt`). Verified on hardware (reads the closed
  channel when open). Fixed in `sensors.cpp`.
- **Photoelectric detectors are DIGITAL, not analog.** They show two clean states only
  (~0.2 V / ~3.2 V). The `15-16`/`245-250` readings were the raw 12-bit values mangled
  by an erroneous `>>4`. Now `lidOpen/lidClosed/rotor/chute/hopper` are read as GPIO
  (PullNone) returning 0/1. Rotor encoder counts **digital rising edges** (no more
  analog hi/lo thresholds).
- **Removed the bad `>>4` on the genuinely-analog channels too** (current shunts,
  battery) — it was capping them at ~255, so the 12-bit stall (3600) / jam (3600)
  guards could *never* have tripped. Now full 0..4095.
- **Sensors API is now named accessors** (`lidCurrent/feedCurrent/battery` analog;
  `lidOpen/lidClosed/rotor/chute/hopper` digital) instead of the old `read(Ch)` enum.
- **`lid.goto` endstop condition is now digital:** stops when the target detector
  equals `level` (param, default LOW = blocked beam at end). Polarity still to be
  confirmed on the bench by watching `open_end`/`closed_end` while moving by hand.

## 2026-06-20 — Contract: motors auto-power their sensor emitters

- **Invariant: a motor cannot move without its sensor emitters lit.** Baked into the
  drive primitives, not the callers, so `access_control`/`feeder` can't bypass it:
  - `Lid::open/close` power the lid endstop emitter (P0_4) first.
  - `Feed::run` powers the dispense/rotor encoder emitter (P0_5) **and** the
    hopper+chute emitter (P0_7) first.
  - If the emitter power-up fails (expander unreachable), the primitive **refuses to
    drive** (calls `stop()`, logs `refuse:` event) — no moving blind.
- Emitters are left ON after a move (low-power IR; keeps the `sensors`/endstop/encoder
  readings live in the dashboard). Removed the now-redundant explicit `emitter()` calls
  from the lid/feed harness commands.

## 2026-06-20 — Phase 1: display driver (RAW DRIVER LAYER COMPLETE)

- **`drivers/display`** — two WS1625 (TM1629-family) chips, pseudo-I2C bit-bang on
  PA_13 (SCL) / PA_14 (line L) / PA_15 (line R). The `(x,y)→(line,grid,bit)` mapping
  — `MAP_L[7][28]` (`(grid<<3)|bit`), line-R mirror at `28−x`, x27 split (grid15
  top / grid14 bottom) — plus the bit-bang primitives and 5×7 font are **ported
  verbatim** from the pinmap explorer's verified `xy2cell()`. That map was solved
  empirically and confirmed dot-by-dot; do **not** re-derive or "simplify" it.
- Replaced the probe's blocking `dScrollText` with a **non-blocking marquee**
  (`Display::update()` from `loop()`); static `showText` for short alerts (~4 chars).
- Commands: `disp.on/off/clear/bright/text/scroll/px/border/fill`; state `display:{on,bri,scrolling,msg}`.
- **Raw driver layer COMPLETE:** aw9523 · rfid · sensors · lid · feed · buttons · display.
  Next: app layer (access_control, feeder, alerts, time/RTC) → connectivity hardening
  (SoftAP provisioning, NTP) → dashboard. Bench pin/direction corrections pending from
  hardware verification will be folded into the relevant drivers.

## 2026-06-20 — Phase 1: sensors + lid + feed drivers (low-level layer done)

- **`drivers/sensors`** — ADC cluster via mbed `analogin_api` on PB_1..PB_7 (12-bit,
  `>>4` from the 16-bit HAL value to match documented ~3928 readings) + hopper digital
  PA_17. Channels: lid_i/feed_i (current shunts), lid_open/lid_closed (endstops),
  rotor (encoder), chute, batt.
- **`drivers/lid`** — H-bridge PWM (mbed `pwmout_api`): PA_28=OPEN, PA_30=CLOSE, 20 kHz;
  both 0% = STOP (driven low, never hi-Z). **`drivers/feed`** — H-bridge via AW9523
  P0_2/P0_3.
- **Non-blocking motor design.** `open/close/run` start the drive and return; `update()`
  (pumped from `loop()`) auto-stops on hard timeout (≤8 s), stall/jam current, or — in
  closed-loop `lid.goto`/`feed.dispense` — endstop threshold / rotor-pulse count. Keeps
  the dashboard polling live ADCs *while* the motor moves, so you characterize the
  mechanism before trusting closed-loop. Every stop emits a `lid`/`dispense` event.
- **Bench-tunable guesses (characterize first):** endstop "blocked" threshold (def 2000),
  rotor encoder hi/lo (2800/1200), stall/jam current (3600). All overridable via command
  query params. Endstop polarity and dispense direction are unconfirmed — use the timed
  raw drives + live `sensors` readout to establish them, then closed-loop.
- Commands: `sensors.read`; `lid.open/close/stop/goto`; `feed.run/stop/dispense`. State
  adds `sensors`, `lid`, `feed` subtrees. `loop()` now pumps `Lid::update()`/`Feed::update()`.
- **`drivers/buttons`** — front-panel keys PA_0/PA_2/PA_4/PB_26 as active-low GPIO inputs
  (con1 ribbon, via the touch controller), debounced + edge-detected in `update()`. Each
  press emits a `button` event + bumps a counter; state adds `buttons` subtree. Icon↔pin
  mapping is tentative (con1 lines are "?" in the pinout doc) — confirm by pressing each
  key and reading the emitted event. This completes the raw driver layer (display pending).

## 2026-06-20 — Phase 1: RFID driver (JY-L601D Modbus master)

- **`drivers/rfid`** — Modbus RTU master on **UART3 (PA_26 TX / PA_25 RX), 19200,
  slave 0x03**, tag-ready IRQ on **PA_16** (active-low). CRC16/framing/decode ported
  from the proven Pico bridge `rfid_master`. FDX-B decode: 4 regs @ `0x000E` →
  `country(3) + national(12)` = 15-digit ID. Uses the mbed `serial_api` directly on
  explicit PinNames (not Arduino `Serial1`, whose pin table differs).
- **Asymmetric-parity decision: fixed 8O1, do NOT reconfigure.** One hardware UART
  can hold only one parity. Verified in `serial_api.c` that `serial_getc` →
  `UART_CharGet` returns the RX byte with **no parity check** (the PE flag is just
  status). So we configure **8O1** once: commands carry the odd parity the module
  requires, and the module's 8E1 replies still deliver their 8 data bytes — Modbus
  CRC guards integrity. This mirrors the stock master and avoids racy mid-transaction
  `serial_format` calls (`serial_putc` only blocks until the FIFO accepts a byte, so
  a reconfigure could truncate an in-flight byte).
- Commands: `rfid.read/regs/wreg/tx/irq/init`; state adds `rfid:{up,present,last_tag}`.
  **Power the module first** via the existing `rfid.power` (AW9523 P0_6) — reads time
  out until the rail is up. `rfid.regs addr=0x0040 qty=4` (tag-invariant module ID)
  is the cleanest link-up proof; `0x000E` always answers (cached ID when no tag).
- Known collar (repo): country 130 / national 023370514455 → **130023370514455**.

## 2026-06-20 — Phase 1: AW9523B expander driver

- **First real driver, exercised through the harness.** `hal/i2c_bb` (bit-bang I2C,
  ported verbatim from the proven pinmap explorer) + `drivers/aw9523b`. Commands:
  `aw9523.scan/dump/rreg/wreg/out/in`, `rfid.power`, `beeper.beep`, `emit` — all
  auto-rendered in the dashboard. State contributor adds `aw9523:{present,addr,…}`.
- **Bus correction.** The expander is on **bit-bang I2C, SDA=PA_18 / SCL=PA_19,
  addr 0x58** (per `Docs/RTL8721DM_module_pinout.txt`, confirmed-on-HW). NOT the
  `PB_6/PB_5` hardware-I2C0 path that a stale pinmap *code comment* claimed — that
  was a failed earlier attempt. Driver auto-detects SDA/SCL orientation by probing
  ID reg `0x10 == 0x23` in both orderings.
- **Authoritative pin sources going forward:** `Docs/RTL8721DM_module_pinout.txt`
  (module-pad ↔ RTL-pin, with C/? confidence) and `Docs/AW9523B_pinout.txt`. Treat
  `FEEDER_PINMAP.md` as historical/noisy and stale source comments as suspect.
- **Per-pin assert (RMW):** GCR `0x11|=0x10` (P0 push-pull) → LEDM GPIO-mode → CFG
  output → OUT level. P0_6=RFID PWEN, P0_4/5/7=emitters, P1_7=beeper, P0_2/3=feed
  motor (raw only — careful). Watch for KM0 bus contention on the bench (doc warns;
  our framework KM0 shouldn't touch the AW9523B, so scan should read 0x23 cleanly).

## 2026-06-20 — Phase 0 foundation: web-first bench harness

- **Goal.** Replace the vendor cloud with a self-hosted appliance: RFID-gated lid +
  scheduled/manual dispensing, with the **data/analytics spine as a first-class
  concern**, surfaced through a **local web dashboard served from the RTL itself**.
  Config over the network; the dot-matrix display is an **alert surface** only.
- **Web-first.** The dashboard is built as the **bench harness first**, then matured
  into the product UI — same endpoints throughout. Drivers plug into a **registry**
  (state contributors + named command handlers); `/api/commands` lets the dashboard
  **auto-render a control per command**, so a new driver's telemetry + controls
  appear with no UI edits.
- **API contract.** `GET /api/state` (namespaced snapshot), `GET /api/commands`
  (manifest), `GET|POST /api/cmd?cmd=…` (dispatch), `GET /api/events?since=N`
  (event-log tail). Command input via URL query params (no JSON parser needed yet);
  output is hand-built JSON. See `Docs/DATA_MODEL.md`.
- **Single-core (KM4) + FreeRTOS.** Everything on KM4 for v1; actuation+safety will
  run at highest task priority so WiFi/HTTP can never starve a motor stop. Phase 0
  services HTTP synchronously from `loop()`; moves to a task in Phase 2.
- **Toolchain.** PlatformIO + ameba-arduino-d (tmmsunny012 port), board
  `sparkfun_awcu488` = RTL8721DM. Builds to flashable `km0_km4_image2.bin`; flash via
  the Pico bridge. First build: KM4 image ≈481 KB code / 632 KB total at `0x08006000`.

## 2026-06-20 — Storage strategy

- **Config → `Preferences`** (ESP32/NVS-compatible API). On AmebaD it compiles
  against Realtek **DCT** (`NVS_USE_DCT` → `dct.h`), region ~24 KB near `0x1FA000`,
  CRC'd. The library is a submodule of the SDK clone
  (`vshymanskyy/Preferences`) — fetched via `git submodule update --init`.
- **Event log → LittleFS** (port already in the SDK at `file_system/littlefs/`,
  unwrapped — call the C `lfs_*` API with a custom `lfs_config`). Copy-on-write
  wear-leveling + power-loss safety = correct for an append journal. Placed in a
  carved region in upper free flash (dump shows ~2.7 MB free `~0x500000–0x7CB000`).
- **Hard rule (Realtek's own warning).** Default storage offsets assume a tiny
  image and will collide with a real one. **Pin config + log regions above the
  actual built KM4 image end, with a guard sector — verified per build**, not
  guessed. Never use `FlashMemory`'s default base `0x100000` (overlaps XIP code).
- **FTL is off-limits** for app data: ~2 KB logical ceiling and it's the BT stack's
  private store.
- **Dev WiFi creds** live in gitignored `src/secrets.h`; SoftAP provisioning portal
  replaces this in Phase 3.

## Roadmap

0. ✅ Foundation: skeleton + registry + HTTP router + harness page + in-RAM event ring (**builds**).
1. Drivers, each exercised through the browser (aw9523b, rfid, lid_motor, feed_motor, sensors, buttons, display).
2. Core loops offline-capable: access_control (gated lid), feeder (schedule + manual), alerts → display; HTTP→FreeRTOS task; config→Preferences; log→LittleFS.
3. Connectivity hardening: SoftAP provisioning, mDNS, NTP→RTC, reconnect.
4. Dashboard: harness → analytics + config UI.
