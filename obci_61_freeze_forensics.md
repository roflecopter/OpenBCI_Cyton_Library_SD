# OBCI_61 freeze forensics (2026-06-30) — post-SPIROV-fix soak test FROZE

Board: Cyton B, firmware = SPIROV fix commit 3e93cde (flashed 2026-06-29 23:08, verify-clean).
Soak test (board NOT on a person), activity 16, started 2026-06-29 23:12:31 → OBCI_61.TXT.
Found LED-off in the morning; power-cycle (SD out) → LED double-blink → BOARD HW + BATTERY FINE.

## Hard evidence (from the SD card)
- OBCI_61: %BOOT seq=0047 rcon=0x00 session=0001 prev=NONE resume=0 (our fresh start; %META correct).
- Data ends at 764.6 MB (61.4% of the 1.245 GB pre-alloc), EXACTLY block-aligned (1,493,280 blocks).
  Rest of file = NUL padding. NO %Total time footer => unclean halt mid-write.
- ~7.1 h of recording (last %CKPT t=25542813 ms ≈ 425 min), then halted ~56 s after the last ckpt.
- **ALL 425 checkpoints: e=0 r=0 n=0 o=0 x=0** (max over the whole file = 0). ZERO SD write errors,
  retries, reinits, overruns the ENTIRE 7.1 h. Confirmed by full scan, not just the tail.
- Last data line cut mid-sample (`C8,800000,800000,800000,0000`); active channels railed to 800000
  = floating ADS inputs on a no-electrode bench soak (NOT diagnostic of power loss).

## Comparison to the ORIGINAL pre-fix freeze (OBCI_5A)
- OBCI_5A: %BOOT seq=003B fresh resume=0; halted ~4.2 h (455.6 MB), last %CKPT e=0 r=0 n=0 o=0;
  mid-block, no footer; last samples clean EEG (electrodes on that night).
- IDENTICAL failure signature: abrupt mid-write halt, ZERO SD errors, variable time (4.2 h vs 7.1 h).

## Conclusion
- The freeze is NOT the ENH_BUFFER/SPIROV-overrun-in-card.writeData() path that prep.md/grill (commit
  3e93cde) hardened. Proof: (a) our bounded writeData logs e/r when SPIROV recovery fires — e/r stayed
  0 across all 425 ckpts; (b) bounding writeData changed NOTHING — 5A (unbounded) and 61 (bounded) froze
  identically. The SPIROV fix is sound defensive hardening but addresses the wrong failure mode.
- This is an intermittent MCU/firmware HANG at a variable time (4–7 h), no SD-error precursor, board HW
  + battery fine, recoverable only by power-cycle. Root cause UNKNOWN and currently UNOBSERVABLE
  (the diagnostics layer was dropped for the ~424 B flash budget).
- Candidate hang sites to investigate: the ADS1299 DRDY-wait / SPI read path; an ISR (DRDY) deadlock;
  any non-SD unbounded/blocking wait in loop(); the radio/RFduino path; a hard fault / stack issue;
  memory corruption accumulating over hours. The variable multi-hour time suggests a rare race or an
  accumulating condition, not a fixed sample-count boundary.

## Implication for the fix (to plan in /prep)
Observability + auto-recovery FIRST, since the cause is unknown:
- Hardware watchdog (PIC32 WDT) auto-reset on hang -> boot-level auto-resume salvages the night across
  slots. ⚠ Investigate the chipKIT DP32 bootloader WDT-config lock (CLAUDE.md flagged the HW WDT as
  possibly bootloader-locked). Distinct from the DISABLED soft-WDT/executeSoftReset night-eraser.
- A freeze breadcrumb (last loop phase + hang counter to EEPROM) surfaced in the next %BOOT, so the
  next freeze pinpoints the hang site. ⚠ ~424 B flash budget is the hard constraint (118360/118784 B);
  may need further reclamation or a tighter encoding.
- Plus a code audit of ALL unbounded waits/loops outside the SD block-write path.
