# prep-review-log — Cyton freeSD mid-recording HANG fix (WDT + bounded ADS-read + breadcrumb)

Plan: `prep.md` (this repo). Re-diagnosis evidence: `obci_61_freeze_forensics.md`.
Reviewers: Codex (gpt-5.5/xhigh) + Gemini (3.1 Pro). GLM not on z13.
Builder/orchestrator: Claude (no review vote). Backstop: 7 rounds, adaptive early-stop.

## Round 1 — Codex: CHANGES_REQUESTED | Gemini: CHANGES_REQUESTED
### Codex findings
1. [BLOCKER] Reverting bounded SD SPI makes the WDT reset mid-CMD25 with ambiguous FAT state (≠ returning an error).
2. [BLOCKER] DEVCFG1/WDTPS not a soft risk: if FWDTEN=1 the kill-switch is dead; if WDTPS short a healthy night resets. Gate flashing on verified FWDTEN=0 + measured timeout.
3. [BLOCKER] Slot setup/rollover (card.init/createContiguous/erase/writeStart) legitimately exceed a short timeout; top-of-loop pet doesn't cover code already inside them.
4. [BLOCKER] FAT root walks (chainSize/openRoot) are unpetted long loops reachable during replay/setup/slot ops.
5. [BLOCKER] allocContiguous cluster-linking is another unpetted long loop.
6. [MAJOR] ADS timeout needs a hard sample-valid contract — don't cache/write/send a torn/stale sample; reset ADS CS/SPI on fail.
7. [MAJOR] Bounding only the ADS sample path leaves other live raw-DSPI callsites able to hang; centralize ALL through one bounded primitive.
8. [MAJOR] Pet semantics underspecified: petting inside a blind wait hides a real hang; not petting inside known-progress FAT loops false-resets. Pet only on proven progress.
9. [MAJOR] EEPROM breadcrumb atomicity/wear/map incomplete; DEE writes tear; clear "active" only after durable footer.
10. [MAJOR] RAM no-init not a reliable discriminator (SRAM may survive brownout; stale magic mislabels a power-cycle). Treat as advisory; require EEPROM/session evidence.
11. [MAJOR] Don't drop the wdt kill-switch for budget — it's part of false-fire mitigation, non-negotiable.
12. [MAJOR] Plan overfit to ADS-read; safer: keep SD bounded, bound ADS, breadcrumb, compile WDT but prove WDTPS+pet coverage.
### Gemini findings
1. [BLOCKER] Reverting restores raw unbounded DSPI in the SD path → can't pet inside the system lib → a short WDTPS false-fires on legit FAT alloc / slot rollover. DON'T revert; SHARE the bounded primitive across ADS+SD (reclaims flash via reuse, lets a SPI hang fault).
2. [BLOCKER] If FWDTEN=1 the one-flash deploy bricks (infinite reset in setup/handshake/idle, unpetted). Can't discover DEVCFG1 in the same flash; defensively pet everywhere.
3. [MAJOR] chipKIT precompiled crt0 likely zeroes all .bss → RAM no-init breadcrumb destroyed on recovery. Use a known-safe absolute address (or — Claude: __attribute__((persistent))).
4. [MAJOR] EEPROM DEE page-pack erase stalls CPU ~20 ms → drops ~20 EEG samples at 1000 Hz on the 60 s flush. Only write EEPROM on clean transitions / synchronously on fault, never periodically.
5. [MINOR] Reverting then re-implementing the bounded technique for ADS is wasteful — share one primitive.
### Resolution (Claude)
- Codex#1 / Gemini#1 / Gemini#5 ACCEPTED (overrides the user's "revert" answer, tagged ASSUMPTION in prep.md Decision 2): do NOT revert; SHARE one bounded `spiByteBounded` across ADS-read AND SD-write. Dedup reclaims the flash the revert was for; no unbounded SPI remains; a wedged bus faults in software.
- Codex#2 / Gemini#2 RESOLVED: added the FWDTEN=0 PROOF-BY-INFERENCE (current firmware never pets the WDT yet records 7 h → WDT must be off at reset → FWDTEN=0 → no brick) + a HARD pre-flash DEVCFG1 verify gate in /grill (Decision 7); if it ever shows FWDTEN=1/unusable WDTPS, ship without the WDT.
- Codex#3/#4/#5 ACCEPTED (Decision 6): WDT armed ONLY in the steady sample+write loop; DISARMED around every long FAT/SD op (card.init/createContiguous/allocContiguous/chainSize/openRoot/erase/writeStart/rollover) + boot/handshake/idle/replay. Enumerate + gate each in /grill.
- Codex#6 ACCEPTED (Decision 4): updateChannelData returns sampleValid; no torn/stale cache; reset ADS CS/SPI on fault.
- Codex#7 ACCEPTED (Decision 3): inventory + route EVERY live `board.spi/_spi->transfer()` through the shared bounded primitive, not just the ADS sample read.
- Codex#8 ACCEPTED (Decision 5): pet on PROVEN PROGRESS only; bounded primitives return ≪ WDTPS so need no internal pet; never pet inside a raw spin (none remain).
- Codex#9 / Gemini#4 RESOLVED (Decision 8): DROP the EEPROM breadcrumb entirely → no DEE stall, no wear, no map conflict.
- Codex#10 / Gemini#3 RESOLVED (Decision 8, using review_output.md#2): breadcrumb = `__attribute__((persistent))` RAM `{magic,phase,state}` (toolchain-supported, survives warm WDT reset); magic excludes cold-boot false positives; SD no-footer is the cross-check; treated advisory for the rare brownout-preserves-RAM edge.
- Codex#11 ACCEPTED (Decision 10): the `wdt` kill-switch is non-negotiable; drop finer breadcrumb phases first.
- Codex#12 PARTIALLY ACCEPTED: bounding the WHOLE SPI surface (#3) + the WDT for non-SPI hangs + the breadcrumb to confirm removes the ADS-overfit; the breadcrumb is explicitly there to TEST the hypothesis, not bet on it (Context + R4).
- Added (review_output.md#1): boot-thrash via the EXISTING resumeCount cap (the WDT reset lands in it) + verify the clear-timing pitfall, switch to a 45 s-active clear if needed (Decision 9, R6).
### prep.md changes this round
Rewrote Goal/Context/Decisions/Plan/Files/Test/Risks: title → "universal SPI bounding + WDT + persistent breadcrumb"; no-revert + shared primitive; FWDTEN=0 proof + hard DEVCFG1 gate; WDT arm-only-steady / disarm-around-long-ops / pet-on-progress; sampleValid; drop EEPROM breadcrumb → persistent RAM; non-negotiable kill-switch; resumeCount-reuse boot-thrash.

## Round 2 — Codex: CHANGES_REQUESTED | Gemini: CHANGES_REQUESTED
### Codex findings
1. [BLOCKER] Armed steady loop STILL has SD busy-waits (CMD25 multi-block card-busy, SD_Card_Stuff ~1253-1263); a short WDTPS can false-reset a healthy SD write. Fix: WDT off for every SD block/writeStop/sync OR prove all SD waits are hard-deadlined below WDTPS.
2. [BLOCKER] "Disarm every long FAT/SD op" not auditable — writeReplayFail() 744-778, replay tail 1005-1008 also do FAT work; centralize WDT-off inside the SD wrappers.
3. [BLOCKER] Rollover is an unprotected dead zone (card.init 1838/1899, CS-held 1790-1967) — a hang there is never recovered; make rollover a bounded state machine or preallocate the next slot.
4. [MAJOR] Persistent magic is not a reset-cause discriminator; require magic+CRC + SESSION.TXT resume + prior-slot no-footer before labeling hung=.
5. [MAJOR] Shared byte primitive != SPI ownership contract — a timeout must release the owning CS + restore mode; add owner-scoped transaction wrappers.
6. [MAJOR] FWDTEN inference weaker than the gate — make the pre-flash tool hard-fail on a DEVCFG1 decode mismatch.
7. [MAJOR] resumeCount clear-timing still deferred — change to ">=45s active streaming" NOW.
### Gemini findings
1. [BLOCKER] WDT disarm creates a permanent freeze window in FAT ops (circular cluster chain -> infinite FAT walk -> board frozen forever). DON'T disarm; pet inside the FAT loops; verify WDTPS>500ms.
2. [BLOCKER] FWDTEN=0 inference ignores background ISR petting — a chipKIT-core CoreTimer ISR could implicitly pet the WDT, making it useless for main-loop hangs; must statically audit the core.
3. [BLOCKER] Bounded SPI abort leaves hardware wedged — on a CP0 timeout SPI1 is left incomplete -> next call triggers SPIROV; MUST reset the SPI peripheral before returning.
4. [MAJOR] SRAM retention mislabels battery pulls as hangs (capacitance retains SRAM seconds-minutes; fast battery-swap keeps magic valid) — require SD no-footer cross-validation or document false hangs.
5. [MAJOR] resumeCount clears on %CKPT punishing short sessions — fix NOW (45s clear), don't defer.

### Resolution (Claude)
- **Codex#1/#2/#3 + Gemini#1 — KEY TENSION resolved by FLIPPING the WDT strategy.** Codex says "disarm around long ops"; Gemini says "DON'T disarm (disarm = a permanent freeze window for a circular FAT chain)". Both critiques of round-1's disarm-enumeration are correct: it is (a) not auditably enumerable (Codex#2), (b) a dead zone during rollover (Codex#3), and (c) a permanent freeze window in a corrupt-FAT infinite walk (Gemini#1). RESOLUTION (Decision 5/6, fully rewritten): **never disarm; pet EXACTLY ONCE at the top of loop(); never pet inside any sub-loop/waitNotBusy/FAT walk.** This makes the WDT contract "every loop() iteration completes within WDTPS" — any hang within one iteration (SPI re-spin, circular FAT walk, rollover wedge) prevents the next top-of-loop pet -> reset. No enumeration needed (kills Codex#2), no dead zone (kills Codex#3), and a circular FAT loop IS caught precisely because we DON'T pet inside it (kills Gemini#1). Codex#1's SD busy-wait is handled by the new constraint: WDTPS MUST exceed the worst legit single iteration; SD_WRITE_TIMEOUT=1500ms (verified in Sd2Card.cpp:32) is the dominant term, so /grill measures WDTPS+worst-iteration and lowers SD_WRITE_TIMEOUT and/or chunks rollover to fit (Decision 6).
- **Gemini#2 (background ISR petting) — ACCEPTED + AUDITED THIS SESSION.** grep -riE 'WDTCON|ClearWDT|WDTCLR|watchdog' over the entire chipKIT pic32 core (cores/pic32/, incl. wiring.c + CoreTimer ISR) returns NOTHING -> no background petter. This both (a) keeps the FWDTEN=0 inference valid (nothing hidden explains 7h survival with WDT on) and (b) confirms the armed WDT can actually catch a main-loop hang. Folded into Context + Decision 7 + Plan step 3.
- **Gemini#3 (bounded abort leaves SPI wedged) — ACCEPTED, new Decision 3b.** On a CP0 timeout the fault path MUST reset the SPI module (reuse the SPIROV fix's sdSpiModuleFlush: clear SPI1STAT overflow, drain SPI1BUF, toggle SPI1CON.ON) before returning, else the next access raises SPIROV and relocates the wedge. New risk R8.
- **Codex#5 (owner CS not released) — ACCEPTED, folded into Decision 4.** Two-level cleanup: primitive resets the SPI module (3b); the CALLER releases its own CS + restores mode (ADS path raises ADS CS, SD path chipSelectHigh). The leaf byte helper can't own a CS, so the caller must.
- **Codex#4 + Gemini#4 (breadcrumb mislabels battery-pull as hang) — ACCEPTED, Decision 8 downgraded to ADVISORY.** SRAM capacitance retains magic for seconds-minutes, so a fast battery pull-and-reconnect keeps magic valid. hung= is now emitted ONLY when corroborated: magic+CRC valid AND SESSION.TXT resume AND prior slot has NO %Total time footer (the authoritative unclean-end signal). Documented the battery-pull false-positive; for untouched soak tests a corroborated hung= is a real hang.
- **Codex#7 + Gemini#5 (resumeCount clear) — ACCEPTED, fixed NOW not deferred.** Decision 9 now firmly specifies the clear moves from %CKPT to ">=45s of active streaming" (millis() delta), removing the short-session accumulation hole. Plan step 6 implements it (not "verify and maybe fix").
- **Codex#6 (FWDTEN gate hard-fail) — ACCEPTED.** Plan step 3: /grill HARD-FAILS to Phases 0+2 (no WDT) unless DEVCFG1 decodes FWDTEN=0 AND WDTPS > worst-iteration+margin.

### prep.md changes this round
- Goal: WDT reframed as armed-continuously + top-of-loop pet (catches any in-iteration stall).
- Context: FWDTEN paragraph hardened with the chipKIT-core no-background-petter audit (Gemini#2).
- Decisions: 3b added (SPI module reset on timeout); 4 rewritten (owner-scoped CS cleanup + sampleValid); 5 fully rewritten (top-of-loop pet, never disarm); 6 fully rewritten (WDTPS>worst-iteration contract, lower SD_WRITE_TIMEOUT / chunk rollover); 8 rewritten (advisory + magic+CRC + no-footer corroboration); 9 rewritten (>=45s-active clear, fix NOW).
- Plan: Phase 1 (steps 3-6) rewritten to arm-once + single top-of-loop pet + WDTPS gate + rollover state-machine fallback + now-not-deferred cap clear.
- Files to touch + Test plan static checks + Risks (R2/R3 rewritten, R8 added) all updated to the flipped strategy.

## Round 3 — Codex: CHANGES_REQUESTED | Gemini: CHANGES_REQUESTED
### Codex findings
1. [BLOCKER] WDT is off in the recovery path it depends on — setup()->replaySessionFile()->card.init/openRoot runs before steady streaming; a WDT reset from a torn SD/FAT state can hang there unprotected. Bound/cover the recovery path or fail to idle.
2. [BLOCKER] WDTPS gate insufficient for COMPOSED iterations — a real iteration combines slow block write + checkpoint/footer + error handling + rollover; don't "fix" a short WDTPS by lowering SD_WRITE_TIMEOUT into false SD failures.
3. [BLOCKER] The 45s resumeCount clear defeats the thrash cap — a reset every 46+s clears the cap each time -> unbounded slot churn instead of stopping at 25. Track consecutive unclean resumes; clear only on a durable clean footer.
4. [MAJOR] SPI reset-on-timeout doesn't reset external slave state — SD left in CMD25/busy or ADS mid-frame; WDT reset runs no owner cleanup. Add a boot/fault bus scrub (deassert all CS, reinit SPI, idle clocks, bounded SD abort/init, ADS SDATAC) before resume.
5. [MAJOR] Owner-scoped CS cleanup incomplete — Phase 0 includes accel/live-register SPI; every slave needs an owner wrapper or one missed owner holds the bus wedged.
6. [MAJOR] Breadcrumb internally inconsistent — Decision 8 requires CRC + corroboration but Phase 2 struct omits CRC and appends on magic alone. Implement magic+CRC, emit only after resume + prior no-footer, clear after durable footer.
7. [MAJOR] CP0 deadline wrap safety unspecified — CP0 Count wraps ~215s at 20MHz, repeatedly overnight; the timeout primitive must use signed wrap-safe delta + be tested near-wrap.
8. [MAJOR] Rollover state-machine fallback lacks a sample-data contract — splitting FAT work across iterations doesn't say where 500/1000Hz samples go; preallocate slots or define buffering/drop markers.
### Gemini findings
1. [BLOCKER] Lowering SD_WRITE_TIMEOUT corrupts physical SD writes (hardware busy up to 250-500ms+); waitNotBusy is already time-bounded -> pet the WDT INSIDE it instead of shrinking the timeout.
2. [BLOCKER] Refusing to pet inside long legit FAT walks guarantees false resets (traversals take seconds on fragmented cards); rewriting SdFat into a state machine is unacceptable scope creep. Pet inside FAT walks but BOUND the walk (max-cluster cap) to defeat circular chains.
3. [MAJOR] SPI module reset (3b) while a slave's CS is asserted clocks garbage (toggling SPI1CON.ON forces pins to PORT state -> spurious edges desync the slave). Return the fault FIRST, caller deasserts CS, THEN flush.
4. [MAJOR] Clearing resumeCount after 45s incurs an EEPROM DEE stall mid-recording (violates "no EEPROM write in steady loop"). Defer the clear to a clean boundary or accept+document the drop.
5. [MAJOR] wdt=0 kill-switch erased when a boot-loop clears SESSION.TXT -> defaults ON -> repeat failure. Persist the kill-switch in EEPROM independent of the session file.
6. [MINOR] Sticky SPI fault clearing unspecified — if it stays sticky across iterations a single timeout fast-fails all future SPI forever. Clear it at top-of-loop / start of a new owner transaction.

### Resolution (Claude)
- **THE FAT-PET QUESTION RESOLVED via a SYNTHESIS superseding both round-1 (disarm) and round-2 (never-pet-inside).** Gemini#1+#2 + Codex#2+#8 converged: do NOT lower SD_WRITE_TIMEOUT (tears legit card-busy writes) and do NOT chunk rollover into a state machine (scope creep + silent sample loss). NEW discipline (Decision 5/6 rewritten): **arm continuously; pet at top-of-loop AND inside the long-but-BOUNDED loops (waitNotBusy [already 1500ms-bounded], FAT walks); make every petted loop INDEPENDENTLY bounded** — add a max-cluster cap to chainSize/allocContiguous so a circular chain ABORTS (Gemini#2). A petted+independently-bounded loop can't run forever, so petting can't mask an infinite hang; the WDT still catches a genuinely unbounded un-petted spin. Then **WDTPS need only exceed the short pet-to-pet GAP, not the whole iteration** (resolves Codex#2) -> no SD_WRITE_TIMEOUT lowering needed for a short WDTPS.
- **Codex#1 (WDT off in recovery path) — ACCEPTED, new Decision 10b.** Arm the WDT at the entry to the auto-resume path so card.init/FAT during replaySessionFile is covered; bounded by the thrash cap; cold fresh-start setup (no SESSION.TXT) stays uncovered (immediate user-visible failure, not a night-eraser).
- **Codex#3 (45s clear defeats the cap) — ACCEPTED, Decision 9 REWRITTEN.** Replaced the 45s-active clear (defeated by a 46s-recurring hang) with: count consecutive resumes whose prior slot has NO %Total time footer, clear ONLY on a durable clean footer. Bounds the rapid thrash (hits 25 in ~20min), the multi-hour salvage (2-3 unclean chunks/night, never near 25), AND clears on a clean manual stop. This ALSO resolves Gemini#4 (the clear is now at the footer/stop boundary + the increment at resume -> neither in the steady loop -> no mid-recording DEE stall).
- **Gemini#3 + Codex#5 (SPI flush ordering + all slaves) — ACCEPTED, Decision 3b/4 REWRITTEN.** Order: leaf primitive sets sticky fault + returns WITHOUT flushing; the OWNER wrapper deasserts its CS, THEN calls sdSpiModuleFlush on the idle bus, restores mode. EVERY slave (ADS/register/accel/SD) gets an owner wrapper. Sticky fault cleared at the start of the next owner transaction (Gemini#6).
- **Codex#4 (external slave state) — ACCEPTED, new Decision 10c.** Full bus scrub on the resume path (deassert all CS, idle clocks, SD re-init, ADS SDATAC/reset) before re-using the bus, since a WDT reset runs no owner cleanup.
- **Codex#7 (CP0 wrap) — ACCEPTED, folded into Decision 6 + Plan step 3 + Test (b).** Deadline math must be signed-wrap-safe ((int32_t)(now-deadline)>=0) across the ~215s CP0 wrap; /grill verifies the existing spiByteBounded + a forced near-wrap test.
- **Gemini#5 (kill-switch erased) — ACCEPTED, Decision 10 REWRITTEN.** wdt kill-switch lives in the EEPROM tune store independent of SESSION.TXT, so a session-file wipe to recover the board doesn't re-enable the WDT. (The thrash cap independently self-terminates a boot-loop, so recovery never depends on the switch.)
- **Codex#6 (breadcrumb inconsistency) — ACCEPTED.** Phase 2 struct now carries the crc field + corroborated-emit, matching Decision 8.
- **Codex#8 (rollover sample contract) — RESOLVED by the synthesis.** No rollover state machine -> the concern dissolves; rollover stays atomic (pets inside its bounded sub-loops), sample handling during rollover is unchanged from current firmware (not made worse).
- **Gemini#6 (sticky fault clear) — ACCEPTED.** Cleared at the start of each owner-scoped transaction (folded into 3b).

### prep.md changes this round
- Decisions 5 + 6 fully rewritten (pet-inside-bounded-loops synthesis; WDTPS>pet-to-pet-gap; CP0 wrap-safe; max-cluster FAT cap).
- Decision 3b re-ordered (flush AFTER CS release); Decision 4 extended to all slaves + sticky-fault clear.
- Decision 9 rewritten (consecutive-unclean footer-clear thrash cap, supersedes 45s); 10 rewritten (EEPROM kill-switch); 10b added (WDT covers resume path); 10c added (resume bus scrub).
- Plan Phase 1 (3-6) rewritten; Phase 2 struct gains crc + corroborated emit; Phase 3 kill-switch -> EEPROM store.
- Files-to-touch, Test plan static checks (a)/(b)/(d)/(h), Risks R2/R3/R6 all updated to the synthesis.

## Round 4 — Codex: CHANGES_REQUESTED | Gemini: CHANGES_REQUESTED
### Codex findings
1. [BLOCKER] Resume WDT still arms AFTER SD discovery — confirming SESSION.TXT requires card.init/openRoot/FAT on the torn SD state; a hang there is before the WDT/scrub/counter. Arm before the first SD/FAT recovery access (minimal CS setup) + an EEPROM pre-session attempt cap.
2. [MAJOR] Top-of-loop pet can mask a no-progress livelock — an unconditional pet doesn't prove recording progress; DRDY/sample/SD progress can stop while the loop spins and pets forever. Gate top-level pets on streaming progress, or stop after a CP0 no-progress deadline.
3. [MAJOR] Petted-loop "bounds" depend on weak coverage — waitNotBusy is only bounded if its timeout source can't stall; a millis()/ISR bound + internal pets can mask interrupt-starvation. Also only waitNotBusy + chainSize/allocContiguous named, not all SD init/cmd/token/seek loops. Use CP0 signed-delta for every petted wait + inventory every SdFat loop under WDT.
4. [MAJOR] Clean footer doesn't suppress a stale SESSION resume — a WDT reset after the footer but before SESSION.TXT cleanup leaves a clean slot + stale session; plan clears the cap but doesn't cancel replay. Footer-present must be authoritative: clear cap, ignore/delete SESSION.TXT, don't auto-resume.
5. [MAJOR] Resume bus scrub isn't a real interrupted-CMD25 abort — idle clocks + card.init may not terminate a card mid multi-block awaiting a stop token/busy completion. Scrub must include bounded SD CS assert + CMD25 stop-tran token + clocks/wait-not-busy, then full reinit; fail idle if it can't.
### Gemini findings
1. [BLOCKER] Masked hang via stopped millis() in petted waitNotBusy — if the hang disables interrupts/halts CoreTimer, millis() stops, waitNotBusy spins forever AND keeps petting -> WDT masked, perfectly hiding the exact hang. The independent bound for any petted loop MUST use hardware CP0 Count or a strict iteration counter, not millis().
2. [BLOCKER] Consecutive-unclean cap permanently locks out battery-pull users — clearing only on a clean footer means a user who chronically pulls the battery/SD accumulates unclean resumes across independent days -> after 25 normal uses, refuses to resume forever. Clear on a cold power-on reset (distinguishable via RCON or persistent-RAM magic invalid) so the 25-cap applies only to a contiguous rapid-fire boot-loop.
3. [MAJOR] Unbounded/unpetted SPI bus scrub on resume — Phase 10c issues ADS SDATAC/reset before the WDT is armed or streaming begins; raw transfer()/blocking here -> permanent freeze (WDT not armed) or false-fire. Scrub must use the Phase-0 bounded primitive and be WDT-covered.

### Resolution (Claude)
- **CONVERGENT BLOCKER (Codex#3 + Gemini#1, found independently): millis()-bounded petted loops are masked by a stopped millis().** This is the sharpest finding of the review — the actual unknown hang could BE an interrupt-disable/CoreTimer-halt, in which case a millis()-bounded waitNotBusy spins forever while petting the WDT, masking the very hang we're catching. ACCEPTED -> Decision 5 REFORMULATED around **CP0-gated PROVEN-PROGRESS petting**: maintain lastProgressCP0 (bumped on real recording work — a sample cached / a 512B block written); gate the top-of-loop pet on a CP0 no-progress deadline (sized > worst rollover, fully under our control); convert waitNotBusy to a CP0 signed-delta bound; add a max-cluster ITERATION cap to the FAT walks; millis() gates NOTHING WDT-related. This single rule also resolves Codex#2 (no-progress livelock: loop spins, no samples -> no progress -> no pet -> reset).
- **Codex#2 (top-of-loop pet masks livelock) — ACCEPTED, folded into the CP0-progress reformulation above** (the pet is gated on forward progress, not bare iteration).
- **Codex#3 (coverage) — ACCEPTED.** Plan step 5: inventory EVERY blocking loop reachable under the armed WDT (all SdFat init/cmd/token/seek), each with a CP0 or iteration bound, not just waitNotBusy + the two FAT walks.
- **Gemini#2 (battery-pull lockout) — ACCEPTED, Decision 9 REFINED.** The cap now counts only WARM-reset (magic-valid) consecutive no-footer resumes and clears on a clean footer OR a COLD boot (magic invalid). So the 25-cap bounds only a contiguous rapid-fire loop (SRAM-retained magic across fast resets); separate cold-start sessions always clear it -> no permanent lockout.
- **Codex#1 (arm WDT before SD discovery) — ACCEPTED, Decision 10b SHARPENED.** Arm the WDT immediately after minimal CS setup, BEFORE the first SD/FAT access (reading SESSION.TXT itself touches the torn SD), + an EEPROM pre-session attempt counter bounding a card.init-stage hang loop.
- **Codex#4 (stale SESSION resume) — ACCEPTED, new Decision 10d.** Resume decision = SESSION.TXT present AND last slot has NO clean footer; a present footer cancels replay + clears the cap + deletes stale SESSION.TXT (authoritative).
- **Codex#5 + Gemini#3 (bus scrub not a real abort / unpetted) — ACCEPTED, Decision 10c REWRITTEN.** Scrub runs ONLY through the bounded primitive, AFTER the WDT is armed, with a real interrupted-CMD25 abort (stop-tran token + bounded waitNotBusy) before card.init + ADS SDATAC/reset, failing to clean idle if it can't complete.

### prep.md changes this round
- Decisions 5 + 6 reformulated (CP0-gated proven-progress petting; software CP0 no-progress deadline; millis() banned from all WDT/petted-loop bounds; signed-wrap-safe CP0).
- Decision 9 refined (warm-reset-only count + cold-boot clear, Gemini#2); 10b sharpened (arm before first SD access + EEPROM attempt cap); 10c rewritten (real bounded CMD25 abort, WDT-covered, bounded-primitive-only); 10d added (footer-authoritative resume cancellation).
- Plan Phase 1 (3-6) rewritten; Test plan static (a)/(d); Risks R2/R6 updated to CP0-progress + cold-boot-clear.

## Round 5 — Codex: CHANGES_REQUESTED | Gemini: CHANGES_REQUESTED
### Codex findings
1. [MAJOR] WDT lifetime conflicts with non-recording idle/fail-safe states — pets gated on recording progress, but clean idle/ledReplayFail/command-idle has no sample/block progress -> WDT reset loop. Scope "never disarm" to recovery/active recording; disarm or use a separate bounded idle policy before fail-safe/command idle.
2. [MINOR] Serial-drain bound still says millis() in Phase 3 step 9 — make it iteration or CP0 only, matching Decision 11 + the static test contract.
3. [MINOR] Files-to-touch omits the SdFat sources that own the FAT-loop fixes — chainSize/allocContiguous/seek/init loops live in SdVolume.cpp/SdFile.cpp, not SD_Card_Stuff.ino; add them so /grill doesn't under-patch.
### Gemini findings
1. [BLOCKER] "Proven progress" WDT gate causes infinite resets during setup, recovery, and idle — armed before first SD access + never disarmed + pet strictly gated on lastProgressCP0; during setup/scrub/card.init/idle-waiting-for-host no samples advance -> no-progress deadline expires -> pet skipped -> endless reset before recording begins. The CP0 no-progress gate must apply ONLY while isStreaming; during setup/recovery/idle the pet must be unconditional (or lastProgressCP0 kept fresh).
2. [BLOCKER] EEPROM pre-session attempt cap permanently locks out SD init for casual users — increments every boot, clears only on a clean streaming start; a user powering on N times to check settings / stream without an SD card hits the cap, then card.init is aborted forever -> can never reach a clean streaming start to clear it -> permanent SD brick. The attempt cap must ALSO clear on a cold boot (magic invalid); a card.init WDT reset is a warm reset so cold-boot clear still bounds a contiguous card.init loop.
3. [MINOR] FAT32 cluster iteration cap size — a 16-bit cap falsely aborts legit FAT walks on large cards (millions of clusters). Use uint32_t bounded against the volume's actual VolumeClusterCount, not a hardcoded small number.

### Resolution (Claude)
- **CONVERGENT BLOCKER (Codex#1 + Gemini#1, same issue + same fix): the CP0 no-progress pet gate false-resets during non-recording states.** This is the last architectural hole — a side effect of the round-4 CP0-progress reformulation. ACCEPTED -> new Decision 5b: the no-progress deadline applies ONLY while isStreaming==true; during setup/scrub/card.init/command-idle the WDT is fed by LOCAL liveness (each bounded long op pets on its CP0/iteration sub-progress; the command-idle host-wait loop pets per iteration — its correct behaviour IS to wait, so no livelock to catch, and a true freeze stops the poll loop -> reset). At TERMINAL clean idle (ledReplayFail / clean stop) the WDT is DISARMED (intentional halt, nothing to recover) — so "never disarm" is scoped to recording + active recovery (resolves Codex#1's fail-safe-idle reset-loop too).
- **Gemini#2 (attempt cap locks out casual users) — ACCEPTED, Decision 10b.** The EEPROM pre-session attempt cap now ALSO clears on a cold boot (magic invalid), exactly like the resume-thrash cap (uniform cold-boot-clear for both warm-reset boot-loop caps). Bounds a contiguous card.init loop while allowing unlimited independent non-streaming/no-card sessions.
- **Codex#2 (serial-drain millis) — ACCEPTED.** Plan step 9 changed to iteration/CP0, matching Decision 11 + the test contract.
- **Codex#3 (SdFat files missing from touch list) — ACCEPTED.** Added OBCI32_SD utility/SdFile.cpp + SdVolume.cpp (where chainSize/allocContiguous/seek/init loops live) to Files-to-touch; /grill greps utility/ for every blocking loop reachable under the armed WDT.
- **Gemini#3 (FAT cap width) — ACCEPTED.** The cluster-count cap is uint32_t bounded against the volume's actual cluster count, not a hardcoded 16-bit number.

### prep.md changes this round
- Decision 5 (FAT cap -> uint32_t/VolumeClusterCount); new Decision 5b (isStreaming-scoped no-progress gate + non-streaming liveness pet + terminal-idle disarm).
- Decision 10b (pre-session attempt cap ALSO cold-boot-cleared).
- Plan step 4 (streaming-vs-non-streaming pet policy), step 9 (iteration/CP0 serial-drain).
- Files-to-touch (+ SdFile.cpp/SdVolume.cpp + the waitNotBusy CP0 bound + CMD25 abort in Sd2Card.cpp).
- Test plan static (a)/(d) updated to the isStreaming scoping + both caps cold-boot-cleared.
- NOTE: Round 5 produced ONE convergent net-new BLOCKER (the non-streaming gate) + MINORs; no new architectural holes. Next round expected to converge.

## Round 6 — Codex: APPROVED | Gemini: CHANGES_REQUESTED
### Codex findings
1. [MINOR] Stale WDT-lifetime wording in the DefaultBoard.ino touch-list summary ("arm-on-stream + single top-of-loop pet (NO disarm)") conflicts with the converged design. Update so /grill doesn't implement the obsolete round-2/3 model.
(Codex VERDICT: APPROVED — no BLOCKER/MAJOR.)
### Gemini findings
1. [MAJOR] Disarming the WDT on a clean user stop leaves subsequent same-power-cycle sessions unprotected — a clean stop ('s') returns to command-idle to await the next 'b'; since the WDT is armed only in setup(), a later 'b' streams with the WDT off. Disarm ONLY at true terminal traps (ledReplayFail); command-idle already pets safely via !isStreaming.
2. [MAJOR] Pre-session attempt cap acts as a global warm-boot limit, ruining multi-hour salvage — it increments on every warm reset before SD access and clears only on cold boot; every mid-recording WDT hang recovery IS a warm reset, so N nightly recoveries accumulate it and falsely trip ledReplayFail. Clear it immediately after the early SD-recovery phase (card.init + resume decision) succeeds, so it only accrues on a hang WITHIN that phase.
3. [MAJOR] Internal WDT pets inside bounded loops bypass the no-progress gate and mask livelocks — pets inside waitNotBusy/FAT walks run on local sub-progress without the global gate; a wedged-but-retrying SD write keeps feeding the WDT forever, defeating the no-progress trap. ALL pets while isStreaming must be subject to lastProgressCP0 — use one centralized petWDT() enforcing isStreaming ? (CP0-lastProgressCP0 < deadline) : true at every site.
4. [MINOR] lastProgressCP0 init gap on streaming start — the deadline goes active when isStreaming flips true; a stale lastProgressCP0 instantly trips it before the first sample. Set lastProgressCP0=CP0 in the 'b' start handler.

### Resolution (Claude)
- Codex APPROVED (only a wording MINOR). Gemini raised 3 genuinely net-new MAJORs — sharp interaction bugs in the round-4/5 pet model — ALL ACCEPTED:
- **Gemini#3 (internal pets bypass the gate) — ACCEPTED, the key unifier.** Decision 5b rewritten around ONE centralized petWDT() inline: `if(!isStreaming){clear;return;} if((int32_t)(CP0-lastProgressCP0)<NO_PROGRESS_TICKS) clear;`. The no-progress gate now applies at EVERY streaming pet site (top-of-loop AND inside waitNotBusy/FAT walks), so a retry-storm with no block written stops petting -> reset. This supersedes the round-3/4 "pet inside bounded loops on local sub-progress" (which let a wedged retry feed the WDT forever).
- **Gemini#1 (disarm on clean stop) — ACCEPTED.** Disarm ONLY at a terminal trap (ledReplayFail), NOT on clean stop 's'; command-idle keeps the WDT armed+petted via the !isStreaming branch so the next 'b' is protected.
- **Gemini#2 (attempt cap defeats salvage) — ACCEPTED.** The pre-session attempt cap now clears the instant card.init+resume-decision SUCCEED (not "clean streaming start"), so a mid-recording hang recovery re-inits the card and clears it; only contiguous EARLY-PHASE failures accrue. The resume-thrash cap (separate) bounds the salvage chain. (Cold-boot clear retained.) The two caps are now cleanly separated.
- **Gemini#4 (init gap) — ACCEPTED.** lastProgressCP0=readCP0() set when isStreaming flips true (the 'b' handler).
- **Codex#1 (wording) — ACCEPTED.** DefaultBoard.ino touch-list summary updated to the converged petWDT()/terminal-disarm model.

### prep.md changes this round
- Decision 5b fully rewritten around the centralized petWDT() inline (global no-progress gate at all sites; disarm-only-at-terminal-trap; lastProgressCP0 re-stamp on stream start).
- Decision 10b: attempt cap clears on early-phase SUCCESS (+ cold boot), cleanly separated from the resume-thrash cap.
- Plan step 4 + Files-to-touch (DefaultBoard.ino) + Test plan static (a)/(d) all updated.
- Codex APPROVED this round; remaining work is to confirm Gemini's 3 MAJORs are cleanly resolved (round 7).

## Round 7 — Codex: APPROVED | Gemini: CHANGES_REQUESTED
### Codex findings
1. [MINOR] Stale local-progress wording in Decision 5 ("pet inside bounded loops on their CP0/iteration sub-progress") would conflict with 5b's centralized petWDT() gate if implemented literally. Reword to "call centralized petWDT() at CP0/iteration checkpoints."
(Codex VERDICT: APPROVED — no BLOCKER/MAJOR, 2nd consecutive approval.)
### Gemini findings
1. [MAJOR] lastProgressCP0 init gap on AUTO-RESUME — the 'b' handler re-stamp is correct, but a WDT-recovered board auto-resumes streaming from setup()/replaySessionFile(), flipping isStreaming true WITHOUT going through 'b'; the CP0 ticks accumulated during the long FAT recovery walk instantly trip the no-progress gate on the first loop() -> immediate false reset -> infinite recovery loop. Mandate lastProgressCP0=readCP0() wherever isStreaming is set true, including auto-resume.
2. [MAJOR] Un-petted delay() during the WDT-armed resume scrub — Decision 10c issues ADS SDATAC/reset after the WDT is armed; standard ADS reset uses hard delay() (>100ms oscillator settle) that doesn't pet -> with a short WDTPS it exceeds the timeout -> false reset -> infinite recovery boot-loop. Replace any delay() in the WDT-armed setup/scrub with a petted CP0-bounded wait loop.
3. [MINOR] Stale wording in Decision 10b ("CP0-progress pet discipline covering card.init/FAT") contradicts 5b (non-streaming phases pet unconditionally). Align the wording.

### Resolution (Claude)
- Codex APPROVED for the 2nd consecutive round. Gemini's two MAJORs are both real and both LOCALIZED applications of already-approved principles (not new architecture) — ALL ACCEPTED:
- **Gemini#1 (lastProgressCP0 on auto-resume) — ACCEPTED.** Decision 5b now mandates lastProgressCP0=readCP0() at EVERY isStreaming->true site (the 'b' handler AND the auto-resume replaySessionFile path), via a single helper that sets both together. The auto-resume path was the dangerous omission (long FAT walk -> stale CP0 -> instant false reset).
- **Gemini#2 (un-petted delay in armed window) — ACCEPTED.** Decision 10c(e) added: NO bare un-petted delay()/spin between WDT-arm and the first loop(); the ADS reset/SDATAC settle delays become CP0-bounded petWDT() loops (the unconditional non-streaming branch feeds them); /grill audits for any bare delay() in the armed window.
- **Gemini#3 + Codex#1 (wording) — ACCEPTED.** Decision 5 reworded to "call centralized petWDT() at CP0/iteration checkpoints"; Decision 10b reworded to "the unconditional non-streaming petWDT() covering card.init/FAT" (aligns with 5b).
- Test plan static (a) augmented: re-stamp at every isStreaming->true site + no bare un-petted delay() in the armed window.

### prep.md changes this round
- Decision 5 (wording -> centralized petWDT()); Decision 5b (lastProgressCP0 at EVERY stream-start incl. auto-resume); Decision 10b (wording -> unconditional non-streaming pet); Decision 10c (e) added (no un-petted delay() in the WDT-armed window); Test plan static (a) augmented.
- NOTE: nominal prep backstop is 7 rounds and this is round 7. Extending to ONE confirmation round (8) is justified: Codex has approved twice, Gemini's remaining items are localized mechanical applications of already-approved rules (not architecture), so a clean dual sign-off is within reach and worth capturing on no-ICSP, brick-risk hardware.

## Round 8 (confirmation) — Codex: APPROVED | Gemini: APPROVED  ✅ DUAL SIGN-OFF
### Codex findings
(none — VERDICT: APPROVED)
### Gemini findings
(none — confirmed all round-7 fixes fully integrated: Decision 5b lastProgressCP0 at every isStreaming->true site incl. auto-resume; 10c(e) bans un-petted delay() in the WDT-armed window; 5/10b wording aligned to the centralized petWDT() model. "The architecture is now comprehensively hardened, self-consistent, and robust against interrupt-disabled livelocks, mid-FAT-walk wedges, and edge-case boot-loops. No new architectural vulnerabilities or contradictions." VERDICT: APPROVED)

### Resolution (Claude)
BOTH Codex and Gemini APPROVED with no BLOCKER/MAJOR/MINOR findings. The plan is signed off.
Rounds: 8 total (Codex approved rounds 6,7,8; Gemini approved round 8). The adversarial chain drove
three full strategy revisions of the WDT design (disarm-around-ops -> never-pet-inside -> pet-inside-
bounded-loops -> CP0-progress-gated centralized petWDT()) plus a long tail of interaction-bug fixes.
Next step: /grill to implement prep.md (which will run its OWN full Codex+Gemini code-diff review).
