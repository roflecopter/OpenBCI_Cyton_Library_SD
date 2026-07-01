// ===== patches/wdt-stage-b.cpp — REPRODUCIBLE FORK RECORD (Stage B, hardened over 3 deep review rounds) =====
// Out-of-repo: ~/Arduino/libraries/OpenBCI_32bit_SD/utility/Sd2Card.cpp. See CLAUDE.md Stage B + grill-review-log.md.
// WDT module + 20s CP0 progress deadline:
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
uint32_t wdtNoProgTicks = 400000000u; // no-progress deadline: 20.0s @20MHz CP0. Must EXCEED the longest
                                      // LEGIT no-recording-progress stretch = the FULL SD-ERROR-RECOVERY
                                      // cascade: skip-forward ~1.5s + sdBusRecover TWO busy waits (2× SD_RECOVER_
                                      // BUSY_MS = 4s) + card.init ~2.3s + tuneExtRecoveryWindowMs=8s ≈ 15.8s
                                      // (deep review B, Codex #2 — the earlier 14s undercounted sdBusRecover's
                                      // 4s). 20s gives ~4s margin so a slow-but-HEALING card isn't false-reset.
                                      // Petting DURING the cascade (cardCommand's waitNotBusy + petWDT() in
                                      // sdBusRecover's busy-waits + the ext chunk) feeds the ~1s HW WDT; THIS
                                      // deadline is the CP0 backstop that still fires if the recovery genuinely
                                      // HANGS past 20s. Normal recording stamps progress on every accepted block
                                      // so 20s is never approached. (Coupled to ext_recovery_window_ms=8000 —
                                      // raise both together if that tune is increased.)

void petWDT(void) {
    if (!wdtArmed) return;   // not recording (disarmed on stop, or the FWDTEN/WDTPS gate failed) -> no-op
    // recording: pet ONLY while progress is recent. Unsigned-delta is wrap-safe over one CP0 wrap
    // (the ~20s deadline << the 215s wrap; wdtLastProg is bumped every sample ~2ms + every completed block).
    if ((uint32_t)(cp0Now() - wdtLastProg) < wdtNoProgTicks)
        WDTCONSET = _WDTCON_WDTCLR_MASK;
    // else: DON'T pet -> the WDT fires within WDTPS -> reset -> auto-resume salvages the night
}
// petWDT inside waitNotBusy (feeds HW WDT; progress stamp is NOT here — see writeData):
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
    petWDT();   // Stage B: feed the WDT from INSIDE the card-busy wait. A legit block write can wait up
                // to SD_WRITE_TIMEOUT=1500ms > the ~1s WDTPS, so without this a healthy slow write would
                // false-reset. petWDT is CP0-progress-gated, so if the card genuinely hangs here (millis
                // may even halt under an interrupt lockup) it STOPS petting after ~20s -> the WDT fires.
    if (sdSpiFault) return false;   // poisoned bus -> bail (Decision 7: trust the latch, not the byte)
    uint8_t r = spiRec();
// recording-progress stamp on an ACCEPTED block (writeData):
    }
    if ((status_ & DATA_RES_MASK) != DATA_RES_ACCEPTED) {
        error(SD_CARD_ERROR_WRITE);
        chipSelectHigh();
        return false;
    }
    wdtProgress();   // a recording block was ACCEPTED by the card = real recording progress (deep review B,
                     // Codex #1). This is the ONLY recording-block-completion stamp on the SD side (waitNotBusy
                     // no longer stamps — it also runs for non-recording card.init/cardCommand). A hung write
                     // never reaches here -> wdtLastProg ages -> the 20s CP0 deadline trips -> the WDT fires.
    return true;
}
//------------------------------------------------------------------------------
/** Start a write multiple blocks sequence.
 *
 * \param[in] blockNumber Address of first block in sequence.
