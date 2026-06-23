# Petlibro Dot-Matrix Display Protocol

The Petlibro smart feeder features a dot-matrix LED display. Reverse engineering of the control signals reveals that it is driven by two **WS1625** LED driver ASICs operating in parallel.

**Chip family (confirmed 2026-06-17):** WS1625 speaks the **Titan Micro TM1629 / TM1638 / TM1640 command set** — address command `0xC0+grid`, data byte = SEG bits, display-control `0x88|brightness` (B3=ON, B0–2=PWM duty). It is *not* the TM1680/HT1632 class: that part is slave-addressed (`0xE4–0xE7` first byte, never seen in any capture) and its `0x88` means BLINK-OFF (wouldn't light our panel). WS1625 is a clone of the TM1629 family with **24 grids** (vs TM1629's 8) over a **2-wire I²C-like bus** (SCL+SDA+ACK) instead of TM1629's 3-wire SPI (CLK/STB/DIN).

**No selectable memory mapping.** The TM1629 family has a *fixed* GRID×SEG RAM→LED map — there is no COM-option / ROW-COM mode like the TM1680. Verified on the bench: firing every TM1680-style COM-option byte (`0xA0/A4/A6/A8/AC`) before a grid write **never changed the mapping**. So the data→dot layout is the fixed physical wiring of the GRID/SEG pins to the matrix and must be mapped empirically (below).

Each chip handles half of the display matrix (24 Grids x 7 Segments). To save GPIO pins and increase refresh rate, the MCU bit-bangs both chips sequentially using a shared clock line and two independent data lines.

## Pin Mapping (RTL8721DM)

Confirmed on the bench (driving the panel with custom firmware, 2026-06-17):

*   **SCL (Shared Clock):** `PA_13`
*   **SDA_LEFT (data line A):** `PA_14`
*   **SDA_RIGHT (data line B):** `PA_15`
*   **PWM:** `PA_12` — the connector's PWM line; **not** a data line and never driven by stock firmware (function TBD).

> Correction: an earlier revision of this doc listed `PA_12` as `SDA_LEFT`. The bench shows `PA_12` carries no display data — the two data lines are `PA_14` and `PA_15` (adjacent pins). "LEFT"/"RIGHT" here are just the two data lines; which one is the physical left half is determined by the dot-mapping probe below.

## Physical Protocol (Pseudo-I2C)

The communication protocol heavily mimics I2C but **lacks I2C Device Addresses**. 
*   **Start Condition:** `SDA` transitions HIGH to LOW while `SCL` is HIGH.
*   **Stop Condition:** `SDA` transitions LOW to HIGH while `SCL` is HIGH.
*   **Bit Order:** MSB First.
*   **ACK Bit:** After 8 bits of data, the master releases `SDA` (HIGH), and pulses `SCL` a 9th time. The WS1625 may or may not pull `SDA` LOW to acknowledge (ACK). Best to ignore.

Because there is no device address, the very first byte sent after the Start condition is always a **Command Byte**.

## Command Set

The chips use the standard Weaver Semiconductor / Titan Micro instruction set:

### 1. Data Command (Optional but standard)
*   `0x40`: Write data (auto-increment address)
*   `0x44`: Write data (fixed address)

### 2. Display Control (Brightness / Power)
Controls the global PWM duty cycle and powers the LEDs.
*   `0x80`: Display OFF
*   `0x88`: Display ON (Minimum Brightness, 1/16 duty)
*   ...
*   `0x8F`: Display ON (Maximum Brightness, 14/16 duty)

### 3. SRAM Address Command
Sets the target Grid (column) in the display RAM. The WS1625 has 24 bytes of SRAM (`0xC0` through `0xD7`).
*   `0xC0`: Grid 1
*   `0xC1`: Grid 2
*   ...
*   `0xD7`: Grid 24

## Typical Transaction Sequence

To update the display, the original firmware uses **Fixed Address Mode**. It issues 24 separate 18-bit transactions (Start -> Command -> Data -> Stop) to update the entire matrix, followed by a Display Control command to turn it on.

### Example: Turning on all LEDs at Max Brightness

1. **Write Display RAM (Repeated 24 times):**
   *   `[START]`
   *   Write `0xC0` (Address 0) -> `[ACK]`
   *   Write `0xFF` (All 8 segments ON) -> `[ACK]`
   *   `[STOP]`
   *   *(Repeat for `0xC1` through `0xD7`)*

2. **Enable Display:**
   *   `[START]`
   *   Write `0x8F` (Display ON, 14/16 PWM) -> `[ACK]`
   *   `[STOP]`

## Implementation Notes

When writing custom firmware, you can optimize the refresh rate by writing to both `SDA_LEFT` and `SDA_RIGHT` simultaneously in your bit-banging loop while toggling the shared `SCL` pin. This updates both halves of the screen in exactly the same number of clock cycles as updating a single screen.

> Note on the connector: the display ribbon also carries a **`PWM`** line on **`PA_12`** (silkscreen order `PWM, SDA2, SDA1, SCL, GND, 3.3V`). The stock firmware **never drives `PA_12`** — the panel lights from the SCL/data lines alone — so it is *not* a display enable/brightness gate. Its actual function is still TBD (left undriven by our probe firmware too).

## LED Dot Mapping (work in progress — 2026-06-17)

The panel is physically **7 rows × 28 columns**. The **leftmost two columns** sit behind a red filter (alert indicator); the rest are the panel's native color. The two driver chips are **side-by-side** — each drives a left/right block of columns over its own data line (`PA_14` and `PA_15`, sharing SCL `PA_13`) — not stacked top/bottom.

**The data→dot mapping is interleaved — a grid byte is _not_ simply "one column of 7 vertical rows."** Evidence, from the `display_i2c.csv` capture of the scrolling **"add food"** message:

- Fixed-height (7-row) scrolling text can only ever light the *same* ≤7 bit positions within any single grid. Instead, several grids (`0xC2/0xC4/0xC6/0xC8`) cycle through **all 8 bit-positions** over the capture (max popcount per byte is 6; no byte is `0xFF`), and per-frame reconstruction does **not** render legible text.
- The capture's frame boundaries are also noisy (many partial refreshes) and the scroll step is non-integer, so the byte stream alone is a poor basis. We therefore map the panel **empirically** by lighting known cells and reading off the physical dot.

### Bench probe firmware (`firmware/rtl8721dm-pinmap`, `disp` command)

Bit-bangs the **exact stock sequence** (per grid: `START, 0xC0+grid, data, STOP`; then display-control `0x88|bri`). Open-drain emulation on `PA_13`(SCL)/`PA_14`(line L)/`PA_15`(line R); ACK clock pulsed and ignored. Because each chip only acts on a START seen on *its own* data line, lines L and R can be driven in isolation despite the shared clock. (Pins are runtime-settable with `disp pins <scl> <sdaL> <sdaR>`.)

| Command | Effect |
|---|---|
| `disp on [bri]` / `disp off` | display-control on (brightness 0–7, default 7) / off |
| `disp clear` | all dots off, both lines |
| `disp fill L\|R\|B <hex>` | every grid = `<hex>` |
| `disp col L\|R\|B <grid 0-23>` | one grid = `0x7F` (lights whatever that grid maps to) |
| `disp bit L\|R\|B <bit 0-7>` | that one bit set in **all** 24 grids |
| `disp px  L\|R\|B <grid> <bit>` | exactly one cell on (clears the rest) |
| `disp raw L\|R\|B <grid> <hex>` | set one grid byte (no clear) |

`L` = data on `PA_14`, `R` = `PA_15`, `B` = both. (Which line is the physical *left* half is itself TBD — determined by the probe below.)

### Probe procedure (fill in as we go)

1. **Comms / coverage:** `disp fill B 7F` → should light most of the panel. Note any always-dark rows/columns and where the red columns are.
2. **What is a grid?** `disp col L 0`, then `disp col R 0` → is the lit set a vertical line (grid = column) or horizontal (grid = row)? Which physical half does each line drive?
3. **What is a bit?** `disp bit L 0` … `disp bit L 6` → does each bit light one full row? In what top→bottom order do bits 0–6 map to rows? Does bit 7 light anything?
4. **Grid order:** `disp col L 0,1,2,…` → do successive grids step one physical column at a time, or interleave (even/odd, reversed, paired)?
5. **Pinpoint:** `disp px L <grid> <bit>` to resolve any ambiguous cell.

Coordinates used on the bench: **`(x, y)`, origin `(0,0)` = bottom-left dot, x = 0…27 left→right (x0, x1 = the red pair), y = 0…6 bottom→top.**

**FULLY SOLVED & VERIFIED (2026-06-18).** Coordinates: `(x,y)`, origin bottom-left, x = 0…27 left→right (**x0,x1 = the red pair**), y = 0…6 bottom→top. The complete `(x,y)→(line,grid,bit)` map lives in `xy2cell()` in `firmware/rtl8721dm-pinmap/src/main.cpp` and is exercised by `disp xy / hline / vline / border` and `tools/walk_pixel.py`. `disp border` (all four edges) and a full-width walk confirm every one of the 196 dots.

- **Line L (`PA_14`) — x0–x16.** A direct lookup table `MAP_L[7][28]` (exact per-cell, no formula), recovered from the 8-plane `disp idx L 0..7` scan. Structure: horizontal **row-blocks** for x1–x11 — even grid `2k` drives block A (x2–x8), odd grid `2k+1` drives block B (x9,x10,x11 and the red x1), where row `y = 6 − k`; plus single-column **verticals** (`bit b → y = 6−b`) for x0 (=grid22, left red), x12=grid14, x13=grid16, x14=grid18, x15=grid20, x16 (grids 15/17/19/21). The `bit→x` within a block is a fixed per-grid permutation — read it from `MAP_L`, don't assume `x=b+1`.
- **Line R (`PA_15`) — x17–x26.** Mirror of line L: physical `x` maps to line-L column `localX = 28 − x`, driven with the same grid/bit numbers on `PA_15`. (Line L's verticals/red mirror off the right edge — the "cut off" halves.)
- **x27 (rightmost) — split vertical** on line R, `bit = 6 − y`: **grid15** drives the top 3 rows (y4–y6 = bits 0–2), **grid14** drives the bottom 4 (y0–y3 = bits 3–6).

### How it was solved (and the two traps)
Binary-coded scan (`disp idx`, 8 planes encode `index = grid·8 + bit`) → hand-transcribed to 28×7 pixel art → direct decode (`Tools/decode_pixelart.py`). Because the smoked lens makes dot-by-dot coordinate reading impractical, the loop was: **transcribe once, then fix systematic errors via single-pixel `disp px` hardware checks** rather than re-collecting bulk data. Two systematic bugs surfaced and were fixed this way:
1. **One transcribed plane (plane 2) was shifted by one column** — corrupting bit2 across a whole column of cells, which presented as *entire columns out of order* (not random single-dot errors). Found by `disp px L 6 6` lighting only one of two colliding dots, pinpointing the bad plane.
2. **Line-R mirror axis off-by-one** (`27−x` vs `28−x`) — left one boundary column skipped and the last column dark. Fixed in one line.
