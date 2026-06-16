#!/usr/bin/env python3
"""
Separate "is the module POWERED" from "is the Modbus link working", without
relying on Modbus. A powered module drives its UART TX to idle HIGH, so the
snoop's reader tap (GP5 = module TX) sees NO start bits -> its byte counter
stops growing. An unpowered/floating line produces noise -> the counter climbs.

Hold the RTL in reset (its PWEN pin hi-Z, no contention), run the snoop, and
compare the reader-tap byte rate at PWEN=0 vs PWEN=1.  COM40 = control console.
"""
import re
import sys
import time

import serial

CTRL, BAUD = "COM40", 115200
RE_BYTES = re.compile(r"reader.*?/\s*(\d+)\s*bytes", re.I)


def ccmd(ser, c, wait=1.0):
    ser.reset_input_buffer(); ser.write((c + "\r\n").encode()); ser.flush()
    end, buf = time.time() + wait, ""
    while time.time() < end:
        d = ser.read(512).decode("latin1", "replace")
        if d:
            buf += d
            end = time.time() + 0.35
    return buf


def reader_bytes(ser):
    m = RE_BYTES.search(ccmd(ser, "status", 1.2))
    return int(m.group(1)) if m else -1


def main():
    try:
        ser = serial.Serial(CTRL, BAUD, timeout=0.2)
    except Exception as e:
        print(f"COM40 busy/locked ({e}) — release it and rerun"); return 1
    try:
        time.sleep(0.4); ser.reset_input_buffer()
        ccmd(ser, "master off", 1.2)     # leave master so the snoop owns GP4/GP5
        ccmd(ser, "rst on")              # hold RTL in reset: its PWEN pin goes hi-Z
        ccmd(ser, "snoop on")
        for lvl in (0, 1):
            ccmd(ser, f"pwen {lvl}")
            time.sleep(0.6)
            b0 = reader_bytes(ser)
            time.sleep(2.5)
            b1 = reader_bytes(ser)
            print(f"PWEN={lvl}: reader-tap bytes {b0} -> {b1}   (+{b1 - b0} in 2.5s)  "
                  f"=> line {'IDLE/HIGH (module powered?)' if (b1 - b0) < 30 else 'NOISY (floating / not powered)'}",
                  flush=True)
        # restore a sane state: stop driving PWEN, release reset
        ccmd(ser, "pwen off")
        ccmd(ser, "rst off")
        print("\n[restored: PWEN released, RTL out of reset, snoop on]")
    finally:
        ser.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
