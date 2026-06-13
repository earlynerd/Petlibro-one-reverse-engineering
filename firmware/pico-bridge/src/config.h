#pragma once
#include <Arduino.h>

// ===========================================================================
//  Petlibro RTL8721DM programming / RFID-snoop bridge
//  Pin & parameter configuration
// ===========================================================================
//
//  Target : Raspberry Pi Pico (RP2040) installed inside the feeder housing.
//  Wired to the RTL8721DM mainboard "ISP" header, and tapped onto the
//  JY-L601D RFID module's UART lines.
//
//  >>> VERIFY THESE AGAINST THE ACTUAL "ISP" HEADER SILKSCREEN BEFORE WIRING <<<
//      The ISP pad functions/order have NOT yet been confirmed for this board.
//      See README.md "Wiring" and update the pins below to match.
// ===========================================================================

// ---- RTL8721DM <-> Pico bridge UART (PIO UART, not hardware UART0) ----------
//   Pico GP0 (TX) -> RTL ISP RX   (data into the RTL)
//   Pico GP1 (RX) <- RTL ISP TX   (log / data out of the RTL) -- this is also
//                    the UART_DOWNLOAD strap line. We use a PIO UART (SerialPIO)
//                    rather than hardware UART0 so the pin can be reclaimed for
//                    strapping and handed back cleanly at runtime; the hardware
//                    UART's begin/end save+restore its pin function and desync
//                    when we GPIO it in between. Any GPIOs work for SerialPIO.
#define RTL_UART_TX_PIN     0
#define RTL_UART_RX_PIN     1

// Baud used before the host opens COM-A. The Realtek AmebaD Image Tool's
// download default is 1500000; the ROM log prints at 115200. The bridge
// auto-tracks whatever baud the host opens COM-A at, so this is only the
// power-on default.
#define RTL_UART_DEFAULT_BAUD   1500000UL

// ---- RTL8721DM reset (CHIP_EN) + UART download strap -----------------------
//   Per UM0401 RTL872xD datasheet (QFN68 = RTL8721DM):
//     * CHIP_EN ......... QFN68 pin 6.  1 = enable, 0 = shutdown/reset.
//                         DEDICATED reset, NOT on the ISP header. Run one wire
//                         from RTL_RST_PIN to chip pin 6 for Pico-driven reset
//                         and hands-free download entry.
//     * UART_DOWNLOAD ... PA[7] = QFN68 pin 7.  Low at boot = download from
//                         UART; high = boot from flash (internal pull-up).
//                         PA[7] is ALSO the LOGUART TX, i.e. the ISP "TX" data
//                         line already wired to the Pico -- so the strap needs
//                         NO extra wire. We hold that data line low across a
//                         reset, then release it back to the UART.
//
//   WARNING: never pull PA[27]/NORMAL_MODE_SEL (QFN68 pin 33) low at power-on
//            -> boot fails / enters Realtek test mode. Leave pin 33 alone.
//
//   CHIP_EN is driven OPEN-DRAIN (low = reset, hi-Z = let the board pull it
//   high), so an unconnected pin or the board's own drive is never fought.

#define RTL_RST_PIN            2               // -> CHIP_EN (QFN68 pin 6)
#define RTL_RST_ACTIVE_LOW     1               // CHIP_EN low = shutdown/reset

// The download strap is the chip's TX/PA[7] line, which the Pico sees on its
// UART RX pin. If the ISP RX/TX turn out to be swapped, set this to
// RTL_UART_TX_PIN instead (whichever Pico pin lands on chip pin 7 / PA[7]).
#define RTL_DLSTRAP_PIN        RTL_UART_RX_PIN // Pico pin on the PA[7] net
#define RTL_DLSTRAP_ACTIVE_LOW 1               // UART_DOWNLOAD is low-active

#define RTL_RST_PULSE_MS       20              // CHIP_EN reset pulse width
#define RTL_BOOT_SETTLE_MS     10              // strap settle before reset release
#define RTL_DL_STRAP_HOLD_MS   40              // hold strap low past CHIP_EN rise
                                               // + ROM trap sampling, then release

// ---- USB DTR/RTS auto-reset on the bridge port (CDC0) ----------------------
//   When enabled, the flashing host's modem-control lines drive the RTL into
//   download mode automatically, so stock Realtek / community flashers work as
//   a drop-in adapter without the CDC1 `download` command (which still works).
//
//   Behaviour: on the RESET line's assert edge we run the *atomic* reset macro,
//   using the STRAP line as the mode selector -- strap asserted -> enter UART
//   download mode (`enterDownload`), else -> plain reset (run). The strap is
//   only ever driven inside that macro (never held), so there's no contention
//   with the chip's UART and the chip is never left stuck in reset.
//
//   Convention: Realtek flashers typically wire DTR->RESET and RTS->DOWNLOAD.
//   Set RTL_AR_RESET_ON_DTR 0 for the esptool convention (RTS->reset/DTR->strap).
//   Flip RTL_AR_INVERT if your tool assumes inverting transistors (host
//   asserting a line should mean "release" rather than "assert").
//   Expected host order: assert the strap line, then pulse the reset line.
//
//   Note: opening the bridge port in a terminal asserts DTR, so it will reset
//   the RTL once (esptool-like) -- harmless; set RTL_DTR_RTS_AUTORESET 0 if you
//   want the bridge to be fully passive for log watching.
// NOTE: set to 0 during bring-up / when using the manual `download` command.
// Flashers like RTL872xDx_Flasher drive DTR/RTS themselves, and leaving this on
// lets our edge-trigger fire mid-session and knock the chip out of download.
// Re-enable (1) only if you want a fully hands-off drop-in adapter.
#define RTL_DTR_RTS_AUTORESET   0
#define RTL_AR_RESET_ON_DTR     1     // 1: DTR=reset, RTS=strap; 0: swapped
#define RTL_AR_INVERT           0     // 1: invert the asserted sense of both lines
#define RTL_AR_DEBOUNCE_MS      120   // collapse a burst of edges into one action

// ---- JY-L601D RFID snoop (PIO RX-only UARTs) -------------------------------
//   Tap each of the two UART lines between the RFID module and the RTL.
//   RX-only: wire the Pico pin straight onto the line you want to listen on.
//     READER line = data the RFID module transmits (slave replies / tag reads)
//     HOST   line = data the RTL transmits to the module (master commands)
//                   Set RFID_HOST_RX_PIN to -1 if the link is one-wire.
//
//   WIRING CORRECTED 2026-06-13 (bench, logic analyzer): the physical taps are
//   the OPPOSITE of the original guess -- GP4 sits on the RTL's TX (master
//   commands) and GP5 on the module's TX (slave replies). The pins below are
//   set to match the wiring, so "RFID-R" really tags the module and "RFID-H"
//   the RTL. If you re-wire, swap these back.
#define RFID_READER_RX_PIN  5        // module TX (slave replies)
#define RFID_HOST_RX_PIN    4        // RTL TX (master commands; -1 to disable)

// CONFIRMED 2026-06-13: the JY-L601D <-> RTL link is Modbus RTU @ 19200,
// slave addr 0x03 -- but parity is ASYMMETRIC (verified on the logic analyzer,
// see modbus_parity.png / modbus_parity_RFID.png):
//     RTL  -> module (commands) = 8O1 (odd)
//     module -> RTL  (replies)  = 8E1 (even)
// The RTL master never noticed because it doesn't validate RX parity; our snoop
// does (SerialPIO silently drops parity-mismatched bytes), so each tap MUST be
// told its own framing or the mismatched direction goes 100% silent. Baud is
// the same both ways. (Earlier 9600/8N1 guess undersampled it into garbage.)
#define RFID_SNOOP_BAUD     19200UL
#define RFID_READER_FORMAT  SERIAL_8E1   // module TX (replies)  -- even parity
#define RFID_HOST_FORMAT    SERIAL_8O1   // RTL TX (commands)    -- odd parity

// Inter-byte idle gap (microseconds) that ends a frame for the snoop logger.
// ~3 character times at the snoop baud is a sensible default.
#define RFID_FRAME_GAP_US   3000UL

// ---- Status LED ------------------------------------------------------------
#define STATUS_LED_PIN      LED_BUILTIN
