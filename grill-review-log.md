# grill-review-log — Cyton freeSD mid-recording HANG fix (implementation)

Plan: `prep.md` (8-round dual-signed-off). Reviewers: Codex (gpt-5.5) + Gemini (3.1 Pro);
Claude builds + orchestrates, casts no review vote. Board = Cyton B (OFFLINE this session —
no dongle on USB). collect-bci STOPPED. Build as lst, abs sketch path. Flash ceiling = REAL
118784 B (not the nominal 122880). NEVER flash without explicit user OK (brick-risk, no ICSP).

## Build summary (Stage A)
- **Baseline (HEAD 3e93cde, SPIROV fix):** 118360 B (96%), RAM 11552 B.
- **After Phase 0 (universal ADS+accel SPI bounding):** 118148 B — **−212 B** (the shared
  `spiByteBounded` free function replaces duplicated inlined `DSPI::transfer()` calls at 8 sites →
  dedup SHRINKS flash while bounding the prime-suspect path).
- **After + DEVCFG1 %BOOT readout:** **118452 B (96%), 332 B free** under the 118784 ceiling, RAM 11552 B.

## Implementation consults (scoping decision — resolved by me, to be panel-endorsed in review)
**FORK: which phases ship in this flash?** The plan (prep.md) is staged with a HARD gate. Two facts force
the scope:
1. **Phase 1 (the hardware WDT) has an explicit pre-flash gate** — prep.md Decision 7 / Plan step 3:
   "do NOT ship the WDT unless FWDTEN=0 AND WDTPS usable — a pre-flash gate, not a soft risk." WDTPS lives
   in DEVCFG1 @0xBFC00BF8 and **can only be read from a connected board. The board is OFFLINE** (no dongle),
   so the gate's precondition is UNVERIFIABLE pre-flash. The plan's own logic for that case: "ship Phases
   0+2 only (no WDT)."
2. **Phase 2's persistent breadcrumb is INERT without the WDT.** In Stage A there is no WDT, so a freeze
   HANGS until a manual power-cycle = a COLD boot = RAM drains = `__attribute__((persistent))` magic invalid
   = the breadcrumb captures NOTHING. A persistent-RAM breadcrumb only records a phase across a WARM reset,
   which only the WDT (or a soft reset, disabled) produces. So the breadcrumb belongs with Phase 1.

**DECISION (Stage A = the build-able, gate-satisfying scope):**
- **Phase 0 — universal SPI bounding (BUILT):** `OpenBCI_32bit_Library::xfer()` (all ADS reads/writes/
  commands) + the 3 LIS3DH accel helpers now route through the fork's bounded `spiByteBounded` instead of
  the UNBOUNDED `DSPI::transfer()`. This DIRECTLY bounds the prime hang suspect (the per-sample ADS read,
  which had `while(!SPITBE)/while(!SPIRBF)` with no timeout). On a CP0-deadline stall it latches the
  existing sticky `sdSpiFault` + returns 0xFF; the EXISTING top-of-loop `if (sdSpiFault) sdSpiModuleFlush()`
  guard (DefaultBoard.ino:141) flushes the SPI module next iteration → the bus recovers → a faulted sample
  reads 0xFF (one garbage sample per rare wedge, acceptable) instead of a frozen board. If the 7h freeze IS
  the ADS DSPI unbounded wait (the diagnosis), this fix alone makes it recover — no WDT needed.
- **Phase 2 (partial) — DEVCFG1 readout in %BOOT (BUILT):** every %BOOT now records ` wd=0x<DEVCFG1>` so
  the next flash's card REVEALS the actual FWDTEN+WDTPS, making the Stage-B WDT decision data-driven instead
  of inferred. Parser-safe (host ignores unknown trailing %BOOT tokens, Decision 8).
- **DEFERRED to Stage B (hard data dependency, NOT a value-judgment skip):** the hardware WDT (Phase 1) +
  the persistent breadcrumb + the WDT-only recovery machinery (bus-scrub CMD25 abort already partly exists
  as sdBusRecover; footer-authoritative resume; the two boot-loop caps). Stage B is unblocked once Stage A's
  flash reveals WDTPS from the %BOOT readout AND tells us whether Phase 0 already fixed the freeze (a clean
  full overnight slot = fixed, WDT may be unnecessary; a recurring truncation = the hang is NOT the ADS path
  and the WDT/breadcrumb become necessary to locate it).

This staging is dictated by the plan's gate + the breadcrumb's warm-reset dependency, not by effort. To be
endorsed (or challenged — e.g. "build the WDT now with a runtime self-gate") by the Codex+Gemini panel below.

## Review round 1 — Codex: APPROVED | Gemini: CHANGES_REQUESTED
Both ENDORSE the Stage-A scoping (defer WDT to Stage B): Codex — "correct with ~332B free + unreadable
DEVCFG1; gather wd=0x first, avoid unproven reset behavior." Gemini — "a runtime WDT self-gate now is an
unacceptable brick risk (no ICSP; a short WDTPS boot-loops); flash-starved; Phase 0 attacks the root cause;
collect DEVCFG1 telemetry first." Gemini also independently confirmed the two architecture questions:
(a) mixing spiByteBounded (raw SPI1) with DSPI setMode/setSpeed is safe; (b) the loop-ordering + the
spiByteBounded entry short-circuit makes every out-of-band ADS/accel access safe (returns 0xFF, never
touches poisoned/ENHBUF-set registers).
### Codex findings
1. [MINOR] Lib now hard-depends on the SD-fork symbol spiByteBounded (won't link for non-SD sketches).
2. [MINOR] Recovery isn't local to ADS/accel entry points — setup/cmd-handler calls can silently no-op on a sticky fault until the loop guard; flush before non-loop transactions OR prove they can't run mid-fault.
3. [MINOR] DEVCFG1 readout is raw-only — easy to misdecode by eye; document the FWDTEN/WDTPS decode formula.
### Gemini findings
1. [BLOCKER] LIS3DH_read16: `spiByteBounded(0x00) | (spiByteBounded(0x00) << 8)` — UNSPECIFIED C++ operand evaluation order can read MSB before LSB and swap the auto-incremented axis bytes → corrupted accel data. Sequence into locals.
### Resolution (orchestrator)
- **Gemini#1 [BLOCKER] — ACCEPTED + FIXED.** LIS3DH_read16 now reads `byte lsb=spiByteBounded(0x00); byte msb=spiByteBounded(0x00); inData=lsb|(msb<<8);` — explicit order. (Was a latent bug in the stock code too; my call-target swap could have activated it, so fixing it is correct.) Build unchanged (byte-neutral).
- **Codex#3 [MINOR] — ACCEPTED.** Added the exact PIC32MX DEVCFG1 decode formula (FWDTEN bit23, WDTPS bits20:16 ≈2^N ms, WINDIS bit22) as an in-code comment beside the %BOOT readout + here.
- **Codex#1 [MINOR] — REJECTED (documented).** The cross-library extern is an ACCEPTED single-target fork coupling: this is the SD-recording firmware fork; DefaultBoard ALWAYS links OBCI32_SD, and DefaultBoard.ino already externs sdSpiFault/sdSpiModuleFlush from the same fork. A weak-symbol fallback would add complexity + mask a genuinely-missing-fork link error. z13 builds only DefaultBoard. Not worth the flash/complexity for a single-target fork.
- **Codex#2 [MINOR] — REJECTED (proven safe, documented).** The spiByteBounded entry short-circuit (returns 0xFF WITHOUT touching SPI1STAT when sdSpiFault is set) makes a stale-fault ADS/accel read non-corrupting (Gemini's own review (b) confirmed). setup()/replay run BEFORE any SD bulk write (ENHBUF never set then) and replay's card.init/sdBusRecover flushes the fault before the session feeds ADS commands; the only silent-0xFF window is a command-handler ADS read in the same iteration after a faulted SD write — rare, self-healing next top-of-loop, non-corrupting. Adding flushes to every entry point costs flash we don't have for zero correctness gain.
### Tests after this round
Build green: 118452 B (96%, 332 B free under the 118784 ceiling), RAM 11552 B. Static: 0 raw spi.transfer() on the live path (8 sites → spiByteBounded); cp0Past wrap-safe; DEVCFG1 addr/parser-safe.

## Review round 2 — Codex: APPROVED | Gemini: APPROVED  ✅ DUAL SIGN-OFF
### Codex findings
(none — "No net-new findings. Gemini#1 is fixed as shown — LIS3DH_read16 now sequences the two bounded reads into locals before combining, so operand evaluation order can no longer swap LSB/MSB." VERDICT: APPROVED)
### Gemini findings
(none — "The LIS3DH_read16 evaluation order hazard is resolved by explicit sequencing; the DEVCFG1 decode documentation is added accurately. No net-new architecture hazards, SPI state-machine breakages, or flash overheads." VERDICT: APPROVED)
### Resolution (orchestrator)
BOTH reviewers APPROVED with no open BLOCKER/MAJOR. Stage A signed off in 2 rounds (the round-1 accel
byte-order BLOCKER fixed; all MINORs resolved or rejected-with-reason). Both also endorsed the Stage-A
scoping (Phase 0 + DEVCFG1 readout now; WDT + breadcrumb deferred to Stage B once the flash reveals WDTPS).
### Tests after this round
Build green: 118452 B (96%, 332 B free), RAM 11552 B. No code change this round (BLOCKER fix was round 1).

## Deploy = FLASH (USER-GATED — not performed this session)
The "deploy" for this firmware is a `pic32prog` flash of the board. That is the one IRREVERSIBLE,
brick-risk step (no ICSP — a partial/bad write bricks the board, exactly how Cyton A died) and is
explicitly user-gated by prep.md + CLAUDE.md + the standing rule "NEVER flash without explicit user OK."
The board is also OFFLINE this session (no dongle). So this run delivers: reviewed + dual-approved +
build-verified firmware, committed, ready to flash. Post-deploy verification (Act 5) = the USER's flash +
overnight soak (the multi-hour freeze is only reproducible on a real overnight run; cannot be done here).
Flash artifact: build-grill/DefaultBoard.ino.hex. Flash cmd (NEVER a kill-timeout):
  /home/lst/.arduino15/packages/chipKIT/tools/pic32prog/v2.1.46/pic32prog -d /dev/ttyUSB0 -b 115200 <hex>

## FLASHED 2026-06-30 — Cyton B, clean verify ✅
Dongle reconnected (/dev/ttyUSB0, FTDI 0403:6015); collect-bci inactive. Flashed
build-grill/DefaultBoard.ino.hex via pic32prog (detached / NO kill-timeout — the cardinal rule),
caught the bootloader window first try (no reset tap needed):
  Erase: done | Program flash: ############ done | Verify flash: ############ done | 796 B/s | EXIT=0
Data 119756 bytes. Clean verify (contrast Cyton A's repeatable verify failures). Firmware on the board.
Functional verification = the USER's overnight `-a 16` soak (the multi-hour freeze only reproduces on a
real run) — move the dongle switch OFF reset first. Then pull the card and report: (a) the %BOOT wd=0x
value (reveals FWDTEN+WDTPS for the deferred Stage-B WDT decision), (b) clean full slot past 7h (Phase 0
fixed it) vs another mid-sample truncation (hang not the ADS path → build Stage B).

# ===== STAGE B — hardware WDT auto-recovery (2026-07-01) =====
Branch grill/cyton-hang-stage-b (off Stage A). CONTEXT: Stage A flashed + soaked -> OBCI_62 froze
AGAIN at 4.9h (e=0 r=0, same signature) -> the hang is NOT the ADS SPI path. The %BOOT wd= readout
delivered the gate: wd=0xFF6A0D5B -> FWDTEN=0 (WDT arm-able, no brick) + WDTPS=0x0A (~1.02s) + WINDIS=1.

## Build summary (Stage B)
- Removed Stage A's ` wd=0x` %BOOT readout (~304 B — it did its job) to reclaim flash for the WDT.
- WDT-only build: **118536 B (96%, 248 B free under 118784), RAM 11572 B**.
- Breadcrumb DEFERRED: designed but overflows this fragmented flash; the EXISTING resumed-%BOOT `rcv=`
  field is a free coarse substitute (rcv=1 => card wedged => SD-write-path hang; rcv=0 => non-SD hang;
  resume=N counts the hangs).

## Design (WDT-only, minimal-robust)
- The WDT primitive lives in the OBCI32_SD fork (Sd2Card.cpp) so the SD block-write busy-wait
  (waitNotBusy, up to 1500ms > the ~1s WDTPS) can pet it from INSIDE the wait — else one legit slow
  block write false-resets. `petWDT()`: unconditional pet while NOT streaming; while streaming, pet
  ONLY if recording progress (a sample) is within wdtNoProgTicks=4s (CP0, HW-timer, halt-proof). A
  genuine hang stops bumping progress -> petWDT stops -> WDT fires -> reset -> EXISTING
  replaySessionFile() auto-resume salvages the night into the next slot.
- Armed on the stream-start transition (gated on the runtime FWDTEN==0 read), NOT during setup/resume
  (those are already CP0/millis-bounded by Stage A + the SPIROV fix, so they can't freeze; and after a
  WDT reset FWDTEN=0 makes WDTCON.ON revert to 0 -> the WDT is OFF through setup/resume automatically,
  then re-arms on stream-start). resumeCount cap (25, clears on first %CKPT) bounds any rapid loop.
- Static checks all pass (arm-gated on FWDTEN==0; petWDT at top-of-loop + inside waitNotBusy; progress
  stamped on stream-start + each sample; 4s deadline > 1.5s worst write; WDTCON masks correct).

## Stage B — Review round 1 — Codex: CHANGES_REQUESTED | Gemini: CHANGES_REQUESTED
Both CONVERGED on the same BLOCKER + MAJOR (strong signal):
### Findings (Codex / Gemini)
1. [BLOCKER, both] Arm gate ignores WDTPS — only checks FWDTEN==0. A variant board/bootloader with a SMALLER postscale (1-32ms) would false-reset a healthy recording (a 512B block SPI burst isn't petted mid-transfer). Gate on WDTPS too.
2. [MAJOR, both] wdtProgress bumped only on ADS samples, not block writes — a legit multi-block flush (data+FAT+dir, each up to SD_WRITE_TIMEOUT=1.5s) with no sample between can exceed the 4s no-progress deadline -> false-reset mid-write. Bump progress on each block completion (or raise the deadline).
### Resolution (orchestrator) — all ACCEPTED
- **BLOCKER (WDTPS gate) — FIXED.** wdtEnableGate now = (FWDTEN==0) && (WDTPS=(devcfg1>>16)&0x1F) >= 0x0A. Only arms if the HW timeout is >= the validated ~1s; a shorter-postscale board fails closed (ships no-WDT = Stage A behaviour), never risking a false-reset brick. This board (WDTPS=0x0A) passes.
- **MAJOR (block-completion progress) — FIXED.** wdtProgress() now stamps in waitNotBusy's success path (r==0xFF = card ready = a block/op completed). A multi-block flush keeps the deadline fresh; a HUNG write never reaches the success path -> no stamp -> the deadline still trips -> WDT fires. Preserves hang detection.
- **Defensive (proactive):** WDTCONCLR=_WDTCON_ON_MASK as the first act of setup() — the WDT is OFF through the whole setup/replaySessionFile resume path regardless of the "ON reverts to FWDTEN on reset" assumption; re-armed only on stream-start.
### Tests after this round
Build green: 118572 B (96%, 212 B free), RAM 11572 B. WDTCONCLR compiles in the sketch (SFR mask available).

## Stage B — Review round 2 — Codex: APPROVED | Gemini: CHANGES_REQUESTED
### Findings
- Codex: APPROVED (round-1 fixes confirmed real).
- Gemini [MAJOR]: WDT stays armed after stream-stop -> a long idle command (e.g. `?` register dump) blocking loop() >WDTPS without petWDT would false-reset. Since FWDTEN=0, software can/should disable the WDT when idle; then wdtStreaming + the unconditional-pet branch can be removed entirely.
### Resolution (orchestrator) — ACCEPTED (also simplifies)
- Disarm the WDT on stream-stop (new wdtDisarm(): WDTCONCLR ON + wdtArmed=0). The WDT is now armed ONLY while recording -> at idle it is HARDWARE OFF -> no idle-command false-reset. Re-armed on the next stream-start (subsequent same-power-cycle recordings are protected). Removed wdtStreaming + wdtSetStreaming + the non-streaming unconditional-pet branch: "armed" == "streaming", so petWDT is just `if(!armed) return; if(progress recent) pet;`. wdtArm() now stamps progress on arm (folds in the init-gap guard).
### Tests after this round
Build green: 118664 B (96%, 120 B free), RAM 11572 B.

## Stage B — Review round 3 — Codex: APPROVED | Gemini: APPROVED  ✅ DUAL SIGN-OFF
- Gemini: APPROVED (the disarm-on-stop fix confirmed).
- Codex: first returned a spurious CHANGES_REQUESTED that was NOT a code finding — a transient sandbox
  glitch ("the firmware tree is not visible... I cannot verify"; it read the repo fine in R1-R2). Re-ran
  Codex with the code inline-only -> APPROVED. No net-new code findings from either reviewer.
### Resolution
Stage B signed off in 3 rounds. Both reviewers converged R1 on the two real safety holes (WDTPS gate,
block-completion progress), both fixed; Gemini's R2 disarm-at-idle MAJOR fixed + simplified; R3 both APPROVED.
### Tests after this round
Build green: 118664 B (96%, 120 B free under the 118784 ceiling), RAM 11572 B.

## Deploy = FLASH (USER-GATED — not performed by /grill)
Same as Stage A: the flash is the irreversible, brick-risk, user-gated step. Board dongle is connected
(/dev/ttyUSB0) but the flash needs the user's explicit OK. Post-deploy verification = the user's overnight
soak: SUCCESS = the night SALVAGES across chained slots (OBCI_63/64...) with resume=N counting the WDT
recoveries + rcv= discriminating SD-path vs non-SD hangs — instead of dying at one 4.9h slot. A healthy
night must still return as ONE clean slot (no spurious WDT resets). Artifact: build-grill/DefaultBoard.ino.hex.

## Stage B — DEEP review round A (exhaustive false-reset enumeration; xhigh + an independent code-trace subagent)
Attacked the #1 risk: can a HEALTHY recording false-reset? Independent Explore subagent + Gemini both
converged on the SD-ERROR-RECOVERY path (Codex hit a sandbox-read glitch, non-finding).
### Findings (subagent + Gemini)
1. [BLOCKER] Extended recovery window (SD_Card_Stuff.ino:2063-2093, up to tuneExtRecoveryWindowMs=8s): the delay(1) chunk loop doesn't petWDT, and the whole cascade bumps no progress -> false-reset ~5s into a slow-but-healing card's recovery.
2. [BLOCKER] sdBusRecover() busy-waits (SD_Card_Stuff.ino:769,773, up to SD_RECOVER_BUSY_MS=2s): tight `while(spiByteBounded!=0xFF)` with NO petWDT -> a 2s bus recovery (called while streaming/armed) exceeds the ~1s WDTPS -> false-reset.
3. [BLOCKER] card.init CMD0/ACMD41 loops (Sd2Card.cpp) -> **FALSE POSITIVE (verified + rejected):** cardCommand() calls waitNotBusy(300) on EVERY command (which now pets), so each ACMD41 iteration is fed; the CMD0 "loop" has an unconditional goto-fail (one-shot). Gemini didn't trace into cardCommand.
### Resolution (orchestrator)
- #1,#2 ACCEPTED + FIXED: added `petWDT()` inside sdBusRecover's two busy-waits and the extended-recovery
  delay chunk loop (feeds the ~1s HW WDT during a legit bounded recovery). Also raised `wdtNoProgTicks`
  4s -> 14s so the ~11.8s recovery cascade (skip-forward 1.5s + card.init 2.3s + ext window 8s) doesn't age
  progress past the deadline; the 14s CP0 deadline is still the backstop that fires if the recovery genuinely
  HANGS (millis may halt there — CP0 doesn't). Note the coupling: 14s must exceed ext_recovery_window_ms
  (8000 default). Normal recording bumps progress every ~2ms so 14s is never approached.
- #3 REJECTED (false positive, verified against cardCommand -> waitNotBusy).
- Gemini also audited + confirmed SAFE: waitNotBusy, waitStartBlock (300ms), skip-forward retry (pets via
  waitNotBusy), setupSDcard probe (streaming==false, disarmed), sendChannelData/BLE (<3ms).
### Tests after this round
Build green: 118716 B (96%, 68 B free), RAM 11572 B.

## Stage B — DEEP review round B (verify recovery fixes + attack the SALVAGE path; xhigh)
### Findings (Codex / Gemini)
1. [BLOCKER, Codex] wdtProgress() in waitNotBusy is TOO BROAD — fires for cardCommand/card.init too, so a millis-stalled init loop that keeps seeing SPI-ready could keep wdtLastProg fresh and defeat the CP0 no-progress gate. Move the progress stamp to a real recording-block completion.
2. [BLOCKER, Codex] 14s undercounts the recovery envelope — missed sdBusRecover's TWO 2s busy windows; real worst case ~15.8s (skip 1.5 + recover 4 + init 2.3 + ext 8). 14s can still false-reset a healing card.
3. [MAJOR, Codex] Terminal close/footer runs before the next-loop disarm -> a reset mid-FAT-cleanup could corrupt the volume (worse than a data-block reset). Disarm synchronously before close.
4. [MAJOR, Gemini] CS-assertion for boot-time sdBusRecover -> **REJECTED (false positive):** sdBusRecover's FIRST line is board.csLow(SD_SS), and setup() drives SD_SS OUTPUT; CS is asserted. Gemini didn't see the csLow.
5. [MINOR, Gemini] delay(1) safety comment; [MINOR, Gemini] document the 0-byte salvaged-slot expectation.
Both otherwise CONFIRMED the salvage path sound: data preserved (partial CMD25 block cleanly discarded by the card), FAT consistent (pre-alloc slot, resume opens a fresh slot, prior clusters untouched), WDT off through setup/replaySessionFile (WDTCONCLR + not armed), resumeCount cap robust (real multi-hour hang doesn't accumulate; rapid loop caps at 25 -> clean idle), and STRICTLY BETTER than Stage A (rescues the rest of the night vs frozen-at-hang).
### Resolution (orchestrator)
- #1 ACCEPTED+FIXED: removed wdtProgress() from waitNotBusy (kept petWDT there); added it in writeData() on an ACCEPTED block (DATA_RES_ACCEPTED) = the ONLY recording-block-completion stamp. card.init/cardCommand no longer stamp progress -> can't mask a hang.
- #2 ACCEPTED+FIXED: wdtNoProgTicks 14s -> 20s (400000000) to cover the full ~15.8s cascade incl. sdBusRecover's 4s, +4s margin.
- #3 ACCEPTED+FIXED: wdtDisarm() at the START of closeSDfile() (before the FAT/footer writes) -> no reset window mid-close; re-armed on the next stream-start.
- #4 REJECTED (false positive, verified sdBusRecover does csLow first).
- #5 ACCEPTED: delay(1) << WDTPS warning comment added; 0-byte doc -> CLAUDE.md (+ note the host's is_unwritten_txt/build_session_chains already tolerate an empty pre-alloc head slot).
### Tests after this round
Build green: 118740 B (96%, 44 B free), RAM 11572 B.

## Stage B — DEEP review round C (verify deep-B + races/timing/wrap/concurrency; xhigh)
Gemini VERIFIED all deep-B fixes SAFE (progress-stamp move to writeData, 20s deadline, torn-reads/
concurrency [MIPS 32-bit access atomic; DRDY ISR only touches an 8-bit flag; no ISR touches WDT state],
CP0-wrap [unsigned delta correct; a lockup fires at 20s+1s << 215s wrap], first-loop-after-arm [near-zero
delta]). One finding (both Gemini + my own parallel check): wdtDisarm() in closeSDfile leaves the WDT off
if closed mid-stream (streaming stays true at the end-of-slot rollover) -> subsequent slots unprotected.
Codex: APPROVED ("No blocking findings"; MINOR: a stale ~4s deadline comment).
### Resolution (orchestrator)
- closeSDfile disarm GUARDED on `!board.streaming` (fixed before deep-C returned): terminal stop disarms
  (reset-proof FAT cleanup, Codex#3-B); a mid-session rollover (streaming true) stays armed. Verified
  closeSDfile IS called with streaming==true at the >12h "do not stop streaming" rollover site.
- Stale ~4s/14s deadline comments corrected to 20s (Codex MINOR).

## Stage B — FINAL confirmation round — Codex: APPROVED | Gemini: APPROVED  ✅ DUAL SIGN-OFF
Both confirmed on the FINAL guarded code: closeSDfile guard correct (terminal disarms, rollover stays
armed); 20s deadline comments clean; fail-closed FWDTEN&&WDTPS gating correct; wrap-safe CP0 delta sound.
No remaining BLOCKER/MAJOR. Stage B signed off: 3 standard + 3 DEEP + 1 confirmation = 7 rounds.
### Tests after this round
Build green: 118772 B (96%, 12 B free under the 118784 ceiling), RAM 11572 B.
