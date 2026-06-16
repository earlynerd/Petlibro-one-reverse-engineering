#!/usr/bin/env python3
"""
Follow-up: PWEN=1 powered the rail but the module stays silent. Try the boot
sequence the stock RTL would do -- bump the reply timeout, send the boot-enable
register write (0x0000 = 0x0002), and power-cycle PWEN for a clean module boot --
then re-read. COM40 = control console.
"""
import sys
import time

import serial

CTRL, BAUD = "COM40", 115200


def ccmd(ser, c, wait=1.0):
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
        ccmd(ser, "pwen 1")
        ccmd(ser, "master on", 1.2)
        show("master timeout 200", ccmd(ser, "master timeout 200"))
        show("master init (0x0000=0x0002, boot enable)", ccmd(ser, "master init", 1.2))
        time.sleep(0.5)
        show("read 0000 1", ccmd(ser, "master read 0000 1", 1.5))
        show("read 0040 4", ccmd(ser, "master read 0040 4", 1.5))

        # clean power-cycle of the module via PWEN, then re-init + read
        ccmd(ser, "pwen 0"); time.sleep(0.5)
        ccmd(ser, "pwen 1"); time.sleep(1.3)
        show("after PWEN power-cycle: master init", ccmd(ser, "master init", 1.2))
        time.sleep(0.4)
        show("read 0040 4", ccmd(ser, "master read 0040 4", 1.5))
        show("dump 0000..0050", ccmd(ser, "master dump 0000 0050 8", 5.0))
    finally:
        ser.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
