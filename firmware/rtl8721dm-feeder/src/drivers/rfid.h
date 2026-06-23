#pragma once
#include <Arduino.h>

// ===========================================================================
//  JY-L601D RFID reader — Modbus RTU master on UART3 (PA_26 TX / PA_25 RX),
//  19200 baud, slave 0x03. Tag-ready IRQ on PA_16 (active-low held level:
//  HIGH = no tag, LOW = tag in field). Protocol fully decoded in
//  Docs/RFID_PROTOCOL.md; CRC/framing ported from the proven Pico bridge.
//
//  ASYMMETRIC PARITY: the module accepts 8O1 (odd) commands and replies 8E1
//  (even). We configure the single UART 8O1 so commands are correct; the
//  Realtek serial_getc returns RX data regardless of the parity-error flag, so
//  the even-parity replies' data bytes come through and Modbus CRC guards
//  integrity — exactly how the stock master works.
//
//  Power the module first via Aw9523::rfidPower(true)  (expander P0_6 PWEN).
// ===========================================================================
namespace Rfid {
    struct Tag {
        bool     valid;
        uint16_t country;        // reg 0x000E
        uint64_t national;       // 38-bit, regs 0x000F..0x0011[hi]
        uint8_t  quality;        // low byte of 0x0011 (per-read, not part of ID)
        char     id[16];         // "130023370514455" + NUL (3-digit country + 12-digit national)
    };

    void begin();                                 // open UART3 (8O1) + PA_16 input
    bool tagPresentIRQ();                          // PA_16 LOW == tag present

    // Read Holding Registers (fn 0x03). Returns reg count, or: 0 = timeout/silent,
    // -1000 = bad frame (len/CRC), negative-small (-1..-255) = Modbus exception code.
    int  readRegs(uint16_t addr, uint16_t qty, uint16_t* out, size_t cap);
    // Write Single Register (fn 0x06). True on matching echo ack.
    bool writeReg(uint16_t addr, uint16_t value);
    // Read 4 regs @ 0x000E and decode the FDX-B tag. False on no/short reply.
    bool readTag(Tag& t);
}

void rfidInit();   // open the link + register harness commands/state
