# Feeder firmware — data model (the dashboard contract)

The dashboard is built knowing this exists: every subsystem **emits events**, and
the web layer + display are **readers**. Analytics ("how often / how much does the
pet eat") are computed **client-side** from the raw event stream, so the device
stays dumb and the metrics stay flexible.

## Event stream — `GET /api/events?since=<seq>`

Returns events with `seq > since`, oldest-first. Every event has:

| field | type | meaning |
|-------|------|---------|
| `seq`  | uint | monotonic sequence number (the `since` cursor) |
| `ts`   | uint | timestamp — **Phase 0: `millis()`**; becomes RTC epoch (s) after NTP |
| `type` | str  | event class (below) |
| `detail` | str | **Phase 0: free-form string.** Becomes structured fields per type when the log moves to LittleFS records (see below). |

### Event types (target schema)

| `type` | structured payload (post-Phase-0) |
|--------|-----------------------------------|
| `boot`      | `{reason, build}` |
| `time_sync` | `{epoch, source}` — NTP set the RTC |
| `visit`     | `{tag, dur_s, q}` — authorized/unknown collar in field, start→end |
| `dispense`  | `{trigger:"scheduled"\|"manual"\|"web", target_revs, actual_revs, grams, jam:bool}` |
| `lid`       | `{action:"open"\|"close", ok:bool, fault:bool, ms}` |
| `feed_skip` | `{reason:"bowl_full"\|"chute_blocked"\|"no_time"}` |
| `alert`     | `{kind, active:bool}` — see alert kinds below |

> **Phase 0 simplification:** the in-RAM ring stores one `detail` string. When the
> log moves to a LittleFS journal, records become fixed/structured per type and
> `/api/events` serializes the fields above. The cursor/`type`/`ts` contract is
> stable across that swap so the dashboard JS does not change.

### Alert kinds (also drive the display)

`hopper_low`, `rotor_jam`, `lid_open_fault`, `lid_close_fault`, `low_batt`,
`wifi_down`. The display shows the highest-priority **active** alert; clear when
its condition resolves.

## Live snapshot — `GET /api/state`

A single JSON object; each subsystem owns a namespaced subtree it appends via a
registered state contributor. Phase 0 ships only `sys`:

```json
{ "sys": { "uptime_ms": 1234, "wifi": true, "rssi": -52, "ssid": "Sly.fi", "ip": "192.168.1.50" } }
```

Phase 1+ adds: `rfid:{present,tag,q}`, `lid:{state,endstop_open,endstop_closed,current_ma}`,
`feed:{revs,current_ma,jam}`, `sensors:{hopper,chute,batt_mv,adapter_mv}`,
`alerts:[...]`, `time:{epoch,synced}`.

## Command surface — `GET /api/commands`, `POST /api/cmd?cmd=NAME&...`

`/api/commands` returns `[{name,args,help}]`; the dashboard auto-renders a control
per command (`args` is a comma list of `name:type` hints). `/api/cmd` dispatches by
name with URL query params and returns a JSON result. Driver bring-up commands
(planned): `lid.open/close/stop`, `feed.dispense?revs=N`, `aw9523.set?pin=P0_6&v=1`,
`rfid.read`, `beeper.test`, `disp.alert?kind=hopper_low`.

## Config store — `Preferences` (DCT-backed), namespace `feeder`

| key | type | meaning |
|-----|------|---------|
| `wifi_ssid` / `wifi_pass` | str | Phase 3 (provisioning); dev uses `secrets.h` |
| `tz_off_min`     | int  | timezone offset from UTC, minutes |
| `whitelist`      | bytes/str | allowed FDX-B tag IDs |
| `sched`          | bytes | feeding schedule: list of `{hh,mm,portions}` |
| `grams_per_rev`  | float | dispense calibration (rotor encoder revs → grams) |
| `lid_hold_s`     | int  | how long lid stays open after the pet leaves |

Config writes come from the dashboard (`POST /api/config`) in Phase 3+; Phase 0
has no persistent config yet.
