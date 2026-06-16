#!/usr/bin/env python3
"""
Power the RFID module via the new PWEN tap (GP6) and prove it's alive: drive
PWEN high, then enter master mode (Pico becomes the Modbus master, RTL held in
reset) and read known registers. A reply = the module is powered and talking.

  0x0000 = mode/status reg ; 0x0040 = 4-reg ID block (answers with no tag too).

Control console = COM40 (no prompt; fixed-drain reads).
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
    ser = None
    for _ in range(12):                      # Pico re-enumerates after flashing
        try:
            ser = serial.Serial(CTRL, BAUD, timeout=0.2); break
        except Exception:
            time.sleep(1.0)
    if not ser:
        print(f"could not open {CTRL} (still re-enumerating after flash?)"); return 1
    try:
        time.sleep(0.6); ser.reset_input_buffer()
        show("pwen read (resting level of PWEN net)", ccmd(ser, "pwen read"))
        show("pwen 1 -> power the module ON", ccmd(ser, "pwen 1"))
        time.sleep(0.8)                       # let the module boot
        show("master on", ccmd(ser, "master on", 1.5))
        time.sleep(0.3)
        show("master read 0000 1 (mode reg)", ccmd(ser, "master read 0000 1", 1.5))
        show("master read 0040 4 (ID block)", ccmd(ser, "master read 0040 4", 2.0))
        show("status", ccmd(ser, "status", 1.2))
        print("\n(leaving master mode ON + PWEN driven so you can keep probing; "
              "'master off' then 'pwen off' to restore)")
    finally:
        ser.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
