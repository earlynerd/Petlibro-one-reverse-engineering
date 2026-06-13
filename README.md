# Petlibro RFID Feeder — Reverse Engineering & Custom Firmware

Independent teardown, reverse engineering, and (in-progress) custom-firmware
effort for a **Petlibro RFID smart pet feeder** (mainboard `PLNF301`, product
`AF301`). The goal is to understand how the feeder works end-to-end — the
RTL8721DM SoC, the RFID collar reader, and the cloud link — and ultimately run
custom firmware that can read the collar and actuate the lid **without the
vendor cloud**.

> **Disclaimer.** Unofficial and not affiliated with, endorsed by, or supported
> by Petlibro. "Petlibro" and any product names are trademarks of their
> respective owners, used here only for identification. Everything here is for
> interoperability, education, and repair of **your own device**. Opening and
> reflashing a feeder voids its warranty and can brick it — proceed at your own
> risk.

## What we learned

- **SoC:** Realtek **RTL8721DM** (Ameba-D, QFN68) with an external **8 MB**
  GigaDevice **GD25Q64** SPI flash.
- **Flash is plaintext (no RSIP) and secure boot is _not_ enforced** — unsigned
  images boot. So custom firmware can be built and flashed with no signing,
  encryption, or eFuse lock. No embedded private key or client certificate
  exists anywhere in the image (see [security notes](#security-observations)).
- **RFID:** an off-the-shelf **JY-L601D-V2** module (dual-freq 134.2 kHz +
  125 kHz, ISO 11784/5 FDX-B + EMID) on a **Modbus RTU** UART link. The collar
  tag ID is read from a handful of holding registers — **fully decoded**, see
  [`Docs/RFID_PROTOCOL.md`](Docs/RFID_PROTOCOL.md).
- **Cloud:** AWS-fronted MQTT (`mqtt.us.petlibro.com`) + audio assets
  (`oss.us.petlibro.com`) — the de-cloud target.

## Repository layout

```
.
├── firmware/pico-bridge/   # Pi Pico (RP2040) in-housing programming + RFID-snoop bridge
│   ├── src/                # config.h (pin map), rtl_bridge, rfid_snoop, control_console
│   ├── DECISIONS.md        # forward-facing log of design/behaviour decisions
│   └── DEBUG_LOG.md        # bench-bug breadcrumbs
├── Docs/
│   ├── RFID_PROTOCOL.md    # the Modbus/FDX-B protocol + register map (decoded)
│   └── UM0401-RTL872xD-Datasheet-v2.9.pdf
├── Tools/SharpRTL872xTool/ # AmebaD flasher used to dump/flash over the bridge
├── flash_dump/
│   └── stock_backup_8M_sanitized.bin   # stock image, personal data redacted (see below)
└── Photos/                 # board shots
```

## The Pi Pico bridge

A Raspberry Pi Pico (RP2040) is installed **inside the housing** as a permanent,
no-disassembly programming port and passive bus tap. It enumerates as two USB
serial ports:

| Port | Role |
|------|------|
| CDC0 | Transparent USB↔RTL UART bridge (point any AmebaD flasher or terminal at it; auto-tracks baud) |
| CDC1 | Control console (reset/boot macros, status) **+ live RFID snoop with inline Modbus/FDX-B decode** |

Build, wiring, flashing procedure, and the full command set are in
[`firmware/pico-bridge/README.md`](firmware/pico-bridge/README.md). Quick start:

```bash
cd firmware/pico-bridge
pio run -t upload      # hold BOOTSEL while plugging the Pico in
pio device monitor
```

## RFID protocol (decoded)

The RTL is the Modbus **master** (slave addr `0x03`); the JY-L601D module is the
slave. Full writeup in [`Docs/RFID_PROTOCOL.md`](Docs/RFID_PROTOCOL.md). Highlights:

- **Link:** Modbus RTU, **19200 baud**, **asymmetric parity** — commands
  (RTL→module) are **8O1 (odd)**, replies (module→RTL) are **8E1 (even)**. The
  master doesn't validate RX parity, so the product works despite the mismatch;
  a passive listener must decode each direction with its own framing.
- **Tag ID** is read as 4 holding registers at `0x000E`:
  - `0x000E` = FDX-B country code
  - `0x000F`–`0x0011`(hi) = 38-bit national ID
  - `0x0011`(lo) = per-read signal/quality (not part of the ID)
- Worked example (the collar in `Photos/`): country `130` + national
  `023370514455` → **`130023370514455`**.
- **Presence** = whether the `0x000E` read is answered; no tag → the module
  simply doesn't reply.

The snoop firmware decodes this live, e.g.:

```
[  57.111][RFID-R] 13: 03 03 08 00 82 05 70 FD C0 17 01 82 59  :: READ reply: TAG 130023370514455  q=1
    *** TAG 130023370514455 DETECTED (q=1) ***
```

## Dumping / flashing the RTL8721DM

The bridge presents as a normal USB-serial adapter, so standard AmebaD tools
(Realtek Image Tool, [`Tools/SharpRTL872xTool`](Tools/SharpRTL872xTool), Seeed
`ambd_flash_tool`) work through it. **Always dump the stock flash first** —
reads are the reliable direction on this part:

```bash
# arm download mode (CDC1: `download`), point the flasher at the bridge port (CDC0)
RTL872xDx_Flasher.exe -p COMx -b 1500000 rf 0 0x800000 stock_backup_8M.bin
```

See the [firmware README](firmware/pico-bridge/README.md) for download-mode
mechanics, the one-wire `CHIP_EN` reset mod, and DTR/RTS auto-reset.

## Security observations

For others studying this SoC/product:

- **No enforced secure boot.** Boot image headers (`0x0`/`0x4000`) are minimal
  (8-byte magic `0x96969999`/`0xFC66CC3F` + size + load-addr, then raw ARM) with
  no signature/pubkey/hash manifest. Unsigned images that boot ⇒ not enforced.
- **No flash encryption (RSIP).** Code/strings are plaintext in the dump.
- **No hardcoded private key or client certificate.** A full-image audit found
  zero PKCS#1/PKCS#8/EC DER key material and no populated PEM bodies; the
  `BEGIN … PRIVATE KEY`/`CERTIFICATE` strings present are just mbedTLS PEM label
  constants in the TLS library. Device cloud identity is a pair of per-device
  tokens in the config region (redacted in the published dump).

## The included flash dump

[`flash_dump/stock_backup_8M_sanitized.bin`](flash_dump/) is the stock 8 MB image
with **personal data redacted in place** (same byte lengths, structure intact):
WiFi SSID + password and the two per-device cloud tokens have been overwritten.
Left intact for analysis: the device serial, the `petlibro.com` endpoints, and
the collar IDs / feeding history. **The raw, un-redacted dump is intentionally
excluded** from version control (see [`.gitignore`](.gitignore)) — if you dump
your own feeder, keep that file private.

## Status & roadmap

- [x] In-housing Pico bridge (flash + snoop), flashing proven end-to-end
- [x] Full 8 MB stock dump + flash-map analysis
- [x] RFID Modbus/FDX-B protocol decoded; live decoder in the snoop
- [ ] Build a minimal custom AmebaD image (hello-world) and confirm it boots
- [ ] Reimplement RFID read → lid actuation locally
- [ ] Cut the AWS/MQTT cloud dependency

## References

- [`Docs/UM0401-RTL872xD-Datasheet-v2.9.pdf`](Docs/) — RTL872xD datasheet
- Realtek **AmebaD SDK v6.2C** (stock build); `ameba-arduino-d` core for custom builds
- [arduino-pico (earlephilhower)](https://github.com/earlephilhower/arduino-pico) — Pico core / SerialPIO

## License

The bridge firmware and documentation in this repo are released under the
**MIT License** (see [`LICENSE`](LICENSE)). Third-party materials (the RTL872xD
datasheet, `Tools/SharpRTL872xTool`, captured firmware images) remain the
property of their respective owners and are included for
reference/interoperability only.
