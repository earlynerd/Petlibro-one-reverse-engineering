/*
 * RTL8721DM "hello world" — first custom firmware on the Petlibro feeder SoC.
 *
 * Goal: prove the toolchain → build → flash → boot → serial loop end-to-end on
 * the real chip, before any real de-cloud work.
 *
 * `Serial` on AmebaD maps to the LOGUART (PA[7]/PA[8]) — the *same* two lines
 * the in-housing Pico bridge taps as ISP TX/RX. So this banner comes straight
 * back out the bridge's CDC0 port: flash through CDC0, then open a monitor on
 * CDC0 and you should see it boot and tick.
 *
 * The LED_BUILTIN toggle is harmless but probably invisible on the feeder —
 * that pin is the SparkFun dev board's LED, not necessarily wired on the
 * feeder mainboard. The serial output is the real proof of life.
 */
#include <Arduino.h>

void setup() {
    Serial.begin(115200);
    delay(50);
    Serial.println();
    Serial.println("=====================================================");
    Serial.println(" hello from RTL8721DM — custom firmware is running!");
    Serial.println(" Petlibro feeder SoC, built via PlatformIO + AmebaD");
    Serial.println("=====================================================");

    pinMode(LED_BUILTIN, OUTPUT);
}

void loop() {
    static uint32_t n = 0;

    digitalWrite(LED_BUILTIN, (n & 1) ? HIGH : LOW);

    Serial.print("alive: tick ");
    Serial.print(n);
    Serial.print("  millis=");
    Serial.println(millis());

    n++;
    delay(1000);
}
