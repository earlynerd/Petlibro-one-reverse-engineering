# RTL8721DM Feeder — replacement firmware

De-clouded firmware for the Petlibro RFID feeder: RFID-gated lid + scheduled/manual
dispensing, with a **local web dashboard served from the device** for control and
analytics, and the dot-matrix panel used as an **alert surface**. No vendor cloud.

This is the integration firmware the reverse-engineering work was building toward —
it consumes the pin map, RFID Modbus protocol, and display map decoded elsewhere in
this repo.

## Architecture — web-first bench harness

The web UI is built first, as the instrument we drive each subsystem with; it then
matures into the product dashboard. Subsystems never touch the web layer directly —
they register into a **registry**:

- **state contributors** → each appends its slice of `GET /api/state`
- **command handlers** → named actions listed in `GET /api/commands` (the dashboard
  auto-renders a control for each) and invoked via `POST /api/cmd?cmd=NAME&...`
- **event log** → everything emits timestamped events; `GET /api/events?since=N`
  tails them; analytics are derived client-side

See [`Docs/DATA_MODEL.md`](Docs/DATA_MODEL.md) for the event/config schema and
[`DECISIONS.md`](DECISIONS.md) for the design rationale and roadmap.

## Status

**Phase 0 (foundation) — builds.** WiFi STA + HTTP server + registry + auto-rendering
dashboard + in-RAM event ring, with demo commands (`ping`, `echo`, `log.test`) to
prove the loop. mDNS, Preferences config, and the LittleFS event log come next.

## Build / flash / watch

```bash
# one-time: fetch the Preferences submodule in the SDK clone, and set creds
git -C C:/Users/mmsyl/ameba-arduino-d submodule update --init Arduino_package/hardware/libraries/Preferences
cp src/secrets.example.h src/secrets.h     # then edit WiFi creds (gitignored)

pio run                                     # build
pio run -t upload --upload-port COM<CDC0>   # flash; type `download` on CDC1 during the countdown
pio device monitor -b 115200 -p COM<CDC0>   # watch LOGUART via the bridge
```

Then open `http://<printed-ip>/` (mDNS `feeder.local` arrives in the next increment).
