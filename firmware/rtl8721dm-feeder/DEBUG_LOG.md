# Feeder firmware — debug log

Bug breadcrumbs. Newest first. (Architectural decisions live in `DECISIONS.md`.)

## 2026-06-24 — `time.tz` (and other late-registered commands) 404'd — registry overflow

- **Observation (bench):** setting the timezone from the panel failed; `POST /api/cmd?cmd=time.tz&min=-420`
  returned **404**, and "Match this device" toasted "could not set timezone". Yet `/api/state` showed the
  full `clock` subtree (`tz_min`, `dst_n`, …), so the firmware was clearly the new build and NTP worked.
- **Diagnosis:** 404 on `/api/cmd` means `regDispatch` found no command of that name — i.e. `time.tz` was
  never registered, despite `timeInit()` calling `regAddCommand("time.tz", …)`. The tell: *state* worked
  but the *command* didn't. State and commands use **separate tables** with separate caps
  (`MAX_STATE=16`, `MAX_CMD=48`), and `regAddCommand` **silently no-ops past the cap**. Counted 65 command
  registrations; `timeInit()` runs **last** in `setup()` (after display's 13), so everything past #48 —
  most of `disp.*` and **all of `time.*`** — was dropped. State (12 contributors) stayed under its cap,
  so `clock` still appeared. That's why `tz_min` was visible but unsettable.
- **Root cause:** `MAX_CMD=48` outgrown (65 commands) + silent-drop on overflow + time.* registered last.
- **Fix:** raised `MAX_CMD`→96 and `MAX_STATE`→24 (`registry.cpp`), and made overflow **loud** — both
  `regAdd*` now `Serial.println` a "DROPPED (raise MAX_*)" warning instead of failing silently.
- **Class:** silent capacity overflow / fixed-array cap outgrown. **Recently-touched?** partially — the cap
  was old, but adding the panel's many features pushed `time.*` over the edge and surfaced it.
- **Lesson:** registration order now matters until caps are generous; a dropped registration must never be
  silent. Watch the boot serial log for `[registry] … DROPPED` after adding subsystems.

## 2026-06-23 — Boot hardfault on main_task after event-log → LittleFS

- **Observation:** hardfault on thread `main_task` immediately at boot, only after the
  event-log→LittleFS change (the prior config/Preferences build ran fine). `eventLogInit()`
  runs at the top of `setup()`, consistent with an immediate fault.
- **Diagnosis path (measure, don't guess):** ruled out the obvious by reading the SDK —
  stack is 64 KB, flash access works (DCT proves it), `flash_read_id` has the same XIP guard
  as the stream ops, `device_mutex_lock` self-inits. Added flushed serial breadcrumbs →
  localized to `eventLogInit` after a *successful* mount. `addr2line` on the device backtrace
  → `lfs_rawstat`/`lfs_file_rawopen` → `lfs_dir_find` → **`lfs_dir_find_match` (lfs.c:1358)**,
  `lfs_bd_cmp` overrunning. Red flag: `mount rc=0` on a *blank* region. A raw flash
  erase/write/read-back round-trip at the same base **PASSED and was cache-coherent**,
  isolating the fault to lfs, not flash. Same fault at 0x200000 (adapter's own `g_lfs_cfg`)
  and 0x500000 (my own config) → not region, not our config.
- **Root cause:** the SDK's **prebuilt littlefs core** (in `lib_arduino.a`) faults on directory
  operations in this build — mounts garbage as valid, then walks into it. No Arduino example
  uses it; DCT (the working config store) is a different, simpler flash store.
- **Fix:** dropped littlefs entirely. Reimplemented `eventlog` as a **raw-flash sector-ring
  journal** on the proven `flash_stream_read/write` + `flash_erase_sector` primitives:
  `seq` IS the record index (O(1) slot = `(seq-1)%TOTAL`), a `0xFEED1234` magic trailer written
  last makes torn writes self-detecting and ignores leftover stock-firmware bytes, sector-erase-
  on-entry does FIFO eviction. JEDEC capacity guard + RAM-ring fallback retained.
- **Class:** prebuilt-library-fault / wrong-abstraction (reached for a finicky vendored FS where
  raw flash was sufficient and proven)
- **Recently-touched?** yes — the lfs code was written the same session it faulted.
- **Time to fix:** ~1 session (breadcrumbs + addr2line + raw-flash isolation test, then rewrite)
