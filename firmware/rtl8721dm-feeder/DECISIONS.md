# Feeder firmware — decisions log

Forward-facing breadcrumbs for the de-clouded replacement firmware. Newest first.

## 2026-06-24 — Timezone + automatic DST (browser-pushed transition schedule)

NTP only carries UTC, and tz was a manual fixed offset (default 0, no DST) — i.e. wrong local time
until set, and a twice-a-year manual nudge. Fixed **without** putting a tz database on the device.

- **The panel derives it from the browser.** On load it computes the current offset
  (`-getTimezoneOffset()`) **and** the upcoming DST transition instants for the browser's zone (probing
  `Date` daily, refined to the minute), then pushes both. Auto-syncs once per load when the device
  disagrees (offset or next-transition mismatch); a "Match this device" button on Home's Device card
  re-syncs on demand.
- **New `time.dst list=epoch:offMin,...`** stores up to **MAX_DST=4 FUTURE transitions**; `Timekeeper`
  applies each when its UTC moment passes (checked in `update()` and at boot) — **self-healing across
  power-downs** (a transition that elapsed while off is applied on the next check). `time.tz` now skips
  the flash write when the offset is unchanged (the panel may re-push an identical value).
- **Persistence:** `time` namespace gains `dn` (int count) + `dst` (bytes blob, 6 B/transition, ≤24 B),
  mirroring feeder's schedule pattern. time module is now 4 DCT vars (was 1) — well within the 27 cap.
  `clock` state adds `dst_n` + `dst_next`.
- **Caveat:** between a DST boundary and the next panel visit the device self-corrects from the stored
  schedule; it would only need a manual re-open if the panel weren't opened for >~13 months (the
  precompute horizon). Affects: `src/app/timekeeper.cpp`, `src/web/dashboard.h`.

## 2026-06-24 — Panel: Schedule / History / Access tabs built out + Home lid controls

Supersedes the "roadmap placeholders" note in the entry below — those three tabs are now functional,
all on the existing API surface (no firmware changes; `dashboard.h` only). `node --check` clean.

- **Home gains a Lid card:** Open / Close / Stop -> `lid.goto target=open|close` (closed-loop to the
  endstop, auto-stop) and `lid.stop`. Position derived from the bench-verified inverse endstops
  (`closed_end==0` = at OPEN end, `open_end==0` = at CLOSED end) + drive direction. Shows a hint when
  `acl.enabled` (access control owns the lid then).
- **Schedule:** `feeder.sched.list/add/clear` + the `feeder.auto` toggle. Add = native `<time>` +
  portions (max 8 slots; add/clear only — no per-slot edit command exists). Hint surfaces
  clock-not-synced / auto-off states.
- **History:** client-side analytics from `/api/events` (most-recent ≤200) — parses `meal` detail
  strings into a 7-day cups/day bar chart + KPIs (today / week) + recent-meals list. Only events with
  a real epoch (`ts>=1e9`) are dated; pre-NTP meals are skipped.
- **Access:** `acl.enable` toggle, `acl.hold`, whitelist via `acl.list/add/remove/clear`, an "Add
  current collar" shortcut (uses live `acl.last_tag`/`rfid.last_tag`), and a visit log parsed from
  `visit` events.
- Added a global toast for action feedback (the old inline notice lived in the Home banner, invisible
  on other tabs). Affects: `src/web/dashboard.h`.

## 2026-06-24 — Phase 4: owner web control panel (Home + Bench), same APIs

The dumb Phase-0 bench page (`dashboard.h`) is replaced by a **tabbed owner UI**, still served
straight from flash (self-contained: vanilla JS + inline CSS, no CDN, no build step) on the same
four endpoints — `/api/state`, `/api/commands`, `/api/cmd`, `/api/events`. **No firmware/registry
changes:** the UI rides the existing contract, so drivers still appear with zero UI edits.

- **Home = the daily driver.** Feed-Now (portions stepper -> `feeder.feed`, live dispense progress
  from `feed.parcels`/`feed.target`, Stop -> `feeder.stop`); a live status strip (online / battery /
  hopper / clock / auto); last-meal card (`feeder.last_meal`); hopper/chute/battery detail;
  auto-schedule toggle (`feeder.auto`); device card (wifi/ip/clock/uptime); alert banner
  (`alerts.active`, highest-prio).
- **Bench tab (⚙) preserves the original harness verbatim** — raw `/api/state`, an auto-rendered
  control per `/api/commands`, and the event-log tail. Nothing lost for bring-up.
- **Schedule / History / Access are roadmap placeholders** (next build step). Their APIs already
  exist: `feeder.sched.*`, the `meal`/`visit` event stream, `acl.*`.
- Honest call: **battery is a normalized bar off the raw, uncalibrated ADC** (no fake %); raw count
  in the title. CSS bug fixed in passing: the auto-schedule toggle needed `display:inline-block`
  (a `<label>` not inside a flex parent had a collapsed track); bench state `<pre>` height restored.
- Affects: `src/web/dashboard.h` only. Partially lands roadmap item 4.

## 2026-06-24 — Chute sensor polarity: bench-confirmed active-low; display/classifier corrected (counter untouched)

Folds in operator bench observation. Both break-beams are **active-low**; the hopper logic was
already correct (empty = HIGH), and **only the chute *interpretation* read backwards** at idle.

- **Root cause:** `g_chuteBlockedLvl` (default `true`) was misnamed — it actually held the chute's
  **CLEAR/resting level** (active-low HW: idle = HIGH = 1). The parcel counter correctly keys its
  **beam-restored** edge on that level (one count per kibble — hardware-verified clean 1-parcel
  dispense). But the live state (`chute_blocked`) and the `diagnose()` jam classifier treated
  `chute == that level` as *blocked*, so at idle (HIGH) they reported "blocked".
- **Fix (counter path deliberately untouched):** renamed `g_chuteBlockedLvl` -> **`g_chuteClearLvl`**
  and flipped the two interpretation sites to **`blocked = (chute != g_chuteClearLvl)`** (state +
  classifier). `setChuteDebounce(...)`, `feed.cpp`, and the persistence key `"chuB"` are **unchanged**,
  so counting behaviour is byte-for-byte identical (operator said "don't touch the counting" — honored).
- **Surface:** `feeder.cfg` knob renamed `chute_blocked=` -> **`chute_clear=`** (it sets the clear/idle
  level; default unchanged) + its echo key. The **state JSON key `feeder.chute_blocked` is kept** (the
  dashboard reads it) — only its computed value is corrected.
- General rule, now bench-proven for this board: **chute & hopper are active-low break-beams** (idle
  HIGH, interrupted LOW); lid endstops also confirmed correct. Affects: `src/app/feeder.cpp`.

## 2026-06-23 — Timestamps: clock established at boot (RTC-retain + flash re-seed)

So logged events carry a real UTC epoch in ~all cases (needed for plottable feed history),
not just after the NTP fix lands.

- **`Timekeeper` now establishes a clock at `timeInit()`**, before NTP: trust the RTC if it
  already holds a sane epoch (`>= SANE_EPOCH` ~2023; survives a **warm reboot**); else re-seed
  it from the last epoch **persisted to flash** (survives a **cold power-down**, stale by the
  downtime). NTP corrects either when it arrives. `g_synced` now means "have a usable clock from
  any source"; a `source` field (none/rtc/flash/ntp/manual) is exposed in `time.now` + `clock` state.
- **Flash persistence of the epoch** (Preferences "time" key `epoch`): on NTP fix, on `time.set`,
  on a retained-RTC boot, and **hourly** while running (`PERSIST_MS`). Light on flash endurance.
- **`eventlog` ts contract unchanged:** `>= 1e9` = real Unix epoch, `< 1e9` = boot-relative millis
  (only the rare first-ever boot before any time source). Self-describing for the dashboard plotter.
- Wiring: `main` calls `eventLogSetClock()` **before** `timeInit()` so the boot-time
  "rtc retained" / "restored from flash" events themselves get real timestamps.

## 2026-06-23 — Event log: DROPPED LittleFS, raw-flash journal instead (supersedes below)

The LittleFS approach in the next entry **hardfaulted on the bench** (see `DEBUG_LOG.md`): the
SDK's prebuilt lfs core faults on directory ops in our build (mounts a blank region as valid,
then `lfs_dir_find_match` overruns). A raw flash erase/write/read round-trip at the same region
PASSED and was cache-coherent, so the flash layer is solid — only lfs is broken.

- **`eventlog` is now a RAW-FLASH sector-ring journal** on `flash_stream_read/write` +
  `flash_erase_sector` (no filesystem). Same `eventLogAppend/HeadSeq/BuildJson/since` API; RAM-ring
  fallback + JEDEC capacity guard retained. Region **0x500000, 256 KB** (64 x 4 KB sectors), in the
  dump-verified free window.
- **`seq` IS the record index** → slot = `(seq-1) % TOTAL` (3392 slots), O(1) address, no metadata.
  On-flash record = `Event` (72 B) + a **`0xFEED1234` magic trailer written LAST** → a torn write
  (power loss mid-record) leaves no magic and is skipped; the magic also makes the one-time boot
  scan ignore leftover stock-firmware bytes. Entering a sector erases it (FIFO-drops the oldest 53).
  ~3340 events retained.
- Dropped the littlefs `-I` paths; kept `mbed/hal_ext` (flash_api.h) + `os_dep/include`
  (device_lock.h). **No dependency on the SDK lfs/adapter at all.** Boots + serves HTTP confirmed.
- Process lesson: don't reach for a heavyweight vendored abstraction (a filesystem) when the need
  is a fixed-size append log and the raw primitive is already proven. Verify the dependency early.

## 2026-06-23 — Phase 2: event log -> LittleFS (durable analytics spine)
> SUPERSEDED by the entry above — LittleFS hardfaulted; replaced with a raw-flash journal.

- **The event journal is now flash-backed and survives reboot** (same `eventLogAppend/HeadSeq/
  BuildJson/since` API). The in-RAM ring is kept only as a **fallback** when flash can't be validated.
- **Reuse the SDK's lfs flash adapter** (`g_lfs`, `g_lfs_cfg`, `lfs_diskio_*` — all in linked
  `lib_arduino.a`), so NO block-device code of ours. Adapter places the FS at **0x200000, 1 MB**
  (256 x 4 KB) — just above the 2 MB app slot and immediately above the DCT region (which ends at
  0x200000). Image currently ends ~0xBB000, so ~1.2 MB guard below the FS. Satisfies the hard rule
  (above image end + guard; NOT the FlashMemory 0x100000 XIP trap).
- **Runtime capacity guard = the "verified" half of the rule.** On init we read the JEDEC capacity
  (`flash_read_id`, 3rd byte = log2 bytes) and only mount/format if `0x200000+0x100000 <= capacity`;
  otherwise we never touch flash and fall back to the RAM ring. Prevents formatting an out-of-bounds
  region on a smaller-flash board. (The feeder dump showed ~8 MB, so it mounts.)
- **Journal layout: two-file rotation, contiguous seqs.** Append fixed 72 B `Event` records to
  `/ev.bin`; at **CAP_REC=4000** drop `/ev.old` and rename `/ev.bin`->`/ev.old`, start fresh
  (=> 4000..8000 retained, ~576 KB). Seqs are strictly contiguous, so `buildJson(since)` maps
  seq->global index in O(1) and reads only the most-recent slice (**MAX_EMIT=200** cap) — fast on the
  dashboard's `since=recent` poll, bounded response. seq recovered from the newest record on boot.
- **Power-loss safe:** open/write/**close** per append (close commits => each event durable; a torn
  append just isn't there on reboot, seq continues from the last durable one). Rotation order
  (remove old -> rename) loses at worst the already-oldest generation and self-heals next append.
- `events.stats` / `events.clear` harness commands (fs up?, record count, head seq). First boot
  formats the 1 MB region (one-time, a few seconds); later boots just mount.
- **BUILD:** added `-I` for `littlefs/r2.50`, `mbed/hal_ext` (flash_api.h), `os_dep/include`
  (device_lock.h) to platformio.ini. **Pending bench verify (do carefully — first writes at 0x200000):**
  `events.stats` shows `fs:true`; `log.test` -> reboot -> the event persists AND the unit boots
  normally (confirms 0x200000 region is free on this hardware).

## 2026-06-23 — Phase 2: config persistence (Preferences/DCT)

- **Settings now survive reboot.** Each module owns its own Preferences namespace and does
  **load-on-init / save-on-change** (consistent with the registry pattern), no central blob:
  - `acl` — whitelist + hold + enable. Whitelist can't be one DCT value (16 tags x 16 B > the
    **132 B/value cap**), so each tag is its own `tN` string key; stale slots removed on save.
  - `feeder` — all tunables (ppp/dir/jam/msp/hopE/chuB/chuMs/stall/rev/tries) + `auto` + the
    schedule as one packed blob (4 B/slot). Recovery params moved to live in feeder (so they
    persist) and are pushed to `Feed::setRecovery`/`setChuteDebounce` on load.
  - `time` — tz offset.
  - `config.wipe` / `config.info` (new `app/config`) — factory erase (reboot to apply) + per-module free count.
- **Power-loss resume IS intended:** a persisted `acl.enable` re-powers the RFID rail and resumes
  the gated lid on boot; `feeder.auto` resumes the schedule. Both default off, so nothing resumes
  unless you enabled it. BENCH NOTE: if you've enabled them, a reboot will move the lid / dispense —
  disable first or `config.wipe` when working on a disassembled unit.
- **DCT limits (verified in `Preferences_impl_dct.h`):** 4 KB module, **27 vars/module**, **6 modules**,
  key <=16 chars, value <=132 B. We use 3 modules; acl=19 vars, feeder=13, time=1 — all within budget.
  DCT self-places at top of flash (~0x1F4000 in the 2 MB map, CRC'd + backup) — SDK-managed config
  region, **distinct from** the LittleFS event-log region that gets pinned above the KM4 image next.
- **BUILD GOTCHA (cost: a confusing fs-backend error):** the Preferences lib selects its backend from
  `ARDUINO_ARCH_AMEBAD`, which the **Arduino IDE defines but this PlatformIO port does NOT** (it defines
  `CONFIG_PLATFORM_8721D`). Without it the lib compiled the *filesystem* backend (`_fs_*` undefined).
  Fix in `platformio.ini`: `-D NVS_USE_DCT` + `-I …/component/common/file_system/dct`. DCT symbols are
  in the linked `variants/common_libs/lib_dct.a`. Builds clean (6 s). **Pending bench verify:** a
  `time.tz` (or `feeder.cfg`) -> reboot -> read-back round-trip, and `config.info` showing DCT init OK.

## 2026-06-23 — Dispense mechanism: real-world tuning (user bench knowledge)

Folds in the operator's knowledge of how this auger actually behaves. All bench-tunable
via `feeder.cfg`; the polarities/thresholds are still guesses pending hardware.

- **Rotor is SLOW: ~8 s+ per pulse (per rev).** Consequences fixed:
  - `ms_per_parcel` 4000 -> **15000** (the old 4 s timed out before a single rev completed).
  - time-based **stall jam detection OFF by default** (`stall_ms` 2000 -> **0**) — a short stall
    window false-trips a healthy slow dispense. **Current is the PRIMARY jam signal** (`jam` mA);
    stall is an opt-in backstop, only sane with a window well above the inter-pulse time.
- **Chute sensor is NOISY: one parcel flickers the beam many times** as kibbles tumble past.
  The old "count every chute rising edge" would over-count one parcel as many. Fixed: count a
  parcel on the **leading beam-break edge only**, then a **refractory window** (`chute_ms`, default
  **1000 ms**) absorbs the rest of that parcel's burst. Safe because real parcels are seconds apart
  (rotor turn time) — refractory << inter-parcel gap, so it never merges two. `Feed::setChuteDebounce
  (ms, activeLevel)`; feeder keeps `chute_blocked` level in sync (same "beam broken" level).
- **Jam/fault classifier** (`feeder.cpp diagnose()`), from the 4-signal set
  {rotor turned?, peak current, chute blocked?, hopper full?} on an under-delivery:
  - chute blocked + not turning -> **OVERFILL** ("FULL") — bowl full, food backed up the chute.
  - chute blocked + turning -> **CHUTE** — output obstructed.
  - clear + not turning + **high current** -> **JAM** — hard obstruction.
  - clear + not turning + low/unknown current + hopper full -> **BRIDGE** — soft stall under a bridge/rathole.
  - turning + hopper full + nothing delivered -> **BRIDGE** (auger spinning in a void).
  - hopper low + nothing -> **EMPTY**.
  Mechanism insight: a hard jam stalls at HIGH current; a bridge stalls SOFT (low current) or spins
  free — so **current is what separates JAM from BRIDGE**. Uncalibrated (`jam`=0) the two are
  indistinguishable and read BRIDGE. `Feed::peakCurrentMa()` feeds the classifier; a `diag` event
  logs the full snapshot (`cur/rev/d/hop/ch`) for bench analysis. Alert under key `feed`.

## 2026-06-23 — Phase 2: alerts -> display (+ fix: feeder update was never pumped)

- **`app/alerts`** — the dot-matrix panel as a fault surface. **Push model:** producers call
  `Alerts::raise(key,msg,prio,ttlMs=0)` / `Alerts::clear(key)`; the module arbitrates by
  priority (tie -> most recent), renders the winner (`showText` if <=4 chars else `scroll`),
  and clears the panel to dark when nothing is active. **`raise()` also writes the event log**
  (only on new/changed, so re-raising a level condition each loop doesn't spam) — one call =
  live surface + history. ttl=0 sticky; else auto-expire. Re-render only on shown key/msg change
  (re-issuing `scroll()` each loop would reset the marquee). Why push, not event-tailing: the
  producer is where "is it resolved?" is known. Commands `alerts.test/clear`; state `alerts{shown,active[]}`.
- **Feeder wired:** `hopper-low` is a **continuous level** (`update()` raises/clears each loop
  from `Sensors::hopper()` — clears when refilled); `rotor-jam` (FAULT) and `short` (WARN) are
  **latched** and **cleared by a clean delivery** (RES_DONE). Emitter-power refuse -> `HW!` FAULT.
  Messages are uppercase font-charset (`LOW/JAM/SHORT/HW!`). The old `eventLogAppend("alert",…)`
  calls in feeder were replaced by `Alerts::raise` (no double-log).
- **BUG FIXED (shipped in the feeder step): `Feeder::update()` was never added to `loop()`.**
  Manual `feeder.feed` still physically dispensed (Feed::update() *is* pumped) but the completion
  handler never ran — no `meal` events, `last_meal` frozen, latched alerts dead. Now pumped in
  `loop()` (after AccessControl, before Alerts). Lesson: a registered module isn't wired until
  its `update()` is in the loop — grep the loop when adding one.
- **Lid-fault alert deferred:** lid only signals faults as ambiguous log events (open-loop
  timeout is a *normal* stop), so clean surfacing needs a `Lid::result()` mirroring `Feed`'s, plus
  an owner to poll it. Scoped as a follow-up; the Alerts infra already supports it.
- Wiring/order in `loop()`: …AccessControl -> **Feeder -> Alerts** -> Display(marquee). Builds clean (5.5 s).

## 2026-06-23 — Phase 2: time (NTP -> RTC), un-gates the feeder schedule

- **`app/timekeeper`** (namespace `Timekeeper`, `timeInit()`). The on-chip **RTC is the
  canonical free-running clock**; **SNTP sets it** on each fix. SNTP is async + non-blocking
  (`sntp_init()` fires immediately, retries w/ backoff, re-syncs hourly), default server
  `pool.ntp.org`. Key SDK fact: **`sntp_init` does NOT write the RTC** — it stashes the fix in
  static vars; `Timekeeper::update()` polls `sntp_get_lasttime()` and on a new sane fix
  (`tick` changed, `sec > 1e9`) does `rtc_write((time_t)sec)` (UTC). `synced()` flips true then.
- **APIs/build facts (verified):** RTC via mbed `rtc_api.h` (`rtc_init/rtc_read/rtc_write`,
  on the include path — the shipped `RTC` Arduino lib includes it unqualified). SNTP via
  `extern "C"` prototypes (symbols live in prebuilt `variants/common_libs/lib_arduino.a`, so no
  sntp.h path dependency). **`CONFIG_SYSTEM_TIME64` is OFF** → 32-bit `time_t` + the `long*`
  `sntp_get_lasttime` signature (would be `long long*` if on). No `gmtime`/`localtime` dep:
  epoch→civil date via inline Hinnant algorithm.
- **Timezone = a config offset in minutes from UTC** (`time.tz min=-300` / `h=-5`), **no DST**.
  Local time = `rtc_read() + tz`. Commands: `time.now/tz/set/sync` (`time.set epoch=` for the
  bench / no-net; `time.sync` forces a fresh query via stop+init). State: `clock{synced,utc,local_iso,tz_min}`.
- **Wiring:** `main` calls `timeInit()` right after `connectWifi()` (SNTP needs the link up) and
  pumps `Timekeeper::update()` each loop. **Event log now timestamps with real epochs** —
  `eventLogSetClock(Timekeeper::epochOrZero)`; pre-sync events fall back to `millis()` (small
  values <1e9, distinguishable from epochs). **Feeder schedule un-gated:** its `timeNowMin()`
  stub is replaced by `Timekeeper::minOfDayLocal()` (returns -1 until synced, so it still won't
  fire blind). Builds clean (5.5 s). **Pending bench verify:** confirm a real NTP fix on the
  device (watch for the `time ntp sync` event / `clock.synced`), then set the local `time.tz`.

## 2026-06-23 — Phase 2: feeder (manual/volumetric dispense) + dispense rework

- **`app/feeder`** — second headline behavior. Manual `feeder.feed portions=K`,
  an in-RAM daily schedule (`feeder.sched.add/list/clear`, `feeder.auto`), portion/guard
  config (`feeder.cfg`), and a `feeder` state subtree. Emits **`meal`** analytics events
  (source, delivered/wanted parcels, **cups**, result) — the dispense half of the data spine.
- **Unit model: 1 parcel = 1 auger chamber = 1/12 cup** (the smallest unit in the stock
  app). Canonical analytics unit is the **parcel**; cups are derived (`PARCELS_PER_CUP=12`,
  reported in state + meal events). A "portion" = `parcels_per_portion` parcels, **default 1**
  (= 1/12 cup, matching stock). Tune via `feeder.cfg parcels=`.
- **`Feed::dispense()` reworked from rev-counting to CHUTE-CONFIRMED VOLUMETRIC delivery.**
  The auger is positive-displacement/chambered, so dispense targets N **chute-confirmed**
  parcels — food only counts when the chute beam (PA_17) sees it fall. Turning an empty
  chamber (hopper low) never counts; it just runs to timeout. **Dispenses regardless of
  hopper state** — low hopper is a warning (`alert hopper-low`), not a blocker.
- **Jam handling in the driver:** a current spike (`jam` mA) OR a stalled rotor
  (`stall_ms` with no encoder advance) triggers a brief **reverse** (`reverse_ms`) to clear,
  then resumes forward; after `jam_tries` attempts it gives up → `RES_JAM` + `alert rotor-jam`.
  Tunable via `Feed::setRecovery()` / `feeder.cfg stall_ms/reverse_ms/jam_tries`.
- **Driver contract added so the app doesn't duplicate the loop:** `Feed::dispense(parcels,…)`
  / `runFor()` primitives now own all the once-scattered state setup; `busy()`/`result()`/
  `parcelCount()`/`revCount()` let `feeder` learn the outcome. `feed.dispense` harness cmd now
  takes `parcels=` (legacy `revs=` aliased). Builds clean (8.4 s). **Pending bench verify:**
  chute/hopper polarity (both guesses, like the endstops were), `stall_ms` vs real rotor RPM,
  jam current threshold. Schedule tick is **gated** (`timeNowMin()` returns -1) until `time` lands.

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
