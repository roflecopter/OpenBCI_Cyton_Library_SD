// ============================================================================
// patches/wdt-stage-b.cpp — REPRODUCIBLE FORK RECORD (Stage B, hardware WDT)
// ============================================================================
// Out-of-repo change to: ~/Arduino/libraries/OpenBCI_32bit_SD/utility/Sd2Card.cpp
// Adds the hardware-WDT auto-recovery primitive (Stage B). Paired with the DefaultBoard.ino sketch
// changes (arm-on-stream + gate + petWDT top-of-loop). See CLAUDE.md "Stage B" + grill-review-log.md.
//
// WHY: Stage A (bound all ADS+SD SPI) STILL froze (OBCI_62 @4.9h, e=0 r=0) -> the hang is a non-SPI MCU
// lockup. %BOOT wd=0xFF6A0D5B confirmed FWDTEN=0 (arm-able) + WDTPS=0x0A (~1.02s). A hardware WDT resets
// on the hang -> setup() -> the existing replaySessionFile() auto-resume salvages the night across slots.
//
// The WDT primitive lives in this fork (not the sketch) so waitNotBusy (the SD card-busy wait, up to
// SD_WRITE_TIMEOUT=1500ms > the ~1s WDTPS) can pet it from INSIDE the wait. Two edits:
//   (1) the WDT module (below), inserted after spiBlockBounded / before spiRec;
//   (2) petWDT() + a wdtProgress() success-stamp added INSIDE Sd2Card::waitNotBusy's poll loop.
// LIVE SNAPSHOT:
// ----------------------------------------------------------------------------
// ===========================================================================
// Hardware WDT auto-recovery (Stage B, prep.md). The Cyton freezes mid-recording at a
// variable 4-7h with ZERO SD errors, and Stage A's ADS+SD SPI bounding did NOT stop it
// (OBCI_62 froze at 4.9h, e=0 r=0) -> the hang is a non-SPI MCU lockup somewhere in the
// loop. The %BOOT wd=0xFF6A0D5B readout confirmed DEVCFG1 FWDTEN=0 (WDT off at reset, the
// app CAN arm it -> no infinite-reset brick) + WDTPS=0x0A (~1.02s timeout). A hardware
// watchdog resets the board on the hang -> setup() -> the EXISTING replaySessionFile()
// auto-resumes into the next slot, salvaging the night across slots instead of dying at 4.9h.
//
// The WDT primitive lives HERE (not the sketch) so the SD block-write busy-wait (waitNotBusy,
// up to SD_WRITE_TIMEOUT=1500ms > the ~1s WDTPS) can pet it from INSIDE the wait -- otherwise
// one legit slow block write would false-reset a healthy night.
//
// petWDT() discipline: the WDT is ARMED ONLY while actively recording (armed on stream-start,
// DISARMED on stop -> WDT hardware OFF at idle, so a long idle command that blocks loop() >WDTPS
// can't false-reset; Gemini grill review R2). So "armed" == "streaming" — no separate flag. While
// armed, pet ONLY if recording progress (a cached sample OR a completed block) is recent -> a
// genuine hang stops bumping wdtLastProg, petWDT stops petting, the WDT fires. CP0 Count ticks in
// HARDWARE regardless of interrupt state, so an interrupt-disable lockup (which halts millis())
// still trips the no-progress gate. The sketch owns the arm/disarm transitions + the FWDTEN/WDTPS gate.
volatile uint8_t  wdtArmed     = 0;   // 1 ONLY while recording; petWDT is a no-op when 0 (idle/setup/resume)
volatile uint32_t wdtLastProg  = 0;   // CP0 ticks at the last recording progress (sample or completed block)
uint32_t wdtNoProgTicks = 80000000u;  // no-progress deadline: 4.0s @20MHz CP0 (> worst multi-block flush)

void petWDT(void) {
    if (!wdtArmed) return;   // not recording (disarmed on stop, or the FWDTEN/WDTPS gate failed) -> no-op
    // recording: pet ONLY while progress is recent. Unsigned-delta is wrap-safe over one CP0 wrap
    // (the ~4s deadline << the 215s wrap; wdtLastProg is bumped every sample ~2ms + every completed block).
    if ((uint32_t)(cp0Now() - wdtLastProg) < wdtNoProgTicks)
        WDTCONSET = _WDTCON_WDTCLR_MASK;
    // else: DON'T pet -> the WDT fires within WDTPS -> reset -> auto-resume salvages the night
}
void wdtProgress(void) { wdtLastProg = cp0Now(); }                                            // on each sample/block
void wdtArm(void)      { wdtLastProg = cp0Now(); WDTCONSET = _WDTCON_ON_MASK; wdtArmed = 1; }  // stream start (stamps first)
void wdtDisarm(void)   { WDTCONCLR = _WDTCON_ON_MASK; wdtArmed = 0; }                          // stream stop -> WDT OFF

/** Soft SPI receive */
// --- inside waitNotBusy ---
  do {
    petWDT();   // Stage B: feed the WDT from INSIDE the card-busy wait. A legit block write can wait up
                // to SD_WRITE_TIMEOUT=1500ms > the ~1s WDTPS, so without this a healthy slow write would
                // false-reset. petWDT is CP0-progress-gated, so if the card genuinely hangs here (millis
                // may even halt under an interrupt lockup) it STOPS petting after ~4s -> the WDT fires.
    if (sdSpiFault) return false;   // poisoned bus -> bail (Decision 7: trust the latch, not the byte)
    uint8_t r = spiRec();
    if (sdSpiFault) return false;   // spiRec just latched a deadline fault — its 0xFF is the bail
                                    // sentinel, NOT a "card ready" 0xFF; don't misread it (Gemini review #2)
    if (r == 0XFF) { wdtProgress(); return true; }   // card ready = a block/op COMPLETED = recording
                                    // progress. Stamps here (not just on ADS samples) so a legit
                                    // multi-block flush (data+FAT+dir, each up to SD_WRITE_TIMEOUT) keeps
                                    // the no-progress deadline fresh and can't false-reset a healthy write
                                    // (Codex+Gemini review #2). A HUNG write never reaches here -> no stamp
                                    // -> the deadline still trips -> the WDT fires. (No-op until armed.)
  }
  while (((uint16_t)millis() - t0) < timeoutMillis);
  return false;
}
//------------------------------------------------------------------------------
