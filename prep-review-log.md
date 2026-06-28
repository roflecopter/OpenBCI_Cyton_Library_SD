# prep-review-log — robust post-reset SD auto-resume

## Round 1 — Codex: CHANGES_REQUESTED | Gemini: CHANGES_REQUESTED
Both converged strongly on SD-SPI-protocol correctness of the `sdBusRecover` mechanism.

### Codex findings
1. [BLOCKER] reviewability — codex sandbox couldn't read the repo; couldn't verify file:line. → feed code inline next round.
2. [BLOCKER] SD-protocol — CMD12 is the wrong stop for a stuck CMD25 WRITE; the terminator is the stop-tran DATA token 0xFD. CS-high idle clocks + CMD12 may be ignored/eaten as data.
3. [MAJOR] SD-recover — plan doesn't identify what differs from existing card.init (already does CS-high + ≥74 idle clocks + CMD0). Must prove the missing step is stop-token/busy-drain/SPI-reinit, not "more clocks."
4. [MAJOR] init-state — helper setting cardInit=true then running before the `if(!cardInit)` block can bypass volume.init/root.openRoot. Make recovery bus-only OR fully replace the init path.
5. [MAJOR] data-integrity — "fresh slot can't hurt prior slot" is false: FAT is shared; reset mid-allocation can corrupt FAT; card.erase() can erase wrong clusters if FAT already damaged. Weaken claim or preallocate slots before the night.
6. [MAJOR] destructive — openfile.remove(currentFileName) on resume can delete valid data (counter wrap / stale SESSION.TXT / un-ingested card / collision). Never remove an existing target; scan forward to a free name or fail read-only.
7. [MAJOR] forensic — making writeReplayFail recover-the-bus-and-write REPLAYFL turns a previously-FAT-untouching failure path into a new post-fault FAT mutation. Don't allocate forensic files after a failed resume unless preallocated.
8. [MAJOR] boundedness — raw spiSend before SPI init can spin forever (PIC32 SPI status); soft-WDT disabled → bricks boot. Init SPI/pins at safe speed; every wait needs a millis timeout.
9. [MINOR] flash — "Sketch uses N bytes" insufficient; require map/symbol delta; reject new Print/string pulls.
10. [MINOR] test — random mid-record MCLR pulse may land between writes (false pass). Trigger reset while CS low / inside the CMD25 window.
11. [MINOR] RF-current — deferral OK as scope control, but if resets are brownout-current related, gating RF TX during SD-only logging may PREVENT vs recover; make it the next measured mitigation.

### Gemini findings
1. [BLOCKER] SD-protocol — clocks with CS HIGH are ignored; CMD12 is wrong for CMD25; card.init() already sends 80 idle clocks. Proper CMD25 abort: CS LOW → 0xFD stop-tran token → wait MISO high (BUSY) → optional CMD12 (for stuck CMD18 reads) → CS HIGH → card.init().
2. [BLOCKER] hw-state — after a warm reset the PIC32 SPI peripheral is at disabled default; using library SPI sends before init → hardware wait loop (while(!SPIRBF)) hangs forever on boot. Fix: SPI.begin() first OR bit-bang via digitalWrite/digitalRead.
3. [MAJOR] data-integrity — un-wedging the card REOPENS the rapid FAT-corruption race: today a wedged card's createContiguous FAILS (shields FAT); succeeding it means a FAT update a second physical bounce can corrupt → the 6f6efe8 risk. Fix: stabilization/debounce delay (~3s) at boot before SD init + createContiguous.
4. [MAJOR] concurrency — naive while(MISO==LOW) busy-wait on 0xFD flush bricks the MCU on a dead/locked card (soft-WDT off). Needs strict millis() timeout (~500ms) + graceful abort.
5. [MINOR] flash — modifying Sd2Card to expose SPI risks ballooning size; implement recovery as a tiny inline GPIO bit-bang.
6. [MINOR] power — DEFER the RF-TX idea; modifying the streaming/handshake path is risky.

### Resolution (Claude — orchestrator, no vote)
ACCEPTED (all material findings):
- Recovery sequence corrected to the proper CMD25 abort: **CS LOW → send 0xFD stop-tran token →
  bounded busy-wait for MISO high (millis timeout ~500ms) → CS HIGH → then the existing
  card.init/volume.init/root path.** Drop CMD12-as-primary (optional, only to also clear a stuck
  CMD18 read). (Codex #2/#3, Gemini #1)
- **Bit-bang the recovery over GPIO** (digitalWrite MOSI/CLK/CS, digitalRead MISO) so it does NOT
  depend on the (uninitialized) SPI peripheral and stays flash-tiny. (Gemini #2/#5, Codex #8)
- **Every wait bounded by millis() timeout**; helper provably cannot hang. (Codex #8, Gemini #4)
- **sdBusRecover is BUS-CLEANUP-ONLY** — it does NOT set cardInit/volume/root; the existing
  `if(!cardInit){card.init; volume.init} … root.openRoot` block runs unchanged after it. (Codex #4)
- **openfile.remove guard**: on the resume path, never remove a nonzero existing target — scan the
  file counter forward to a guaranteed-free name (bounded), else fail read-only (REPLAYFL). (Codex #6)
- **FAT-race**: add a stabilization delay before the resume's createContiguous; ALSO evaluate
  **preallocating the continuation slot at stable session-start** (no resume-time FAT write at all)
  as the stronger option — round 2 to judge feasibility within flash. Safety claim weakened. (Codex #5/#7, Gemini #3)
- **REPLAYFL**: only via the same bounded bus-recover; accept it is itself a small FAT mutation —
  gate it so it's attempted at most once and never loops. (Codex #7)
- **Codex reviewability**: round 2 prompt embeds the actual code inline. (Codex #1)
- **Test plan**: reset injected inside the CMD25 write window (CS low), not a random pulse. (Codex #10)
- **RF-TX**: stays DEFERRED but explicitly named the next measured mitigation. (Codex #11, Gemini #6)
- **Flash gate**: map/symbol delta, reject new Print/string pulls. (Codex #9, Gemini #5)
REJECTED: none (all findings accepted or folded).
prep.md updated accordingly for round 2.

## Round 2 — Codex: CHANGES_REQUESTED | Gemini: CHANGES_REQUESTED (code embedded this round)
### Codex findings
1. [BLOCKER] SD recovery — single 0xFD not phase-safe: a reset can land card-busy or mid-data-block; 0xFD then ignored or eaten as payload. Fix: CS low → drain busy (timeout) → clock 0xFF to complete/realign a partial block → bounded wait → 0xFD at a token boundary → bounded busy-wait → CS high → idle clocks → card.init.
2. [BLOCKER] GPIO recovery — pin ownership underspecified; board.csLow/csHigh is DSPI-coupled (Sd2Card card(&board.spi,SD_SS), L20/L103); GPIO writes won't clock the card if DSPI owns SCK/MOSI or pins are inputs. Fix: don't use board.cs*; disable/release DSPI, set SD_SS/SCK/MOSI OUTPUT + MISO INPUT, drive idle, bit-bang, then card.init reinits DSPI.
3. [BLOCKER] setupSDcard — createContiguous/contiguousRange failure (L1224-1241) only sets cardInit=false, falls through into card.erase(bgnBlock,endBlock) + writeStart with STALE range → erase/write WRONG blocks. Fix: fail-fast, release CS, return before erase/writeStart; never erase/write unless contiguousRange succeeded.
4. [MAJOR] FAT race — Decision 5 rejects preallocation without evidence; the alloc primitives are already linked so a reserved-next-slot may be small + no obvious host-protocol bump. Fix: prototype/map-size 1-slot preallocation before rejecting, OR explicitly accept the design can orphan prior data on a 2nd bounce. Preallocation is safer for irreplaceable data.
5. [MAJOR] REPLAYFL — writeReplayFail unconditionally SdFile::remove("REPLAYFL.TXT") (L769) before recreate; a reset after the unlink loses prior forensics + another FAT mutation; a RAM one-shot resets across MCU resets. Fix: don't remove an existing REPLAYFL; skip/append-without-alloc; gate after stabilization; persistent-enough guard.
6. [MINOR] normal-night — stabilization delay before the SESSION.TXT-absent return breaks behaviour-identical. Fix: delay only after valid resume intent, immediately before the first resume FAT write.

### Gemini findings
1. [BLOCKER] GPIO vs DSPI — DSPI owns SCK/MOSI/MISO once board.begin() ran; digitalWrite silently fails; post-reset pins are hi-Z inputs. Fix: disable SPI peripheral (SPI.end()/clear DSPI ON) + pinMode SCK/MOSI OUTPUT, MISO INPUT before bit-bang; card.init reinitialises DSPI.
2. [BLOCKER] writeReplayFail — sdBusRecover does no CMD0/ACMD41/SPI config; replacing card.init guarantees volume.init fails. Fix: sdBusRecover must PRECEDE card.init; keep card.init.
3. [MAJOR] remove-guard scope — setupSDcard's openfile.remove runs for ALL starts; a resume-only wrapper leaves cold-starts vulnerable to counter-wrap overwrite. Fix: size-guard UNIVERSALLY in setupSDcard — open target O_READ; if exists & fileSize()>0, bounded incrementFileCounter to a free/0-byte slot before remove().
4. [MINOR] CMD12 — if reset during CMD18 multi-block READ, card drives MISO, 0xFD ignored, wait times out, bus still wedged. Fix: make bit-banged CMD12 MANDATORY after 0xFD.
- Gemini ANSWER on the split: delay defensible, preallocation needs EEPROM state + host-protocol mods exceeding flash.

### Resolution (Claude — orchestrator, no vote)
ACCEPTED:
- Phase-safe abort (Codex#1): CS low → bounded busy-drain → 0xFF realign clocks → 0xFD at token boundary → bounded busy-wait → MANDATORY CMD12 (Gemini#4) → CS high → idle clocks → existing card.init.
- DSPI handling (Codex#2, Gemini#1): sdBusRecover disables the SPI/DSPI peripheral + pinMode SCK/MOSI OUTPUT, MISO INPUT, drives idle, bit-bangs raw GPIO; does NOT use board.cs*; card.init afterward reinitialises DSPI. All waits millis-bounded.
- setupSDcard FAIL-FAST (Codex#3): slot-alloc returns success/failure; on create/contiguousRange failure release CS + return BEFORE erase/writeStart. (Pre-existing latent bug; fix it.)
- UNIVERSAL remove-guard (Gemini#3, Codex#6-ish): inside setupSDcard, before remove(), O_READ the target; if exists & fileSize()>0 bounded-incrementFileCounter to a free/0-byte name; never remove a non-zero file. Applies to cold + resume.
- writeReplayFail (Gemini#2, Codex#5): sdBusRecover PRECEDES card.init (keep card.init); do NOT remove an existing REPLAYFL — skip if present; gate after stabilization; one attempt.
- Stabilization delay CONDITIONAL (Codex#6): only after resume intent confirmed (SESSION.TXT valid), immediately before the first resume FAT write — normal bedtime boot unchanged.
SPLIT RESOLUTION (FAT race, Codex#4 vs Gemini): Gemini's EOF-parsing objection is factually wrong (collect_bci already binary-searches the NUL-padded tail of every full-size file, so a preallocated padded continuation reads identically — no host-protocol change). BUT preallocation still needs a setupSDcard hot-path branch (open-existing-preallocated-slot vs createContiguous) + covers only the 1st reset. RESOLUTION: PRIMARY = stabilization delay (zero-flash, covers all resets, empirically 0/3 events corrupted); /grill MUST map-size a 1-slot preallocation prototype and ADOPT it if it fits flash cleanly without destabilising the normal path (it's strictly safer — no resume-time FAT write); else ship delay-only with the residual risk DOCUMENTED + named escalation. Codex's "don't reject without measuring" honored by deferring the measurement to /grill.
REJECTED: none.
prep.md updated for round 3.

## Round 3 — Codex: CHANGES_REQUESTED | Gemini: CHANGES_REQUESTED
### Codex
1. [BLOCKER] replay path can skip post-recovery card.init: sdBusRecover disables DSPI; if cardInit already true the `if(!cardInit)` block skips card.init → disabled bus for root.openRoot/sess.open. Fix: force cardInit=false (or call card.init unconditionally) after recovery.
2. [BLOCKER] "a few 0xFF" not phase-safe: reset mid-512B-payload → card awaits rest of block + CRC; 0xFD/CMD12 swallowed as payload. Fix: fixed worst-case drain (≥1 full data packet + margin), CS reselect to reset framing, bounded response consumption.
3. [BLOCKER] delay-only fallback still unsafe for irreplaceable data: 2nd interruption during createContiguous can orphan prior slot. Fix: REQUIRE no resume-time FAT alloc — preallocation (open reserved slot → contiguousRange → writeStart, no remove/createContiguous) OR fail read-only with REPLAYFL.
4. [MAJOR] preallocation can't be safely adopted by current setupSDcard: it always increments+removes/creates; a full-size preallocated slot trips the guard (fileSize>0) or gets removed; size/contiguity alone can't tell preallocation from a real full-size recording. Fix: explicit resume-only adopt-branch with a reliable empty-marker before remove/create.
5. [MINOR] 0-byte audit slots still removable (guard only scans past fileSize>0; a 0-byte collision is still removed). Fix: scan past any OBCI_*.TXT, or weaken "leave 0-byte slots" language.
### Gemini
1. [BLOCKER] preallocation ↔ universal-guard contradiction (same as Codex #4): guard skips fileSize>0 → never adopts the preallocated slot on cold boot → orphans a 512MB file every boot, exhausts card. Fix: explicit adopt-branch + magic header to distinguish preallocation from real data.
2. [BLOCKER] "a few 0xFF" insufficient (same as Codex #2): must clock ≥514 bytes (512+2 CRC) to flush a partial payload block before 0xFD.
3. [MAJOR] setupSDcard "return false" won't compile — claims it's void. → REJECTED: setupSDcard is `boolean setupSDcard(char limit)` (L1143), returns fileIsOpen (L1403). `return fileIsOpen;`/`return false;` compiles. Gemini factual error; verified against source.
4. [MAJOR] file-handle leak in remove-guard loop: O_READ test-open of the global openfile must .close() each iteration before incrementFileCounter/next open, else opens fail / createContiguous corrupts. → ACCEPTED.
5. [MINOR] REPLAYFL "skip if exists" = permanent blind spot if host never deletes it. → collect_bci ALREADY unlinks REPLAYFL on ingest (py-qs-data: copy to backup + unlink), so it won't persist across nights; keep skip-if-exists + note host-side removal. (Alt: O_APPEND.)

### Resolution (Claude — orchestrator)
ACCEPTED: phase-safe drain must clock a FULL block+CRC (~514B) not "a few" (Codex#2/Gemini#2); force
card.init after recovery (Codex#1); remove-guard must .close() the test handle each loop (Gemini#4);
clarify guard preserves NON-ZERO REAL-DATA files only, 0-byte slots may be overwritten (Codex#5);
REPLAYFL skip-if-exists is fine — collect_bci removes it nightly (Gemini#5).
REJECTED: Gemini#3 (setupSDcard is boolean, not void — source-verified).
ESCALATED TO USER (Codex#3/#4, Gemini#1): the panel has converged that RESUME-TIME FAT WRITE is unsafe
for irreplaceable data, and the safe alternative (preallocation + empty-marker adopt-branch) is a
materially bigger/riskier change that may not fit the ~4.4KB flash. This is a data-loss-risk SHAPE
decision = the user's call (forensics-only+hardware vs full preallocation recovery vs delay-only). Plan
finalisation pauses on the answer.

## Round 4 — Codex: CHANGES_REQUESTED | Gemini: CHANGES_REQUESTED
(First round on the DELAY-ONLY scope after the user's decision; preallocation dropped.)
### Codex findings
1. [MAJOR] 0xFD can still be sent while CMD25 is busy — after the ≥514 drain the card emits a
   data-response then holds MISO low to program; a stop token in that busy phase is ignored.
   Fix: consume data-response + wait MISO-high before 0xFD; timeout → fail-fast before FAT writes.
2. [MAJOR] Shared-SPI slaves not explicitly deselected — ADS1299 shares DSPI; its CS low/floating
   can corrupt MISO / eat clocks during bit-bang. Fix: drive every non-SD CS GPIO-output-high first.
3. [MAJOR] DSPI re-acquire underspecified after a raw ON-bit clear — card.init may not fully re-init
   PPS/peripheral. Fix: release/reacquire through the same DSPI API, or re-run DSPI/PPS before card.init.
4. [MAJOR] Remove-guard exhaustion behaviour undefined — a ≤16 scan conflicts with MAX_RESUMES=25;
   if all candidates non-zero, no hard-fail path stated. Fix: align cap to namespace, fail before remove.
5. [MINOR] Bit-bang SPI/CMD12 details — pin down SPI mode 0, discard CMD12 stuff byte, parse R1/R1b.
### Gemini findings
1. [BLOCKER] Missing busy-wait after flushing the dummy block (same as Codex#1) — 0xFD during BUSY
   ignored → permanently stuck in CMD25. Fix: bounded MISO-high wait AFTER ≥514 0xFF and BEFORE 0xFD.
2. [BLOCKER] Remove-guard fall-through destroys data on exhaustion (same as Codex#4) — loop hits its
   bound with currentFileName still non-zero → falls through to remove() deleting a valid file.
   Fix: after the loop re-check fileSize()>0; if still non-zero, fail-fast (abort/return false), never remove.
3. [MINOR] Ambiguous SCK idle polarity — mode 0 needs SCK idle LOW; if it idles HIGH, CS-low makes a
   false clock edge that misaligns the bit counter. Fix: drive SCK LOW in the initial GPIO setup.
### Resolution (Claude)
ALL net-new (none re-raise R1-R3; the dropped preallocation was correctly NOT re-raised). Both models
independently flagged the busy-wait-before-0xFD hole AND the remove-guard exhaustion hole = strong signal.
- Codex#1 + Gemini#1 → ACCEPTED. Decision 2 step 3 added: consume data-response + bounded MISO-high
  busy-wait BEFORE 0xFD; timeout = fail-fast. (This was the genuinely dangerous miss — without it the
  recovery silently leaves the card wedged in exactly the mid-payload case it exists to fix.)
- Codex#2 → ACCEPTED. Decision 2: park ADS1299 CS (+ any shared-DSPI slave-select) GPIO-output-HIGH
  before bit-bang. Added a /grill risk that the analog frontend re-inits after card.init.
- Codex#3 → ACCEPTED. Decision 2: release DSPI through the owning object; card.init must re-map PPS,
  else sdBusRecover re-runs DSPI/PPS init before returning. Flagged as the highest-uncertainty point
  for hardware verification in /grill.
- Codex#4 + Gemini#2 → ACCEPTED. Decision 6: scan cap aligned to the resume namespace; after the loop
  RE-CHECK fileSize()>0 and fail-fast (csHigh + return fileIsOpen) instead of removing the target.
- Codex#5 + Gemini#3 → ACCEPTED. Decision 2: explicit SPI mode 0 (CPOL0/CPHA0, MSB-first, SCK idles
  LOW, sample on rising edge); CMD12 discards the R1 stuff byte then parses R1/R1b.
- No rejections this round (all findings were correct and actionable).
### prep.md changes this round
Decision 2 fully rewritten: SPI-mode-0 preamble; DSPI released via owning object + ADS1299 CS parked;
6-step bounded abort with the NEW data-response-consume + MISO-high busy-wait before 0xFD; CMD12 stuff-
byte handling; PPS re-acquire requirement. Decision 6: exhaustion re-check + fail-fast. Test-plan static-
safety (a)/(d) updated. Risks: added the PPS re-acquire + ADS1299-reinit hardware-verification items.

## Round 5 — Codex: CHANGES_REQUESTED | Gemini: CHANGES_REQUESTED
(Panel refining the R4 SD-protocol fixes — several findings CORRECT an R4 overcorrection.)
### Codex findings
1. [BLOCKER] CMD25 token-wait phase still fails — between 512-byte writes, 514 0xFF produce NO
   data-response token, so "token timeout => fail-fast" never sends 0xFD. Fix: if no token + MISO high,
   treat as between-packets and send 0xFD; only busy-wait after an actual busy indication.
2. [MAJOR] Recovery after possible pre-recovery DSPI traffic — board.begin() brings up DSPI before
   replaySessionFile; if SD_SS not GPIO-high first, ADS clocks hit a still-selected CMD25 card.
   Fix: park SD_SS + ADS CS + shared CS high as raw GPIO before board.begin().
3. [MAJOR] "prior slot clusters never touched" false mid-payload — the 514 drain programs the
   interrupted sector with partial real + fill bytes → prior file tail has garbage/0xFF.
   Fix: declare the interrupted sector sacrificial; verify collect_bci truncates/ignores invalid tail.
### Gemini findings
1. [BLOCKER] Step-3 token-consumption race guarantees mid-payload recovery failure — the data-response
   token is emitted DURING the step-2 drain, so a step-3 "consume token" times out and fail-fasts a
   valid recovery. Fix: remove token parse; step 3 = pure bounded busy-wait for MISO HIGH (clocking 0xFF).
2. [MAJOR] Busy-waits must actively clock the bus — polling MISO without toggling SCK won't advance the
   card's state machine → permanent BUSY → timeout. Fix: all MISO-high waits loop sdBbByte(0xFF).
3. [MAJOR] Pin-init glitch — pinMode(SCK,OUTPUT) while LAT is stale-HIGH emits a phantom clock. Fix:
   digitalWrite(SCK,LOW)+digitalWrite(MOSI,HIGH) BEFORE the pinMode(...,OUTPUT) direction change.
4. [MINOR] Leaked handle on exhaustion fail-fast — close the re-check SdFile before return fileIsOpen.
### Resolution (Claude)
ALL net-new and ALL correct. The two BLOCKERs (Codex#1 + Gemini#1) jointly CORRECT the R4 fix: my R4
step 3 said "consume the data-response token then busy-wait", which both models showed is wrong —
the token may have already passed during the drain (mid-payload) or never appear (between packets), so
a parse/consume would fail-fast a valid recovery. Re-designed step 3 to a pure clock-and-wait-for-
MISO-high with NO token parse; fail-fast ONLY on MISO-stuck-low. This is the right simplification.
- Codex#1 + Gemini#1 → ACCEPTED. Decision 2 step 3 rewritten: no token consume; if MISO already high,
  go straight to 0xFD; only busy-wait (clocking) while MISO low; token-absence is normal, not a fail.
- Gemini#2 → ACCEPTED. Decision 2: ALL busy-waits explicitly loop sdBbByte(0xFF) while sampling MISO.
- Gemini#3 → ACCEPTED. Decision 2 pin-setup: set latches (SCK LOW, MOSI/CS HIGH) BEFORE flipping TRIS.
- Codex#2 → ACCEPTED. New Decision-2 bullet + Plan step: park SD_SS/ADS-CS high in DefaultBoard.ino
  setup() BEFORE board.begin(). DefaultBoard.ino added to Files-to-touch.
- Codex#3 → ACCEPTED (doc correction + verify, no firmware change). Decision 9 rewritten: interrupted
  block is sacrificial (partial+0xFF), harmless because every night is already footerless; /grill
  verifies collect_bci tolerates the partial+0xFF tail. Test plan got a collect_bci-tolerance item.
- Gemini#4 → ACCEPTED. Decision 6: .close() the re-check handle before the fail-fast return.
- No rejections this round.
### prep.md changes this round
Decision 2: glitch-free pin-init order; all busy-waits clock the bus; step 3 redesigned (no token
parse, fail-fast only on MISO-stuck-low); new pre-board.begin() CS-park bullet. Decision 6: close
re-check handle. Decision 9: sacrificial-block correction. Plan step 2 + Files-to-touch: DefaultBoard.ino
CS-park. Test-plan static-safety rewritten (a-g) + collect_bci tail-tolerance verify item.

## Round 6 — Codex: CHANGES_REQUESTED | Gemini: CHANGES_REQUESTED
(Findings now deep PIC32-silicon specifics; one BLOCKER (PPS) is critical + genuinely new.)
### Codex findings
1. [MAJOR] DSPI/GPIO takeover not proven interrupt-safe — an ADS DRDY ISR / timer / bg SPI user could
   re-enter DSPI while pins are raw GPIO. Fix: mask the ADS/SPI interrupt around recovery; if masking
   global ints, don't use millis() for bounds.
2. [MAJOR] Busy-low timeouts too short — 250/500ms can misclassify a programming card as dead. Fix:
   base MISO-low waits on SdFat write/erase timeout constants / seconds-scale bound.
3. [MAJOR] setupSDcard fail-fast must close/reset openfile — a partially-created file object must be
   closed before return, not just CS deasserted.
4. [MINOR] CMD12 result must be explicitly non-fatal — illegal/no-op after CMD25 0xFD; fail only on
   MISO-stuck-low, not on CMD12 R1.
### Gemini findings
1. [BLOCKER] Spurious clock on DSPI release — SPI.end() BEFORE setting latches/dirs snaps pins to stale
   LAT/TRIS → phantom SCK edge. Fix: write idle latches + set directions BEFORE clearing the DSPI ON bit.
2. [BLOCKER] PPS hijacking MOSI — on MX250 MOSI=SDO1 is PPS-routed; SPI.end() does NOT release PPS, so
   digitalWrite(MOSI) is overridden by the disabled peripheral → bit-bang clocks all zeros → 0xFD/CMD12
   never sent. Fix: clear the PPS output map for SDO1 (RPxnR=0) before bit-banging; card.init re-maps it.
3. [MINOR] CMD12 R1 stuff-byte fragility — cards send 0/1/many idle bytes; blind 1-byte discard can eat
   the real R1. Fix: clock until a byte with MSB=0 (bounded ~8) to capture R1 robustly.
4. [MINOR] Residual FAT-corruption race — ACKNOWLEDGED as user-accepted (delay-only). No action.
### Resolution (Claude)
ALL net-new and correct. Gemini#2 (PPS) is the most important catch of the entire review: it would make
the whole recovery a silent no-op on real silicon (bit-bang would clock zeros). Both BLOCKERs are about
the PIC32 DSPI->GPIO handover, which is genuinely subtle hardware sequencing.
- Gemini#2 → ACCEPTED. Decision 2 pin-handover step 3: clear the PPS output map for MOSI/SDO1 (RPxnR=0)
  so LAT drives the pin; card.init re-establishes PPS. Flagged as the single highest-risk step, /grill
  HW-verifies the exact register sequence.
- Gemini#1 → ACCEPTED. Decision 2 reordered to explicit 4-step register sequence: latches FIRST → TRIS
  → clear PPS → THEN disable module (so disabling can't snap a stale LAT/TRIS into a phantom clock).
- Codex#1 → ACCEPTED. New Decision-2 bullet: mask the ADS DRDY/SPI ISR (specific int, not global di so
  millis() runs) across recovery; /grill confirms no ISR touches DSPI in the window.
- Codex#2 → ACCEPTED. Decision 2: MISO-low programming waits use a conservative seconds-scale bound
  (~2s, aligned to SD_WRITE_TIMEOUT) so a programming card is never misread as dead; drain stays short.
- Codex#3 → ACCEPTED. Decision 5: openfile.close() on the fail-fast path before csHigh+return.
- Codex#4 + Gemini#3 → ACCEPTED. Decision 2 step 5: CMD12 best-effort, find R1 by MSB=0 scan (bounded
  8), never fail recovery on CMD12 R1, only on MISO-stuck-low.
- Gemini#4 → no action (confirms user-accepted residual risk).
- No rejections.
### prep.md changes this round
Decision 2: pin-handover rewritten to the explicit PIC32 PPS-aware 4-step register sequence + ISR-mask
bullet + seconds-scale MISO-low bound; step 5 CMD12 made best-effort with MSB=0 R1 scan. Decision 5:
close openfile on fail-fast. Test-plan static-safety rewritten (a-i).

## Round 7 — Codex: APPROVED | Gemini: APPROVED  ✅ CONVERGED
(Final backstop round. Both approve; only MINORs remain.)
### Codex findings
1. [MINOR] Plan step 1 shorthand "DSPI-disable + pinMode first" is looser than the required
   latch→TRIS→clear-PPS→disable-module order in Decision 2/test-plan. Implementation should follow
   the detailed order. → FIXED: Plan step 1 reworded to point at the exact Decision-2 order.
### Gemini findings
1. [MINOR] CMD12 R1 scan bound — extend ~8 → ~16 bytes so a non-compliant/wedged card has margin to
   return R1 (CMD12 is best-effort + non-fatal anyway; costs microseconds). → FIXED: bound raised to ~16.
### Resolution (Claude)
BOTH APPROVED. Two trivial MINORs, both accepted and applied. No BLOCKER/MAJOR. /prep converges at
round 7 (the planning backstop). The approach is signed off by both independent reviewers.
### prep.md changes this round
Plan step 1 wording aligned to the exact Decision-2 handover order; CMD12 R1 scan bound ~8 → ~16 bytes.
