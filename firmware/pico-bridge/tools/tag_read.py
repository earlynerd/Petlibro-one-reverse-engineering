#!/usr/bin/env python3
"""
Read the RFID module over master mode (Pico is the Modbus master; module powered
via AW9523B P0_6, which stays latched). Reads the reader-ID (sanity), the FDX-B
tag block (latches the last tag, per bench note), and the mode/status + tag-gated
blocks -- candidates for a LIVE tag-present signal we can trust for actuation.

  0x0040 x4  reader's own ID (always answers)
  0x0000 x1  mode / status
  0x000E x18 FDX-B tag block (latched)
  0x0050 x5  "tag-gated" block

Run once with the tag ON the antenna, once with it OFF -> whatever changes is the
real presence indicator.  COM40 = control console.
"""
import sys
import time

import serial

CTRL, BAUD = "COM40", 115200


def ccmd(ser, c, wait=2.0):
    ser.reset_input_buffer(); ser.write((c + "\r\n").encode()); ser.flush()
    end, buf = time.time() + wait, ""
    while time.time() < end:
        d = ser.read(512).decode("latin1", "replace")
        if d:
            buf += d
            end = time.time() + 0.35
    return buf.strip()


def show(tag, txt):
    print(f"\n[{tag}]")
    print(txt.encode("ascii", "replace").decode("ascii"))


def main():
    try:
        ser = serial.Serial(CTRL, BAUD, timeout=0.2)
    except Exception as e:
        print(f"COM40 busy/locked ({e}) — release it and rerun"); return 1
    try:
        time.sleep(0.4); ser.reset_input_buffer()
        ccmd(ser, "master on", 1.5)
        show("0x0040 x4  reader ID (sanity)", ccmd(ser, "master read 0040 4"))
        show("0x0000 x1  mode/status",        ccmd(ser, "master read 0000 1"))
        show("0x000E x18 FDX-B tag block",    ccmd(ser, "master read 000E 18", 2.5))
        show("0x0050 x5  tag-gated block",    ccmd(ser, "master read 0050 5", 2.0))
    finally:
        ser.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
