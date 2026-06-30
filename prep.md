# Prep: Cyton freeSD mid-recording HANG — universal SPI bounding + WDT auto-recovery + persistent breadcrumb

## Goal
Stop the OpenBCI Cyton freeSD board from going permanently dead after an intermittent
**mid-recording MCU hang** (observed: clean recording for a variable 4–7 h, then an abrupt
mid-block halt, board frozen until a manual power-cycle; **zero SD write errors logged the entire
time**). Achieve this in ONE flash by: (1) **bounding ALL shared-SPI waits** (ADS-read AND SD-write)
through ONE shared CP0-deadline primitive so a wedged bus FAULTS instead of spinning, (2) a
**hardware-WDT universal auto-recovery** — armed continuously during recording, petted once per
`loop()` iteration, so ANY stall within an iteration (a wedged FAT walk, a rollover hang, a re-spinning
bus) trips a reset that auto-resumes the night into a fresh slot — and (3) a **`persistent` RAM freeze
breadcrumb** so the next hang self-identifies its phase in the resumed `%BOOT`.

## Context & constraints
- **Hardware/toolchain:** PIC32MX250F128B, 32 KB RAM, chipKIT DP32-style bootloader (no ICSP
  recovery — a bad flash bricks the board, cf. dead Cyton A), SYSCLK 40 MHz → CP0 Count = 20 MHz
  (20 ticks/µs). ONE hardware SPI (DSPI0 → SPI1) shared by ADS1299 reads + SD writes. DRDY ISR only
  sets a flag; all SPI is sequential in `loop()`.
- **Evidence (premise — see `obci_61_freeze_forensics.md`):** a soak on the freshly-flashed SD-write
  SPIROV fix (`3e93cde`) STILL froze at ~7.1 h; ALL 425 `%CKPT` = `e=0 r=0 n=0 o=0 x=0`; identical
  signature to the pre-fix freeze `OBCI_5A` (~4.2 h). Bounding the SD *write* changed nothing and its
  recovery never fired (it increments `e`/`r`; they stayed 0). Board HW + battery FINE (LED
  double-blinks after a power-cycle, SD out). → an **unobserved hang in the normal recording loop,
  not the SD-write SPIROV path.**
- **Likely root cause (discovery):** the ADS1299 sample read calls `board.spi.transfer()` = raw
  chipKIT `DSPI::transfer()`, whose internals are UNBOUNDED (`while(!SPITBE){}`, `while(!SPIRBF){}`
  DSPI.cpp:585/589; `while(toWrite||toRead){}` :630) — no timeout. A shared-bus stall there spins the
  per-sample read forever, mid-recording, with no SD error. Same bug-CLASS as the SPIROV spin, on the
  read side. **NOTE it is a HYPOTHESIS, not proven** — the plan therefore bounds the WHOLE SPI surface
  and adds the breadcrumb to confirm, rather than betting on this one site.
- **FWDTEN=0 inference, now hardened against the "hidden background petter" objection (Gemini#2 —
  audited this session):** the current firmware never pets the WDT (no `WDTCON`/`ClearWDT`/`WDTCLR`
  anywhere in the sketch or library) yet records 7+ h. Gemini#2 correctly noted this only holds if NO
  background ISR pets the WDT (a chipKIT-core timer ISR could) — so the core was statically audited:
  `grep -riE 'WDTCON|ClearWDT|WDTCLR|watchdog'` over the entire chipKIT pic32 core
  (`.../cores/pic32/`, incl. `wiring.c` + the CoreTimer ISR) returns **NOTHING** → the core never
  touches the WDT in the background. ∴ if DEVCFG1 had FWDTEN=1, an unpetted WDT would reset within
  WDTPS and the board could never record for hours → **FWDTEN=0** (WDT off at reset, software owns
  `WDTCONbits.ON`, FWDTEN=1 infinite-reset-brick risk excluded). The same audit confirms the WDT, once
  armed, is **not silently petted by anything but our top-of-loop pet** — so it can actually catch a
  main-loop hang. (The DEVCFG1 read in /grill is still the HARD gate; inference + core-audit corroborate.)
- **WDT mechanics:** config bits (FWDTEN/WDTPS @DEVCFG1 0xBFC00BF8) are bootloader-owned/read-only;
  the **WDTPS timeout is UNKNOWN** (no value in repo history; a prior note's "~1 s" is unverified) so
  the design must be safe for a SHORT worst-case timeout. App can pet
  (`WDTCONSET=_WDTCON_WDTCLR_MASK`) and, with FWDTEN=0, enable (`WDTCONbits.ON=1`). A WDT reset lands
  in the SAME `setup()→replaySessionFile()` auto-resume path the (disabled) soft-reset would have.
  **The bootloader clears RCON before user code** (`rcon=0x00` always) → a WDT reset is
  indistinguishable from a power-on via RCON → the breadcrumb is the only "the board hung" signal.
- **Rapid-fire-erase safety (confirmed):** `resumeCount` (EEPROM[7], cap `tuneMaxResumes`=25) bumps
  BEFORE replay, resets to 0 on the FIRST `%CKPT` of a healthy resume (`SD_Card_Stuff.ino` ~883-898,
  ~1835-1843). Since the hang is multi-hour, a WDT reset → resume yields another full multi-hour
  chunk → a night salvages into 2–3 chained slots, cap never near 25; an immediate-on-resume hang
  stops at 25 (clean idle, `ledReplayFail`), never an infinite loop. **The WDT reset lands in this
  same capped resume path, so it inherits the rapid-fire bound for free** — but /grill must verify the
  cap's *clear* timing has no "<60 s session never clears" pitfall (see `review_output.md` #1).
- **`persistent` attribute:** the pic32gcc toolchain supports `__attribute__((persistent))` — a RAM
  variable it places in a section crt0 does NOT zero, so it survives a warm WDT/soft reset (verified
  by `review_output.md` #2). This is the breadcrumb mechanism (no EEPROM, no wear, no DEE stall).
- **Flash ceiling = 118784 B** (linker reserves a 4 KB DEE-EEPROM + 4 KB splitflash page off the
  nominal 122880). Current `3e93cde` = 118360 B → ~424 B free. Sharing the EXISTING bounded primitive
  for the ADS read is near-zero new flash (the ADS read just routes through `spiByteBounded`); the
  WDT + persistent breadcrumb + serial-drain bound + kill-switch must fit in ~424 B (the persistent
  breadcrumb is RAM + a few bytes, not EEPROM).
- **HOST-CONTRACT strings — NEVER trim/reorder:** `"Size "`, `"META OK/FAIL/ERR"`, `"PERSIST
  OK/FAIL"`, `"TUNE OK/FAIL"`, the `"%Total time"` footer marker, the `%CKPT` keys `t b e r n o x`
  (append-only), `"$$$"`, lib `"Sample rate is …Hz"` + `?` dump. (`"Size "`-trim broke every start in
  2026-06-28.)
- **The soft-WDT / `executeSoftReset(0)` stays DISABLED** (rapid-fire self-reset was a night-eraser,
  rollback `6f6efe8`). The hardware WDT is the sanctioned successor (in-code note DefaultBoard.ino
  ~237).
- **NEVER flash without explicit user OK; NEVER kill-timeout pic32prog** (bricked Cyton A).
- **Validation is overnight/soak only** (user declined a bench injector; the hang is a 4–7 h
  intermittent) → every piece must be conservatively correct by construction; the WDT must NEVER
  false-fire on a healthy night (the single worst outcome on no-ICSP hardware).
- **collect-bci stays STOPPED** (preserve `OBCI_5A`/`OBCI_61` forensics on the card).

## Decisions
1. **All in one flash (user).** Universal SPI bounding + WDT + persistent breadcrumb + serial-drain
   bound + WDT kill-switch, together.
2. **DO NOT revert the SD SPIROV fix — SHARE one bounded SPI primitive across ADS-read AND SD-write
   (panel-overridden user answer).**
   > ASSUMPTION (auto-resolved, overrides the user's "revert it" answer): Codex (#1,#7) AND Gemini
   > (#1,#5) UNANIMOUSLY flagged that reverting restores the raw unbounded `DSPI::transfer()` inside
   > the SD path — which (a) can't be petted (it's inside a system library) so a short-WDTPS reset
   > false-fires mid-CMD25 with ambiguous FAT state, and (b) wastefully re-implements the same
   > bounded technique for the ADS read. SHARING the existing `spiByteBounded` across both paths
   > reclaims flash via dedup (serving the user's flash goal), leaves NO unbounded SPI wait anywhere,
   > and lets a wedged bus FAULT in software (often no WDT reset needed). The user can still override
   > back to "revert" — but the panel's safety case is strong and the flash goal is met either way.
3. **Universal SPI fault, not just ADS.** Inventory EVERY `board.spi.transfer()` / `_spi->transfer()`
   reachable while recording (ADS sample read, ADS register reads, SD path, accel) and route them
   through the shared bounded primitive (Codex#7). On a deadline → set a sticky fault, abort the
   operation cleanly (no torn sample/block), let `loop()` continue; the WDT is the backstop only for
   NON-SPI hangs.
3b. **On a bounded-primitive timeout, flush the SPI module — but ONLY AFTER the owner has released its
    CS (Gemini#3 ordering fix).** A CP0 deadline leaves SPI1 mid-transaction; the next access raises
    SPIROV and wedges all ADS+SD comms → "fault instead of spin" would just relocate the wedge. BUT
    toggling `SPI1CON.ON` while a slave's CS is still asserted forces the SPI pins to PORT state →
    spurious clock edges that desync the slave. So the order is: (1) the leaf primitive sets the sticky
    fault and RETURNS without flushing; (2) the OWNER wrapper deasserts its CS; (3) THEN it calls the
    SPIROV-fix `sdSpiModuleFlush` (clear `SPI1STAT` overflow, drain `SPI1BUF`, toggle `SPI1CON.ON`) on
    an idle bus; (4) restores mode. The sticky fault is CLEARED at the start of the next owner
    transaction (Gemini#6) so a recovered bus retries instead of fast-failing forever.
4. **ADS read returns `sampleValid`; fault cleanup is OWNER-SCOPED for EVERY slave (Codex#5,#6).** A
   mid-sample SPI fault must NOT cache/write/send a torn or stale sample; `updateChannelData()` returns
   a validity flag and the caller skips the sample. The shared leaf primitive can't know which CS is
   asserted, so EVERY SPI slave reachable while recording — ADS sample read, ADS register reads, **the
   accelerometer**, the SD path — gets an owner wrapper that, on the sticky fault, releases its OWN CS,
   flushes the module (3b), restores mode, and propagates the fault. One missed owner can hold the
   shared bus wedged, so /grill enumerates and verifies ALL of them.
5. **WDT discipline (Round-4 reformulation) — pet ONLY on PROVEN FORWARD PROGRESS measured in
   hardware CP0 ticks, NEVER on `millis()`, NEVER on bare loop-iteration (Codex#2,#3 + Gemini#1, a
   convergent BLOCKER).** The masking trap both reviewers found independently: if the unknown hang is
   an interrupts-disabled / CoreTimer-halted state, `millis()` STOPS advancing → a `millis()`-bounded
   petted loop (`waitNotBusy`) spins forever AND keeps petting → the WDT is masked, defeating the fix;
   likewise a bare top-of-`loop()` pet keeps petting while the loop spins making no recording progress
   (a livelock). FIX: drive every bound and the pet decision off **CP0 Count**, which ticks regardless
   of interrupt/ISR state. Concretely: (a) maintain `lastProgressCP0`, updated when a real unit of
   recording work completes (a sample acquired+cached, a 512 B block written); (b) the top-of-loop pet
   is GATED — pet only while `(int32_t)(CP0 - lastProgressCP0)` is under a software no-progress deadline
   (sized > the worst legitimate no-sample-progress stretch = a slot rollover, measured in /grill, and
   fully under our control — NOT the unknown WDTPS); past that deadline, STOP petting → the WDT fires;
   (c) convert `waitNotBusy`'s bound from `millis()` to a CP0 signed-delta deadline, and add a
   max-cluster ITERATION cap to the FAT walks (`chainSize`/`allocContiguous`) so a circular chain
   aborts; call the centralized `petWDT()` (Decision 5b) at CP0/iteration checkpoints inside these
   long-but-bounded loops — the pet/no-pet decision is 5b's gate, NOT a separate "local sub-progress"
   rule (Codex#1-r7). Result: an
   interrupt-disabled hang (CP0 keeps ticking, no progress → no pet → reset), a loop-alive-but-
   acquisition-dead livelock, and any unbounded SPI spin are ALL caught. **`millis()` must not gate any
   WDT pet or any petted-loop bound anywhere.** (The FAT-walk iteration cap is a `uint32_t` bounded
   against the volume's actual cluster count, NOT a small hardcoded number — a FAT32 card has millions
   of clusters; Gemini#3-r5.)
5b. **ONE centralized `petWDT()` enforces the no-progress gate at EVERY pet site; the deadline applies
   ONLY while `isStreaming`; disarm ONLY at a true terminal trap (Codex#1+Gemini#1-r5; Gemini#1,#3,#4-r6).**
   There is exactly one pet inline, used at the top of `loop()` AND inside every bounded long loop:
   ```
   inline void petWDT() {
     if (!isStreaming) { WDTCONSET=_WDTCON_WDTCLR_MASK; return; }       // non-streaming: unconditional liveness
     if ((int32_t)(readCP0() - lastProgressCP0) < NO_PROGRESS_TICKS)    // streaming: ONLY if making recording progress
       WDTCONSET=_WDTCON_WDTCLR_MASK;                                    // else DON'T pet -> WDT fires
   }
   ```
   - **The no-progress gate is applied at ALL streaming pet sites, NOT just top-of-loop (Gemini#3-r6).**
     A wedged-but-retrying SD write that keeps calling `waitNotBusy` must NOT be able to feed the WDT
     "locally" forever — every internal pet routes through the SAME `petWDT()` and so is subject to
     `lastProgressCP0`. `lastProgressCP0` advances ONLY on real recording work (a sample cached / a
     512 B block written), so a retry-storm with no block written stops petting → reset.
   - **`lastProgressCP0 = readCP0()` MUST be set at EVERY point `isStreaming` flips TRUE — the `b` start
     handler AND the auto-resume stream-start path (`replaySessionFile`) (Gemini#4-r6 + Gemini#1-r7).**
     The auto-resume path is the dangerous one: it flips `isStreaming` after long FAT recovery walks, so
     a stale `lastProgressCP0` would instantly trip the no-progress gate on the first `loop()` → an
     immediate false reset → infinite recovery boot-loop. Re-stamp it wherever streaming begins, no
     exceptions (a single helper that sets `isStreaming=true` and `lastProgressCP0=readCP0()` together).
   - **Non-streaming phases** (`setup()`, scrub, `card.init`, FAT ops, command-idle host-wait) pet
     unconditionally via the `!isStreaming` branch — the no-progress deadline measures *recording*
     progress, which legitimately doesn't advance there, so it must not apply (else endless resets
     before recording begins). A true freeze in those phases still stops the loop reaching `petWDT()`
     → reset. `NO_PROGRESS_TICKS` is sized > the worst legit no-recording-progress stretch (a rollover).
   - **Disarm ONLY at a true TERMINAL trap** — `ledReplayFail`/a cap dead-end (a deliberately halted
     board with nothing to recover, so it must not reset-loop). A **clean user stop (`s`) does NOT
     disarm** (Gemini#1-r6): `s` returns to command-idle to await the next `b`, and since the WDT is
     armed only in `setup()`, disarming on `s` would leave a subsequent same-power-cycle recording
     unprotected. Command-idle keeps the WDT armed + petted via the `!isStreaming` branch, so the next
     `b` is protected. So "NEVER disarm" is scoped to recording + recovery + command-idle; only a
     terminal trap disarms.
6. **The hardware WDTPS need only exceed the short pet-to-pet GAP during healthy/legit-long operation;
   the long no-progress catch is the SOFTWARE CP0 deadline we control (resolves Codex#2 + the WDTPS-
   unknown problem).** Because we pet frequently (every loop + inside the bounded long loops on CP0
   sub-progress), the hardware WDT only backstops a TOTAL stall that stops reaching any pet site, so an
   unknown/short bootloader WDTPS is workable WITHOUT lowering `SD_WRITE_TIMEOUT` or chunking rollover.
   The "is the board making recording progress" judgment lives in the software CP0 no-progress deadline
   (point 5b), which we size precisely. **All CP0 math is signed-wrap-safe** (Codex#7-r3): CP0 wraps
   every ~215 s at 20 MHz, so use `(int32_t)(now - deadline) >= 0`, never `now >= deadline`; since the
   per-wait deadlines (≤1500 ms) and the no-progress deadline (a few × a rollover, ≪215 s) each fit
   inside one wrap window, signed-delta is exact. /grill verifies the existing `spiByteBounded` is
   wrap-safe + tests a forced near-wrap, reads DEVCFG1, and gates: if FWDTEN≠0 or WDTPS can't clear the
   max pet-to-pet gap with margin, ship Phases 0+2 only.
7. **FWDTEN=0 is proven by inference (see Context) → no brick risk; HARD-VERIFY DEVCFG1 in /grill
   before flashing** (read-only). If the empirical read somehow shows FWDTEN=1 or an unusable WDTPS,
   do NOT ship the WDT (fall back to SPI-bounding + breadcrumb only) — a pre-flash gate, not a soft
   risk (Codex#2, Gemini#2).
8. **Breadcrumb = `__attribute__((persistent))` RAM only, ADVISORY + cross-validated (Gemini#3,#4 +
   Codex#4,#9,#10; review_output.md#2).** A `persistent struct {uint32_t magic; uint8_t phase, state;}`
   (+ a tiny CRC/complement of `mag` per Codex#4 so a partial-RAM-corruption can't pass as valid):
   `phase` updated free every loop stage, `state`=1 before a risky op / 0 after. `setup()` reads it →
   APPENDS ` hung=<phase>` to the resumed slot's `%BOOT`. No DEE page-erase stall (~20 ms ≈ 20 dropped
   samples at 1000 Hz), no EEPROM wear/map conflict. **`hung=` is ADVISORY, NOT a definitive
   reset-cause discriminator (Codex#4 + Gemini#4):** board capacitance retains SRAM for seconds-to-
   minutes unpowered, so a *fast battery pull-and-reconnect* keeps `magic` valid and would emit a
   `hung=` that was really a power event, not an MCU hang. So the breadcrumb is only TRUSTED when
   corroborated: emit `hung=` only when (magic+CRC valid) AND SESSION.TXT shows a resume AND the prior
   slot ends with NO `%Total time` footer — the no-footer SD file is the authoritative "unclean end"
   signal; the breadcrumb only adds the *phase*. For the untouched soak tests (no one pulls the
   battery) a corroborated `hung=<phase>` is a real hang; the battery-pull false-positive is documented
   for completeness. A cold power-cycle that fully drains RAM → `magic` invalid → no `hung=`.
9. **Resume-thrash cap = consecutive WARM-RESET unclean resumes; cleared by a clean footer OR a COLD
   boot (Codex#3 + Gemini#2 — the cold-boot clear prevents a permanent battery-pull lockout).** Two
   refinements layered on round-3: (i) a naive "clear after 45 s" is defeated by a ≥46 s-recurring hang
   (clears every cycle → unbounded slot churn), so the cap counts *consecutive resumes whose prior slot
   has NO `%Total time` footer*; (ii) but clearing ONLY on a clean footer would permanently lock out a
   user who habitually ends sessions by pulling the battery / yanking the SD (never writes a footer) —
   after 25 such INDEPENDENT normal days the board would refuse to resume forever (Gemini#2). FIX: the
   cap counts only **warm-reset** resumes (persistent-RAM `magic` still valid ⇒ a contiguous rapid-fire
   loop, since SRAM is retained across fast resets) and is **cleared on a COLD boot** (`magic` invalid
   ⇒ a genuinely new user-initiated start, RAM fully drained) **OR a durable clean footer**. So the
   25-cap bounds ONLY a contiguous boot-loop (real multi-hour hang → 2–3 warm chunks/night, never near
   25; a rapid 46 s-thrash → 25 in ~20 min → clean idle `ledReplayFail`) while separate cold-start
   sessions always reset it. The increment is at resume (boot) and the clear is at the footer/cold-boot
   boundary — **never in the steady loop, so no mid-recording DEE/EEPROM stall** (Gemini#4-r3 resolved).
10. **WDT kill-switch `wdt=0/1` (default ON) lives in EEPROM, INDEPENDENT of SESSION.TXT (Gemini#5 +
    Codex#11 — NON-NEGOTIABLE).** Recovering a wedged board by deleting SESSION.TXT must NOT silently
    re-enable the WDT — so the kill-switch is stored in the EEPROM tune store (which already backs
    `%TUNE`), not only in SESSION.TXT, so it persists across a session-file wipe and across the resume.
    (The resume-thrash cap independently self-terminates any boot-loop at 25, so recovery never
    *depends* on the kill-switch — but a user who finds the WDT subtly wrong can disable it for good
    without a reflash.) Drop finer breadcrumb phases before EVER dropping this.
10b. **Arm the WDT BEFORE the first SD/FAT access on the recovery path, not after (Codex#1-r4
    sharpens Codex#1-r3).** A WDT reset lands in `setup()→replaySessionFile()`, but even READING
    SESSION.TXT to "confirm a resume" requires `card.init`/`openRoot`/FAT on the same torn SD state — a
    hang there is before the WDT is armed → permanent freeze. So **arm the WDT immediately after minimal
    CS setup, BEFORE the first SD/FAT recovery access**, with the **unconditional non-streaming
    `petWDT()`** (Decision 5b — `card.init`/FAT are NOT streaming, so the no-progress gate does not apply
    there; the pet is unconditional + the loops carry their own CP0/iteration bounds; Gemini#3-r7).
    Bound the early-boot phase with an **EEPROM pre-session attempt
    counter** (incremented before the SD is touched) so a card.init-stage hang loop is bounded
    independently of the per-slot resume-thrash cap; at the attempt cap → clean idle (`ledReplayFail`).
    ⚠ **This counter bounds ONLY the early SD-recovery PHASE and is cleared the instant that phase
    SUCCEEDS — `card.init` + the resume decision complete — NOT at "a clean streaming start" (Gemini#2-r6),
    and also on a COLD boot (`magic` invalid; Gemini#2-r5).** The phase-success clear is the load-bearing
    fix: EVERY mid-recording WDT hang recovery is itself a WARM reset, so if the attempt cap cleared only
    on a clean stream/cold-boot it would accumulate across the night's legitimate multi-hour-salvage
    recoveries and falsely trip `ledReplayFail`, defeating the whole salvage. By clearing it the moment
    `card.init`+resume-decision succeed, it only accrues if the board hangs WITHIN that specific early
    phase (a genuine card.init boot-loop) — a mid-recording hang re-inits the card fine → cap cleared →
    the SEPARATE resume-thrash cap (Decision 9) is what bounds the salvage-chain length. The cold-boot
    clear additionally stops a casual no-SD / settings-check user from ever accumulating it. The two caps
    are thus cleanly separated: attempt cap = contiguous early-phase failures; thrash cap = consecutive
    unclean resume chunks.
10c. **Resume BUS SCRUB = a REAL bounded CMD25 abort, using the bounded primitive, UNDER the armed WDT
    (Codex#5 + Gemini#3 — sharpens Codex#4-r3).** A WDT reset runs no owner cleanup, so the SD may be
    left mid-multiblock-write awaiting a stop-tran token and the ADS mid-frame. Idle clocks + `card.init`
    alone may NOT terminate that state → resume loops/fails on the same SD state. The scrub MUST: (a)
    run ONLY through the Phase-0 bounded primitive (never raw `transfer()`) and AFTER the WDT is armed
    (10b), so a wedged bus during the scrub itself faults/recovers instead of freezing or false-firing;
    (b) deassert all CS, send idle clocks, then a proper interrupted-CMD25 abort — assert SD CS, send
    the **stop-tran token**, bounded `waitNotBusy`, THEN full `card.init`; (c) issue ADS `SDATAC`/reset;
    (d) if the abort/reinit can't complete within its bounds → fail to clean idle (don't loop forever
    on a dead card). Only then does the resume write. ⚠ **(e) NO un-petted blocking in the WDT-armed
    window (Gemini#2-r7):** the ADS reset/`SDATAC` sequence (and any `setup()` step after arming) uses
    hard `delay()` calls (>100 ms for oscillator settle) that don't pet — with a short WDTPS an un-petted
    `delay()` here false-fires the WDT → an infinite recovery boot-loop. So every `delay()`/blocking wait
    in the WDT-armed portion of `setup()`/scrub MUST be a CP0-bounded loop that calls `petWDT()` (the
    unconditional non-streaming branch keeps it fed), not a bare `delay()`. /grill audits for any bare
    `delay()`/spin between WDT-arm and the first `loop()` and converts it.
10d. **A clean `%Total time` footer is AUTHORITATIVE — cancel any stale resume (Codex#4-r4 — new
    MAJOR).** A WDT reset that fires AFTER the footer is durably written but BEFORE SESSION.TXT is
    cleaned leaves a clean prior slot + stale session state; keying resume on SESSION.TXT-exists alone
    would wrongly auto-resume an already-finished session. FIX: the resume decision is "SESSION.TXT
    present AND the last slot has NO clean footer." A present footer ⇒ the session ended cleanly ⇒
    clear the thrash cap, delete/ignore the stale SESSION.TXT, and DO NOT auto-resume.
11. **Bound the recovery serial-drain loops** (`SD_Card_Stuff.ino` ~2069-2070,
    `while(board.hasDataSerialN()) feedEscape(...)`) with an iteration/CP0 cap (latent unbounded loop;
    only on SD-error recovery so not OBCI_61's hang, but close it while here).
12. **No bench hang-injector (user).** Validation = the SD `%BOOT` breadcrumb + slot-chain pattern
    after real overnight/soak runs → the plan must be self-evidently correct.

## Plan
**Phase 0 — share the bounded SPI primitive across ALL live SPI (NO revert).**
1. Keep the OBCI32_SD fork's bounded primitives (`spiByteBounded` etc.). Inventory every
   `board.spi.transfer()` / `_spi->transfer()` callsite reachable during live recording
   (`OpenBCI_32bit_Library.cpp` ADS sample read + register reads + accel; the SD fork). Route them all
   through the shared CP0-deadline-bounded primitive; on deadline set the per-path sticky fault.
2. `updateChannelData()` (and ADS register reads on the live path): replace raw `board.spi.transfer()`
   with the bounded read; return `sampleValid`; on fault, reset ADS CS/SPI and skip the sample (no
   torn/stale cache write).

**Phase 1 — hardware WDT auto-recovery (gated; armed across recording + resume, pet at top-of-loop
AND inside bounded loops).**
3. /grill FIRST reads DEVCFG1 (`*(volatile uint32_t*)0xBFC00BF8`), decodes FWDTEN+WDTPS, and HARD-FAILS
   (ships Phases 0+2 only, no WDT) unless FWDTEN=0 AND the decoded WDTPS exceeds the (short) max
   pet-to-pet gap with margin (Decision 6). Also re-confirm the core-ISR audit (no background WDT
   petter) holds for the pinned core version, and that the bounded primitive's deadline math is
   signed-wrap-safe across the ~215 s CP0 wrap (Codex#7).
4. Arm the WDT (`WDTCONbits.ON=1`, gated by the EEPROM `wdt` tune) BEFORE the first SD/FAT access in
   both the cold and resume `setup()` paths (Decision 10b). Add the single `petWDT()` inline (Decision
   5b) — no-progress-gated while `isStreaming`, unconditional otherwise — and call it at top-of-`loop()`
   AND at every internal bounded-loop pet site (so a retry-storm can't feed the WDT locally). Bump
   `lastProgressCP0` on each sample cached / 512 B block written, and set `lastProgressCP0=readCP0()`
   when `isStreaming` flips true. Convert `waitNotBusy` to a CP0 signed-delta bound (NOT `millis()`);
   add a `uint32_t` cluster-count cap to `chainSize`/`allocContiguous`. Disarm ONLY at a terminal trap
   (NOT on a clean stop); `millis()` gates NOTHING WDT-related.
5. Inventory EVERY blocking loop reachable while the WDT is armed (all SdFat init/command/token/seek
   loops, not just `waitNotBusy`+the two FAT walks) and give each a CP0 or iteration bound (Codex#3-r4).
   Do NOT lower `SD_WRITE_TIMEOUT` and do NOT chunk rollover into a state machine (Decision 5/6 — both
   rejected). Add the EEPROM pre-session attempt counter (Decision 10b) bounding a card.init-stage loop.
6. A WDT reset → `setup()` → arm WDT → bus scrub with a real bounded CMD25 abort (Decision 10c) →
   resume decision = SESSION.TXT present AND last slot has NO clean footer (Decision 10d; a clean footer
   cancels replay + clears the cap + deletes stale SESSION.TXT) → resume under the warm-reset
   consecutive-unclean thrash cap, cleared on a clean footer OR a cold boot (Decision 9).

**Phase 2 — persistent breadcrumb + %BOOT.**
7. Add `static __attribute__((persistent)) struct {uint32_t magic; uint8_t crc; uint8_t phase, state;}
   bc;` — the `crc` (a complement/checksum of `magic`+`phase`+`state`) rejects partial-RAM-corruption
   so a stale half-valid struct can't pass (Codex#6, matching Decision 8). In `setup()`: if
   `bc.magic==MAGIC` AND `crc` checks → prior run WARM-reset (WDT/soft). Then re-stamp `magic`/`crc`,
   `phase=BOOT`.
8. Update `bc.phase`/`crc` at each coarse loop stage; APPEND ` hung=<phase> st=<state>` to `%BOOT`
   **only when corroborated** — magic+crc valid AND SESSION.TXT resume AND the prior slot has NO
   `%Total time` footer (Decision 8). Never reorder existing `%BOOT` fields (host ignores unknown
   trailing tokens). Clear to a clean code on a normal stop/footer.

**Phase 3 — serial-drain bound + kill-switch + budget.**
9. Cap the `while(board.hasDataSerialN())` drains with an **iteration or CP0** bound (NOT `millis()` —
   matches Decision 11 + the static test contract; Codex#2-r5).
10. Wire `wdt=0/1` into the %TUNE parser (host-contract `TUNE OK/FAIL`), default ON, persisted in the
    **EEPROM tune store independent of SESSION.TXT** (Decision 10 — survives a session-file wipe).
11. Build green under **118784 B**; report per-component bytes. If over, drop order (kill-switch and
    WDT + SPI-bounding are NON-negotiable): finer breadcrumb phases → the DEVCFG1 logging line.

## Files to touch
- `~lst/Arduino/libraries/OpenBCI_32bit_SD/utility/Sd2Card.cpp` (fork) — KEEP bounded primitives;
  ensure they're the shared path (no revert); convert `waitNotBusy` to a CP0 signed-delta bound; add
  the CMD25 stop-tran abort to the resume scrub.
- `~lst/Arduino/libraries/OpenBCI_32bit_SD/utility/SdFile.cpp` + `SdVolume.cpp` (fork) — the
  `uint32_t` cluster-count caps on `chainSize`/`allocContiguous` (and any seek/init loops) live HERE,
  not in `SD_Card_Stuff.ino` — add them to the touch set so /grill doesn't under-patch (Codex#3-r5).
  /grill greps the OBCI32_SD `utility/` for every blocking `while`/`for` reachable under the armed WDT.
- `OpenBCI_32bit_Library.cpp` / `.h` — route ADS sample + register reads through the bounded
  primitive; `updateChannelData` returns `sampleValid`; breadcrumb phase at the sample-read stage.
- `examples/DefaultBoard/DefaultBoard.ino` — arm the WDT BEFORE the first SD/FAT access in `setup()`;
  the centralized `petWDT()` inline (no-progress gate while `isStreaming`, unconditional otherwise) used
  at top-of-`loop()` and everywhere; `lastProgressCP0=readCP0()` on the `b` start handler; disarm ONLY
  at a terminal trap (NOT on clean stop `s`); `persistent` breadcrumb declare + `setup()` check +
  corroborated `%BOOT` append; DEVCFG1 read; keep current SPIROV externs.
- `examples/DefaultBoard/SD_Card_Stuff.ino` — breadcrumb phase at SD-write/%CKPT/FAT/recovery stages;
  the centralized `petWDT()` at all internal pet sites + terminal-trap-only disarm; arm-WDT before the
  first SD access + the CMD25-abort bus scrub on the resume path; footer-authoritative resume decision;
  bound the recovery serial-drain (iteration/CP0); `wdt` tune key in the EEPROM store; the warm-reset
  consecutive-unclean thrash cap (cold-boot/footer-cleared) + the pre-session attempt cap (cleared on
  early-phase success AND cold boot).
- `CLAUDE.md` — document the re-diagnosis, the shared-SPI bounding, the WDT design + FWDTEN=0 proof,
  the persistent breadcrumb + new `%BOOT` field + `wdt` tune key.
- `patches/sd2card-spirov-fix.{cpp,patch}` — keep/refresh (no revert).

## Test plan
- **Build** green under **118784 B** (per-component byte report), as lst with the ABSOLUTE sketch path.
- **Static correctness (panel code-trace, no injector):** (a) the WDT is armed BEFORE the first SD/FAT
  access in both setup paths and is disarmed ONLY at a terminal trap (NOT on a clean stop `s`, which
  returns to command-idle awaiting the next `b`); ALL pets route through ONE `petWDT()` whose
  no-progress deadline applies at EVERY pet site while `isStreaming` (so a wedged-but-retrying SD write
  cannot feed the WDT locally), and which pets unconditionally while not streaming so setup/scrub/
  `card.init`/command-idle can't false-reset; `lastProgressCP0` is re-stamped at EVERY `isStreaming`→true
  site (the `b` handler AND the auto-resume path) so a recovery walk's CP0 ticks can't instantly trip
  the gate; NO bare un-petted `delay()`/spin exists between WDT-arm and the first `loop()` (the ADS
  reset/scrub delays are CP0-bounded `petWDT()` loops); `millis()` gates no pet and no bound anywhere; the no-progress deadline is sized > worst rollover,
  `waitNotBusy` uses a CP0 signed-delta bound, and EVERY blocking loop reachable under the armed WDT (all
  SdFat init/cmd/token/seek + the FAT walks, `uint32_t` cluster-count caps) has a CP0 or iteration bound
  — so an interrupt-disabled / CoreTimer-halted hang cannot be masked (NO `SD_WRITE_TIMEOUT` lowering, NO
  rollover state machine); (b) every SPI blocking point on the live path
  is a bounded primitive with wrap-safe deadline math (CP0 215 s wrap), and its timeout path releases
  the owning CS FIRST, THEN flushes the SPI module (Decision 3b/4), for ALL slaves (ADS/register/accel/
  SD); (c) NO raw `transfer()` remains on the live path; (d) a WDT reset reaches `replaySessionFile()` →
  WDT armed → real bounded CMD25 abort/scrub → footer-authoritative resume decision (10d) → resume under
  TWO separate caps: the resume-thrash cap (consecutive unclean resume chunks, cleared on a clean footer
  OR cold boot) and the pre-session attempt cap (cleared the instant `card.init`+resume-decision SUCCEED,
  so mid-recording hang recoveries don't accumulate it and defeat the multi-hour salvage — Gemini#2-r6,
  also cold-boot-cleared); neither permanently locks out a casual or battery-pull user; (e) the
  `persistent` breadcrumb
  survives a warm reset on this toolchain, magic+CRC excludes cold-boot/partial-corruption false
  positives, and `hung=` is emitted ONLY corroborated by a no-footer prior slot (Decision 8); (f) the
  ADS bound never caches a torn/stale sample nor corrupts 500/1000 Hz timing; (g) `%BOOT` append is
  host-parser-safe; (h) NO EEPROM write in the steady loop — the thrash-cap increment is at resume and
  its clear is at the footer/stop boundary, neither mid-loop (no DEE stall).
- **Empirical (in /grill, read-only on the board):** DEVCFG1 → FWDTEN=0 + usable WDTPS (gate).
- **Hardware (user-gated, post-flash):** overnight/soak. SUCCESS = a clean full-night footer, OR — on
  a hang — the night SALVAGED as 2–3 chained slots whose resumed `%BOOT` carries `hung=<phase>` naming
  the site. A healthy night MUST return as ONE clean slot (no spurious WDT resets) — the gating check.
- **Regression:** a normal recording is byte-identical except the appended `%BOOT` field + the
  already-shipped footer trim; `%CKPT` keys unchanged; collect-bci chain-stitching still groups slots.

## Risks & open questions
- **R1 — Can't bench-validate (no injector, 4–7 h intermittent).** Mitigation: conservative-by-
  construction + heavy panel code-tracing; the WDT is the simplest auditable piece; the `wdt`
  kill-switch is the escape hatch.
- **R2 — WDTPS unknown / short, AND `millis()` can halt.** Two layered guards: the hardware WDTPS need
  only exceed the short pet-to-pet gap (we pet on CP0 sub-progress inside the bounded long loops), and
  the "is the board making recording progress" judgment is the SOFTWARE CP0 no-progress deadline we
  control. Critically every petted-loop bound and the pet gate use CP0 (halt-proof), NEVER `millis()`,
  so an interrupt-disabled hang can't mask the WDT (Codex#3/Gemini#1-r4). Residual: an un-petted stretch
  we failed to enumerate exceeds WDTPS — mitigated by the /grill inventory of ALL armed-WDT blocking
  loops + the DEVCFG1 gate (abort the WDT if WDTPS can't clear the max gap with margin).
- **R3 — WDT false-fire erases/fragments a healthy night (WORST case).** Guards: WDTPS > max pet-to-pet
  gap (R2) + every petted loop independently bounded + the consecutive-unclean (footer-clear) thrash
  cap (so even a false-fire LOOP self-terminates at 25 → clean idle) + the EEPROM `wdt` kill-switch.
  The panel must confirm no legit un-petted stretch can exceed WDTPS.
- **R8 — bounded-primitive timeout leaves SPI wedged (Gemini#3).** Without the module reset (3b) a
  fault would raise SPIROV on the next access and relocate the wedge. Mitigation: the fault path
  reuses `sdSpiModuleFlush`; /grill verifies the next SPI op after a forced fault succeeds.
- **R4 — ADS-read-is-the-hang is unproven.** Mitigated by bounding the WHOLE SPI surface (not just
  ADS) + the WDT for non-SPI hangs + the breadcrumb to confirm. If the breadcrumb later fingers a
  different phase, that's the WIN (we finally know) + a targeted follow-up.
- **R5 — `persistent` survives a brownout that preserves RAM → could mislabel.** Magic + the SD
  no-footer cross-check bound this; a cold power-cycle clears RAM → magic invalid → no false `hung=`.
- **R6 — `resumeCount` clear-timing pitfall** (review_output.md#1 + Codex#3 + Gemini#2): the cap must
  bound a rapid resume-thrash WITHOUT tripping on legit multi-hour chunked salvage, short clean manual
  sessions, OR a habitual battery-pull user across separate days. Resolved (Decision 9): count only
  warm-reset (magic-valid) consecutive no-footer resumes; clear on a clean footer OR a cold boot (magic
  invalid) — so the 25-cap bounds only a contiguous boot-loop, never independent cold-start sessions.
- **R7 — Sharing the primitive misses a live SPI callsite.** Mitigation: exhaustive /grill inventory
  of every `transfer()` reachable while recording; the WDT backstops any miss.

## Out of scope
- A bench hang-injector / synthetic self-test (user declined).
- Re-enabling the soft-WDT / `executeSoftReset` (stays disabled).
- Editing chipKIT system libraries / linker script / DSPI.cpp (bound at the OpenBCI-library callsites).
- Chasing a hardware/electrical (JST/power) root cause — board+battery tested fine; firmware hang.
- Changing host `collect_bci`/`openbci_functions` ingest (already chains slots, tolerates unknown
  trailing `%BOOT` tokens).
- Flashing — user-gated, separate from planning.
