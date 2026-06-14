# RTL8721DM — "hello world" custom firmware

First custom firmware for the feeder's **RTL8721DM** SoC. It does nothing useful
on purpose: it prints a banner + a 1 Hz heartbeat on the LOGUART and toggles the
built-in LED, to prove the whole **toolchain → build → flash → boot → serial**
loop works on the real chip before any de-cloud work begins.

Because `Serial` on AmebaD is the **LOGUART (PA7/PA8)** — the very lines the
in-housing Pico bridge taps as ISP TX/RX — you flash *and* watch the output
through the same bridge port you already use for dumping the RTL.

## Toolchain

PlatformIO, using the community **AmebaD platform** (tmmsunny012's port of
Realtek's `ameba-arduino-d` Arduino SDK). The board def **"SparkFun AzureWave
Thing Plus" (`sparkfun_awcu488`) is the RTL8721DM** — our exact SoC. (The
AMB21/AMB23 boards are the *RTL8722DM*, a different part — don't use those.)

### One-time setup

1. Clone the SDK fork locally (~1.3 GB; it is the framework package):

   ```bash
   git clone --depth 1 --branch feature-platformio-support \
     https://github.com/tmmsunny012/ameba-arduino-d.git C:/Users/mmsyl/ameba-arduino-d
   ```

   If you put it elsewhere, edit the two paths in [`platformio.ini`](platformio.ini).

2. Build (first run downloads PlatformIO's ARM GCC 10.3 toolchain):

   ```bash
   pio run        # in this directory (or: pio run -d firmware/rtl8721dm-hello)
   ```

   A clean build produces `…/.pio/build/sparkfun_awcu488/km0_km4_image2.bin` —
   the flashable application image.

## Flashing through the Pico bridge

> ⚠️ **Have your stock 8 MB dump on hand first.** The AmebaD upload writes the
> KM0/KM4 **boot images and the app** (`0x8000000`/`0x8004000`/`0x8006000`), so
> it replaces the stock bootloaders. If anything goes wrong, recovery = write the
> stock backup back over the bridge (`RTL872xDx_Flasher.exe -p COMx -b 1500000 wf
> 0 stock_backup_8M.bin`, or the equivalent). The upper flash partitions
> (config/data) are outside the erased regions and should survive — but the
> stock dump is your guaranteed un-brick.

The `sparkfun_awcu488` board def ships with `auto_mode: Disable`, so the uploader
does **not** reset the chip itself — it prints a 5-second countdown and expects
you to drop the RTL into download mode **during that window**.

⚠️ **Timing matters — confirmed on the bench:** arming download *before* you start
the upload does **not** work. The AmebaD ROM only emits its SYNC (`0x15`) burst
right after it enters download mode; if you pre-arm, that burst is long over by
the time the flasher starts reading, and it hangs at "wait 5s". Do it in this
order:

1. Start the upload pointed at the **bridge port (CDC0)**:
   ```bash
   pio run -t upload --upload-port COM<bridge-CDC0>
   ```
   It prints `Please enter the upload mode (wait 5s)` and counts `05 04 03 02 01`.
2. **During that countdown**, on the **control port (CDC1)**, type:
   ```
   download
   ```
   The flasher catches the SYNC, negotiates baud (auto-tracked by the bridge up
   to 1.5 Mbaud), erases, and writes the boot + app images.
3. It boots the new app on its own; if it sits in download mode, type `run` on CDC1.

> **Hands-free alternative.** The AmebaD uploader can drive reset/download over
> DTR/RTS (`DTR→CHIP_EN`, `RTS→download strap` — the same mapping the bridge
> implements). To use it, set `auto_mode` to `Enable` for this board *and* set
> `RTL_DTR_RTS_AUTORESET 1` in the bridge's `config.h`. The manual `download`
> route above is more predictable for bring-up, so start there.

## Watch it boot

```bash
pio device monitor -b 115200 -p COM<bridge-CDC0>
```

Expected (the heartbeat proves your code is running on the RTL):

```
=====================================================
 hello from RTL8721DM — custom firmware is running!
 Petlibro feeder SoC, built via PlatformIO + AmebaD
=====================================================
alive: tick 0  millis=51
alive: tick 1  millis=1053
...
```

(The `LED_BUILTIN` toggle won't be visible on the feeder — that's the SparkFun
dev board's LED pin, not necessarily wired on the feeder mainboard. The serial
heartbeat is the real proof of life.)

## Notes / gotchas

- **Machine-specific paths.** `platformio.ini` hard-codes the SDK clone location
  (`C:/Users/mmsyl/ameba-arduino-d`). Edit it if you clone elsewhere.
- **App image is 2 MB / RAM 512 KB** per the board def; the chip's 8 MB flash
  holds the rest (other partitions) — Arduino only touches boot + app.
- **`symlink://` on Windows** needs symlink privileges (Developer Mode or an
  elevated shell) the first time PlatformIO links the framework package. If it
  errors, enable Windows Developer Mode and re-run `pio run`.
- This is the Arduino on-ramp. The eventual de-cloud reimplementation targets
  the full Realtek **AmebaD GCC SDK** (FreeRTOS), but proving boot/serial here
  first de-risks everything downstream.
