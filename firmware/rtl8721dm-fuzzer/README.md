# RTL8721DM Display Init Fuzzer

This project brute-forces the undocumented dot-matrix display controller initialization sequence for the Petlibro feeder. It generates a sweep of 8-bit commands using a custom 4-pin bit-banged interface.

### Wiring (Update in `src/main.cpp`)
- **SDA1**: PA_12
- **SDA2**: PA_13
- **SCL**: PA_14
- **PWM/OE**: PA_15

### Usage
1. Make sure your path to the Ameba Arduino SDK is correct in `platformio.ini`.
2. Build: `pio run`
3. Flash via Pico bridge: `pio run -t upload --upload-port COM<bridge-CDC0>`
4. Connect via terminal: `pio device monitor -b 115200 -p COM<bridge-CDC0>`
