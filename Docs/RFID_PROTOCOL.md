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

When **no tag is on the antenna**, the master's `0x000E` read simply **gets no
reply** — the module does not answer and does not return a Modbus exception.
So presence is detectable purely from *whether a reply arrives*, before parsing
the ID:

- **Detected** — first valid `0x000E` reply after an absence.
- **Removed** — `kTagMissThresh` (3) consecutive `0x000E` polls go unanswered.

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

- `reg 0x0000` exact meaning (mode? channel? gain?).
- `0x0005`–`0x000D` block semantics (likely per-antenna RSSI / LC tuning).
- The `0x0011` low byte: confirm whether it is RSSI, a read counter, or a
  decode-confidence metric.
- The data half of the boot `WRITE-MULTI @0x000E` (what the RTL seeds there).

## Where this is implemented

The live decoder is in `firmware/pico-bridge/src/rfid_snoop.cpp`
(`RfidSnoop::decodeFrame`): it annotates every captured frame with the Modbus
action and, for `0x000E` replies, prints the decoded FDX-B tag ID inline plus
`*** TAG <id> DETECTED ***` / `*** TAG REMOVED ***` markers.
