# work-log — root-cause: which firmware change increased the Cyton mid-recording failure rate

## Task (restated)
User: "before your fixes board were failing less (2-3 / 50); you made it worse." Find the MOST
LIKELY candidate change (last ~2 months) that increased the hang/lost-night rate, via exhaustive
Codex+Gemini rounds, autonomously, and land a concrete testable solution.

## Lane: INVOLVED (scaled up — user asked for many exhaustive rounds / hours). Diagnostic → solution.

## Context recovered (Act 0)
- Forensics: hang = mid-recording halt, e=0 r=0 n=0 o=0 x=0, no %Total time footer. Times 4.2h(5A,
  pre-any-fix) / 7.1h(61) / 4.9h(62) / 45min(64 = RESET+failed-resume) / clean 12h(63). Card healthy.
- Author's own Stage B note: "Stage A ADS+SD SPI bounding did NOT stop it (62 e=0 r=0) → non-SPI MCU
  lockup somewhere in the loop." So the freeze is NOT the SPI path.
- Smoking gun already in git: 6f6efe8 (2026-06-23) "rollback: disable mid-night self-reset (was
  erasing whole nights) … user lost FEWER nights before these resets existed."
- Single shared SPI1 bus (ADS reads + SD writes). PIC32MX250F128B, 32KB RAM, no ICSP.
- recall store returned nothing for this topic.

## Panel round 1 — root-cause RCA (Codex xhigh + Gemini)
BOTH converge: **#1 = Stage B HW WDT is the regression** (converts a genuine freeze into a
reset-mid-CMD25 that wedges the SD → auto-resume fails; not a false-fire — the 20s deadline is safe).
Minimal fix both propose: disable wdtArm() → revert to freeze-preserves-partial baseline.
- Gemini NEW root-freeze hypothesis (high value): the underlying non-SPI freeze is a **blocking UART
  TX in board.sendChannelData() (Serial1 to the RFduino radio)** — TX buffer fills, HardwareSerial::write
  spins forever → freeze, e=0 r=0, no footer. Fix = bounded/dropping UART write → PREVENTS the freeze.
- Both rank bounded-SPI/RX/CKPT LOW (e=0 r=0 + 5A pre-dates them). Codex: confirm reset cause via RCON.
- TENSION to press R2: OBCI_64's 77MB partial SURVIVED → the "WDT = catastrophic total loss" claim is
  not fully proven; pin the exact worse-mechanism (false-fire? resume re-alloc? or WDT ~neutral?).

## Between R1 and R2 — Claude verification of R1 hypotheses (code-grounded)
- Gemini's blocking-UART root-freeze hypothesis FALSIFIED @500Hz: sendChannelData() gates the radio
  send on `curSampleRate==SAMPLE_RATE_250`; the sleep/ECG recording is `sf:500` (session_start.yml)
  → the radio path is DEAD during recording. writeSerial(Serial0.write) only runs on command
  responses, not the 500Hz sample loop. board.loop()@500Hz is trivial (LED+timer).
- ADS read: DRDY hardware ISR (ADS_DRDY_Service) sets channelDataAvailable flag only; updateBoardData
  reads via xfer()→spiByteBounded (bounded, Stage A). So @500Hz all SPI is bounded + radio off.
- REFINED TENSION: wdtProgress() is stamped on EVERY sample acquisition (loop.txt:35) AND on
  block-accept. So a write-failing-but-still-sampling loop keeps petting → the WDT fires ONLY on a
  TRUE total hang (no sample for 20s). A true hang preserves the 77MB partial either way → WDT may be
  ~NEUTRAL for the failure COUNT. The catastrophic-loss (all-NUL) mechanism in 6f6efe8 was the
  auto-resume CHAIN re-allocating the slot, not the reset per se. → the real regressor may be
  auto-resume-on-unclean-reset, not the WDT firing.
- Slot-alloc finding: a resume runs setupSDcard→incrementFileCounter→createContiguous of the NEXT slot
  (OBCI_N+1); it does NOT re-allocate/erase the previous partial (SD_Card_Stuff.ino:92-93). Confirms
  OBCI_64's 77MB partial survived. So for a SINGLE hang the WDT+resume is neutral (resume-fail → same
  as freeze) or better (resume-ok → salvage). "Worse" ⇒ must be false-fire, rapid-resume thrash
  (MAX_RESUMES=25), or a genuinely risen freeze frequency. Open for R2.

## Panel round 2 — SPLIT
- Gemini 95%: auto-resume ERASES partial (createContiguous reclaims same unsynced clusters + card.erase).
  Fix: disable wdtArm() AND stop SESSION.TXT.
- Codex: resume-erase only 10%; resume targets OBCI_N+1 (incrementFileCounter) → different clusters;
  OBCI_64 partial survived. Ranks risen-hang-frequency 45% / neutral-perception 30% / WDT 15%.
  Fix: disable wdtArm() ONLY. Real regressor = the still-unidentified 500Hz total-hang.
- BOTH: 500Hz freeze = unhandled exception → Default_Handler while(1). BOTH propose forensic
  exception-handler → persist Cause/EPC + loop-phase breadcrumb to catch the next hang. BOTH propose
  the same fault-injection test (reset mid-CMD25, check partial survives w/ vs w/o resume).

## Claude adjudication (favours Codex): OBCI_64.TXT was READABLE (77MB, valid dir entry) after the
reset → its dir+FAT reached disk → its clusters are on-disk USED → resume's createContiguous gets
DIFFERENT clusters, cannot erase it. Disproves Gemini's same-cluster-erase for the HW-WDT path. Plus
setupSDcard's remove-guard (1367-1389) probe-skips real-data slots. So WDT+resume ≈ count-neutral.

## Panel round 3 — CONVERGED (Codex + Gemini agree)
- Q1: BOTH concede OBCI_64 readability disproves same-cluster erase → WDT+resume COUNT-NEUTRAL on
  committed partials. (Erase risk remains only for a reset during the INITIAL createContiguous, pre-sync.)
- Q2: regressor = recovery-machinery CLASS (auto-resume). Codex 70% / Gemini high. Mechanism =
  Gemini's BROWNOUT DEATH LOOP: real-use resets are marginal-battery/JST brownouts; pre-resume →
  safe IDLE (30mA, partial preserved); with auto-resume → reboot forces SD-init+FAT+card.erase
  (~200mA spike) → re-crashes marginal rail → thrash ×MAX_RESUMES=25 → corruption DURING FAT writes →
  whole nights lost. Explains 6f6efe8. Hot-path (bounded SPI/RX) ranked LOW + PROTECTIVE (keep).
- Q3 SOLUTION (both): (1) disable Stage B WDT; (2) neutralize SESSION.TXT auto-resume + MAX_RESUMES=0
  → any reset halts at IDLE; (3) KEEP bounded-SPI + RX; (4) spend freed ~412B on a forensic exception
  breadcrumb (_general_exception_handler → EEPROM Cause/EPC/BadVAddr/phase, printed in %BOOT), NOT in
  the sample hot path. Codex: also remove SD-path software resets; re-enable recovery later only if
  SdFat loops are WDT-kicked + WDT disarmed during prealloc/erase + resume gated on committed marker + resumes≤2.

## Implementation plan (this /work) — build-gated phases, 2-model review, commit (NOT flash — user-gated)
P1 (regression fix, a REVERT — low risk): disable wdtArm(); replaySessionFile()→early-return; MAX_RESUMES=0.
P2 (diagnostic — new code): _general_exception_handler breadcrumb → EEPROM + %BOOT print.
Keep bounded-SPI + RX untouched.

## Direction change (user, 2026-07-05): KEEP auto-resume (power now hardwired/solid → death loop can't
## form) + prevent freezes. Phase 1 (disable) abandoned. New branch work/hang-prevent-and-diagnose (off
## Stage B = resume+WDT ON). Bundle = MAX_RESUMES cap + SPI settle + (next) exception breadcrumb → 1 flash.
### Committed increment (Codex+Gemini, 3 review rounds, both APPROVED):
- DEFAULT_MAX_RESUMES 25→3 + HARD CEILING in applyTune (the sole setter; host %TUNE max_resumes=25 was
  overriding the default — R1 catch). R2 BLOCKER (both): clamp-before-reset made rc>=3 clear the counter
  every 3rd resume → infinite thrash; FIXED by reordering (reset checks original 25, clamp after).
- SPI bus-settle delayMicroseconds(5) before each SD write (ADS→SD shared-SPI1 transition) — the one
  low-risk race-prevention bet. Build 118780 B (fits, 4 B margin), RAM unchanged.
### NEXT (same branch → one flash): exception breadcrumb — weak _general_exception_handler override →
### .noinit Cause/EPC/BadVAddr → soft-reset → %BOOT exc=/epc=. Needs the flash reclamation Stage B
### deferred (swap rcv=/g= for exc=/epc=). Mechanism confirmed (exceptions.c weak handler).
