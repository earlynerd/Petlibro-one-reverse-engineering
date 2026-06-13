/*
 * Custom TinyUSB configuration for the Petlibro RTL8721DM bridge.
 *
 * We expose two USB CDC ports:
 *   CDC0 = transparent RTL flash/console bridge
 *   CDC1 = control console + RFID snoop
 *
 * The arduino-pico core's bundled include/tusb_config.h hard-pins CFG_TUD_CDC
 * to 1 (plain #define, no guard), which would limit us to one port. We select
 * THIS file for every TinyUSB translation unit via
 *   -DCFG_TUSB_CONFIG_FILE=\"custom_tusb_config.h\"   (with -Iinclude/)
 * so the CDC count is consistent everywhere.
 *
 * This is a self-contained copy of the core's Adafruit TinyUSB rp2040 config
 * (src/arduino/ports/rp2040/tusb_config_rp2040.h) with the ONLY change being
 * CFG_TUD_CDC = 2. It is mirrored rather than #included because the core's own
 * translation units (sdkoverride/*.c, main.cpp) do not have the Adafruit
 * library's src/ dir on their include path. If you bump the arduino-pico core,
 * re-diff against that file and re-apply CFG_TUD_CDC = 2.
 *
 * Original MIT License (c) 2018 hathach for Adafruit.
 */

#ifndef _PETLIBRO_TUSB_CONFIG_H_
#define _PETLIBRO_TUSB_CONFIG_H_

#ifdef __cplusplus
extern "C" {
#endif

//--------------------------------------------------------------------
// COMMON CONFIGURATION
//--------------------------------------------------------------------

#ifdef USE_TINYUSB_HOST
// native as host
#define CFG_TUD_ENABLED 0
#define CFG_TUH_ENABLED 1
#define CFG_TUH_RPI_PIO_USB 0

#else
// native as device
#define CFG_TUD_ENABLED 1

#if __has_include("pio_usb.h")
// Enable host stack with pio-usb if Pico-PIO-USB library is available
#define CFG_TUH_ENABLED 1
#define CFG_TUH_RPI_PIO_USB 1

#else
// Otherwise enable host controller with MAX3421E
#define CFG_TUH_ENABLED 1
#define CFG_TUH_MAX3421 1

#endif // pio_usb.h
#endif // USE_TINYUSB_HOST

#ifndef CFG_TUSB_MCU
#define CFG_TUSB_MCU OPT_MCU_RP2040
#endif

#ifndef CFG_TUSB_OS
#define CFG_TUSB_OS OPT_OS_PICO
#endif

#ifndef CFG_TUSB_DEBUG
#define CFG_TUSB_DEBUG 0
#endif

#define CFG_TUSB_MEM_SECTION
#define CFG_TUSB_MEM_ALIGN TU_ATTR_ALIGNED(4)

//--------------------------------------------------------------------
// Device Configuration
//--------------------------------------------------------------------

#define CFG_TUD_ENDPOINT0_SIZE 64

#ifndef CFG_TUD_CDC
#define CFG_TUD_CDC 2   // <-- bridge port + control/snoop port (was 1 in core)
#endif
#ifndef CFG_TUD_MSC
#define CFG_TUD_MSC 1
#endif
#ifndef CFG_TUD_HID
#define CFG_TUD_HID 2
#endif
#ifndef CFG_TUD_MIDI
#define CFG_TUD_MIDI 1
#endif
#ifndef CFG_TUD_VENDOR
#define CFG_TUD_VENDOR 1
#endif
#ifndef CFG_TUD_VIDEO
#define CFG_TUD_VIDEO 1 // number of video control interfaces
#endif
#ifndef CFG_TUD_VIDEO_STREAMING
#define CFG_TUD_VIDEO_STREAMING 1 // number of video streaming interfaces
#endif

// video streaming endpoint buffer size
#define CFG_TUD_VIDEO_STREAMING_EP_BUFSIZE 256

// CDC FIFO size of TX and RX. TX bumped to 2048 so the control port can hold
// several snoop frames (reader + host both print ~10Hz); at 256 the host frame
// got dropped by emitFrame's non-blocking guard whenever the terminal lagged.
#define CFG_TUD_CDC_RX_BUFSIZE 256
#define CFG_TUD_CDC_TX_BUFSIZE 2048

// MSC Buffer size of Device Mass storage
#define CFG_TUD_MSC_EP_BUFSIZE 512

// HID buffer size Should be sufficient to hold ID (if any) + Data
#define CFG_TUD_HID_EP_BUFSIZE 64

// MIDI FIFO size of TX and RX
#define CFG_TUD_MIDI_RX_BUFSIZE 64
#define CFG_TUD_MIDI_TX_BUFSIZE 64

// Vendor FIFO size of TX and RX
#define CFG_TUD_VENDOR_RX_BUFSIZE 64
#define CFG_TUD_VENDOR_TX_BUFSIZE 64

//--------------------------------------------------------------------
// Host Configuration
//--------------------------------------------------------------------

// Size of buffer to hold descriptors and other data used for enumeration
#define CFG_TUH_ENUMERATION_BUFSIZE 256

// Number of hub devices
#define CFG_TUH_HUB 1

// max device support (excluding hub device): 1 hub typically has 4 ports
#define CFG_TUH_DEVICE_MAX (3 * CFG_TUH_HUB + 1)

// Number of mass storage
#define CFG_TUH_MSC 1

// Number of HIDs
// typical keyboard + mouse device can have 3,4 HID interfaces
#define CFG_TUH_HID (3 * CFG_TUH_DEVICE_MAX)

// Number of CDC interfaces
// FTDI and CP210x are not part of CDC class, only to re-use CDC driver API
#define CFG_TUH_CDC 1
#define CFG_TUH_CDC_FTDI 1
#define CFG_TUH_CDC_CP210X 1
#define CFG_TUH_CDC_CH34X 1

// RX & TX fifo size
#define CFG_TUH_CDC_RX_BUFSIZE 128
#define CFG_TUH_CDC_TX_BUFSIZE 128

// Set Line Control state on enumeration/mounted:
// DTR ( bit 0), RTS (bit 1)
#define CFG_TUH_CDC_LINE_CONTROL_ON_ENUM 0x03

// Set Line Coding on enumeration/mounted, value for cdc_line_coding_t
// bit rate = 115200, 1 stop bit, no parity, 8 bit data width
// This need Pico-PIO-USB at least 0.5.1
#define CFG_TUH_CDC_LINE_CODING_ON_ENUM                                        \
  { 115200, CDC_LINE_CODING_STOP_BITS_1, CDC_LINE_CODING_PARITY_NONE, 8 }

#ifdef __cplusplus
}
#endif

#endif /* _PETLIBRO_TUSB_CONFIG_H_ */
