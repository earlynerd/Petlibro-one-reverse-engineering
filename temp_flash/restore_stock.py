#!/usr/bin/env python3
"""
AmebaD UART flash uploader.

Cross-platform Python implementation of the Realtek AmebaD flash protocol.
Replaces the platform-specific C++ upload_image_tool binaries.

Usage:
    python upload_amebad.py <tools_dir> <serial_port> [options]

Protocol reference:
    Ameba_misc/Autoflash_patch/src/upload_image_tool.cpp
"""

import argparse
import os
import struct
import sys
import time

try:
    import serial
except ImportError:
    sys.stderr.write(
        "Error: pyserial is required. Install with: pip install pyserial\n"
    )
    sys.exit(1)

# ---------------------------------------------------------------------------
# Constants
# ---------------------------------------------------------------------------

SYNC = 0x15
ACK = 0x06

# Baud rate table: (speed, sub_command_byte)
SPEED_TABLE = [
    (1500000, 0x18),
    (1444400, 0x17),
    (1382400, 0x16),
    (1000000, 0x15),
    (921600, 0x14),
    (500000, 0x13),
    (460800, 0x12),
    (380400, 0x11),
    (230400, 0x10),
    (153600, 0x0F),
    (128000, 0x0D),
    (115200, 0x0C),
]

# Flash memory map
FLASHLOADER_ADDR = 0x082000
FLASH_MAP = [('stock_backup_8M.bin', 0x8000000)]

VERBOSE = 0


def log(level, msg):
    if VERBOSE >= level:
        sys.stdout.write(msg)
        sys.stdout.flush()


# ---------------------------------------------------------------------------
# DTR / RTS control
# ---------------------------------------------------------------------------

def set_dtr_rts(ser, level):
    """Set DTR/RTS signals.

    level encoding (matching the C++ convention):
        bit 0 = 1 -> RTS# HIGH (inactive)
        bit 0 = 0 -> RTS# LOW  (active)
        bit 1 = 1 -> DTR# HIGH (inactive)
        bit 1 = 0 -> DTR# LOW  (active)

    pyserial: ser.rts = True means RTS active (low on RS-232).
    The C++ code uses EscapeCommFunction(SETRTS) when bit0==0,
    which asserts RTS (drives it active/low). So:
        bit0==0 -> RTS active  -> ser.rts = True
        bit0==1 -> RTS inactive -> ser.rts = False
    Same logic for DTR with bit1.
    """
    ser.rts = not bool(level & 0x1)
    ser.dtr = not bool(level & 0x2)


def enter_download_mode(ser):
    """Toggle DTR/RTS to enter UART flash download mode."""
    set_dtr_rts(ser, 0x2)   # EN LOW (reset)
    time.sleep(0.5)
    set_dtr_rts(ser, 0x1)   # TX_LOG LOW (download mode)
    time.sleep(0.2)
    set_dtr_rts(ser, 0x3)   # both HIGH (release)
    time.sleep(0.5)


def reset_to_boot(ser):
    """Toggle DTR/RTS to reset into normal boot mode."""
    set_dtr_rts(ser, 0x2)   # EN LOW
    time.sleep(0.5)
    set_dtr_rts(ser, 0x3)   # release


# ---------------------------------------------------------------------------
# Serial helpers
# ---------------------------------------------------------------------------

def read_byte(ser, timeout_ms=2000):
    """Read a single byte with timeout. Returns the byte value or -1."""
    old_timeout = ser.timeout
    ser.timeout = timeout_ms / 1000.0
    data = ser.read(1)
    ser.timeout = old_timeout
    if data:
        log(7, "%02X " % data[0])
        return data[0]
    return -1


def wait_sync(ser, count=1):
    """Wait for `count` consecutive SYNC (0x15) characters."""
    remaining = count
    attempts = 5000
    tick = 0
    while remaining > 0 and attempts > 0:
        attempts -= 1
        b = read_byte(ser, timeout_ms=2000)
        if b == -1:
            continue
        if b != SYNC:
            if remaining == count and count > 1:
                if tick == 0:
                    log(0, ".")
                tick = (tick + 1) % 4
                continue
            else:
                return False
        remaining -= 1
        time.sleep(0.001)
    if remaining > 0:
        log(0, "** sync timeout.\n")
        return False
    return True


def wait_ack(ser):
    """Wait for ACK (0x06) response character."""
    for _ in range(5000):
        b = read_byte(ser, timeout_ms=2000)
        if b == ACK:
            return True
        if b == SYNC:
            continue
        if b != -1:
            log(3, "[wait_ack] unexpected: 0x%02X\n" % b)
            return False
        time.sleep(0.001)
    return False


def flush_serial(ser):
    """Drain any pending data from the serial buffer."""
    for _ in range(300):
        b = read_byte(ser, timeout_ms=100)
        if b == -1:
            break
        if b == SYNC:
            break
    ser.reset_input_buffer()
    ser.reset_output_buffer()


def send_cmd(ser, data):
    """Send a command: wait sync, write data, wait ACK."""
    cmd_byte = data[0]
    log(5, "send command 0x%02X len=%d\n" % (cmd_byte, len(data)))

    if cmd_byte != 0x07:
        if not wait_sync(ser, 1):
            return False

    ser.write(data)

    # Wait for ACK, retrying on SYNC chars
    for _ in range(5000):
        b = read_byte(ser, timeout_ms=2000)
        if b == ACK:
            return True
        if b == SYNC:
            continue
        if b != -1:
            log(0, "[send_cmd] error response: 0x%02X\n" % b)
            return False
        time.sleep(0.001)
    log(0, "[send_cmd] timeout waiting for ACK\n")
    return False


def verify_cmd(ser, data):
    """Send a verify command and read back 4-byte checksum."""
    if not wait_sync(ser, 1):
        log(0, "[verify_cmd] sync error\n")
        return None

    ser.write(data)

    # Response: [0x27, u32_checksum_le]
    i = 0
    result = bytearray(4)
    for _ in range(5000):
        b = read_byte(ser, timeout_ms=2000)
        if b == -1:
            time.sleep(0.001)
            continue
        if i == 0 and b == SYNC:
            continue
        if i == 0 and b != 0x27:
            return None
        if i > 0:
            result[i - 1] = b
        i += 1
        if i == 5:
            break
        time.sleep(0.001)

    if i != 5:
        return None
    return struct.unpack("<I", result)[0]


# ---------------------------------------------------------------------------
# Baud rate negotiation
# ---------------------------------------------------------------------------

def find_max_speed(ser):
    """Probe the serial port for the highest supported baud rate."""
    for speed, sub_cmd in SPEED_TABLE:
        try:
            ser.baudrate = speed
            # Workaround: skip 1000000 (unreliable on CP2102)
            if speed == 1000000:
                continue
            log(5, "check speed %d ... supported.\n" % speed)
            return speed, sub_cmd
        except (serial.SerialException, OSError, ValueError):
            log(5, "check speed %d ... not supported.\n" % speed)
            continue
    return 115200, 0x0C


def get_speed_subcmd(speed):
    """Look up the sub-command byte for a given baud rate."""
    for s, cmd in SPEED_TABLE:
        if s == speed:
            return cmd
    return 0x0C  # default 115200


def set_max_speed(ser, speed):
    """Negotiate baud rate change with the device."""
    sub_cmd = get_speed_subcmd(speed)

    if speed != 115200:
        if not wait_sync(ser, 1):
            return False
        send_cmd(ser, bytes([0x05, sub_cmd]))
        ser.baudrate = speed

    if not send_cmd(ser, bytes([0x07])):
        return False
    if not wait_sync(ser, 1):
        return False
    return True


# ---------------------------------------------------------------------------
# File loading and checksum
# ---------------------------------------------------------------------------

def file_checksum(data):
    """Compute uint32 word-sum checksum matching the C++ implementation."""
    checksum = 0
    n_words = len(data) // 4
    remainder = len(data) % 4

    for i in range(n_words):
        word = struct.unpack_from("<I", data, i * 4)[0]
        checksum = (checksum + word) & 0xFFFFFFFF

    if remainder > 0:
        last_word = struct.unpack_from("<I", data, n_words * 4)[0] if n_words * 4 + 4 <= len(data) + 4 else 0
        # Read remaining bytes
        last_bytes = data[n_words * 4:]
        padded = last_bytes + b'\x00' * (4 - len(last_bytes))
        last_word = struct.unpack("<I", padded)[0]
        mask = (1 << (remainder * 8)) - 1
        checksum = (checksum + (last_word & mask)) & 0xFFFFFFFF

    return checksum


def load_bin_file(filepath):
    """Load a binary file and return its contents."""
    with open(filepath, "rb") as f:
        return f.read()


# ---------------------------------------------------------------------------
# Flash write / erase / verify
# ---------------------------------------------------------------------------

def uint8_checksum(data):
    """Compute single-byte checksum: 0xFF + sum of all bytes, truncated to u8."""
    s = 0xFF
    for b in data:
        s = (s + b) & 0xFF
    return s


def write_block(ser, addr, data, show_progress=False, seq_id=1):
    """Write data to device in 1KB chunks."""
    n_chunks = (len(data) + 1023) // 1024

    log(3, "write addr 0x%X size 0x%X\n" % (addr, len(data)))

    # Pad data to multiple of 1024
    padded = data + b'\xFF' * (n_chunks * 1024 - len(data))

    # Build all packets first (speed optimization from C++ SPEED_UP)
    packets = []
    for i in range(n_chunks):
        chunk_addr = addr + i * 1024
        pkt = bytearray(1032)
        pkt[0] = 0x02
        pkt[1] = seq_id & 0xFF
        pkt[2] = (~seq_id) & 0xFF
        struct.pack_into("<I", pkt, 3, chunk_addr)
        pkt[7:7 + 1024] = padded[i * 1024:(i + 1) * 1024]
        pkt[1031] = uint8_checksum(pkt[:1031])
        packets.append(bytes(pkt))
        seq_id = (seq_id + 1) & 0xFF

    if not wait_sync(ser, 1):
        log(0, "[write_block] sync error\n")
        return False, seq_id

    tick = 0
    for pkt in packets:
        ser.write(pkt)
        if not wait_ack(ser):
            log(0, "[write_block] ACK error\n")
            return False, seq_id
        if show_progress and tick == 0:
            log(0, "#")
        tick = (tick + 1) % 8

    if show_progress:
        log(0, "\n")

    return True, seq_id


def erase_block(ser, addr, size):
    """Erase flash sectors at given address."""
    n_sectors = (size + 4095) // 4096
    log(0, "erase addr: 0x%07X, size:%dKB.\n" % (addr, size // 1024))

    cmd = bytearray(6)
    cmd[0] = 0x17
    struct.pack_into("<I", cmd, 1, addr)
    # The C++ packs num_sectors as u32 at offset 4, but only sends 6 bytes total
    # which means it sends addr[4 bytes] + lower 2 bytes of sector count
    cmd[4] = n_sectors & 0xFF
    cmd[5] = (n_sectors >> 8) & 0xFF

    return send_cmd(ser, cmd)


# ---------------------------------------------------------------------------
# Main flash procedure
# ---------------------------------------------------------------------------

def program_spi_flash(ser, speed, flash_data, flash_map_info, flashloader_data):
    """Execute the full flash programming sequence."""

    flush_serial(ser)
    log(0, "**")

    # Wait for 2 sync chars confirming device is in flash mode
    if not wait_sync(ser, 2):
        log(0, "\nerror: Enter Uart Download Mode\n")
        log(0, "Make sure the board is in download mode.\n")
        return False
    log(0, "\n")

    # Negotiate baud rate
    log(0, "set baudrate to %d.\n" % speed)
    if not set_max_speed(ser, speed):
        log(0, "failed to set baudrate.\n")
        return False

    # Upload flashloader to RAM
    log(0, "upload flash download bootloader to ram.\n")
    ok, _ = write_block(ser, FLASHLOADER_ADDR, flashloader_data)
    if not ok:
        log(0, "flash bootloader upload failed.\n")
        return False

    # Execute flashloader
    if not send_cmd(ser, bytes([0x04])):
        return False

    # Re-init at 115200
    log(0, "re-init and set baudrate to 115200.\n")
    ser.baudrate = 115200

    if not wait_sync(ser, 2):
        log(0, "\nsync error after flashloader init.\n")
        return False
    log(0, "\n")

    # SPI flash init command
    if not send_cmd(ser, bytes([0x26, 0x01, 0x01, 0x00])):
        return False

    # Erase all flash regions
    for name, addr, data in flash_map_info:
        if not erase_block(ser, addr, len(data)):
            log(0, "erase block failed.\n")
            return False

    # Change speed for bulk transfer
    if not wait_sync(ser, 1):
        return False
    log(0, "set baudrate to %d.\n" % speed)
    if not set_max_speed(ser, speed):
        return False

    # Write all flash images (seq_id must be continuous across all writes)
    seq_id = 1
    for i, (name, addr, data) in enumerate(flash_map_info):
        show_bar = (i == len(flash_map_info) - 1)  # progress for app image
        ok, seq_id = write_block(ser, addr, data, show_progress=show_bar,
                                 seq_id=seq_id)
        if not ok:
            log(0, "%s write failed.\n" % name)
            return False
        log(0, "%s has been sent successfully.\n" % name.replace(".bin", ""))

    # Execute
    if not send_cmd(ser, bytes([0x04])):
        return False

    # Re-init for verification
    ser.baudrate = 115200
    flush_serial(ser)

    if not wait_sync(ser, 1):
        log(0, "sync error before verify.\n")
        return False

    # Verify checksums
    log(0, "verifying km0 km4 and app blocks....")
    for name, addr, data in flash_map_info:
        # C++ layout: buf[0]=0x27, *(u32*)&buf[1]=addr, *(u32*)&buf[4]=size
        # The writes overlap at byte 4 (addr byte3 overwritten by size byte0).
        # Only 7 bytes are sent. Replicate this exactly:
        cmd = bytearray(8)
        cmd[0] = 0x27
        struct.pack_into("<I", cmd, 1, addr & ~0x8000000)
        struct.pack_into("<I", cmd, 4, len(data))

        expected = file_checksum(data)
        result = verify_cmd(ser, bytes(cmd[:7]))
        if result is None:
            log(0, "verify failed for %s\n" % name)
            return False
        if result != expected:
            log(0, "checksum mismatch for %s: got 0x%08X, expected 0x%08X\n" % (
                name, result, expected))
            return False

    log(0, "ok.\n")
    return True


def flash_image(port, tools_dir, baudrate=0, auto_mode=True, verbose=0):
    """Main entry point: open serial port and flash all images."""
    global VERBOSE
    VERBOSE = verbose

    # Load binary files from tools_dir
    flashloader_path = os.path.join(tools_dir, "imgtool_flashloader_amebad.bin")
    if not os.path.isfile(flashloader_path):
        log(0, "Error: flashloader not found at %s\n" % flashloader_path)
        return False

    flashloader_data = load_bin_file(flashloader_path)

    flash_map_info = []
    for name, addr in FLASH_MAP:
        filepath = os.path.join(tools_dir, name)
        if not os.path.isfile(filepath):
            log(0, "Error: %s not found at %s\n" % (name, filepath))
            return False
        data = load_bin_file(filepath)
        flash_map_info.append((name, addr, data))

    # Open serial port
    try:
        ser = serial.Serial(
            port=port,
            baudrate=115200,
            bytesize=serial.EIGHTBITS,
            parity=serial.PARITY_NONE,
            stopbits=serial.STOPBITS_ONE,
            timeout=2.0,
        )
    except serial.SerialException as e:
        log(0, "Error opening serial port %s: %s\n" % (port, str(e)))
        return False

    # Find max speed if not specified
    if baudrate:
        max_speed = baudrate
    else:
        max_speed, _ = find_max_speed(ser)
        ser.baudrate = 115200

    if max_speed < 115200:
        max_speed = 115200

    log(0, "set baudrate to 115200.\n")
    ser.baudrate = 115200

    # Auto-reset into download mode
    if auto_mode:
        log(0, "enter download flash mode.\n")
        enter_download_mode(ser)
    else:
        log(0, "Please enter the upload mode (wait 5s)\n")
        for i in range(5, 0, -1):
            time.sleep(1)
            log(0, "    0%d\n" % i)

    # Flash
    status = program_spi_flash(ser, max_speed, None, flash_map_info,
                               flashloader_data)

    # Reset to normal boot
    if auto_mode:
        log(0, "hard resetting via RTS pin..\n")
        reset_to_boot(ser)

    ser.close()

    if status:
        log(0, "All images are sent successfully!\n")
    else:
        log(0, "Upload failed.\n")
    return status


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------

def main():
    parser = argparse.ArgumentParser(
        description="AmebaD UART flash uploader")
    parser.add_argument("tools_dir",
                        help="Path to ameba_d_tools directory")
    parser.add_argument("port",
                        help="Serial port (e.g. COM31, /dev/ttyUSB0)")
    parser.add_argument("--baudrate", type=int, default=0,
                        help="Force specific baud rate")
    parser.add_argument("--auto", action="store_true", default=False,
                        help="Auto-reset via DTR/RTS")
    parser.add_argument("--verbose", type=int, default=0,
                        help="Verbosity level (0-7)")

    # Also accept positional args for backward compat with platform.txt format:
    # upload_tool <tools_dir> <port> <board> <auto_mode> <erase_flash> <speed>
    args, extra = parser.parse_known_args()

    auto_mode = args.auto
    baudrate = args.baudrate

    # Parse legacy positional args if present
    if extra:
        # extra[0] = board name (ignored)
        if len(extra) >= 2 and extra[1] in ("Enable", "Disable"):
            auto_mode = extra[1] == "Enable"
        # extra[2] = erase_flash (ignored for now)
        if len(extra) >= 4:
            try:
                baudrate = int(extra[3])
            except ValueError:
                pass

    sys.exit(0 if flash_image(
        args.port, args.tools_dir, baudrate, auto_mode, args.verbose
    ) else 1)


if __name__ == "__main__":
    main()
