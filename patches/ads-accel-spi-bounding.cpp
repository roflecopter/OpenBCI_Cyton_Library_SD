// ============================================================================
// patches/ads-accel-spi-bounding.cpp  —  REPRODUCIBLE FORK RECORD (Stage A)
// ============================================================================
// Out-of-repo change to: ~/Arduino/libraries/OpenBCI_32bit_Library/OpenBCI_32bit_Library.cpp
// (the OpenBCI core library is forked for this board; this file is the archive of the
//  ADS+accel SPI-bounding change so the fork is reproducible, mirroring patches/sd2card-spirov-fix.cpp.)
//
// WHY (prep.md re-diagnosis 2026-06-30): the intermittent multi-hour MID-RECORDING FREEZE is NOT the
// SD-write SPIROV path (fixed in 3e93cde, still froze at 7.1h with e=0 r=0). The prime suspect is the
// ADS1299 per-sample read: every ADS read/write/command + the LIS3DH accel went through the chipKIT
// DSPI::transfer(), whose internals are UNBOUNDED (while(!SPITBE)/while(!SPIRBF), no timeout). A
// shared-bus stall there spins the per-sample read forever with NO SD error → the frozen board.
//
// FIX (prep.md Decision 2/3): route the WHOLE live SPI surface through the OBCI32_SD fork's bounded
// `spiByteBounded` (CP0-deadline bounded standard single-byte SPI1 transfer; on a stall latches the
// shared sticky `sdSpiFault` + returns 0xFF). The EXISTING top-of-loop `if (sdSpiFault) sdSpiModuleFlush()`
// guard (DefaultBoard.ino) flushes the SPI module next iteration → the bus recovers → a faulted sample
// reads 0xFF (one garbage sample per rare wedge) instead of a frozen board. NET FLASH: -212 B (dedup —
// the shared free function replaces duplicated inlined DSPI::transfer() at 8 sites).
//
// THE EXACT CHANGES (3 edits) — see live file for surrounding context:
//
// (1) Above OpenBCI_32bit_Library::xfer(), add the extern + swap the call:
//     extern uint8_t spiByteBounded(uint8_t);
//     byte OpenBCI_32bit_Library::xfer(byte _data) { byte inByte; inByte = spiByteBounded(_data); return inByte; }
//     // was: inByte = spi.transfer(_data);   (xfer() is the chokepoint for ALL ADS reads/writes/commands)
//
// (2) LIS3DH_read / LIS3DH_write: spi.transfer(x) -> spiByteBounded(x)  (accel shares SPI1; Decision 3)
//
// (3) LIS3DH_read16: spi.transfer -> spiByteBounded AND sequence the two reads into locals (Gemini grill
//     review #1 BLOCKER — C++ leaves `a | (b<<8)` operand order UNSPECIFIED; the LIS3DH auto-increments
//     LSB-then-MSB so inline calls could swap the axis bytes):
//         byte lsb = spiByteBounded(0x00);
//         byte msb = spiByteBounded(0x00);
//         inData = lsb | (msb << 8);
//
// LIVE SNAPSHOT OF THE CHANGED FUNCTIONS (copy from the running fork at record time):
// ----------------------------------------------------------------------------
// Bounded shared SPI byte from the OBCI32_SD fork (Sd2Card.cpp). prep.md Decision 2/3:
// the ADS1299 read path used the UNBOUNDED chipKIT DSPI::transfer() (while(!SPITBE)/
// while(!SPIRBF) with no timeout) — a shared-bus stall there spins the per-sample read
// forever mid-recording with NO SD error (the unobserved multi-hour freeze). spiByteBounded
// is a CP0-deadline-bounded standard single-byte transfer on the SAME SPI1 the ADS uses;
// on a stall it latches the sticky `sdSpiFault` and returns 0xFF instead of hanging. The
// top-of-loop `if (sdSpiFault) sdSpiModuleFlush()` guard (DefaultBoard.ino) then flushes the
// SPI module on the next iteration so the bus recovers — a faulted sample reads as 0xFF
// (one garbage sample per rare fault, acceptable) rather than a frozen board.
extern uint8_t spiByteBounded(uint8_t);

//SPI communication method
byte OpenBCI_32bit_Library::xfer(byte _data)
{
  byte inByte;
  inByte = spiByteBounded(_data);   // was spi.transfer(_data) — bounded, SPIROV-safe, can't infinite-spin
  return inByte;
}

//SPI chip select method
void OpenBCI_32bit_Library::csLow(int SS)

}

// Accel (LIS3DH) SPI — bounded through the shared spiByteBounded too (prep.md Decision 3:
// bound the WHOLE live SPI surface, not just the ADS read; the accel shares SPI1, so a
// wedged accel read would hang the loop the same way). csLow/csHigh already own the CS;
// on a fault the bytes read 0xFF and the top-of-loop guard flushes + recovers.
byte OpenBCI_32bit_Library::LIS3DH_read(byte reg)
{
  reg |= READ_REG;                     // add the READ_REG bit
  csLow(LIS3DH_SS);                    // take spi
  spiByteBounded(reg);                 // send reg to read
  byte inByte = spiByteBounded(0x00);  // retrieve data
  csHigh(LIS3DH_SS);                   // release spi
  return inByte;
}

void OpenBCI_32bit_Library::LIS3DH_write(byte reg, byte value)
{
  csLow(LIS3DH_SS);        // take spi
  spiByteBounded(reg);     // send reg to write
  spiByteBounded(value);   // write value
  csHigh(LIS3DH_SS);       // release spi
}

int OpenBCI_32bit_Library::LIS3DH_read16(byte reg)
{ // use for reading axis data.
  int inData;
  reg |= READ_REG | READ_MULTI;       // add the READ_REG and READ_MULTI bits
  csLow(LIS3DH_SS);                   // take spi
  spiByteBounded(reg);                // send reg to start reading from
  // SEQUENCE the two reads into locals: the LIS3DH auto-increments the address on READ_MULTI
  // (LSB first, then MSB), so byte order is read-ORDER-dependent. C++ leaves the evaluation order
  // of `a | (b << 8)` operands UNSPECIFIED, so writing the two transfers inline could read MSB
  // first on some compilers and silently swap the axis bytes (Gemini grill review #1). Locals make
  // the order explicit. (This also fixed a latent ordering hazard in the original stock code.)
  byte lsb = spiByteBounded(0x00);    // first byte out of the auto-incrementing read = LSB
  byte msb = spiByteBounded(0x00);    // second byte = MSB
  inData = lsb | (msb << 8);          // arrange the data
  csHigh(LIS3DH_SS);                  // release spi
  return inData;
}

int OpenBCI_32bit_Library::getX()
{
// ----------------------------------------------------------------------------
