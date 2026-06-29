/* Arduino Sd2Card Library
 * Copyright (C) 2009 by William Greiman
 *
 * This file is part of the Arduino Sd2Card Library
 *
 * This Library is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This Library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with the Arduino Sd2Card Library.  If not, see
 * <http://www.gnu.org/licenses/>.
 */
#include <p32xxxx.h>



#include <WProgram.h>
#include "Sd2Card.h"

// Definition for the extern declared in Sd2Card.h. Default matches the
// pre-2026-05-15 const value; the firmware's tune-protocol (T wire command,
// key 0x05) can rewrite this at runtime. Every waitNotBusy(SD_WRITE_TIMEOUT)
// call site below reads the current value, so a tune mid-session takes
// effect on the next failed write rather than requiring a reflash.
uint16_t SD_WRITE_TIMEOUT = 1500;

/*	SPIxCON
*/
#define bnOn	15
#define bnSmp	9
#define bnCkp	6
#define bnMsten 5

/*	SPIxSTAT
*/
#define bnTbe	3
#define bnRbf	0

/*	IEC0
*/
#define bnSPI2RXIE	7
#define bnSPI2TXIE	6

uint32_t	spi_state;
uint8_t     fspi_state_saved = false;
uint32_t    interrupt_state = 0;

extern "C" {
    extern uint8_t shiftIn(uint8_t dataPin, uint8_t clockPin, uint8_t bitOrder);
    extern void shiftOut(uint8_t dataPin, uint8_t clockPin, uint8_t bitOrder, byte data);
}

// ===========================================================================
// SPIROV-overrun hang fix (prep.md, 2026-06-29) — bounded, FIFO-safe SPI
// primitives + an atomic SPI-module flush, so the 512-byte SD block write can
// NEVER infinite-spin on a latched SPIROV. See OpenBCI_Cyton_Library_SD/prep.md
// + CLAUDE.md for the full root-cause writeup.
//
// HARDWARE: on this board DSPI0 maps to SPI1 (board.spi -> SPI1). These
// primitives talk to the SPI1 SFRs directly (same module DSPI drives) so they
// can bound the wait + observe SPI1STAT.SPIROV. If the SD DSPI instance is ever
// re-mapped off SPI1 this MUST be revisited.
#if !defined(_SPI1STAT_SPIROV_MASK) || !defined(_SPI1CON_ON_MASK) || !defined(_SPI1CON_ENHBUF_MASK)
#error "Sd2Card SPIROV fix expects the PIC32 SPI1 SFR field masks (proc header)"
#endif
#include <cp0defs.h>

// CP0 Count ticks at SYSCLK/2 = 20 MHz on this part -> 20 ticks per microsecond.
// Used only as a generous INFINITE-SPIN BREAKER (not a tight timing source).
#define SD_CP0_PER_US 20u
static inline uint32_t cp0Now(void) { return _CP0_GET_COUNT(); }
// Rollover-safe: unsigned subtraction wraps correctly across the 32-bit Count.
static inline bool cp0Past(uint32_t t0, uint32_t us) {
    return (uint32_t)(cp0Now() - t0) >= (us * SD_CP0_PER_US);
}

// Sticky SPI-bus fault latch. Set ONLY by the bounded primitives on bail (a
// CP0 deadline OR a latched SPIROV). The owner (writeCache / sdBusRecover /
// the top-of-loop guard) clears it by calling sdSpiModuleFlush(). The latch's
// job is to short-circuit the REMAINING in-flight primitives of an already-
// wedged op so they fail fast instead of each paying the deadline. Callers
// check `sdSpiFault` after spiRec/spiSend rather than trusting the 0xFF byte.
volatile uint8_t sdSpiFault     = 0;
volatile uint8_t sdRecoverEvents = 0;  // live recovery events run this session (diag)
volatile uint32_t sdSpirovSeen  = 0;   // confirmed SPI1STAT.SPIROV events this session (diag %CKPT s=)

// Runtime tunables (host 'T' protocol keys 0x07/0x08; defaults = the fix consts).
uint16_t sdSpiDeadlineUs = 3000;  // key 0x07: max time with NO SPI progress before bail (us)
uint8_t  sdFifoHeadroom  = 7;     // key 0x08: max bytes in-flight in the bulk loop (RX FIFO is 8 deep)

// Atomic SPI1-module flush: clears the host TX/RX FIFO + shift register + the
// latched SPIROV WITHOUT touching the mode/CKE/CKP/MSTEN bits or SPIBRG, so the
// active SPI mode (SD MODE0 / ADS MODE1) and clock divider are preserved. The ON
// and ENHBUF clears are SPLIT because ENHBUF is only writable while ON==0 (PIC32
// rule) — a single combined CLR can evaluate ENHBUF-writability against the old
// ON==1 and silently drop the clear. CLEARS sdSpiFault (the "hardware handled"
// point). CS is a GPIO and is untouched. (prep.md Decision 3.)
void sdSpiModuleFlush(void) {
    SPI1CONCLR = _SPI1CON_ON_MASK;                 // ON -> 0
    (void)SPI1CON;                                 // forced read: settle before the (now-legal) ENHBUF clear
    SPI1CONCLR = (_SPI1CON_ENHBUF_MASK | _SPI1CON_MODE16_MASK | _SPI1CON_MODE32_MASK);
    SPI1STATCLR = _SPI1STAT_SPIROV_MASK;           // clear the latched overrun
    (void)SPI1STAT;
    SPI1CONSET = _SPI1CON_ON_MASK;                 // ON -> 1
    (void)SPI1CON;                                 // forced read: settle before any following transfer
    sdSpiFault = 0;
}

// Bounded standard single-byte transfer (ENHBUF==0). Entry short-circuits on a
// poisoned bus so a naive caller (FAT op, file close) aborts instantly instead
// of paying the deadline before the fault bubbles up. On a CP0 deadline: latch
// sdSpiFault, return 0xFF. (prep.md Decision 5.)
uint8_t spiByteBounded(uint8_t out) {
    if (sdSpiFault) return 0xFF;
    uint32_t t0 = cp0Now();
    while ((SPI1STAT & _SPI1STAT_SPITBE_MASK) == 0) {       // wait TX-buffer-empty (mandatory before write)
        if (cp0Past(t0, sdSpiDeadlineUs)) { sdSpiFault = 1; return 0xFF; }
    }
    SPI1BUF = out;
    t0 = cp0Now();
    while ((SPI1STAT & _SPI1STAT_SPIRBF_MASK) == 0) {       // wait RX-buffer-full
        if (cp0Past(t0, sdSpiDeadlineUs)) { sdSpiFault = 1; return 0xFF; }
    }
    return (uint8_t)SPI1BUF;
}

// Bounded, FIFO-safe bulk write — a local copy of DSPI's ENH_BUFFER bulk loop
// that CAPS in-flight bytes (in-flight < sdFifoHeadroom, default 7, with eager
// draining) so the 8-deep RX FIFO never overruns under DRDY-ISR jitter, PLUS a
// SPIROV check + a no-progress CP0 deadline as the infinite-spin backstop. The
// happy-path ENHBUF toggle MATCHES stock DSPI (set/clear while ON==1) — proven
// on this MX250 and, critically, never toggles ON mid-transaction (the ON==0
// split is reserved for the recovery flush, where the transaction is abandoned).
// On bail: latch sdSpiFault, return false (does NOT itself flush — recovery
// owns that). Replaces writeData's `_spi->transfer(512, src)`. (prep.md Decision 4.)
static bool spiBlockBounded(const uint8_t* src, uint16_t n) {
    if (sdSpiFault) return false;                  // entry short-circuit
    uint16_t headroom = sdFifoHeadroom; if (headroom < 1) headroom = 1; if (headroom > 7) headroom = 7;
    SPI1CONSET = _SPI1CON_ENHBUF_MASK;             // enable ENH_BUFFER (ON stays 1 — matches stock DSPI)
    uint16_t toWrite = n, toRead = n, wPos = 0;
    uint32_t t0 = cp0Now();
    while (toWrite > 0 || toRead > 0) {
        bool progressed = false;
        // Drain eagerly every iteration so the RX FIFO realistically never fills.
        if (toRead > 0 && (SPI1STAT & _SPI1STAT_SPIRBE_MASK) == 0) {
            (void)SPI1BUF; toRead--; progressed = true;
        }
        // Gate the next write on in-flight < headroom AND the TX FIFO not full.
        if (toWrite > 0 && (uint16_t)(toRead - toWrite) < headroom
                        && (SPI1STAT & _SPI1STAT_SPITBF_MASK) == 0) {
            SPI1BUF = src[wPos++]; toWrite--; progressed = true;
        }
        if (SPI1STAT & _SPI1STAT_SPIROV_MASK) {    // overrun latched — THE bug, caught not hung
            sdSpirovSeen++;                         // confirmed SPIROV (diag s=)
            sdSpiFault = 1;
            return false;                           // leave ENHBUF as-is; sdSpiModuleFlush cleans it
        }
        if (progressed) { t0 = cp0Now(); }          // no-progress watchdog re-arms on any progress
        else if (cp0Past(t0, sdSpiDeadlineUs)) { sdSpiFault = 1; return false; }
    }
    SPI1CONCLR = _SPI1CON_ENHBUF_MASK;             // clear ENH_BUFFER on clean exit (matches stock DSPI)
    return true;
}


/** Soft SPI receive */
uint8_t Sd2Card::spiRec(void) {
    uint8_t data = 0;
    if (_spi) {
        data = spiByteBounded(0xFF);   // bounded + SPIROV-safe (was _spi->transfer(0xFF))
    } else {
        // output pin high - like sending 0XFF
        digitalWrite(_mosi, HIGH);
        digitalWrite(_miso, HIGH);
        data = shiftIn(_miso, _clk, MSBFIRST);
    }
    return data;
}
//------------------------------------------------------------------------------
/** Soft SPI send */
void Sd2Card::spiSend(uint8_t data) {
    if (_spi) {
        (void)spiByteBounded(data);   // bounded + SPIROV-safe (was _spi->transfer(data))
    } else {
        digitalWrite(_miso, HIGH);
        shiftOut(_mosi, _clk, MSBFIRST, data);
    }
}
//------------------------------------------------------------------------------
// send command and return error code.  Return zero for OK
uint8_t Sd2Card::cardCommand(uint8_t cmd, uint32_t arg) {
  // end read if in partialBlockRead mode
  readEnd();

  // select card
  chipSelectLow();

  // wait up to 300 ms if busy
  waitNotBusy(300);

  // send command
  spiSend(cmd | 0x40);

  // send argument
  for (int8_t s = 24; s >= 0; s -= 8) spiSend(arg >> s);

  // send CRC
  uint8_t crc = 0XFF;
  if (cmd == CMD0) crc = 0X95;  // correct crc for CMD0 with arg 0
  if (cmd == CMD8) crc = 0X87;  // correct crc for CMD8 with arg 0X1AA
  spiSend(crc);

  //spiSend(0xFF);
  
  // wait for response
  for (uint8_t i = 0; ((status_ = spiRec()) & 0X80) && i != 0XFF; i++) {
    if (sdSpiFault) break;   // poisoned bus -> stop polling; nonzero status_ propagates the failure
  }
  return status_;
}
//------------------------------------------------------------------------------
/**
 * Determine the size of an SD flash memory card.
 *
 * \return The number of 512 byte data blocks in the card
 *         or zero if an error occurs.
 */
uint32_t Sd2Card::cardSize(void) {
  csd_t csd;
  if (!readCSD(&csd)) return 0;
  if (csd.v1.csd_ver == 0) {
    uint8_t read_bl_len = csd.v1.read_bl_len;
    uint16_t c_size = (csd.v1.c_size_high << 10)
                      | (csd.v1.c_size_mid << 2) | csd.v1.c_size_low;
    uint8_t c_size_mult = (csd.v1.c_size_mult_high << 1)
                          | csd.v1.c_size_mult_low;
    return (uint32_t)(c_size + 1) << (c_size_mult + read_bl_len - 7);
  } else if (csd.v2.csd_ver == 1) {
    uint32_t c_size = ((uint32_t)csd.v2.c_size_high << 16)
                      | (csd.v2.c_size_mid << 8) | csd.v2.c_size_low;
    return (c_size + 1) << 10;
  } else {
    error(SD_CARD_ERROR_BAD_CSD);
    return 0;
  }
}

//------------------------------------------------------------------------------
void Sd2Card::chipSelectHigh(void) {
  digitalWrite(_cs, HIGH);
}
//------------------------------------------------------------------------------
void Sd2Card::chipSelectLow(void) {
    digitalWrite(_cs, LOW);
  // digitalWrite(chipSelectPin_, LOW);
}
//------------------------------------------------------------------------------
/** Erase a range of blocks.
 *
 * \param[in] firstBlock The address of the first block in the range.
 * \param[in] lastBlock The address of the last block in the range.
 *
 * \note This function requests the SD card to do a flash erase for a
 * range of blocks.  The data on the card after an erase operation is
 * either 0 or 1, depends on the card vendor.  The card must support
 * single block erase.
 *
 * \return The value one, true, is returned for success and
 * the value zero, false, is returned for failure.
 */
uint8_t Sd2Card::erase(uint32_t firstBlock, uint32_t lastBlock) {
  if (!eraseSingleBlockEnable()) {
    error(SD_CARD_ERROR_ERASE_SINGLE_BLOCK);
    goto fail;
  }
  if (type_ != SD_CARD_TYPE_SDHC) {
    firstBlock <<= 9;
    lastBlock <<= 9;
  }
  if (cardCommand(CMD32, firstBlock)
    || cardCommand(CMD33, lastBlock)
    || cardCommand(CMD38, 0)) {
      error(SD_CARD_ERROR_ERASE);
      goto fail;
  }
  if (!waitNotBusy(SD_ERASE_TIMEOUT)) {
    error(SD_CARD_ERROR_ERASE_TIMEOUT);
    goto fail;
  }
  chipSelectHigh();
  return true;

 fail:
  chipSelectHigh();
  return false;
}
//------------------------------------------------------------------------------
/** Determine if card supports single block erase.
 *
 * \return The value one, true, is returned if single block erase is supported.
 * The value zero, false, is returned if single block erase is not supported.
 */
uint8_t Sd2Card::eraseSingleBlockEnable(void) {
  csd_t csd;
  return readCSD(&csd) ? csd.v1.erase_blk_en : 0;
}
//------------------------------------------------------------------------------
/**
 * Initialize an SD flash memory card.
 *
 * \param[in] sckRateID SPI clock rate selector. See setSckRate().
 * \param[in] chipSelectPin SD chip select pin number.
 *
 * \return The value one, true, is returned for success and
 * the value zero, false, is returned for failure.  The reason for failure
 * can be determined by calling errorCode() and errorData().
 */
uint8_t Sd2Card::init(uint8_t sckRateID, uint8_t chipSelectPin) {
    _cs = chipSelectPin;

    if (_spi) {
        // _spi->begin();
	_spi->setSpeed(125000UL);
	//_spi->setSpeed(1UL);
	//Serial.println("SPEED CHANGED");
	//_spi->setSpeed(1000UL);
	//_spi->setSpeed(400000UL);
	//_spi->setSpeed(1000000UL);
    } else {
        pinMode(_mosi, OUTPUT);
        pinMode(_miso, INPUT);
        pinMode(_clk, OUTPUT);
    }
    
    pinMode(_cs, OUTPUT);
    digitalWrite(_cs, HIGH);

    errorCode_ = inBlock_ = partialBlockRead_ = type_ = 0;

    // 16-bit init start time allows over a minute
    uint16_t t0 = (uint16_t)millis();
    uint32_t arg;

    chipSelectHigh();

    // must supply min of 74 clock cycles with CS high.
    for (uint8_t i = 0; i < 30; i++) spiSend(0XFF);

    chipSelectLow();

    // command to go idle in SPI mode
    while ((status_ = cardCommand(CMD0, 0)) != R1_IDLE_STATE) {
		goto fail;
        if (((uint16_t)millis() - t0) > SD_INIT_TIMEOUT) {
            error(SD_CARD_ERROR_CMD0);
            goto fail;
        }
    }
	// Serial.println("IDLE");  
    // check SD version
    if ((cardCommand(CMD8, 0x1AA) & R1_ILLEGAL_COMMAND)) {
        type(SD_CARD_TYPE_SD1);
    } else {
        // only need last byte of r7 response
        for (uint8_t i = 0; i < 4; i++) status_ = spiRec();
        if (status_ != 0XAA) {
            error(SD_CARD_ERROR_CMD8);
            goto fail;
        }
        type(SD_CARD_TYPE_SD2);
    }
    // initialize card and send host supports SDHC if SD2
    arg = type() == SD_CARD_TYPE_SD2 ? 0X40000000 : 0;

    while ((status_ = cardAcmd(ACMD41, arg)) != R1_READY_STATE) {
        // check for timeout
        if (((uint16_t)millis() - t0) > SD_INIT_TIMEOUT) {
            error(SD_CARD_ERROR_ACMD41);
            goto fail;
        }
    }
    // if SD2 read OCR register to check for SDHC card
    if (type() == SD_CARD_TYPE_SD2) {
        if (cardCommand(CMD58, 0)) {
            error(SD_CARD_ERROR_CMD58);
            goto fail;
        }
        if ((spiRec() & 0XC0) == 0XC0) type(SD_CARD_TYPE_SDHC);
        // discard rest of ocr - contains allowed voltage range
        for (uint8_t i = 0; i < 3; i++) spiRec();
    }
    chipSelectHigh();

    if (_spi) {
//	_spi->setSpeed(10000000UL);
	_spi->setSpeed(20000000UL);
    } else {
   	 setSckRate(sckRateID);
    }
    return true;

fail:
    chipSelectHigh();
    return false;
}
//------------------------------------------------------------------------------
/**
 * Enable or disable partial block reads.
 *
 * Enabling partial block reads improves performance by allowing a block
 * to be read over the SPI bus as several sub-blocks.  Errors may occur
 * if the time between reads is too long since the SD card may timeout.
 * The SPI SS line will be held low until the entire block is read or
 * readEnd() is called.
 *
 * Use this for applications like the Adafruit Wave Shield.
 *
 * \param[in] value The value TRUE (non-zero) or FALSE (zero).)
 */
void Sd2Card::partialBlockRead(uint8_t value) {
  readEnd();
  partialBlockRead_ = value;
}
//------------------------------------------------------------------------------
/**
 * Read a 512 byte block from an SD card device.
 *
 * \param[in] block Logical block to be read.
 * \param[out] dst Pointer to the location that will receive the data.

 * \return The value one, true, is returned for success and
 * the value zero, false, is returned for failure.
 */
uint8_t Sd2Card::readBlock(uint32_t block, uint8_t* dst) {
  return readData(block, 0, 512, dst);
}
//------------------------------------------------------------------------------
/**
 * Read part of a 512 byte block from an SD card.
 *
 * \param[in] block Logical block to be read.
 * \param[in] offset Number of bytes to skip at start of block
 * \param[out] dst Pointer to the location that will receive the data.
 * \param[in] count Number of bytes to read
 * \return The value one, true, is returned for success and
 * the value zero, false, is returned for failure.
 */
uint8_t Sd2Card::readData(uint32_t block,
        uint16_t offset, uint16_t count, uint8_t* dst) {
  if (count == 0) return true;
  if ((count + offset) > 512) {
    goto fail;
  }
  if (!inBlock_ || block != block_ || offset < offset_) {
    block_ = block;
    // use address if not SDHC card
    if (type()!= SD_CARD_TYPE_SDHC) block <<= 9;
    if (cardCommand(CMD17, block)) {
      error(SD_CARD_ERROR_CMD17);
      goto fail;
    }
    if (!waitStartBlock()) {
      goto fail;
    }
    offset_ = 0;
    inBlock_ = 1;
  }



  // skip data before offset
  for (;offset_ < offset; offset_++) {
    spiRec();
  }
  // transfer data — BOUNDED per-byte (was the UNBOUNDED `_spi->transfer(count,0xFF,dst)` DSPI
  // bulk read, which could wedge a FAT/dir/block read forever exactly like the write path did;
  // Codex review #1). Reads are NOT the recording hot path (the recorder only writes during a
  // session; reads happen at init/file-open), so per-byte is fine. spiByteBounded latches
  // sdSpiFault + bails on a CP0 deadline; stop early so we don't pay it per remaining byte.
  for (uint16_t i = 0; i < count; i++) {
    dst[i] = spiRec();        // spiRec dispatches _spi -> spiByteBounded (bounded) / soft-SPI fallback
    if (sdSpiFault) break;
  }

  offset_ += count;
  if (!partialBlockRead_ || offset_ >= 512) {
    // read rest of data, checksum and set chip select high
    readEnd();
  }

  if (sdSpiFault) goto fail;   // poisoned bus during the read path -> deselect + fail (Decision 5)

  return true;

 fail:
  chipSelectHigh();
  return false;
}
//------------------------------------------------------------------------------
/** Skip remaining data in a block when in partial block read mode. */
void Sd2Card::readEnd(void) {
  if (inBlock_) {
      // skip data and crc
    while (offset_++ < 514) {
        spiRec();
    }
    chipSelectHigh();
    inBlock_ = 0;
  }
}
//------------------------------------------------------------------------------
/** read CID or CSR register */
uint8_t Sd2Card::readRegister(uint8_t cmd, void* buf) {
  uint8_t* dst = reinterpret_cast<uint8_t*>(buf);
  if (cardCommand(cmd, 0)) {
    error(SD_CARD_ERROR_READ_REG);
    goto fail;
  }
  if (!waitStartBlock()) goto fail;
  // transfer data
  for (uint16_t i = 0; i < 16; i++) dst[i] = spiRec();
  spiRec();  // get first crc byte
  spiRec();  // get second crc byte
  if (sdSpiFault) goto fail;   // a bounded-read deadline -> CSD/CID is garbage (all 0xFF); fail, don't proceed (Codex review #2)
  chipSelectHigh();
  return true;

 fail:
  chipSelectHigh();
  return false;
}
//------------------------------------------------------------------------------
/**
 * Set the SPI clock rate.
 *
 * \param[in] sckRateID A value in the range [0, 6].
 *
 * The SPI clock will be set to F_CPU/pow(2, 1 + sckRateID). The maximum
 * SPI rate is F_CPU/2 for \a sckRateID = 0 and the minimum rate is F_CPU/128
 * for \a scsRateID = 6.
 *
 * \return The value one, true, is returned for success and the value zero,
 * false, is returned for an invalid value of \a sckRateID.
 */
uint8_t Sd2Card::setSckRate(uint8_t sckRateID) {
  //if (sckRateID > 6) {
  //  error(SD_CARD_ERROR_SCK_RATE);
  //  return false;
  //}
  //// see avr processor datasheet for SPI register bit definitions
  //if ((sckRateID & 1) || sckRateID == 6) {
  //  SPSR &= ~(1 << SPI2X);
  //} else {
  //  SPSR |= (1 << SPI2X);
  //}
  //SPCR &= ~((1 <<SPR1) | (1 << SPR0));
  //SPCR |= (sckRateID & 4 ? (1 << SPR1) : 0)
  //  | (sckRateID & 2 ? (1 << SPR0) : 0);
  return true;
}
//------------------------------------------------------------------------------
// wait for card to go not busy
uint8_t Sd2Card::waitNotBusy(uint16_t timeoutMillis) {
  uint16_t t0 = millis();
  do {
    if (sdSpiFault) return false;   // poisoned bus -> bail (Decision 7: trust the latch, not the byte)
    uint8_t r = spiRec();
    if (sdSpiFault) return false;   // spiRec just latched a deadline fault — its 0xFF is the bail
                                    // sentinel, NOT a "card ready" 0xFF; don't misread it (Gemini review #2)
    if (r == 0XFF) return true;
  }
  while (((uint16_t)millis() - t0) < timeoutMillis);
  return false;
}
//------------------------------------------------------------------------------
/** Wait for start block token */
uint8_t Sd2Card::waitStartBlock(void) {
  uint16_t t0 = millis();
  while ((status_ = spiRec()) == 0XFF) {
    if (sdSpiFault) goto fail;   // poisoned bus -> deselect + fail fast
    if (((uint16_t)millis() - t0) > SD_READ_TIMEOUT) {
      error(SD_CARD_ERROR_READ_TIMEOUT);
      goto fail;
    }
  }
  if (status_ != DATA_START_BLOCK) {
    error(SD_CARD_ERROR_READ);
    goto fail;
  }
  return true;

 fail:
  chipSelectHigh();
  return false;
}
//------------------------------------------------------------------------------
/**
 * Writes a 512 byte block to an SD card.
 *
 * \param[in] blockNumber Logical block to be written.
 * \param[in] src Pointer to the location of the data to be written.
 * \return The value one, true, is returned for success and
 * the value zero, false, is returned for failure.
 */
uint8_t Sd2Card::writeBlock(uint32_t blockNumber, const uint8_t* src) {
#if SD_PROTECT_BLOCK_ZERO
  // don't allow write to first block
  if (blockNumber == 0) {
    error(SD_CARD_ERROR_WRITE_BLOCK_ZERO);
    goto fail;
  }
#endif  // SD_PROTECT_BLOCK_ZERO
  // use address if not SDHC card
  if (type() != SD_CARD_TYPE_SDHC) blockNumber <<= 9;
  if (cardCommand(CMD24, blockNumber)) {
    error(SD_CARD_ERROR_CMD24);
    goto fail;
  }
  if (!writeData(DATA_START_BLOCK, src)) goto fail;

  // wait for flash programming to complete
  if (!waitNotBusy(SD_WRITE_TIMEOUT)) {
    error(SD_CARD_ERROR_WRITE_TIMEOUT);
    goto fail;
  }

  // response is r2 so get and check two bytes for nonzero
  if (cardCommand(CMD13, 0) || spiRec()) {
    error(SD_CARD_ERROR_WRITE_PROGRAMMING);
    goto fail;
  }
  chipSelectHigh();
  return true;

 fail:
  chipSelectHigh();
  return false;
}
//------------------------------------------------------------------------------
/** Write one data block in a multiple block write sequence */
uint8_t Sd2Card::writeData(const uint8_t* src) {
  // wait for previous write to finish
  if (!waitNotBusy(SD_WRITE_TIMEOUT)) {
    error(SD_CARD_ERROR_WRITE_MULTIPLE);
    chipSelectHigh();
    return false;
  }
  return writeData(WRITE_MULTIPLE_TOKEN, src);
}
//------------------------------------------------------------------------------
// send one block of data for write block or write multiple blocks
uint8_t Sd2Card::writeData(uint8_t token, const uint8_t* src) {

    spiSend(token);
    if (_spi != NULL) {
        // Bounded, FIFO-safe bulk write — returns false (and latches sdSpiFault)
        // on a SPIROV/deadline instead of infinite-spinning. (prep.md Layer 1.)
        if (!spiBlockBounded(src, 512)) {
            error(SD_CARD_ERROR_WRITE);
            chipSelectHigh();
            return false;
        }
    } else {
        for (uint16_t i = 0; i < 512; i++) {
            spiSend(src[i]);
        }
    }

    spiSend(0xff);  // dummy crc
    spiSend(0xff);  // dummy crc

    status_ = spiRec();
    if (sdSpiFault) {                 // a crc-send / status-read hit the fault latch
        chipSelectHigh();
        return false;
    }
    if ((status_ & DATA_RES_MASK) != DATA_RES_ACCEPTED) {
        error(SD_CARD_ERROR_WRITE);
        chipSelectHigh();
        return false;
    }
    return true;
}
//------------------------------------------------------------------------------
/** Start a write multiple blocks sequence.
 *
 * \param[in] blockNumber Address of first block in sequence.
 * \param[in] eraseCount The number of blocks to be pre-erased.
 *
 * \note This function is used with writeData() and writeStop()
 * for optimized multiple block writes.
 *
 * \return The value one, true, is returned for success and
 * the value zero, false, is returned for failure.
 */
uint8_t Sd2Card::writeStart(uint32_t blockNumber, uint32_t eraseCount) {
#if SD_PROTECT_BLOCK_ZERO
  // don't allow write to first block
  if (blockNumber == 0) {
    error(SD_CARD_ERROR_WRITE_BLOCK_ZERO);
    goto fail;
  }
#endif  // SD_PROTECT_BLOCK_ZERO
  // send pre-erase count
  if (cardAcmd(ACMD23, eraseCount)) {
    error(SD_CARD_ERROR_ACMD23);
    goto fail;
  }
  // use address if not SDHC card
  if (type() != SD_CARD_TYPE_SDHC) blockNumber <<= 9;
  if (cardCommand(CMD25, blockNumber)) {
    error(SD_CARD_ERROR_CMD25);
    goto fail;
  }
  return true;

 fail:
  chipSelectHigh();
  return false;
}
//------------------------------------------------------------------------------
/** End a write multiple blocks sequence.
 *
* \return The value one, true, is returned for success and
 * the value zero, false, is returned for failure.
 */
uint8_t Sd2Card::writeStop(void) {
  if (!waitNotBusy(SD_WRITE_TIMEOUT)) goto fail;
  spiSend(STOP_TRAN_TOKEN);
  if (!waitNotBusy(SD_WRITE_TIMEOUT)) goto fail;
  chipSelectHigh();
  return true;

 fail:
  error(SD_CARD_ERROR_STOP_TRAN);
  chipSelectHigh();
  return false;
}
