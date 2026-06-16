# RTL8721DM GPIO explorer

Interactive pin prober that runs on the feeder's **own RTL8721DM** (flashed via
the Pico bridge) so we can discover what Petlibro wired where — the RFID-module
UART, the lid motor driver, buttons, limit/position sensors, the tag-ready IRQ,
power-enable lines — before writing the real local-control firmware.

It drives the raw mbed `gpio_t` HAL with `PinName` values directly, so it can
reach **any** `PA_0..PA_31` / `PB_0..PB_31`, not just the handful the SparkFun
variant exposes (the Arduino `digitalRead`/`pinMode` index a fixed table and are
useless for scanning).

## Safety model

- **Only the 34 GPIOs bonded on the QFN68 are ever touched** (from datasheet
  UM0401 Fig 2-3 — see the table the `list` command prints). The other enum
  entries are skipped: several of `PB_8..PB_21` are the **internal MCM SPI-flash
  pads**, and re-muxing one to GPIO kills XIP and **hard-hangs the chip**
  (confirmed: `classify` died the instant it reached `PB_8`, right after the
  bonded `PB_7`). Off-table pins are refused by every command.
- **`PA_7` / `PA_8` (LOGUART = this console) are hard-blocked.**
- **Reading is safe.** `scan` / `classify` / `watch` / `read` use high-Z input,
  which can't drive anything on the board.
- **Driving is gated.** `out` warns (it can move the lid motor or toggle a rail);
  strap/SWD pins (`PA_27` NORMAL_MODE_SEL, `PA_30` SPS_SEL/regulator-mode, `PB_3`
  SWD_CLK) need an explicit `force`. Anything you drive is undone by a reset.
- **If it hangs anyway:** type `reset` on the bridge's CDC1 control port (CHIP_EN
  is wired to the Pico) to reboot this firmware — no reflash needed.

### Bonded QFN68 GPIOs (what gets scanned)

```
Port A: PA_0 PA_2 PA_4 PA_7* PA_8* PA_12 PA_13 PA_14 PA_15 PA_16 PA_17
        PA_18 PA_19 PA_25 PA_26 PA_27† PA_28 PA_30†
Port B: PB_1 PB_2 PB_3† PB_4 PB_5 PB_6 PB_7 PB_13‡ PB_14‡ PB_16‡ PB_17‡
        PB_22 PB_23 PB_26 PB_29 PB_31
   * = LOGUART, blocked.   † = strap/SWD, force-to-drive.
   ‡ = bonded on QFN68 but NOT in the Arduino GPIO HAL pinmap — gpio_init on
       them takes a secure bus fault, so they're blocked. (Reachable only via
       direct register access, if ever needed.)
```

## Build / flash / use

Same toolchain as [`../rtl8721dm-hello`](../rtl8721dm-hello/) — see that README
for the one-time SDK clone and the flashing details.

```bash
pio run -d firmware/rtl8721dm-pinmap                       # build
pio run -d firmware/rtl8721dm-pinmap -t upload --upload-port COM<CDC0>
#   ...type `download` on CDC1 DURING the uploader's 5 s countdown...
pio device monitor -b 115200 -p COM<CDC0>                  # then drive it
```

Type `help` at the `>` prompt.

## Commands

| Command | What it does |
|---|---|
| `classify` | PU/PD test every pin → `H`=driven-high, `L`=driven-low, `.`=floating. **The map of what's connected.** |
| `scan [up\|down\|none]` | read every pin once (default pull-down) |
| `watch [up\|down\|none] [ms]` | print only pins that **change** — actuate something and see which pin moves (default pull-down, 40 ms; Enter stops) |
| `read <pin> [up\|down\|none]` | read one pin |
| `out <pin> 0\|1 [force]` | drive a pin (⚠ may move a motor/rail) |
| `in <pin> [up\|down\|none]` | set a pin back to input |
| `list` / `help` | numbering + safety / command help |

Pins: `PA0`..`PA31`, `PB0`..`PB31` (also `PA_5`, `pb10`, or raw `0..63`).

## Suggested discovery workflow

1. **`classify`** first — the *driven* pins are everything wired to something
   active. Idle-high UART TX lines (e.g. the RFID module's TX into the RTL) show
   as `HIGH (driven)`; pulled control lines show their rest state; everything
   else is floating/unconnected. This narrows ~62 pins down to the interesting
   handful.
2. **`watch`** + physically actuate things, one at a time:
   - **present / remove a tag** at the antenna → the **tag-ready IRQ** line
     toggles (this is the out-of-band presence signal we deferred earlier);
   - **move the lid by hand** → lid limit / position / hall sensor inputs move;
   - **press any buttons** → button inputs move.
   Whichever `Pxx` line flips is that signal. Note it down.
3. **Outputs** (lid motor, LEDs, power enables): from a known-floating or
   known-safe pin, `out <pin> 1` / `out <pin> 0` and watch for a physical effect
   (lid drives, LED lights). Check `classify` first so you don't drive *against*
   a line something else is already driving.
4. **Cross-reference** with the Pico snoop still in the housing: it knows the
   RFID module's UART nets, so a candidate RTL pin that's `HIGH (driven)` and
   sits idle is a strong RFID-UART-RX candidate; confirming TX needs a later
   firmware that actually polls the module.

Log findings as you go — the result is the RTL-side pin map that the
read-collar → open-lid firmware will be built against.
