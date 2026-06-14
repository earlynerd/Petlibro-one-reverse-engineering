#pragma once
#include <Arduino.h>
#include <SerialPIO.h>

// Active Modbus master for the JY-L601D RFID module. Where RfidSnoop only
// *listens* on the two UART lines, RfidMaster *drives* the command line and
// talks to the module itself -- so the Pico can read or write any register
// without the RTL in the loop.
//
// Prerequisites the caller must satisfy before begin():
//   * the RTL host is held in CHIP_EN reset (so it stops driving the command
//     net and stops being the Modbus master), and
//   * RfidSnoop is suspended (so GP4/GP5 are free to be reclaimed).
// The console's `master on` macro does both; `master off` reverses them.
//
// Asymmetric parity (the module's quirk) is preserved from the master side:
// commands go out 8O1 (the module accepts odd parity), replies come back 8E1.
// Because a single SerialPIO applies one format to both directions, TX and RX
// are two separate instances -- TX-only on GP4, RX-only on GP5. They sit on
// different physical nets (command net vs reply net), so we never hear our own
// transmissions echoed back; the link is effectively full-duplex from here.
class RfidMaster {
public:
  RfidMaster();

  bool active() const { return _active; }

  // Claim GP4 (TX, 8O1) + GP5 (RX, 8E1) at the RFID link baud. No-op if the TX
  // pin is disabled in config (RFID_HOST_RX_PIN == -1). Also forces GP4 back to
  // a high-Z input in end() so the line is released before the host un-resets.
  bool begin();
  void end();

  // Return codes shared by readRegs(): 0 = no reply (timeout), kBadFrame = a
  // reply arrived but failed length/CRC, negative small values (-1..-255) = a
  // Modbus exception code (negated). Positive = registers read.
  static constexpr int    kTimeout      = 0;
  static constexpr int    kBadFrame     = -1000;
  static constexpr size_t kMaxBlockRegs = 32;   // dump() block-read buffer depth
                                                // (32 regs reply = 69 B, fits recv buf;
                                                //  enough to bridge the 0x20->0x40 gap)

  // Read Holding Registers (fn 0x03). On success returns the register count and
  // fills out[0..n) (clamped to outCap). See the return-code constants above.
  int  readRegs(uint16_t addr, uint16_t qty, uint16_t* out, size_t outCap);

  // Write Single Register (fn 0x06). Returns true on a matching echo ack.
  bool writeReg(uint16_t addr, uint16_t value);

  // Raw transaction: send `pdu` (slave-addr .. data) and capture the reply.
  // If appendCrc, the Modbus CRC16 is computed and appended; else `pdu` is sent
  // verbatim (caller supplies the CRC). Returns reply length (incl. CRC) in
  // rsp[], or 0 on timeout. Useful for poking at undocumented function codes.
  int  transact(const uint8_t* pdu, size_t n, bool appendCrc, uint8_t* rsp, size_t rspCap);

  // Sweep [first..last] for register *base addresses* that answer (probed with a
  // minimal qty=1 read). For each base that does, follow up with a longer read
  // (up to blockQty registers) to pull the rest of the block -- the module only
  // recognises block bases and silently ignores reads to mid-block / unknown
  // addresses, so a 1-at-a-time sweep alone never sees the registers behind a
  // base. Prints the full block per base; silent addresses are tallied, not
  // printed. Abortable: any byte arriving on `io` stops the sweep early.
  void dump(Stream& io, uint16_t first, uint16_t last, uint16_t blockQty);

  void     setTimeout(uint32_t ms) { _timeoutMs = ms ? ms : 1; }
  uint32_t timeout() const { return _timeoutMs; }

private:
  void sendBytes(const uint8_t* b, size_t n);   // write + flush on the TX UART
  int  recvFrame(uint8_t* buf, size_t cap);     // idle-gap framed, _timeoutMs to first byte
  void drainRx();                               // discard any stale RX bytes
  // Read the whole block at a recognised base: qty=1 discovery, then -- if the
  // module honoured qty rather than returning the whole block -- grow the
  // request up to maxQty to find the block's true length (handles odd sizes
  // like the 9-register boot block). Returns the register count in out[0..n)
  // (clamped to cap), or a <=0 readRegs status if `addr` is not a base.
  int  readBlock(uint16_t addr, uint16_t maxQty, uint16_t* out, size_t cap);

  SerialPIO _tx;        // TX-only, 8O1 -- commands to the module
  SerialPIO _rx;        // RX-only, 8E1 -- replies from the module
  bool      _active = false;
  uint32_t  _timeoutMs;
};

extern RfidMaster Master;
