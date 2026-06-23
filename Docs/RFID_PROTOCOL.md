# JY-L601D RFID module ↔ RTL8721DM protocol

Reverse-engineered from a live serial snoop of the Petlibro feeder mainboard
(see `firmware/pico-bridge/`). The Pi Pico bridge taps both UART lines between
the RFID module and the RTL8721DM and logs framed hex; the steady-state traffic
turned out to be Modbus RTU carrying an FDX-B animal tag ID.

Status: **tag-ID encoding fully decoded and confirmed** against a known tag.
Some configuration/diagnostic registers are still partially understood (noted
below).

## Physical link

| Property | Value | Notes |
|---|---|---|
| Protocol | Modbus RTU | master = RTL, slave = RFID module |
| Slave address | `0x03` | every frame starts with `03` |
| Baud | 19200 | same both directions |
| Parity | **asymmetric** | RTL→module = **8O1** (odd); module→RTL = **8E1** (even) |
| Stop bits | 1 | |
| Tag-ready IRQ | **`PA_16`** (RTL pin 29 / module pad 1), module → RTL | **out-of-band tag-present signal** (not on the UART). The RTL gates its Modbus polling on this line — see [Tag-presence semantics](#tag-presence-semantics). **Active-low held level**: HIGH = no tag, LOW = tag in field (confirmed 2026-06-14). |

⚠️ The **parity differs per direction** — the module transmits its replies in
even parity but accepts commands in odd. The RTL master doesn't validate RX
parity, so the product works; any passive listener must decode each direction
with its own framing. See `firmware/pico-bridge/DECISIONS.md` (2026-06-13) and
the captures `modbus_parity.png` / `modbus_parity_RFID.png`.

Snoop taps (in `config.h`): `RFID-H` = host = RTL TX (commands, 8O1);
`RFID-R` = reader = module TX (replies, 8E1).

## Register map

All accesses are to slave `0x03`. Registers are 16-bit, big-endian on the wire
(Modbus standard).

| Register(s) | Observed value | Typical access | Meaning |
|---|---|---|---|
| `0x0000` | `0x0002` (written) | Write Single (`0x06`), at boot | Mode / enable — exact semantics unconfirmed |
| `0x0005`–`0x000D` | `03 05 05 04 04 … 03 03 00` | Read (`0x03`, qty 9), at boot | Diagnostic / signal block (antenna levels?). Values trend 05→04→03; not the tag ID |
| `0x000E` | `0x0082` (= 130) | Read (`0x03`) | **FDX-B country code** |
| `0x000F` | `0x0570` | Read | **National ID**, bits [39:24] |
| `0x0010` | `0xFDC0` | Read | **National ID**, bits [23:8] |
| `0x0011` | `0x17··` | Read | hi byte = National ID bits [7:0]; **lo byte = read quality/signal** (varies `0x00`–`0x11`, not part of the ID) |
| `0x000E`+13 | — | Write Multiple (`0x10`, qty `0x000D`), at boot | RTL seeds/initialises the tag region (data half not cleanly captured) |

The animal tag is read as **4 registers starting at `0x000E`**:

```
master  03 03 00 0E 00 04 24 28          ; Read Holding Registers, qty 4 @ 0x000E
slave   03 03 08 <CC CC NN NN NN NN NN QQ> <crc>   ; 8 data bytes = 4 registers
```

## Tag-ID (FDX-B) encoding

The 15-digit ID is `country (3 digits)` + `national ID (12 digits)`, packed into
the 8 data bytes of the `0x000E` read reply:

```
data:   CC CC | NN NN NN NN NN | QQ
        └─┬──┘   └──────┬─────┘   └┬┘
       country     national ID   read quality (NOT part of the ID)
       (reg 0x000E) (38-bit, regs 0x000F..0x0011 hi byte)
```

- **country** = `reg 0x000E` as a decimal number
- **national ID** = the 40-bit big-endian value `0x000F ‖ 0x0010 ‖ 0x0011[hi]`
  (38 significant bits), printed zero-padded to 12 decimal digits
- **quality** = low byte of `0x0011` — a per-read signal/quality value that
  changes every poll while the tag sits still; ignore it for the ID

### Worked example (confirmed)

Reply `03 03 08 00 82 05 70 FD C0 17 01 82 59`:

```
country     = 0x0082                       = 130
national ID = 0x05 70 FD C0 17 = 0x0570FDC017 = 23,370,514,455 → "023370514455"
quality     = 0x01

tag ID = "130" + "023370514455" = 130023370514455   ✓ matches the physical tag
```

## Modbus transactions observed

| Function | Code | Seen as | Purpose |
|---|---|---|---|
| Read Holding Registers | `0x03` | continuous polling of `0x000E`; boot reads of `0x0005` | read tag + diagnostics |
| Write Single Register | `0x06` | `reg 0x0000 = 0x0002` at boot | mode/enable |
| Write Multiple Registers | `0x10` | qty 13 @ `0x000E` at boot | init tag region |

## Tag-presence semantics

> **Corrected 2026-06-13 (master-mode probing).** The earlier rule — "no tag →
> the `0x000E` read gets no reply" — was a **host-side artifact, not module
> behaviour**. A master-mode read of `0x000E` (even the RTL's exact qty=4 poll)
> *always answers*, returning the last-read ID with a stale quality byte when no
> tag is present:
>
> ```
> tx  03 03 00 0E 00 04            ; qty=4 poll, no tag on antenna
> rx  03 03 08 00 82 05 70 FD C0 17 FF 03 D9   ; cached ID, quality = 0xFF (stale)
> ```
>
> The module never goes silent on this register. So the snoop only saw silence
> because **the RTL wasn't *issuing* the poll** — presence is signalled to the
> host out-of-band on a separate **tag-ready IRQ line**, and the RTL polls the
> Modbus ID only after that line fires.

Presence detection, restated:

- **Real signal** — the module → RTL **tag-ready IRQ on `PA_16`** (RTL pin 29 /
  module pad 1), separate from the UART. **Active-low, held level:** `PA_16` reads
  HIGH with no tag and is pulled LOW the entire time a tag is in the field
  (confirmed 2026-06-14, tag on→`L` / off→`H`, stable). This is what the firmware
  should watch — level-low or falling-edge — instead of polling.
- **Modbus-level signal** — the reliable bus indicator is **`0x0050`**: it
  ANSWERS (5-reg record) while a tag is in the field and goes **silent** (no
  reply) once the tag is removed. Poll its respond-vs-silent to confirm presence.
  ⚠ **Correction (2026-06-14, standalone master-mode bench):** `0x0012` is **NOT**
  a live present/absent flag — with the tag removed it **stayed `0x8000`**, i.e.
  it latches like the `0x000E` ID (matches the bench note that the reader latches
  the last tag). `0x001F`'s tail byte did move (`0x0D00`→`0x0DFF`) but isn't
  consistent across captures, so don't rely on it. Use `0x0050` respond-vs-silent,
  or better, the out-of-band IRQ line above.

The passive snoop's heuristic (infer "removed" after `kTagMissThresh` unanswered
polls) still works *as a snoop* because it keys off the RTL's IRQ-gated polling
cadence — but it is observing the host's behaviour, not the module's.

## Boot / init sequence (≈ first 2 s after reset)

1. `WRITE reg 0x0000 = 0x0002` (mode/enable), sometimes sent back-to-back.
2. `WRITE-MULTI 13 regs @ 0x000E` (seed the tag region).
3. Repeated `READ 9 regs @ 0x0005` returning the descending `05/04/03`
   diagnostic block (antenna tuning / signal levels — unconfirmed).
4. Then the bus goes idle until a tag enters the field, after which it polls
   `READ 4 regs @ 0x000E` continuously (~10 Hz) for as long as the tag is present.

Note: at boot, frames sent <3 ms apart can be merged by the snoop's idle-gap
framer (e.g. a doubled `0x06` write shows as one 16-byte line). The decoder
validates and labels the first Modbus PDU and flags the remainder as merged.

## Open questions

- `reg 0x0000` exact meaning. Reads `0x001A` under master mode (the RTL writes
  `0x0002` at boot); the bit semantics are still undecoded. Writing it and
  re-sweeping is the way in.
- `0x0040` block (`6F40 1BA6 2217 66C2`, 4 regs, tag-invariant): module ID /
  serial / firmware version — which, and is it per-unit?
- The per-read signal word at `0x001D`/`0x002F` (hi byte constant, lo byte
  varies each poll): RSSI, read counter, or confidence?
- ~~The tag-ready IRQ line~~ **RESOLVED (2026-06-14): `PA_16`, active-low held
  level** (HIGH = no tag, LOW while a tag is in the field — confirmed by tag
  on/off, stable). It's a level, not a per-decode pulse. This is the presence
  signal the firmware should watch (level-low / falling-edge), no polling needed.
- The data half of the boot `WRITE-MULTI @0x000E` (what the RTL seeds there).

Most of the original open questions about the tag block are now **answered** by
the master-mode sweep — see **Register map from the master-mode sweep** below.

## Actively probing the module (master mode)

The Pico bridge can also stop being a passive listener and **become the Modbus
master**: hold the RTL in reset (so it releases the bus), then drive the module
directly to read/write *any* register. This is the way to answer the open
questions above — sweep the full holding-register map, read a register the RTL
never touches, or write a config register and watch the effect.

```
# on the control port (CDC1):
master on            # RTL held in reset; Pico drives GP4 @ 8O1, listens GP5 @ 8E1
master dump          # find base addrs + block-read each; tally exceptions/silent
master read 000E 16  # the full FDX-B tag block (answers zero-filled even w/o a tag)
master tx 03 03 00 0E 00 04   # raw PDU (CRC appended) -> raw reply, for poking codes
master off           # release the bus; the RTL reboots and resumes mastering
```

Master mode preserves the asymmetric parity from the master side (commands out
8O1, replies in 8E1). Mechanics, the safe entry/exit sequence, and the **two
hardware assumptions to bench-verify** (RTL tri-states its UART TX in reset; the
module stays powered with the RTL off) are in
[`../firmware/pico-bridge/README.md`](../firmware/pico-bridge/README.md).

### Three response states

A read to an arbitrary address resolves to one of three things, and the
distinction is part of the map:

- **answered** — a live register/block.
- **silent** (no reply) — a *recognised but currently inactive* register. Seen
  for `0x0050` with no tag: it returns nothing until a tag is read, then becomes
  a live 5-register block.
- **exception `0x02`** (illegal data address) — `addr` is not a known base. A
  full `0x0000..0x00FF` sweep returns ~251 of these.
- **exception `0x03`** (illegal data value) — `addr` *is* a base but `qty`
  overran the block. E.g. `READ 0x000E qty 18` succeeds; `qty 19` → `0x03`.

So the module is a **fully-validating Modbus slave** (not a flat register file
and not a lazy one): it checks the start address against a whitelist (`0x02` if
bad), *then* checks `qty` against that block's length (`0x03` if you overrun),
and otherwise returns exactly the requested count of consecutive registers. That
means **every block has a precise, queryable length** — grow the `qty` until it
flips to `0x03`. Mid-block addresses like `0x000F` are unreadable as a *start*
(they're not whitelisted) but appear as offsets inside a read from `0x000E`.

### Register map from the master-mode sweep

Sweep of `0x0000..0x00FF` with `0x0000 = 0x001A`, before/after presenting the
known tag (country 130 / national `023370514455`). Confirmed values in **bold**,
tentative interpretations italicised.

Block lengths are exact (confirmed by growing `qty` until it returns exception
`0x03`):

| Base | Len | Contents | Meaning |
|---|---|---|---|
| `0x0000` | **1** | `001A` | **mode/status** (qty≥2 → exc `0x02`). *bits undecoded* |
| `0x000E` | **18** | tag block (below); qty 18 OK, 19 → `0x03` | **primary FDX-B tag buffer** |
| `0x0020` | **18** | exact mirror of `0x000E` reg-for-reg (incl. the live signal words) *except* the final reg: `0x0031=1FFF` vs `0x001F=8DFF` | **second window onto the same tag buffer** (per-window status word at the tail) |
| `0x0040` | **4** | `6F40 1BA6 2217 66C2` (tag-invariant) | *module ID / serial / firmware version* |
| `0x0050` | **5** | silent w/o tag → `0082 0570 FDC0 17D8 4C02` w/ tag | **tag-gated "last read" record** (country + national ID + 2 status bytes) |

The `0x000E` block in full (18 regs, `0x000E..0x001F`), decoded with the tag present:

| Reg | Value | Meaning |
|---|---|---|
| `0x000E` | **`0082`** | **country = 130** |
| `0x000F` | **`0570`** | **national ID [39:24]** |
| `0x0010` | **`FDC0`** | **national ID [23:8]** |
| `0x0011` | **`1701`** | **hi = national ID [7:0]; lo = read-quality** |
| `0x0012` | **`8000`** | ~~tag-present flag~~ — **latches** `0x8000`, stays set after tag removed (2026-06-14 correction; use `0x0050` respond/silent instead) |
| `0x0013` | `0005` | *rolling status — seen `0`/`5` in both tag states; not a simple counter* |
| `0x0014`–`0x001B` | `0000` | (unused / reserved) |
| `0x001C` | `0059` | *constant — config / block marker?* |
| `0x001D` | `56xx` | *per-read signal: hi const `0x56`, lo varies (`FF`/`02`/`CF`)* |
| `0x001E` | `0EC7` | *status / signal* |
| `0x001F` | `8DFF` | *status; bit 15 set → likely a second flag* |

This re-confirms the FDX-B encoding bit-for-bit against the physical tag, and
adds a clean **tag-present flag at `0x0012`** the RTL never reads (its qty=4 poll
stops at `0x0011`), plus a defined 18-register extent.

## Where this is implemented

- **Passive decode** — `firmware/pico-bridge/src/rfid_snoop.cpp`
  (`RfidSnoop::decodeFrame`): annotates every captured frame with the Modbus
  action and, for `0x000E` replies, prints the decoded FDX-B tag ID inline plus
  `*** TAG <id> DETECTED ***` / `*** TAG REMOVED ***` markers.
- **Active master** — `firmware/pico-bridge/src/rfid_master.cpp` (`RfidMaster`):
  `readRegs` / `writeReg` / `transact` / `dump`, exposed through the console's
  `master …` subcommands.
