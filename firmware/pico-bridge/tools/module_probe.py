#!/usr/bin/env python3
"""
Probe the JY-L601D RFID module directly via the Pico bridge's master mode
(COM40 / CDC1 control port). With a tag on the antenna, a live module returns
the tag ID at register 0x000E. This tells us if the module is powered/alive at
all (the Pico masters the bus with the RTL held in reset).
"""
import sys
import time

import serial

CTRL = "COM40"
BAUD = 115200


def drain(ser, secs):
    end = time.time() + secs
    buf = ""
    while time.time() < end:
        d = ser.read(512).decode("latin1", "replace")
        if d:
            buf += d
    return buf


def send(ser, cmd, wait):
    ser.reset_input_buffer()
    ser.write((cmd + "\r\n").encode())
    ser.flush()
    return drain(ser, wait)


def main():
    try:
        ctrl = serial.Serial(CTRL, BAUD, timeout=0.2)
    except serial.SerialException as e:
        print("ERROR opening COM40 (close any monitor!):", e)
        return 1
    time.sleep(0.3)
    ctrl.reset_input_buffer()

    seq = [
        ("(nudge)", "", 0.5),
        ("master on", "master on", 2.0),
        ("master timeout 20", "master timeout 20", 1.0),
        ("read 0000 q1 (mode)", "master read 0000 1", 2.0),
        ("read 000E q16 (FDX-B tag block)", "master read 000E 16", 2.5),
        ("read 0020 q16 (mirror)", "master read 0020 16", 2.5),
        ("read 0050 q5 (tag record)", "master read 0050 5", 2.0),
        ("dump 0..0x40", "master dump 0 0x40", 12.0),
        ("master off", "master off", 2.0),
    ]
    for label, cmd, wait in seq:
        print(f"\n===== {label} =====")
        resp = send(ctrl, cmd, wait).strip()
        print(resp.encode("ascii", "replace").decode("ascii"))

    ctrl.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
