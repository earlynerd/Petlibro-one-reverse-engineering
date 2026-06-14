# Petlibro RTL8721DM — Pi Pico programming & RFID-snoop bridge

A Raspberry Pi Pico (RP2040) installed permanently inside a Petlibro RFID smart
feeder. It gives you a no-disassembly programming port for the feeder's
**RTL8721DM** (Realtek Ameba-D) SoC, and passively snoops the **JY-L601D**
RFID module's UART link.

The Pico enumerates as **two USB serial ports**:

| Port | USB descriptor | Purpose |
|------|----------------|---------|
| CDC0 | *(default)* | **Transparent RTL bridge.** Point the Realtek Image Tool or a serial terminal at it. Auto-tracks the host baud (so 1500000 download baud and 115200 log baud both just work). |
| CDC1 | `Petlibro Control + RFID Snoop` | **Control console** (reset/boot macros, line control, status) **+ live RFID snoop**. |

## Project layout

```
firmware/pico-bridge/
├── platformio.ini        # arduino-pico (earlephilhower) + TinyUSB, 2x CDC
└── src/
    ├── config.h          # >>> pin map & parameters — EDIT THIS <<<
    ├── main.cpp           # USB setup + main loop
    ├── rtl_bridge.*       # USB<->RTL passthrough + reset/boot control
    ├── rfid_snoop.*       # two PIO RX-only UARTs, idle-gap framing
    └── control_console.*  # command parser on CDC1
```

## Wiring  ⚠️ confirm before connecting

All signals are 3.3 V — the RTL8721DM, the JY-L601D, and the Pico are all 3.3 V
logic, so connect directly (share grounds). Defaults in [`src/config.h`](src/config.h):

| Pico pin | Net | Connect to |
|----------|-----|------------|
| GP0 (UART0 TX) | `RTL_UART_TX_PIN` | ISP **RX** (→ RTL PA[8], pin 8) |
| GP1 (UART0 RX) | `RTL_UART_RX_PIN` | ISP **TX** (← RTL PA[7], pin 7) — also the DL strap |
| GP2 | `RTL_RST_PIN` | RTL **CHIP_EN** (QFN68 **pin 6**), open-drain — *one extra wire* |
| GP4 | `RFID_HOST_RX_PIN` | RTL → JY-L601D line (master commands, **8O1**; set to `-1` if one-wire) |
| GP5 | `RFID_READER_RX_PIN` | JY-L601D **TX** (slave replies, **8E1**) |
| GND | — | mainboard GND |

The `ISP` header is only **RX / TX / GND**. Everything else rides those: the
download strap (`UART_DOWNLOAD` = `PA[7]`, QFN68 pin 7) *is* the ISP **TX** line,
so it needs no wire. The only signal not on the header is **`CHIP_EN` (RTL pin 6, castellated module pin 28)** —
run one wire from GP2 to it for Pico-driven reset / hands-free download.

> If you skip the `CHIP_EN` wire, you can still flash by holding the strap low
> (`strap on`) and **power-cycling** the feeder. If RX/TX turn out swapped (no
> comms, or strap on the wrong line), swap the two data wires and set
> `RTL_DLSTRAP_PIN` to `RTL_UART_TX_PIN`. **Never** pull `PA[27]`/`NORMAL_MODE_SEL`
> (QFN68 pin 33) low at power-on — boot fails.

## Build & flash the Pico

```bash
cd firmware/pico-bridge
pio run                 # build
pio run -t upload       # hold BOOTSEL while plugging in, then upload (UF2)
pio device monitor      # opens one of the two ports
```

## Flashing / dumping the RTL8721DM through the bridge

Requires the **`CHIP_EN` wire** (Pico GP2 → QFN68 pin 6) so the Pico can reset
the chip. Without it, replace "enter download mode" below with `strap on` +
physically power-cycling the feeder, then `strap off` afterwards.

The bridge is a transparent USB-serial port (CDC0), so any AmebaD flasher works:
the **Realtek Image Tool**, **SharpRTL872xTool** (`RTL872xDx_Flasher.exe`), or
**Seeed `ambd_flash_tool`**. There are two ways to get into download mode:

**A — Automatic (drop-in adapter).** With `RTL_DTR_RTS_AUTORESET 1` (default), the
flasher's DTR/RTS lines drive reset+strap for you, exactly like a stock USB-UART
adapter — just point the tool at the bridge port and go. On the RESET line's
assert edge the firmware runs the atomic download/reset macro, using the STRAP
line as the mode selector (strap asserted → download, else → run). Default
mapping is the Realtek convention **DTR→reset, RTS→strap**; flip
`RTL_AR_RESET_ON_DTR` / `RTL_AR_INVERT` if your tool differs. (Opening the bridge
port in a plain terminal asserts DTR and will reset the RTL once — harmless.)

**B — Manual (deterministic).** Ignore DTR/RTS and arm download yourself:
1. Open the **control port** (CDC1), type `download` → chip waits in UART
   download mode (it stays there, so no rush).
2. Leave the **bridge port** (CDC0) free and point the flasher at it. It tracks
   whatever baud the tool opens (1.5 Mbaud for download).
3. When done, type `run` on CDC1 to boot the application.

### Dump the stock flash (do this first!)

```
# arm download (CDC1: `download`, or rely on auto-reset), bridge = COMx
RTL872xDx_Flasher.exe -p COMx -b 1500000 rf 0 0x800000 stock_backup_8M.bin
```
`rf 0 0x800000` reads the full **8 MB** (GD25Q64). **Reads are the reliable
direction on the RTL8721DM** (flash-unprotect for *writes* is finicky). Keep
`stock_backup_8M.bin` as your recovery image. Then check it in a hex viewer:
readable strings/headers = plaintext (no RSIP encryption); look for a
Manifest / Key-Certificate blob around the `km0_boot`/`km4_boot` images to tell
whether secure boot is provisioned. On this unit it is **not** — headers are
minimal (magic + size + load-addr, then raw ARM), so unsigned images boot. eFuse
read (for the secure-boot *enable* bit) is exposed by the Realtek Image Tool /
SDK tools if you want independent confirmation.

### Download-mode mechanics & tuning

Ameba-D uses an esptool-style strap on two pins (UM0401): `CHIP_EN` (pin 6) =
reset, `UART_DOWNLOAD`/`PA[7]` (pin 7, = LOGUART TX) = boot strap, **low at boot
= download from UART**. `enterDownload()` holds the strap (the ISP TX data line)
low across a `CHIP_EN` reset, then hands it back to the UART. If entry is flaky,
tune `RTL_RST_PULSE_MS`/`RTL_BOOT_SETTLE_MS`, or probe manually with `rst on|off`
and `strap on|off`. No comms at all usually means RX/TX are swapped — swap the
data wires and set `RTL_DLSTRAP_PIN` to `RTL_UART_TX_PIN`.

> ⚠️ Only one program can hold the bridge port at a time — don't leave a serial
> monitor open on it while flashing. The CDC1 snoop watches the *RFID* lines, so
> it won't show the flash session (that's on the bridge/RTL UART path).

## RFID snoop

Both JY-L601D UART lines are sampled by RX-only PIO UARTs (the RP2040's two
hardware UARTs are spoken for, so PIO supplies the extra receivers). Bytes are
reassembled into frames by inter-byte idle gap, **decoded as Modbus RTU**, and
printed on the control port — with the FDX-B tag ID resolved inline:

```
[  57.100][RFID-H]  8: 03 03 00 0E 00 04 24 28              :: READ 4 reg @0x000E [TAG]
[  57.111][RFID-R] 13: 03 03 08 00 82 05 70 FD C0 17 01 82 59 :: READ reply: TAG 130023370514455  q=1
    *** TAG 130023370514455 DETECTED (q=1) ***
```

`RFID-R` = reader = module TX (slave replies, **8E1**); `RFID-H` = host = RTL TX
(master commands, **8O1**). The two directions use **different parity** — set
per tap in [`src/config.h`](src/config.h) (`RFID_READER_FORMAT` / `RFID_HOST_FORMAT`)
or live with `rfidparity r|h n|e|o`. The decoder labels every Modbus action
(READ/WRITE/…), CRC-checks each frame, and emits `TAG … DETECTED/REMOVED`
markers on presence changes. Full protocol + register map:
[`../../Docs/RFID_PROTOCOL.md`](../../Docs/RFID_PROTOCOL.md).

## RFID **master** mode — drive the module directly

A step beyond the passive snoop: the Pico can **become the Modbus master** and
read/write any register on the JY-L601D itself — with the RTL out of the loop.
This is how you dump the *whole* register map (not just the ones the RTL happens
to poll) and probe the still-fuzzy config/diagnostic registers.

It works by reusing the very same two lines the snoop taps, only now **GP4 is
driven**:

```
TX (commands) GP4 @ 8O1  ──▶  module RX   (= the RTL→module "command" net)
RX (replies)  GP5 @ 8E1  ◀──  module TX
```

`master on` runs the safe entry sequence: **(1)** holds the RTL in `CHIP_EN`
reset so it stops mastering the bus and *releases* the command net, **(2)**
suspends the snoop to free GP4/GP5, **(3)** brings up the two master UARTs
(asymmetric parity preserved — we send 8O1, listen 8E1). `master off` reverses
it and reboots the RTL.

```
master on                       # RTL held in reset, Pico takes the bus
master init                     # optional: write 0x0000 = 0x0002 (RTL's boot enable)
master dump                     # sweep 0x0000..0x00FF, print every register that answers
master dump 0 0x40              # narrower/faster sweep (hex args)
master read 000E 4              # read 4 regs @ 0x000E (the tag block) — needs a tag present
master write 0000 0002          # write a single register
master tx 03 03 00 0E 00 04     # send a raw PDU (CRC appended), print the raw reply
master off                      # release the bus, snoop back on, RTL boots
```

The sweep distinguishes three responses, and the split is part of the map:
**answered** (a live base — block-read and printed), **exception `0x02`** (an
address the module doesn't implement — ~251 of them in a full sweep, tallied
with a code histogram), and **silent** (a *recognised but inactive* register —
e.g. `0x0050`, which is silent with no tag and becomes a live 5-register block
once a tag is read). Silent and exception addresses are tallied, not printed
per-line; whichever category is rare is listed so anomalies surface. The
per-transaction reply timeout is `master timeout <ms>` (default 40 ms); drop it
(`master timeout 15`) to make a full sweep ~4 s, and any keypress aborts it.

(Note: the tag `0x000E` register itself *always* answers — presence is signalled
to the host on a separate IRQ line, not by UART silence. See the protocol doc's
[Tag-presence semantics](../../Docs/RFID_PROTOCOL.md#tag-presence-semantics).)

> ⚠️ **Two hardware assumptions to verify on the bench before trusting this:**
> **(1)** the RTL must tri-state its RFID-UART TX pin while held in `CHIP_EN`
> reset (otherwise GP4 fights it — meter the command net during `rst on`);
> **(2)** the module must stay powered with the RTL held off (assumed always-on
> rail). If reads go silent the instant you `master on`, suspect one of these.
> The fix for contention is a series resistor on the GP4 tap. While master mode
> is active the `reset`/`download`/`run`/`rst`/`strap` verbs are **blocked**
> (they would create exactly that contention) — `master off` first.

## Control console commands

`help`, `status`, `reset`, `download`|`boot`, `run`, `rst on|off`,
`strap on|off`, `baud <n>`, `rfidbaud <n>`, `rfidparity [r|h] n|e|o`,
`snoop on|off`, `mon on|off`,
`master on|off|init|read|write|dump|tx|txraw|timeout`.
