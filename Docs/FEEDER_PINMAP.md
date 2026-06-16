# RTL8721DM ↔ feeder board pin map (reverse-engineered)

The RTL-side GPIO assignments on the Petlibro mainboard (`PLNF301`), mapped live
on the real hardware with the [`firmware/rtl8721dm-pinmap`](../firmware/rtl8721dm-pinmap/)
GPIO explorer (custom firmware on the feeder's own RTL8721DM, driven over the
Pico bridge). This is the map the local read-collar → open-lid firmware will be
built against.

Confidence: **confirmed** = observed toggling/driving on the bench; *tentative* =
inferred from icon/role, not yet electrically proven.

## Pins (QFN68)

The RTL8721DM QFN68 bonds **34 GPIOs** (confirmed against datasheet Fig 2-3 and
the live chip). That's the entire I/O universe; 2 are the LOGUART, so **32 are
application-usable**, and the SoM breaks out ~29 of those as castellated pads.

```
Port A (18): 0  2  4  7  8  12 13 14 15 16 17 18 19 25 26 27 28 30
Port B (16): 1  2  3  4  5  6  7  13 14 16 17 22 23 26 29 31
```

Access tiers in the GPIO explorer:
- **`PA_7` / `PA_8`** = LOGUART (the console) — skipped.
- **`PB_13/14/16/17`** = bonded, but not in the Arduino GPIO HAL pinmap
  (`gpio_init` faults) → read via the ROM `GPIO_ReadDataBit` (`rawscan` / `gread`
  / `watch`), no pull control available on these four.
- **the other 28** = full HAL access (`classify`/`scan`/`watch` with pull control).

(`PB_8..PB_12/15/18..21` etc. are NOT bonded — some are the internal MCM
SPI-flash pads; re-muxing them via `gpio_init` hangs XIP, so they're never
touched.)

### Front-panel capacitive-touch buttons — **confirmed**

Active-low momentary GPIO inputs (idle high, press → low). Likely the digital
outputs of an external cap-touch controller (the buttons are NOT on the RTL's
built-in TOUCH_KEY pins `PB_4–7`). Left → right across the panel:

| Pin | Icon | Function (tentative) |
|---|---|---|
| `PA_2` | circular arrows (rotate/refresh) | manual feed / dispense |
| `PA_4` | lid opening | manual lid open |
| `PA_0` | fork & knife | meal / feed |
| `PB_26` | padlock | panel lock / child-lock |

### RFID Modbus UART — **confirmed** (2026-06-14)

| Pin | Function | datasheet alt |
|---|---|---|
| `PA_26` | **RFID-UART TX** (RTL → module, the 8O1 command line) | UART / I2C0_SDA |
| `PA_25` | **RFID-UART RX** (module → RTL, the 8E1 reply line) | UART / I2C0_SCL |

Found by **signal injection** from the Pico bridge: the Pico taps both RFID nets
(GP4 = RTL→module command net, GP5 = module→RTL reply net). Driving each net
statically (`rfinject` on the bridge) and reading the RTL via `rawscan` gives a
clean 1:1 follow — **GP4 → `PA_26`**, **GP5 → `PA_25`** (each reads 1,0,1 tracking
the injected 1,0,1; the other reads 0,0,0). Harnesses:
`firmware/rtl8721dm-pinmap/tools/find_uart.py`, `confirm_uart.py`.

This corrects two earlier mistakes: the UART is **not** `PA_18/PA_19` (that was an
unproven guess), and `PA_25/PA_26` are the **UART, not I²C** — which is exactly why
the AW9523B I²C probing on those pins only ever produced artifacts (poking a UART
with I²C). The AW9523B's I²C, if reachable, is on the other I²C-capable pads.

`PA_25` (RFID-RX) is now the **definitive power detector**: a powered module idles
its TX high, so `PA_25` reads high (this is *proven* valid — master-mode received
framed Modbus replies on this net, which requires mark-idle).

### Other connected pins (from `classify`) — to be resolved

Driven pins seen in a no-tag, idle `classify` (so wired to *something*), function
names from the datasheet pin table:

| Pin | classify | datasheet alt | notes |
|---|---|---|---|
| `PA_12` | LOW | KEY_ROW0 / ICFG0 | driven; role TBD |
| `PA_16` | LOW | KEY_ROW4 | KM0-driven (dynamic) |
| `PA_17` | LOW | KEY_ROW6 | driven; role TBD |
| `PA_27` | LOW | NORMAL_MODE_SEL strap | boot strap — ignore |
| `PA_28` | HIGH | PWM6 | candidate: motor PWM / LED |
| `PB_1`–`PB_6` | LOW | ADC_CH4/5/6, TOUCH/ADC0-2 | analog cluster — sensors? (read with `analogRead`) |
| `PB_7` | HIGH | TOUCH3 / ADC_CH3 | analog cluster |

## RFID module power / enable — **RESOLVED** (2026-06-14)

> **The RFID enable (PWEN) is AW9523B pin `P0_5` — an I2C GPIO-expander output,
> NOT a direct RTL GPIO.** That is why every RTL-pin trace failed (single, combo,
> opposite-bias, leave-one-out, and the forward/reverse PWEN injection): there is
> no RTL pin on the PWEN net to find — the RTL asserts it by writing the expander.
>
> - **Expander:** AW9523B, 7-bit addr 0x58–0x5B (ID reg 0x10 = 0x23).
> - **Bus:** bit-banged I2C on **`PA_18` (module pad 3) / `PA_19` (module pad 4)**.
>   These are NOT hardware-I2C0-capable pins (per the SDK), so the link is software
>   bit-bang — done by KM0 in stock firmware. (KM0 is part of our own combined
>   km0+km4 image and the framework KM0 knows nothing of the AW9523B, so the bus is
>   free for our KM4 to drive — and KM0 is ours to change if needed.)
> - **To power the module:** set `P0_5` push-pull (GCR 0x11 |= 0x10), GPIO mode
>   (LEDM 0x12 |= 0x20), output (CFG 0x04 &= ~0x20), high (OUT 0x02 |= 0x20).
>   Ready-to-run: `firmware/rtl8721dm-pinmap/tools/aw9523_pwen.py` (uses the
>   explorer's `bb` bit-bang on PA_18/PA_19). Confirm via master-mode read.
> - The board photo `Photos/DSC_1482.png` + `Docs/RTL8721DM_module_pinout.txt`
>   (module-pad ↔ RTL-pin map) are the source for this.
>
> Everything below this line is the (now-superseded) GPIO-only hunt, kept for the
> record — its negative results were all correct; we were just looking in the
> wrong place (RTL pins) for a gate that lives on the expander.

**The RFID module is currently OFF.** Confirmed: with the RTL held in reset and
the Pico mastering the bus (`master on` + reads), the module is *completely
silent* — every register read returns no reply, a `0x0000..0x0040` sweep finds 0
base addresses (vs. a full tag dump in earlier sessions). The device is battery-
capable, so peripherals are power-gated; the always-on touch panel still works
(buttons found) but the RFID/sensors sit behind a gate the stock firmware
asserts and our custom firmware does not.

**Enable is NOT any accessible floating GPIO** (automated hunt, `out` verified
working). Driven individually *and* in combination, high *and* low, none of the
floating, drivable pins woke the module or any peripheral:
`PA_13, PA_14, PA_15, PA_18, PA_19, PA_26, PB_23`. Detection used a pulled-down
baseline (`scan down`) so a powered module's push-pull TX would clearly show;
nothing did. (`PA_18/PA_19` are the likely RFID **UART** pins — UART-capable and
floating — so they were kept undriven as detection pins.) Harnesses:
`firmware/rtl8721dm-pinmap/tools/probe_enable.py`, `probe_combo.py`;
`firmware/pico-bridge/tools/module_probe.py`.

**Update — the enable is NOT a simple RTL GPIO.** Following the "enables are
bias-resistor-held driven pins, not floating" insight, every *overridable* driven
pin was driven to the opposite of its bias, individually and all together, with
the UART-capable pins kept undriven as detectors (so a waking RFID TX couldn't be
masked): `PA_12/17/28, PB_1/2/5/7/22`. **Nothing woke** (`probe_driven.py`).

Crucially, some "driven" pins are **not** bias-held — they're *actively* driven by
another chip and won't override: driving `PB_4`/`PB_6` high leaves them reading
low, and `PB_29`/`PB_31` toggle on their own. Together with the working touch
panel, this points to an **always-on companion controller** that owns the touch
matrix (`PB_4/6`, the dynamic bus `PB_29/31`) and **gates the peripheral/RFID
power rail**. The RTL likely *requests* RFID power-on by messaging that controller
over the `PB_29/31` bus — there is no bare RTL pin to pull.

So: enabling the RFID from custom firmware is **not** "drive pin X high"; it needs
either (a) replaying whatever the RTL sends the companion controller, or (b) a
load-switch/PMIC path we haven't mapped.

**Most reliable next step:** reverse-engineer the stock firmware dump
(`flash_dump/`, with the `ghidra/` project) for the RFID bring-up — how it powers
the rail (a controller message on `PB_29/31`? an I2C/SPI write? a pin sequence we
couldn't reach?) and which UART carries the Modbus link. That gives the exact
mechanism directly; blind GPIO probing is now exhausted (all safely-drivable pins
tried, both polarities, single and combined). Also worth characterizing the
`PB_29/31` bus (protocol, peer) as the companion-controller interface.

### Update 2 (2026-06-14) — exhaustive GPIO + I2C probing, inconclusive

There is **no external companion controller** — the only other core is the
RTL8721DM's own **KM0** (our custom image runs the Arduino sketch on KM4 but only
Realtek's *default* KM0 firmware; the dynamic pins `PA_16/25`, `PB_29/31` are
KM0-driven). The RFID enable is one of the 34 bonded pins or an AW9523B output
(the AW9523B is an I2C GPIO/LED-matrix expander on the module, addr 0x58–0x5B).

Probed with `firmware/rtl8721dm-pinmap` (`probe_irq.py`, `probe_leaveout.py`,
`probe_excluded.py`, `i2c_*.py`):
- **No GPIO wakes the module.** Every safely-drivable pin and every excluded pin
  (`PB_3`, `PA_27`, the KM0 dynamics), single *and* all-at-once (leave-one-out),
  both polarities, with newly-HIGH (UART-RX idle) **and** newly-LOW (tag IRQ,
  tag-on-antenna) detection — nothing changed on any pin.
- **KM4 can't reliably reach the AW9523B I2C.** Hardware `Wire` (pinmux-locked to
  SDA=`PA_26`/SCL=`PA_25`) NACKs 0x58–0x5B (0/30 even with retries); bit-bang on
  the swapped assignment gave only **non-reproducible artifacts** (a phantom
  "0x28", a noisy 57-address scan) — consistent with KM0 actively using the bus.

**Conclusion:** the enable mechanism is almost certainly mediated by **KM0**
(which our build doesn't replicate and which contends for the relevant pins/bus),
so blind KM4-side probing can't isolate it. Decisive next steps, at the bench:
1. **Trace the module's enable pad → RTL pin** (one fact unblocks everything).
2. **Logic-analyzer capture during a *stock* power-on** (reflash the stock
   backup): shows KM0's exact AW9523B I2C transaction and/or the enable GPIO
   write. Then replay it from KM4 (and decide whether KM0 must be quieted).
3. Stock-firmware RE for the same, statically.

### Update 3 (2026-06-14) — it's a *disable*, not an *enable* (reset-default reframe)

Re-ran the hunt with the **proven** detector (`PA_25` = RFID-RX, pulled down so
module-off = 0, module-on = 1; `find_enable.py`, `find_enable_multi.py`,
`find_enable_opp.py`). Tested every safely-drivable pin **singly** (both
polarities), **all-high**, **all-low**, and **all-pins-driven-to-opposite-of-bias**
simultaneously (the true "assert every candidate enable at once" combo the uniform
sweeps miss). **`PA_25` never left 0** — no KM4 GPIO state powers the module.

The decisive observation that explains this: **in master mode the module was
powered and replying while the RTL was held in reset.** With the RTL in reset all
34 GPIOs sit at their hi-Z/default-pull state — yet the module ran. So the module
is **enabled by the reset-default pin state**, and it's our *custom firmware*
(Realtek default KM0 image + Arduino core init) that *disables* it. We've been
hunting an "enable to assert" when the reality is a "disable to undo," held by
something KM4 can't override at runtime (KM0, which owns the dynamic pins).

This reframes the fix as **firmware-level, not runtime GPIO toggling**:
- At the bench, the pad→pin trace still pinpoints the physical net.
- In firmware, the experiment is to keep the relevant pin(s) at their *reset
  default* through early boot (don't let KM0/core-init drive them), or to quiet/
  reflash KM0 — not to drive a pin high at runtime.
- The real end-goal path is to bring up an **RTL-side Modbus master on
  `PA_26`/`PA_25`** and use a genuine module reply as the (most robust) detector,
  toggling the candidate disable in early boot.

## Still to find

- [x] **RFID-module enable / power-gate** — **AW9523B `P0_5`** via bit-bang I2C on
      `PA_18`/`PA_19` (see RESOLVED note above; `tools/aw9523_pwen.py`).
- [x] **RFID-module UART** on the RTL side — **`PA_26` = TX, `PA_25` = RX**
      (confirmed by injection, 2026-06-14; see the RFID UART section).
- [ ] **Tag-ready IRQ** (once powered: present/remove a tag → `watch` the toggling pin).
- [ ] **Lid motor** drive output(s) + driver enable (careful `out` probing).
- [ ] **Lid position / limit sensors** (move the lid by hand → `watch`).
- [ ] **Analog sensors** on `PB_1–PB_7` (food level? weight? — `analogRead`).
- [ ] Confirm the button functions against actual behaviour.

## Method

Flash the GPIO explorer, then on the bridge monitor: `classify` to find driven
pins, `watch` while actuating one thing at a time to bind a signal to a pin, and
careful `out` probing to find outputs. Recover from any fault with `reset` on the
bridge CDC1.
