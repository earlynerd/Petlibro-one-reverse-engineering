#include <Arduino.h>
#include "config.h"
#include "rtl_bridge.h"
#include "rfid_snoop.h"
#include "rfid_master.h"
#include "control_console.h"

#ifdef USE_TINYUSB
#include <Adafruit_TinyUSB.h>
// Second USB CDC interface. CDC0 (`Serial`) is the transparent RTL bridge;
// this one carries the control console + RFID snoop output.
Adafruit_USBD_CDC USBControl;
#else
#error "Build with the Adafruit TinyUSB stack (-DUSE_TINYUSB) — the dual CDC ports require it."
#endif

void setup() {
  pinMode(STATUS_LED_PIN, OUTPUT);
  digitalWrite(STATUS_LED_PIN, HIGH);

  // Register the control CDC before enumeration so it shows up as its own
  // COM port alongside the bridge port.
  USBControl.setStringDescriptor("Petlibro Control + RFID Snoop");
  USBControl.begin(115200);

  Serial.begin(115200);          // CDC0 — transparent RTL flash/console bridge

  Rtl.begin();
  Snoop.begin();
  // RfidMaster is constructed but NOT begun here: it would claim GP4/GP5 that
  // the snoop already owns. It is activated on demand by `master on`, which
  // first suspends the snoop and holds the RTL in reset.
  Rtl.setMonitor(&USBControl);          // bridge tap output goes to the control port
  Console.begin(&USBControl, &Rtl, &Snoop, &Master);

  digitalWrite(STATUS_LED_PIN, LOW);
}

void loop() {
  Rtl.pump();          // transparent USB<->RTL bridge (CDC0)
  Console.service();   // commands + live RFID snoop (CDC1)
}
