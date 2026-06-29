# Prep review log — Cyton freeSD MCU-hang fix (SPIROV-bounded SPI primitive)

Adversarial planning review of `prep.md` by **Codex (gpt-5.5)** + **Gemini (3.1 Pro)**.
Claude authors/orchestrates; casts no vote. Backstop raised (user: "deeply, increase
rounds") — iterate while rounds surface net-new BLOCKER/MAJOR holes.

(Prior task's log archived as prep-review-log-busrecover-2026-06-28.md.)

---

## Round 1 — Codex: CHANGES_REQUESTED | Gemini: CHANGES_REQUESTED
(GLM joins from round 2 — glm-review wrapper confirmed present.)

### Codex findings
1. [BLOCKER] SD-fork-only bounding leaves the ADS path able to hang (shared SPI1; if SPIROV hit during an ADS read, writeCache recovery never runs).
2. [BLOCKER] sdBusRecover still contains the unbounded primitive — a one-time SPIROV clear before board.spi.transfer doesn't bound the loop.
3. [BLOCKER] ENHBUF/FIFO state not handled — raw SPI1BUF + SPITBE/SPIRBF poll only valid in standard 8-bit mode with RX drained.
4. [BLOCKER] Clearing SPIROV before every byte can MASK corruption (a lost byte) → continue a corrupted transaction. Treat SPIROV as fatal-for-command, abort, drain+clear only in recovery, retry at a boundary.
5. [BLOCKER] Mid-block/CMD25 stuck → card state ambiguous; "continue same slot" unproven. Fail closed / mark slot suspect unless cache+CMD25-stop+verify audited.
6. [MAJOR] Not all SD transfer paths covered (init, CSD/CID, FAT/cache reads, metadata, spiRec(buf,n)). Wrap every hardware _spi->transfer in the fork.
7. [MAJOR] Stuck handling after spiSend underspecified — check sdSpiStuck immediately after every send, deselect, single failure path.
8. [MAJOR] "Tens of ms" loop-count caps non-deterministic + too large → drop many ADS samples. Use a core-timer/µs deadline + sample-loss counters.
9. [MAJOR] Soft-WDT reset safety asserted not enforced — loop-end + SDfileOpen ≠ clean cache/idle card. Require CS-high + card idle + cache clean, or clean-close first.
10. [MAJOR] Soft-WDT persistence/cap lifecycle incomplete — define cap reset point; ensure tune default 0 can't be overridden by stale EEPROM/host replay.
11. [MAJOR] WDTPS= boot print may break host contract until tested.
12. [MAJOR] Synthetic hang doesn't prove the SPIROV fix; sketch-only macro won't reach the library; inject in the SD lib + log SPI1STAT/CON.
13. [MAJOR] Recovery can silently lose EEG samples — add DRDY-gap/sample-loss accounting to footer; large gaps = failure not clean recovery.

### Gemini findings
1. [BLOCKER] Throughput regression + misdiagnosis: real trigger is the ENH_BUFFER bulk transfer overrunning the 8-byte RX FIFO when CPU delayed; per-byte loop disables the FIFO → drops ADS samples. Write a correct BOUNDED ENH_BUFFER block transfer locally instead.
2. [BLOCKER] SPIROV clear leaves stale FIFO bytes → SPIRBF still set → bounded spin instantly succeeds reading a stale byte → off-by-one shift for all subsequent SD responses. Must DRAIN RX FIFO (read until SPIRBE=1) whenever SPIROV cleared.
3. [BLOCKER] Soft-WDT loop-end "safe point" corrupts FAT: card is mid-CMD25 (CS high but awaiting data/CMD12); MCU reset + CMD0 discards the open write. Must writeStop()/closeSDfile() before executeSoftReset.
4. [MAJOR] Per-night EEPROM cap never resets → accumulates across nights → permanently disables soft-WDT. Reset in setupSDcard() on a fresh (non-replay) session.
5. [MAJOR] Missing SPITBE wait in bounded primitive is NOT optional — writing SPI1BUF while full corrupts the outgoing byte.
6. [MINOR] WDTPS boot print is SAFE — session_start.py hard-drains + reset_input_buffer before its '?' probe.

### Resolution (Claude)
ACCEPTED essentially all. Key redesign for round 2:
- ENHBUF reality CONFIRMED by reading DSPI.cpp: single-byte transfer(uint32) = STANDARD mode (SPITBE/SPIRBF); the 512-byte writeData path uses transfer(512,src) which TEMPORARILY enables ENH_BUFFER and runs `while(toWrite||toRead)` with SPITBF/SPIRBE — its toRead-never-reaches-0-on-overflow IS the primary hang (Gemini#1 correct). → Replace with a LOCAL BOUNDED ENH_BUFFER bulk transfer (keep FIFO throughput) + a deterministic CP0-Count deadline; NOT a per-byte loop.
- SPIROV = FATAL-FOR-COMMAND, never clear-and-continue (Codex#4, Gemini#2): on stuck → fail the SD command → recovery does writeStop(abort CMD25) + DRAIN RX FIFO (read SPI1BUF until SPIRBE) + clear SPIROV + card re-init/sdBusRecover at a clean block boundary + fresh writeStart → continue; if it can't clear → clean stop + REPLAYFL + footer (fail closed, Codex#5). No stale-byte off-by-one.
- sdBusRecover uses the BOUNDED transfer + entry drain+clear, not a one-time clear (Codex#2).
- ADS path (Codex#1): SPIROV's only source is the ENHBUF bulk SD write; the SD write runs sequentially AFTER the ADS read in loop() (DRDY ISR sets a flag only, does no SPI); recovery drains+clears SPIROV before returning so the next ADS read sees a clean bus; belt-and-braces drain at SD-write ENTRY. Will also propose a bounded guard on board.xfer and let round 2 judge necessity.
- Deterministic CP0-Count (_CP0_GET_COUNT @ SYSCLK/2 = 20 MHz) deadlines, rollover-safe unsigned subtraction (Codex#8).
- Soft-WDT (OFF default): on fire, attempt a BOUNDED clean-close (writeStop+footer+closeSDfile — now bounded via L1) THEN executeSoftReset→fresh-slot resume; if the close can't complete → sdCardDead clean-stop, never a blind reset (Gemini#3, Codex#9). Per-night EEPROM cap reset in setupSDcard on fresh (non-replay) session (Gemini#4). Provably inert when flag 0.
- SPITBE wait mandatory (Gemini#5).
- Synthetic test: fault-inject var lives in the FORK (extern set by the sketch token), forces a REAL SPIROV (under-drain once) to exercise true detect+drain+recover, behind SD_DEBUG_FAULT_INJECT in the fork (Codex#12).
- Sample-loss/DRDY-gap accounting in footer + %E per recovered event (Codex#13).
- Cover ALL fork _spi->transfer paths, not just writeData (Codex#6); check sdSpiStuck after every send/recv with one failure path + deselect (Codex#7).
- WDTPS print: KEEP, confirmed host-safe at session_start.py:286 (reset_input_buffer before '?') — Gemini#6 right, Codex#11 over-cautious (SPLIT resolved with evidence).

---

## Round 2 — Codex: CHANGES_REQUESTED | Gemini: CHANGES_REQUESTED | GLM: CHANGES_REQUESTED
(GLM joined. Dense, heavily-corroborated NET-NEW hardware-level blockers.)

### Codex findings (16)
1.[BLOCKER] writeStop() runs while SPI still poisoned. 2.[BLOCKER] mid-block CMD25 abort unproven (0xFD is payload mid-block). 3.[BLOCKER] "continue same slot" not integrity-safe (ambiguous LBA). 4.[BLOCKER] generic chipSelectHigh conflicts with CMD25-recovery ownership. 5.[BLOCKER] bounded bulk copy still permits the overrun (detect≠prevent). 6.[BLOCKER] drain/clear ordering incomplete (TX-idle, FIFO race). 7.[MAJOR] restore prev ENHBUF state not blind-off. 8.[MAJOR] clearing sdSpiStuck at every op entry erases a pending fatal fault. 9.[MAJOR] ADS guard "around" the call can't stop a hang INSIDE DSPI::transfer. 10.[MAJOR] card-busy timeout conflated with SPI hang. 11.[MAJOR] CP0 block budget under-specified/too optimistic. 12.[MAJOR] soft-WDT clean-stop claims stronger than mechanism (footer may also fail). 13.[MAJOR] EEPROM cap not a design. 14.[MAJOR] synthetic SPIROV may not force real overrun. 15.[MAJOR] scope too large for one flash → weak attribution + flash risk; stage it. 16.[MINOR] sample-loss must be in-band parser-visible.

### Gemini findings (5)
1.[BLOCKER] ENH_BUFFER/FIFO state corruption on mid-block bail; sdSpiDrainClear flips SPIRBE/SPIRBF semantics with ENHBUF off → drain loop instantly exits → RX wedged. FIX: disable+re-enable SPI module (ON=0;nop;ON=1) — atomic flush, fewer instructions. 2.[BLOCKER] missing-block/garbage on mid-block abort; skip leaves a 512B hole → parser crash; retry same sector or zero-pad. 3.[BLOCKER] SPIROV relatch race during manual drain (bytes arrive after drain, before clear) — module reset solves. 4.[BLOCKER] ADS-guard order backwards: ADS read precedes SD write in loop(); put the drain/reset at TOP of loop()/updateChannelData, not SD-write entry. 5.[MAJOR] soft/physical reset doesn't power-cycle the card → wedged CMD25 survives reboot; init must do CS-high+≥74 clocks+CMD0 to knock it out.

### GLM findings (7)
1.[BLOCKER] recovery skips the failed block → 512B hole in contiguous extent; retry the SAME block, don't advance blockCounter until written; else fail closed. 2.[BLOCKER] sdSpiDrainClear disables ENHBUF before draining (8→1 FIFO) → stale bytes inaccessible; drain WHILE ENHBUF on, then clear ENHBUF, then SPIROV. 3.[BLOCKER] block budget too tight → false-trip on legit card GC (tens of ms); the CP0 deadline is only to break an INFINITE spin, keep card-busy on SD_WRITE_TIMEOUT. 4.[MAJOR] TX FIFO not drained before CMD12 after mid-block abort. 5.[MAJOR] soft-WDT EEPROM cap reset defeated by the reset itself (setupSDcard sees "fresh") — use a sticky flag/RCON to skip cap reset on a soft-WDT reboot. 6.[MAJOR] soft-WDT clean-close lacks atomicity/fallback (writeStop ok but footer/close fail). 7.[MAJOR] flash budget unproven (~4280 B free) — need per-component size estimate.

### Resolution (Claude) — architecture simplified by reading the EXISTING cascade
DISCOVERY (writeCache 1906-2025 + sdBusRecover 745+): the existing recovery ALREADY does
same-block retry — on card.writeData(pCache) fail it does writeStop()+writeStart(bgnBlock+
blockCounter,…)+writeData(pCache) with the SAME buffered pCache, and only THEN skips forward
(into a pre-ERASED ZERO block — card.erase() pre-zeros the extent at setup — NOT uninitialized
garbage). sdBusRecover already does the SPI-mode CMD25 abort before card.init (the boot wedged-
CMD25 knockout Gemini#5 asked for). So the SPIROV hang is INSIDE card.writeData, BEFORE this
proven cascade runs. → The fix is SURGICAL: make the primitive RETURN (bounded), then PREPEND
an SPI-module reset so the existing, battle-tested cascade runs on a clean bus. This shrinks the
diff (addresses Codex#15/GLM#7 flash) and resolves the same-block BLOCKERs by leveraging existing
logic the reviewers couldn't see.

ACCEPTED → v3 design:
- sdSpiBusReset() = SPI1CONCLR=ON; nop; SPI1CONSET=ON; re-assert MODE0 — atomic flush of TX/RX
  FIFO + shift reg + SPIROV (replaces the broken sdSpiDrainClear; solves Codex#1/6, Gemini#1/2/3,
  GLM#2/4, Codex#7/M, GLM#4). NO manual ENHBUF/drain.
- writeCache: when card.writeData(pCache) returns false with sdSpiStuck set → sdSpiBusReset()
  BEFORE the existing writeStop+writeStart(SAME block)+writeData retry. Same-block retry is the
  existing first action (Codex#2/3, Gemini#2, GLM#1 — pCache preserved). Skip-forward only after,
  into pre-erased zeros (accounted in sdRetries/footer).
- spiBlockBounded FIFO-SAFE: in-flight cap (writes−reads ≤ headroom) so SPIROV never latches under
  jitter (PREVENT, Codex#5); CP0 deadline = generous INFINITE-spin breaker (covers 512B shift +
  ISR jitter, ~ms), SEPARATE from card-busy which stays on waitNotBusy/SD_WRITE_TIMEOUT — and a
  mid-block-shift can't stall (host drives SPI clock; card-busy is BETWEEN blocks), so GLM#3/Codex#10
  GC false-trip can't happen on the shift. sdSpiStuck set ONLY by CP0/SPIROV, never waitNotBusy.
- Sticky fault: sdSpiStuck cleared ONLY by sdSpiBusReset in recovery, not at op entry (Codex#8).
- ADS path: top-of-loop()/updateChannelData-entry bus guard (Gemini#4) + a real bounded ADS byte
  primitive in OpenBCI_32bit_Library.cpp (Codex#9, SHOULD, flash permitting) — but recovery's
  sdSpiBusReset runs synchronously before returning to loop, so the next ADS read already sees a
  clean bus (primary protection); FIFO-safe prevention makes latching rare anyway.
- sdBusRecover: add sdSpiBusReset + bounded primitives (Codex#2, Gemini#5 already partly present).
- Soft-WDT (OFF default, key 0x06): clean-close (writeStop→closeSDfile, bounded) then reset; if
  close fails → mark slot suspect + forensic REPLAYFL, never blind reset (Codex#12, GLM#6). Per-night
  cap via a STICKY EEPROM "soft-wdt-reset-pending" flag (RCON unreliable — bootloader clears it),
  set before reset, checked+cleared in setupSDcard so the reset doesn't reset the cap (GLM#5/Codex#13).
- Synthetic injection: genuinely overflow (write ≥9 bytes w/o reading), test early/mid/late block,
  log SPI1STAT/CON pre/post (Codex#14).
- Sample-loss: existing sdErrs/sdRetries/%CKPT/%E already in-band; SPIROV events increment them +
  an in-band %E discontinuity marker; verify parser tolerates (Codex#16/13).
- Flash: surgical diff is small; self-test compiled OUT (no prod cost); KEEP one-flash bundle
  (USER decision: minimize flash COUNT, not size; watchdog OFF → inert → no attribution loss vs
  Codex#15). Per-component size estimate produced in /grill before flashing (GLM#7).
SPLIT: none material — all three converged. Codex#15 "stage it" REJECTED on the user's explicit
"everything in one flash" + the watchdog being inert when off (no overnight attribution loss).

---

## Round 3 — Codex: CHANGES_REQUESTED | Gemini: CHANGES_REQUESTED | GLM: (empty — transient "no review produced", re-run R4)

### Codex findings (9)
1.[BLOCKER] mid-block failure not a safe input to the cascade (bounded primitive can return written<512 / pre-CRC → writeStop consumed as payload). 2.[BLOCKER] sdSpiStuck lifetime inconsistent — primitive calls reset which clears the flag, so writeCache never sees the fault → treats it as ordinary SD failure. Split HW reset from fault consumption. 3.[BLOCKER] %E emission inside the failure path recurses into SD writing → mutates pCache/blockCounter before same-block retry; defer %E. 4.[MAJOR] SPI reinit underspecified (ON-toggle+nop+MODE0 doesn't restore BRG/MSTEN/SMP/CKE/CKP/MODE16-32/ENHBUF-off/prove SPIROV cleared). 5.[MAJOR] 0xFF timeout sentinel = SD ready → waitNotBusy reads it as not-busy; make status-bearing or check fault after every spiRec/spiSend. 6.[MAJOR] live recovery may bypass sdBusRecover (cascade only does bare card.init, which per its own comments can't abort CMD25). 7.[MAJOR] ADS guard calling fork symbols from OpenBCI_32bit_Library.cpp couples core lib to optional SD fork → breaks non-SD builds; keep register-local or in the sketch. 8.[MAJOR] soft-WDT EEPROM not brownout/power-cycle robust; old persisted tune could enable WDT during an attribution run; magic+complement, consume-once, don't auto-persist key 0x06. 9.[MINOR] specify -DSD_DEBUG_FAULT_INJECT via arduino-cli --build-property compiler.cpp.extra_flags + verify symbol.

### Gemini findings (4)
1.[BLOCKER] writeData failure sets CS high, defeating the cascade's writeStop (STOP_TRAN sent with CS high → ignored). 2.[BLOCKER] mid-block bailout leaves card expecting payload; 0xFD mid-payload is a DATA byte, not a stop → host/card desync; must clock the remaining payload+CRC (≈520) before stop. 3.[BLOCKER] writeCache recovery NEVER calls sdBusRecover (it's static, boot-only); live recovery only does card.init which can't knock a card out of mid-block CMD25. Call sdBusRecover (or the 520-flush) in the live cascade. 4.[BLOCKER] ENHBUF can only be written when ON=0 (PIC32 FRM); restoring ENH_BUFFER=0 while ON=1 fails → next standard spiRec hangs. Clear ENHBUF INSIDE the reset, between ON=0 and ON=1.

### Resolution (Claude) — confirmed sdBusRecover (740-772) is the correct position-agnostic CMD25 abort
sdBusRecover already does: CS-low → 520 dummy bytes (finishes ANY partial payload) → bounded BUSY
wait → 0xFD STOP_TRAN → BUSY wait → CS-high+idle. It's static in SD_Card_Stuff.ino (callable from
writeCache) but currently boot-only, and uses the UNBOUNDED board.spi.transfer. → v4:
- sdSpiModuleFlush(): ON=0 → clear ENHBUF (legal only when ON=0, Gemini#4) → clear MODE16/32 →
  clear SPIROV → ON=1, re-assert MODE0; does NOT clear the fault latch (Codex#2/4). Settle via
  readback, not a bare nop.
- sdSpiFault = STICKY latch set by the bounded primitives on bail; the primitive does NOT
  flush/reset (recovery owns that); cleared ONLY by writeCache after it records+handles (Codex#2).
- Callers (waitNotBusy etc.) check sdSpiFault after every spiRec/spiSend, not the 0xFF byte
  (Codex#5).
- writeCache recovery: if (sdSpiFault) → sdSpiModuleFlush() + sdBusRecover() (LIVE call — the
  proper 520-flush+STOP_TRAN; Gemini#2/3, Codex#6, Codex#1) + card.init + writeStart(SAME block)+
  writeData(pCache); DEFER %E out of the critical section (Codex#3); clear sdSpiFault. The
  NON-SPIROV boundary failure keeps the existing bare cascade (byte-identical).
- sdBusRecover: sdSpiModuleFlush at entry + route its pokes through a bounded byte so the 520-flush
  can't itself hang.
- ADS guard: a top-of-loop() SPIROV check in the SKETCH (DefaultBoard.ino, raw SFRs) — NO core-lib
  edit, no fork coupling (Codex#7, also satisfies Gemini R2#4 read-side guard). Drop
  OpenBCI_32bit_Library.cpp from files-to-touch.
- Soft-WDT: key 0x06 NOT auto-written into the resume SESSION.TXT %TUNE line → defaults OFF every
  session (Codex#8); sticky cap-reset flag = magic+complement, consume-once; audit EEPROM addrs.
- Self-test: -DSD_DEBUG_FAULT_INJECT via --build-property compiler.cpp.extra_flags, verify symbol
  in the .map (Codex#9).
PRE-EXISTING (out of scope): Gemini#1 (writeStop runs with CS high in the boundary cascade) is a
latent issue in the EXISTING boundary path; the SPIROV path uses sdBusRecover's own CS, so THIS
bug is handled. Not touching the boundary path keeps non-SPIROV behaviour byte-identical; noted.

---

## Round 4 — Codex: CHANGES_REQUESTED | Gemini: CHANGES_REQUESTED | GLM: CHANGES_REQUESTED
(Tight convergence — all three caught the SAME fault-clear-ordering bug. Refinements now.)

### Codex (5)
1.[BLOCKER] sticky sdSpiFault poisons its own recovery (still set while sdBusRecover/init/writeStart/writeData run → they all bail). 2.[BLOCKER] sdBusRecover must not honor the pre-existing latch (else 520-flush/STOP no-ops). 3.[MAJOR] sdSpiModuleFlush restore unsafe — saving CON mid-bulk captures ENHBUF=1; blind restore re-enables it. Restore a MASKED CON (force ON=0, ENHBUF=0, MODE16/32=0, keep mode bits, saved BRG). 4.[MAJOR] fault ownership incomplete outside writeCache (init/CSD/erase/writeStart/close/soft-WDT). 5.[MAJOR] top-of-loop ADS guard forces SD MODE0 before the ADS read → corrupts EEG unless ADS reprograms SPI every transfer.

### Gemini (5)
1.[BLOCKER] sdSpiFault cleared too late → every recovery step instantly aborts. FIX: clear it right after sdSpiModuleFlush, before sdBusRecover. 2.[BLOCKER] SPIROV recovery branch lacks failure handling → infinite silent block-drop loop (bypasses sdCardDead). Check writeStart/writeData returns; on fail → sdCardDead cascade. 3.[BLOCKER] soft-WDT defeats itself: aborting the reboot when clean-close fails leaves the board permanently dead — a watchdog MUST reboot. executeSoftReset even if close fails (mark slot suspect). 4.[MAJOR] sdBusRecover 520-loop on a wedged bus = 520×deadline ~5s stall. Add if(sdSpiFault) break in the loop. 5.[MAJOR] sdSpiModuleFlush forcing MODE0 breaks ADS1299 (needs MODE1). Restore SPI1CON exactly as saved, masking only ENHBUF+MODE16/32.

### GLM (5)
1.[BLOCKER] sdSpiFault latch deadlocks recovery primitives — clear immediately after sdSpiModuleFlush, before sdBusRecover. 2.[BLOCKER] sdSpiModuleFlush CON restoration ambiguity + ADS mode corruption via the top-of-loop guard; restore masked-saved CON; for the guard either verify ADS setMode-per-transfer, clear-SPIROV-only, or OMIT it (recovery runs synchronously). 3.[MAJOR] sdBusRecover bounded-byte cross-TU dependency — extern spiByteBounded from the fork or sketch-local. 4.[MAJOR] flash budget fallback missing — define PRODUCTION trim levers (debug prose is already out). 5.[MINOR] in-flight cap of 4 = only 1.6µs headroom; use 7 / eager-drain on SPIRBE.

### Resolution (Claude) — convergent, mostly accepted; ADS guard dropped
- **Fault-clear ordering (unanimous BLOCKER)**: reconcile R3#2 vs R4 cleanly — the PRIMITIVE only
  LATCHES sdSpiFault (so writeCache SEES it) and does NOT flush; **sdSpiModuleFlush() CLEARS
  sdSpiFault** (the "hardware handled" point). writeCache checks sdSpiFault → calls sdSpiModuleFlush
  (clears flag+hw) → THEN sdBusRecover/init/writeStart/writeData run with a clean flag. A NEW fault
  during recovery re-latches (Codex#1/2, Gemini#1, GLM#1).
- **sdSpiModuleFlush restore (Codex#3, Gemini#5, GLM#2)**: save CON; ON=0; in the SAVED value clear
  ENHBUF+MODE16/32 ONLY (preserve mode/CKE/CKP/MSTEN + saved BRG); clear SPIROV; write masked CON;
  ON=1; readback. NEVER hardcode MODE0 (would break ADS MODE1).
- **SPIROV branch fail-closed (Gemini#2)**: check writeStart()/writeData() returns; on failure →
  join the existing sdCardDead/skip-forward/extended-window tail, never silent-exit.
- **sdBusRecover (Gemini#4, GLM#3)**: sdSpiModuleFlush at entry (clears flag+hw); pokes via extern
  spiByteBounded; `if (sdSpiFault) break;` in the 520-loop (and the BUSY waits) so a still-wedged
  bus bails fast instead of 520×deadline.
- **Soft-WDT always reboots (Gemini#3)**: on fire (opt-in only) → best-effort bounded clean close →
  executeSoftReset(0) EVEN IF close fails (mark slot suspect via REPLAYFL), bounded by the per-night
  cap. Safe because: 120s floor = only true freezes (board already hung, not actively writing); the
  slot's dir entry is written at createContiguous so a reset leaves READABLE partial data (like
  5D/5E), not all-NUL; boot sdBusRecover knocks the wedged card out of CMD25; per-night cap + 2.5s
  resume-stabilize prevent a reset storm (the original all-NUL cause). Still OFF by default.
- **DROP the top-of-loop ADS guard entirely (Codex#5, Gemini#5, GLM#2)**: SPIROV's ONLY source is
  the ENH_BUFFER bulk SD write; recovery's sdSpiModuleFlush runs SYNCHRONOUSLY in the same loop
  iteration's SD-write phase, BEFORE the next iteration's ADS read; standard-mode ADS reads can't
  self-overflow. So the ADS path never sees a poisoned bus → no guard needed. Removes the ADS-mode
  risk + saves flash. (No OpenBCI_32bit_Library.cpp edit; Layer 1c removed.)
- **Fault ownership (Codex#4)**: every recovery/init ENTRY calls sdSpiModuleFlush (clears stale
  flag+hw); fail-fast paths (setupSDcard) remain fail-closed (REPLAYFL). The latch only short-
  circuits in-flight primitives of a wedged op; the next owner flushes.
- **FIFO headroom (GLM#5)**: eager-drain every loop iteration on SPIRBE; write-gate at in-flight<7.
- **Production trim levers (GLM#4)**: self-test already compiled out; if over budget, trim verbose
  %BOOT/%CKPT/console strings, and (last resort) the soft-WDT suspect-marking elaboration — measured
  per-component in /grill before flash.

---

## Round 5 — Codex: CHANGES_REQUESTED | Gemini: CHANGES_REQUESTED | GLM: CHANGES_REQUESTED
(Converging — flow-ordering + 2 net-new holes from the round-4 guard removal.)

### Codex (3)
1.[BLOCKER] Plan Layer-1 bullets still say "does NOT touch sdSpiFault" + "restore MODE0" — stale vs corrected Decision 3. 2.[BLOCKER] masked CON `save & ~(ENHBUF|MODE16|MODE32)` keeps ON=1 → writes ENHBUF while ON=1 (violates the rule). Use `~(ON|ENHBUF|MODE16|MODE32)`, write while off, readback, set ON separately. 3.[MAJOR] SPIROV retry failure can fall into the tail with a NEWLY latched fault / active CMD25; normalize (flush+sdBusRecover or fail-closed) + document blockCounter/bgnBlock/pCache/CMD25 invariants at tail entry.

### Gemini (4)
1.[BLOCKER] Terminal SD failures poison the ADS bus — a SPIROV during cardCommand/writeBlock(FAT)/writeFooter bubbles to sdCardDead/soft-WDT which DON'T flush → next ADS read garbage. The mode hazard is gone (Decision 3 masked-CON), so RE-INTRODUCE `if (sdSpiFault){sdSpiModuleFlush();}` at top of loop (or flush in sdCardDead + soft-WDT). 2.[BLOCKER] soft-WDT clean-close is dead-on-arrival — a freeze likely already set sdSpiFault → the close short-circuits instantly. Call sdSpiModuleFlush() BEFORE the clean close. 3.[MAJOR] soft-WDT cap defeated by SESSION.TXT loss — not persisting 0x06 means a soft-WDT resume comes back WDT-OFF → can only fire once/night, cap moot, rest of night unprotected. Allow 0x06 in SESSION.TXT %TUNE (session-scoped, not global). 4.[MINOR] fall-through runs a 2nd doomed same-block retry; jump to skip-forward on live-recovery failure.

### GLM (3)
1.[MAJOR] sdCardDead/boundary tail leaves SPI poisoned for ADS (guard removed) — sdCardDead/terminal tail MUST sdSpiModuleFlush before returning. 2.[MINOR] Plan Layer 1.2 "restore MODE0" contradicts Decision 3 — align to masked-saved CON. 3.[MINOR] SPIROV recovery success path unspecified — add explicit `else { blockCounter++; return; }` so success doesn't fall through to the boundary cascade.

### Resolution (Claude) — all accepted; guard re-introduced (now mode-safe)
- Plan step 2 updated to match Decision 3 (clears sdSpiFault; masked-saved CON; no MODE0) — Codex#1/GLM#2.
- Decision 3: `masked = save & ~(ON|ENHBUF|MODE16|MODE32)`; write masked (ON=0), clear SPIROV,
  readback, then `SPI1CONSET=ON` separately (Codex#2).
- RE-INTRODUCE Layer 1c: top-of-loop `if (sdSpiFault) sdSpiModuleFlush();` — keyed on the LATCH,
  mode-safe via the masked-CON restore (this is WHY round-4's MODE0 version was unsafe and this is
  not). Catches EVERY terminal/non-writeCache path (sdCardDead, soft-WDT, FAT writes) before the
  next ADS read (Gemini#1, GLM#1). [Reverses the round-4 drop, now correct.]
- Soft-WDT: sdSpiModuleFlush() FIRST (clears hw+latch) then the best-effort clean close then always
  reset (Gemini#2).
- Soft-WDT enable 0x06 IS carried in the session's SESSION.TXT %TUNE line (session-scoped → survives
  a within-night resume; a fresh night's host-written SESSION.TXT omits it → default OFF → no leak to
  an attribution night). Reconciles Gemini#3 with Codex R3#8. (Our overnight test runs with it OFF.)
- SPIROV branch flow (Codex#3, Gemini#4, GLM#3): on success → `blockCounter++; byteCounter=0;` +
  deferred %E + return (normal resume). On failure → `if(sdSpiFault) sdSpiModuleFlush();` then jump
  DIRECTLY to skip-forward/extended-window/sdCardDead (bypass the redundant same-block retry). Document
  the tail-entry invariants.

---

## Round 6 — Codex: CHANGES_REQUESTED | Gemini: CHANGES_REQUESTED | GLM: APPROVED
(GLM approves. Remaining = narrow, precise refinements + doc consistency.)

### Codex (1)
1.[MAJOR] Stale bullets negate round-5 fixes: Files-to-touch still says "No top-of-loop SPIROV guard" + "tune key 0x06 (not persisted)"; Risks still says "re-assert SD MODE0". Align all to the corrected Decisions.

### Gemini (3)
1.[BLOCKER] PIC32 simultaneous ON/ENHBUF write hazard: `SPI1CON = masked` clearing ON+ENHBUF in one write may evaluate ENHBUF-writability on the OLD ON=1 → ENHBUF clear silently ignored. SPLIT: SPI1CONCLR=ON; readback; SPI1CONCLR=ENHBUF|MODE16|MODE32; clear SPIROV; SPI1CONSET=ON. 2.[BLOCKER] SPIROV retry failure leaves card wedged in CMD25: if the live retry's writeStart succeeded but writeData failed, the card is mid-CMD25; jumping to skip-forward whose writeStop sends one 0xFD (a data byte) → desync. Call sdSpiModuleFlush AND sdBusRecover before the skip-forward tail. 3.[MAJOR] Soft-WDT cap increment omitted — read/check/skip-reset described but never INCREMENT → fires endlessly → reset storm. Read+increment+writeback before the sticky flag + reset.

### GLM (1) — APPROVED
1.[MINOR] FIFO headroom inconsistent (Decision 4 "≤4" vs final "<7"). Align to in-flight<7 + eager-drain.

### Resolution (Claude) — all accepted
- Decision 3 → use ATOMIC bit ops, no masked-save (cleaner + resolves the hazard): `SPI1CONCLR=ON`
  → readback → `SPI1CONCLR=(ENHBUF|MODE16|MODE32)` → `SPI1STATCLR=SPIROV` → `SPI1CONSET=ON`. Mode/
  CKE/CKP/MSTEN/BRG are NEVER touched (so naturally preserved — no MODE re-assert anywhere). This
  SUPERSEDES Codex R5#2's single-write (Gemini R6#1's split is the safe hardware-correct form).
- Plan step 7 failure path → `sdSpiModuleFlush(); sdBusRecover();` (the 520-flush knocks the card
  out of CMD25) BEFORE jumping to skip-forward (Gemini R6#2).
- Soft-WDT (Decision 12 / Plan 11): read+increment+writeback the per-night cap byte immediately
  before setting the sticky reset-pending flag + executeSoftReset (Gemini R6#3).
- FIFO headroom → in-flight < 7 everywhere (GLM R6#1).
- Fix the 3 stale bullets (Files-to-touch top-of-loop guard + 0x06 session-scoped; Risks drop "MODE0
  re-assert") (Codex R6#1).

---

## Round 7 — Codex: CHANGES_REQUESTED | Gemini: APPROVED | GLM: APPROVED
(Two approvals. Codex raised one net-new MAJOR on the happy-path ENHBUF toggle; minors only otherwise.)

### Codex (1)
1.[MAJOR] spiBlockBounded happy-path ENHBUF toggle under-specified — on MX2xx ENHBUF set/clear also needs ON=0; the non-SPIROV path could leave ENHBUF wrong before spiSend/spiRec. Specify the same ON-clear/readback/ENHBUF/ON-set split for spiBlockBounded entry + successful exit, preserving mode/BRG.

### Gemini — APPROVED (3 MINOR)
1. name the EEPROM counter vs the limit distinctly. 2. force the register readbacks as volatile `(void)SPI1CON;`. 3. note the tail's redundant second STOP_TRAN after sdBusRecover is harmless.
("mature, production-ready plan.")

### GLM — APPROVED (1 MINOR)
1. give the per-night cap byte magic+complement torn-write protection too (deferrable; off-by-default makes it non-material).

### Resolution (Claude)
- ACCEPT Codex#1: spiBlockBounded now enables/disables ENHBUF with the SAME ON=0 split discipline as
  sdSpiModuleFlush (preserve mode/BRG); noted the stock-DSPI ENHBUF-toggle-while-ON empirically works
  on this MX250 (bench-validation reference) but the split is spec-correct; brief ON=0 between blocks
  (CS low, not mid-byte) doesn't disturb CMD25 — verify on bench.
- Folded all MINORs into Risks (counter naming + magic+complement on the cap byte; volatile
  `(void)SPI1CON` readbacks; double-STOP_TRAN-is-safe comment).

---

## Round 8 — Codex: APPROVED | Gemini: CHANGES_REQUESTED | GLM: APPROVED
(The v8 happy-path ENHBUF change — made for Codex R7#1 — introduced a regression Gemini caught.)

### Gemini (3)
1.[BLOCKER] Happy-path SPI pin-glitch: toggling ON=0 per block in spiBlockBounded releases SCK/SDO to PORT/TRIS while CS is low → spurious clock edge → desyncs the card. REVERT the happy-path to the empirically-safe ON=1 stock-DSPI approach; reserve the ON=0 split EXCLUSIVELY for sdSpiModuleFlush (recovery). 2.[MAJOR] soft-WDT clean-close defeated mid-CMD25: sdSpiModuleFlush then writeStop fails (bare 0xFD eaten as a data byte). Call sdBusRecover (520 clocks) after sdSpiModuleFlush before the close. 3.[MAJOR] spiByteBounded must entry-short-circuit on sdSpiFault, else a naive caller (FAT/close) stalls a full CP0 deadline per byte before bubbling up.

### Codex — APPROVED ("v8 closes the ENHBUF gap; no net-new hole").
### GLM — APPROVED (noted the happy-path ENHBUF exit toggle as spec-correct/safe — DISAGREES with Gemini#1).

### Resolution (Claude) — Codex↔Gemini split resolved in Gemini's favor
- **Gemini#1 (BLOCKER) ACCEPTED → REVERT v8's happy-path ON=0 split.** spiBlockBounded happy path now
  matches the stock DSPI (ENHBUF set/clear while ON=1) — EMPIRICALLY proven on this MX250 and it never
  toggles ON mid-transaction (no SCK glitch). The ON=0 split stays EXCLUSIVE to sdSpiModuleFlush, where
  it's safe (the transaction is being abandoned + bus fully reset). This is a genuine Codex(wanted ON=0
  split everywhere)↔Gemini(ON=0 mid-transaction glitches)↔GLM(ON=0 exit is safe) split: resolved for
  Gemini because its mechanism is concrete (ON=0 → SCK to PORT/TRIS → spurious edge) AND the stock
  ON=1 path is proven, so the hot path needs no new toggling at all. Codex R7#1's underlying worry
  (ENHBUF-write-while-ON ineffective) is empirically false on THIS silicon (recordings work).
- **Gemini#2 (MAJOR) ACCEPTED**: soft-WDT close = sdSpiModuleFlush → sdBusRecover → writeFooter/close →
  always reset.
- **Gemini#3 (MAJOR) ACCEPTED**: spiByteBounded (and spiBlockBounded) entry short-circuit
  `if (sdSpiFault) return …;` — instant abort on a poisoned bus, no deadline stall.

---

## Round 9 — Codex: APPROVED | Gemini: APPROVED | GLM: APPROVED  ✅ FULL SIGN-OFF

All three approve. Only non-blocking MINORs remain (carry to /grill):
- Gemini: `sdBusRecover` is `static` in SD_Card_Stuff.ino; the .ino concatenation auto-prototypes it,
  but if a toolchain version mis-handles the static prototype, add a forward decl or drop `static`.
  (Note: sdBusRecover is only called within SD_Card_Stuff.ino — writeCache + soft-WDT — so this is a
  belt-and-braces verification point.)
- GLM: explicitly document + bench-verify that a bailed `spiBlockBounded` (leaves ENHBUF on) relies
  entirely on the recovery owner's `sdSpiModuleFlush()` to restore standard mode. No code change.

### Outcome
Plan converged after **9 adversarial rounds** (Codex + Gemini + GLM). Verdict history:
R1 CCC · R2 CCC · R3 CC(GLM empty) · R4 CCC · R5 CCC · R6 CC**A** · R7 C**AA** · R8 **A**C**A** · R9 **AAA**.
(C=CHANGES_REQUESTED, A=APPROVED). The hardest issues were hardware-level (ENH_BUFFER FIFO overrun
diagnosis, the mid-CMD25 desync recovery via the existing sdBusRecover, the fault-latch lifecycle,
the ON=0/ENHBUF write-ordering, and the happy-path SCK-glitch regression that a fix introduced and
the next round caught). prep.md is the final deliverable; ready for /grill.
