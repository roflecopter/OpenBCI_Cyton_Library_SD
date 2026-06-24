# prep-review-log — Cyton freeSD recording-truncation fix

Adversarial panel: Codex (gpt-5.5) + Gemini (3.1 Pro). Claude orchestrates, casts no vote.

## Round 1 — Codex: CHANGES_REQUESTED | Gemini: CHANGES_REQUESTED
The panel reviewed the FIRST prep.md (root cause = "`sdMetaProcess` `%META` pCache overflow corrupts
`BLOCK_COUNT`"). Both rejected it, and Gemini caught a factual error that — on verification against the
code — **demolished the original root cause**. This round triggered a full re-diagnosis.

### Codex findings (10; key ones)
1. [BLOCKER] Overflow mechanism not reconciled: `%META` 234 B + `%BOOT` 57 B < 512, so the claimed
   in-loop overflow isn't proven. Don't flash until the exact failing path is modelled.
2. [BLOCKER] Raw `writeCache()` callers are a live class — fix all, not just `sdMetaProcess`.
3. [BLOCKER] Final pad-flush could leave `byteCounter==512` / write a stale block.
4-10. [MAJOR/MINOR] `B=` is a canary not proof; `SESS_<NN>.TXT` can disturb SD cache state; footer/
   forced-fill coverage; `.map` may prove the wrong (pointer vs buffer) address; 10-15 min bench can't
   validate the 12 h surface; `try/guard` wrong failure model for embedded; fix `meta verified` honesty.

### Gemini findings (key ones)
1. [BLOCKER] **"Hallucinated `writeCache()` contract → double-increment bug."** Lines 1856-1857
   (`byteCounter=0; blockCounter++;`) are **inside `writeCache()` itself** (success path), NOT inside
   `writeDataToSDcard`. `writeCache()` already resets/advances on every successful block. Injecting that
   bookkeeping into `sdMetaProcess` would **double-increment `blockCounter`**. The bare `writeCache()`
   pattern is already correct on the success path.
2. [BLOCKER] The real `pCache` overflow is the `writeCache()` **error path**: `if(!resumed){ sdCardDead=
   true; … return; }` returns without `byteCounter=0`, so a caller mid-`pCache[byteCounter++]` loop walks
   past 512 on an SD write *failure*. Fix = `byteCounter=0;` in that block.
3. [MAJOR] `SESS_<NN>.TXT` is flash bloat at 96 % + FAT risk — drop it.
4. [MINOR] A short clean bench proves nothing about a transient-failure-triggered bug — must inject a fault.

### Resolution (Claude) — VERIFIED AGAINST THE CODE, root cause REPLACED
- **Gemini #1 ACCEPTED — confirmed by reading the source.** 1856-1857 are inside `writeCache()`; the
  RAM-stage `%META` path (1461-1492) is already correct on the happy path. The original fix is WRONG and
  dropped (prep Decision 4).
- **Codex #1 ACCEPTED — and pursued to ground.** Audited EVERY `pCache` writer (`%META` replay, `%BOOT`
  EMIT macros, `%CKPT` snprintf, `%E` flush, `convertToHex`, `writeFooter`): all bounds-checked / reset on
  success. The ONLY overflow path is Gemini #2's error path, which needs an SD write failure — and the
  06-23 footer recorded **zero** SD errors. ⇒ **no buffer overflow happened; `BLOCK_COUNT` was not
  overflow-corrupted.** Original root cause refuted.
- **Re-diagnosis from the raw card (OBCI_4E):** `BLOCK_COUNT` at fill = **101,000 = exactly the `F`/30-min
  slot value** (integer-scaled), while the file = 2,432,000 = the `K`/12 h slot. The only producer of
  101,000 is `setupSDcard`'s `F` switch case executing. `sdProcessChar` dispatches slot chars to
  `setupSDcard()` **without the `!board.streaming` guard that P/T/M have**, and `setupSDcard` clobbers
  `BLOCK_COUNT` before any guard. ⇒ **NEW root cause: a stray slot-char byte mid-session rewrites
  `BLOCK_COUNT` to a shorter duration.** `%META`/`%START` absence + empty `OBCI_46/4B/4C` corroborate a
  stray slot char in the handshake window calling `setupSDcard` → spurious `incrementFileCounter`.
- **Gemini #2 ACCEPTED as a SEPARATE latent fix** (prep Decision 6) — real but not the 06-23 cause;
  include the `byteCounter=0` one-liner while flashing.
- **Gemini #3 ACCEPTED** — `SESS_<NN>.TXT` dropped (prep Decision 5).
- **Gemini #4 / Codex #8 ACCEPTED** — replaced the weak bench with **deterministic stray-char injection**
  (prep P3.2): send a slot char mid-stream and prove `B=2432000` holds.
- **Codex #4 (`B=` canary not proof) ACCEPTED** — `B=` is explicitly a canary; the guard is the fix
  (prep Decision 3). Codex #2/#3/#5/#7/#10 (raw-caller class, pad-flush, SESS cache, `.map`, meta honesty)
  are **mooted** by the root-cause change (no `sdMetaProcess`/`SESS` edit, no overflow to prove).

### prep.md changes this round
Full rewrite: new root cause (unguarded `setupSDcard` `BLOCK_COUNT` clobber, arithmetic-proven from the
card); fix = `setupSDcard` entry guard `if (board.streaming || SDfileOpen) return fileIsOpen;` + dispatch
guard + `%CKPT B=` canary + Gemini #2 error-path one-liner; dropped the `sdMetaProcess` change and
`SESS_<NN>.TXT`; Phase 0 = guard-safety proof + full command-byte audit; Phase 3 = direct fault injection.

## Round 2 — Codex: CHANGES_REQUESTED | Gemini: CHANGES_REQUESTED
Both reviewed the rewritten (new-root-cause) prep.md and CONVERGED on one net-new escalation.

### Codex findings
1. [BLOCKER] `j`/`s` remain single-byte night-killers — guarding only `setupSDcard` leaves the stray-RX
   class alive; harden stop/control too (ignore during recording or require a multi-byte stop).
2. [MAJOR] `SDfileOpen` guard safety still an assumption — PROVE from code (P doesn't set it; all
   close/error/footer paths clear it; replay doesn't set it; test 2 sessions/uptime after natural & `j` close).
3. [MAJOR] Predicate fixes only the slot clobber, not the wider stray-command class — P0.2 must produce
   actual guards, not just an audit.
4. [MAJOR] Missing `%META`/`%START` over-explained — treat as corroboration; harden/test b/M/s/j paths.
5. [MAJOR] P3.2 insufficient as written — also inject in the `SDfileOpen && !streaming` handshake window
   (before M and before b); assert `%META`/`%START`/counter/`B=`.
6. [MINOR] Gemini `writeCache` `byteCounter=0` fix OK if scoped exactly (no block-counter change).

### Gemini findings
1. [BLOCKER] Better mechanism for missing `%META`/`%START`: a MID-STREAM stray slot char → `setupSDcard`
   → `createContiguous` fails (SPI locked in multi-block write) → keeps old `bgnBlock` → `writeStart(bgnBlock)`
   restarts at block 0 → overwrites `%META`/`%START`/`%BOOT`. Fix the justification (guard still fixes it).
2. [BLOCKER] Unguarded `j`/`s`/channel toggles leave the session brittle — implement **wholesale RX
   hardening in `loop()`: ignore all command bytes while streaming except a hardened multi-byte stop**.
3. [MAJOR] P3.2 must also inject `j`/`s`/`1` mid-stream; recording must continue, channels intact.
4. [MINOR] Decision-1 guard + Gemini `byteCounter=0` are safe (single-threaded dispatch ⇒ no timing gap;
   `SDfileOpen` reliably cleared by `closeSDfile`; auto-resume fine) — but augment with Finding 2.

### Resolution (Claude) — net-new MAJOR accepted; fix escalated to two layers
- **Codex #1 / #3, Gemini #2 ACCEPTED (net-new BLOCKER/MAJOR) → LAYER A.** Adopted wholesale RX
  hardening: `loop()` ignores all inbound command bytes (Serial0/1/wifi) while `streaming && SDfileOpen`,
  except a 3×`j`-within-2 s hardened stop. Confirmed from code that `loop()` (142-180/217-227) feeds RX to
  `sdProcessChar`→`board.processChar` even while streaming, so stray `j`/`s`/`1`-`8` reach destructive
  handlers. Closes the whole class. (prep Decision 1.)
- **Codex #2 ACCEPTED — proven from code (P0.1).** `SDfileOpen` set only by `setupSDcard`/`closeSDfile`,
  cleared on every end path (1357/988/1668/1835), false on fresh boot; `P`/`T`/`M` don't set it. Added a
  HOST belt: `session_start` sends `j` before the slot char so it's false even with no power-cycle
  (prep Decision 3). Gemini #4 independently confirms this is safe.
- **Gemini #1 ACCEPTED — verified + worse.** Read `setupSDcard` 1130-1189: on mid-stream re-entry it runs
  `incrementFileCounter`/`volume.cacheClear`/`card.erase(bgnBlock,endBlock)`/`writeStart(bgnBlock)`/
  `blockCounter=0`/`%BOOT` re-emit with NO early return on `createContiguous` failure — so it can erase the
  live recording, not just clobber `BLOCK_COUNT`. Adopted as the corroborating mechanism (Codex #4:
  corroboration, not load-bearing). The Layer-B guard prevents the whole re-entry. (prep P0.3.)
- **Codex #5 / Gemini #3 ACCEPTED → P3.2 expanded** to inject slot char + `j` + `s` + `1` both mid-stream
  AND in the handshake window, plus a hardened-stop positive test.
- **Codex #6 / Gemini #4 (writeCache fix) ACCEPTED, scoped** — `byteCounter=0;` only, no block-counter
  change (prep Decision 7, P0.4).
- Kept Layer B (`setupSDcard` entry guard) for the pre-streaming handshake window + defense-in-depth.

### prep.md changes this round
Restructured the fix into LAYER A (wholesale RX-ignore-during-stream + 3×`j` stop, DefaultBoard.ino
`loop()`) + LAYER B (`setupSDcard` entry guard + dispatch mirror) + HOST `j`-belt; `%CKPT B=` canary;
scoped `writeCache` `byteCounter=0`. P0 now proves the `SDfileOpen` lifecycle + scopes Layer A + confirms
Gemini's re-entry mechanism. P3.2 expanded to multi-byte fault injection in both windows. Added an
auto-resolved ASSUMPTION tag for the wholesale-RX-hardening choice.

## Round 3 — Codex: CHANGES_REQUESTED | Gemini: CHANGES_REQUESTED
Net-new BLOCKERs that reshaped the fix (NOT a confirmation round). NB: both reviewers reported the sandbox
DENIED repo access → they reviewed plan-text only; Claude's line-cited source reads are the authority.

### Codex findings
1. [BLOCKER] Handshake window still exposes destructive bytes (`j`/`1`/premature `b`) — Layer A inactive
   when `!streaming`, Layer B only blocks slot re-entry. Add a handshake allowlist/lock.
2. [BLOCKER] Host `j`-belt no longer guarantees cleanup once Layer A ignores a lone `j`; host must use the
   hardened stop + verify closed.
3. [MAJOR] `jjj` is not hardened against repeated-byte RF/UART bursts — use a non-repeating token or remove
   in-band stop for sleep builds.
4. [MAJOR] RX hardening must cover EVERY ingress (the `writeCache` extended-recovery drain) — centralize.
5. [MAJOR] P3.2 insufficient — inject `j`/`s`/`1`/premature `b`/bursts/partial sequences, in both windows.
6. [MAJOR] DSPI hard freeze still parked → narrow the "trust a sleep night" claim or include a watchdog.
7. [MINOR] Flash trim order lists host items (no PIC32 cost) — trim firmware first.
8. [MINOR] Sandbox couldn't read the repo → cited lifecycle proof needs direct source verification.

### Gemini findings
1. [BLOCKER] Host `j`-before-slot DELETES the just-written SESSION.TXT (`P` writes it first;
   `closeSDfile`→`SdFile::remove`) → breaks auto-resume. (Send `j` before `P`, or drop the belt.)
2. [BLOCKER] The 3×`j` stop is EATEN by `writeCache`'s 8 s extended-recovery serial drain
   (`while(hasDataSerial0()) getCharSerial0();`) — a stop during a stall never reaches `loop()`.
3. [BLOCKER] Retracts its own round-2 "mid-stream block-0 overwrite" — physically impossible under the
   CMD25 multi-block lock; the total `%META` absence proves a HANDSHAKE-window hit (before block 0 flush).
4. [MAJOR] 3×`j` is the weakest stop vs repeated-byte bursts — use distinct chars.
5. [MAJOR] P3.2 misses recovery-drain + SESSION.TXT-survival checks.

### Resolution (Claude) — fix converged; in-band stop & host belt RETIRED
- **Codex #1/#3/#4, Gemini #2/#4 ACCEPTED → drop the in-band stop entirely (Decision 2).** A stop
  byte/sequence is itself RF-forgeable (a burst IS `jjj`) and is eaten by the recovery drain. Removing it
  closes both holes. Sleep workflow never needs it (auto-fill/power-cycle).
- **Codex #1 ACCEPTED → handshake allowlist (Decision 1).** Generalized Layer A into ONE centralized,
  state-keyed `dispatchCommandByte` (IDLE normal / HANDSHAKE allow only `b`/`~`/`?` / RECORDING drop all),
  called from all 3 ingress sites. Verified from host source the only post-slot-char bytes are `~~`,`M`,`b`
  (`M`/`P`/`T` consumed by their processors first). Covers Codex #4 (centralized; recovery drain only
  discards).
- **Gemini #1 ACCEPTED → drop the host `j`-belt (Decision 4).** It would delete the fresh SESSION.TXT and
  is unnecessary (P0.1 proves `SDfileOpen==false` at legit start); the still-open edge fails LOUD instead.
- **Gemini #3 ACCEPTED → corrected mechanism (P0.3).** Adopted the handshake-window `volume.cacheClear`
  discard-of-unflushed-`%BOOT`+`%META` as the likely path; explicitly marked corroboration-only (exact
  write-pointer behavior NOT relied upon — the guards prevent the re-entry either way).
- **Codex #5 / Gemini #5 ACCEPTED → P3.2 expanded** (slot/`j`/`s`/`1` + bursts, both windows, recovery-drain
  injection, no-in-band-stop check) + **P3.2b SESSION.TXT survival**.
- **Codex #6 ACCEPTED → scope narrowed (Decision 9):** claim covers the stray-RX class only; DSPI freeze
  explicitly out.
- **Codex #7 ACCEPTED** — firmware-only trim order. **Codex #8 / Gemini sandbox-denied ACCEPTED (P0.6)** —
  panel reviewed plan-text only; /grill re-verifies the cited code on the real build before flash.
- Verified from source this round: `loop()` dispatch (159-169), host send order (channel/`~`/`T`/`P` before
  slot; `~~`/`M`/`b` after), `setupSDcard` re-entry body (1130-1189: `cacheClear`/`erase`/`writeStart`/
  `blockCounter=0` with no early return), `SDfileOpen` lifecycle.

### prep.md changes this round
Goal/Decisions rewritten: single centralized state-keyed RX policy (Decision 1) + no in-band stop (2) +
Layer-B guard (3) + no host belt (4) + `B=` canary (5) + scoped error-path fix (7) + narrowed scope (9).
Plan P0.2 now specifies `dispatchCommandByte` covering all ingress; P0.3 corrected mechanism; P0.6 source
caveat. P3.2 expanded + P3.2b. Files/Risks updated; added the unexplained-2.73 h-elapsed anomaly as an
honest open question.

## Round 4 — Codex: CHANGES_REQUESTED | Gemini: CHANGES_REQUESTED
Several net-new BLOCKERs on the round-3 design (still not converged). NB: panel again sandbox-denied repo
access — plan-text only; Claude's source reads are authority.

### Codex findings
1. [BLOCKER] `P`/`T`/`M` bypass the centralized policy (run before it, gated only on `!streaming`) → forged
   `P`/`T` reachable in HANDSHAKE. Gate frame commands too (HANDSHAKE = only the `M` frame).
2. [BLOCKER] A forged `b` in HANDSHAKE starts streaming before `%META` → real `M` then blocked → no `%META`
   (the OBCI_4E symptom). Require a `metaWritten`/armed flag before accepting `b`.
3. [MAJOR] Power-cycle is not a clean stop with auto-resume — leaves SESSION.TXT → next boot resumes; can't
   distinguish "resume after outage" from "user ended early." Document or add an abort path.
4. [MAJOR] "Fails loud" can't be optional — `session_start` MUST hard-fail on slot-char rejection.
5. [MAJOR] The 2.73 h-vs-29 min anomaly needs a pass/fail invariant (`B=` won't catch slow acquisition).
6. [MINOR] `~`/`?` in HANDSHAKE need side-effect proof (could enter a multi-byte setting state).

### Gemini findings
1. [BLOCKER] Removing the in-band stop → unkillable auto-resume LOCKOUT: power-cycle to abort leaves
   SESSION.TXT → boot resumes → RECORDING drops all → locked out until 12 h fills. Add a hardened distinct
   multi-byte escape honored in all states.
2. [BLOCKER] HANDSHAKE allowlist desyncs the multi-byte `~` (`~6`→`~` passes, `6` dropped, next `b` eaten as
   rate param); also a stray `~` could eat the legit `b`. Move rate config before the slot char; remove `~`
   from the allowlist (leave only `b`/`?`).
3. [MAJOR] Layer-B guard `return fileIsOpen` returns TRUE → caller prints "Size N" → SILENT destructive
   merge. Must reject at the caller (`break`, no reassign, no print) so the host times out / fails loud.
4. [MINOR] The 2.73 h anomaly is fully explained by stray `s` (pause) + later `b` (resume) + `F` (clobber) —
   covered by Layer A; no extra code, but verify.

### Resolution (Claude) — converged on a richer policy; verified the lockout & host order in source
- **Gemini #1 / Codex #3 ACCEPTED → reinstated a HARDENED DISTINCT ≥4-byte escape (Decision 2).** Confirmed
  boot-time `replaySessionFile()` is active (DefaultBoard.ino:107) → the lockout is real. Round 3's "no
  stop" went too far; a distinct multi-byte token (immune to repeated-byte bursts) fed by every ingress
  byte incl. the recovery drain (Gemini r3's drain-eats-it) → clean abort (removes SESSION.TXT). Kept
  auto-resume (user's stated preference); flagged the disable-auto-resume alternative for the user.
- **Gemini #2 / Codex #6 ACCEPTED → `~` removed from HANDSHAKE; host drops post-slot `~~`.** Verified the
  host sends rate (`~5`) BEFORE the slot char; the post-slot `~~` (line 451) is a non-load-bearing query.
  HANDSHAKE now allows ONLY `b` (after `metaArmed`).
- **Codex #1 ACCEPTED → gate `P`/`T` to IDLE** (`&& !SDfileOpen` at 288/578); `M` stays HANDSHAKE-legal.
- **Codex #2 ACCEPTED → `metaArmed` flag** gates `b` in HANDSHAKE (premature-`b` fix; also a cleaner
  explanation of the missing `%META`).
- **Gemini #3 ACCEPTED → Layer B moved to the CALLER** (`sdProcessChar` slot-case `break`; NOT inside
  `setupSDcard`) so a rejected slot char prints nothing → host fails loud, and `SDfileOpen` is never
  wrongly reassigned. Critical return-value fix.
- **Codex #4 ACCEPTED → host fail-loud is MANDATORY** (Decision 4 / P1.5).
- **Codex #5 / Gemini #4 ACCEPTED → P3.2c acquisition invariant** (footer elapsed ≈ sample-count/rate);
  anomaly explained as stray `s`-pause, covered by Layer A, verified by the invariant.
- Verified in source this round: `replaySessionFile()` active at setup() (lockout real); host send order
  (`~5`/channels/`T`/`P` before slot; `~~`/`M`/`b` after); `~` is the multi-byte rate prefix.

### prep.md changes this round
Decisions 1-4 rewritten: state-keyed policy with `metaArmed`-gated `b` + IDLE-only `P`/`T` (1); hardened
distinct escape replacing "no stop" (2); caller-side Layer-B guard (3); host fail-loud + drop `~~` (4).
Assumption updated with the auto-resume-vs-escape fork (user to confirm). Plan: P0.2 expanded + P0.2b
(escape spec); P1.1b (escape) + P1.5 mandatory; P3.2 adds `P`/premature-`b`/escape tests, P3.2b
auto-resume, P3.2c acquisition invariant. Files/Risks updated.

## Round 5 — Codex: CHANGES_REQUESTED | Gemini: CHANGES_REQUESTED
Convergent, implementation-level findings (both reviewers hit the same core issues → near-convergence).

### Codex findings
1. [BLOCKER] Escape must NOT call `streamStop()+closeSDfile()` from the recovery drain — latch an abort flag,
   unwind to a safe top-level point (else re-enter SD/FAT/footer mid-error).
2. [BLOCKER] Matcher must run after every RAW byte read (before P/T/M parsers consume bytes), + drain.
3. [MAJOR] Escape token under-specified — use a concrete ≥8-byte non-printable token; test partial overlaps
   + token-in-payload.
4. [MAJOR] `M` still bypasses policy — gate to `SDfileOpen && !streaming && !metaArmed`; set metaArmed only
   on the real META-complete path.
5. [MAJOR] IDLE remains an untrusted destructive window — host must verify final rate/channels before start.
6. [MAJOR] Host fail-loud too weak — flush stale input, require exact block count/file id, require META OK +
   %START before declaring started.
7. [MINOR] Prove no other direct `setupSDcard()` caller bypasses the guarded slot case.
8. [MINOR] Flash trim order wrong — cut B=/debug text before `byteCounter=0`.
9. [MINOR] Auto-resume+escape OK only after the blockers are fixed; else disabling auto-resume is simpler.

### Gemini findings
1. [BLOCKER] RECORDING check must be `streaming && SDfileOpen` — else a PC-only GUI stream (no SD file)
   can't be stopped (its `s` dropped).
2. [BLOCKER] Stray `M` in HANDSHAKE after the legit %META re-enters `sdMetaProcess`, eats `b`/corrupts armed
   state — gate `M` with `!metaArmed`.
3. [MAJOR] Calling `closeSDfile()` from the recovery drain risks CMD12/FAT-mid-CMD25 hang/corruption — use
   an `abortRequested` flag checked by `writeCache`, close at `loop()` top level.
4. [MAJOR] `metaArmed`-requires-`M` breaks stock OpenBCI GUI SD start (GUI sends no `M`) — user must accept.
5. [MINOR] Escape token of printable ASCII false-triggers if it appears in a session name (P/%META payload)
   — use non-printable/control bytes the host filters.

### Resolution (Claude) — all accepted; design now implementation-complete
- **Gemini #3 / Codex #1 ACCEPTED → deferred `abortRequested` flag.** Matcher sets the flag; `writeCache`
  early-returns on it; `loop()` does `streamStop()+closeSDfile()` at the safe top level. No re-entrant close.
- **Codex #2 ACCEPTED → matcher fed every RAW byte before parsers** (+ recovery drain).
- **Codex #3 / Gemini #5 ACCEPTED → ≥8-byte NON-PRINTABLE token** (host strips such bytes from text);
  test token-in-payload/partial-overlap/burst.
- **Gemini #1 ACCEPTED → RECORDING = `streaming && SDfileOpen`** (PC-only stream falls through to IDLE).
- **Gemini #2 / Codex #4 ACCEPTED → gate `M` to `SDfileOpen && !streaming && !metaArmed`.**
- **Gemini #4 ACCEPTED + flagged → SD recording now requires `%META` before `b`** (Decision 4b); stock-GUI
  no-META SD start unsupported on this build (user drives via session_start). Coupled to the premature-`b` fix.
- **Codex #5 / #6 ACCEPTED → host fail-loud strengthened** (drain stale, exact block-count/file-id, require
  META OK + %START + start marker, re-assert rate/channels before `b`).
- **Codex #7 ACCEPTED → P0.1 proves no other `setupSDcard` caller.** **Codex #8 ACCEPTED → trim order fixed**
  (cut `B=`/text before the error-path one-liner).
- **Codex #9 / Gemini auto-resume — noted:** keep auto-resume + escape (now that re-entrancy is fixed);
  disable-auto-resume flagged as the simpler alternative for the user (assumption).

### prep.md changes this round
Decision 1: RECORDING=`streaming&&SDfileOpen`, `M` gated `!metaArmed`, matcher-before-parsers. Decision 2:
deferred `abortRequested` flag + ≥8-byte non-printable token, never close in writeCache. Decision 4:
strengthened host verification. New Decision 4b (GUI-compat trade-off). Assumption: two user confirms
(auto-resume fork + %META-required). P0.1 no-other-caller; P0.2/P0.2b matcher+M-gate+abort-flag; P1 updated;
P1.6 trim order; P3.2 escape tested in RECORDING/resumed/stall + non-trigger cases.

## Round 6 — Codex: CHANGES_REQUESTED | Gemini: CHANGES_REQUESTED
Both converged on the SAME two BLOCKERs (strong signal these are the last real issues).

### Codex findings
1. [BLOCKER] `metaArmed` breaks auto-resume — replay sends no `M`, so its `b` is dropped. Arm `metaArmed`
   on the trusted replay path (or persist/replay `%META`).
2. [BLOCKER] Abort early-return under-specified — if seen at `byteCounter==512`, a bare return recreates the
   overflow; force a safe unwind (no further appends, sanitize `byteCounter`, one top-level writeStop/cleanup).
3. [MAJOR] Host "re-assert rate/channels before b" conflicts with HANDSHAKE deny — move the assert to before
   the slot char (IDLE).
4. [MINOR] Token must be `uint8_t[]` w/ explicit length (not signed-char/C-string); clear `abortRequested`
   after a completed abort.

### Gemini findings
1. [BLOCKER] `writeCache` early-return during the busy-drain leaves the SD BUSY/MISO low → loop()'s CMD12/
   FAT close fails/corrupts. `writeCache` must wait for SD BUSY to clear before returning on abort.
2. [BLOCKER] `replaySessionFile()` drops its own `b` (false `metaArmed`) → resumed session silently fails.
   Set `metaArmed = true` in `replaySessionFile()`.
3. [MAJOR] Escape unreachable via WiFi/Serial1 during a stall — the drain only polls Serial0. Poll all
   active ingress ports.
4. [MINOR] Inlining the matcher at 4 sites risks the flash margin — single non-inlined helper.

### Resolution (Claude) — verified replay path in source; all accepted
- **Codex #1 / Gemini #2 ACCEPTED — verified.** `replaySessionFile` (951-959) feeds bytes DIRECTLY to
  `sdProcessChar`+`board.processChar` (bypasses `dispatchCommandByte`) and is a TRUSTED source (host-written
  SESSION.TXT), so the replayed `b` is not subject to the `metaArmed` gate → resume works. Also set
  `metaArmed=true` in replay defensively. The escape still fires for a resumed session (raw-byte ingress).
- **Codex #2 / Gemini #1 ACCEPTED — safe-abort contract.** `abortRequested` is acted on ONLY at an SD block
  boundary (top of `writeCache` before `writeData`; top of `loop()`); suppress all further `pCache` appends
  (no `byteCounter==512` recurrence); `loop()` runs ONE `streamStop()`+`closeSDfile()` (its `writeStop()`
  polls SD BUSY) then CLEARS the flag. Never close inside `writeCache`/the drain.
- **Codex #3 ACCEPTED** — host re-asserts final rate/channels in IDLE BEFORE the slot char (handshake denies them).
- **Codex #4 ACCEPTED** — `uint8_t token[]` explicit length; clear `abortRequested` post-abort.
- **Gemini #3 ACCEPTED (scoped)** — drain feeds the matcher from all active transports (Serial0 for this user).
- **Gemini #4 ACCEPTED** — single non-inlined matcher helper.

### prep.md changes this round
Decision 1: added the REPLAY-bypass + `metaArmed=true`-in-replay bullet; IDLE config-verify-before-slot-char.
Decision 2: full safe-abort contract (block-boundary action, suppress appends, one top-level close, clear
flag, BUSY-wait via writeStop, all-transport drain, single `uint8_t[]` helper). Decision 4: re-assert config
in IDLE not pre-`b`. Plan P0.2(e)/P1.1/P1.1b updated.

## Round 7 — Codex: APPROVED | Gemini: CHANGES_REQUESTED (1 net-new BLOCKER)
Codex APPROVED (only a MINOR note that it couldn't re-read source — already covered by P0.6). Gemini found
one genuine net-new BLOCKER.

### Gemini finding
1. [BLOCKER] `writeFooter` overflows `pCache` during escape abort. `loop()`'s abort → `closeSDfile()` →
   `writeFooter()` appends ~100-218 B to `pCache` via the bare `pCache[byteCounter++]` pattern (no bounds
   check). If the abort fires at non-zero `byteCounter`, and `writeCache`'s abort early-return does NOT reset
   `byteCounter`, `writeFooter` walks past `pCache[512]` → RAM corruption. The "suppress appends in the
   sample path" approach is fragile (misses `writeFooter`). FIX: `writeCache`'s abort early-return MUST do
   `byteCounter = 0;` — absorbs all oblivious caller writes safely.

### Resolution (Claude) — ACCEPTED (correct, concrete, trivial)
Gemini is right: the existing `sdCardDead` early-return (1666) already does `byteCounter=0` for exactly this
reason; the abort early-return must mirror it. Added to Decision 2's safe-abort contract + P1.1b: `writeCache`'s
abort early-return sets `byteCounter=0`; abort may also skip `writeFooter` (footer-less partial file is valid)
as belt. Closed even though we hit the nominal 7-round backstop — a known RAM-corruption blocker overrides the
round cap; the fix is one line and Codex already approved the rest.

### prep.md changes this round
Decision 2 safe-abort contract: added the `byteCounter=0`-in-abort-early-return as the bulletproof guard (vs
the fragile sample-path suppression) + skip-writeFooter option. P1.1b updated to match.

## Round 8 — Codex: CHANGES_REQUESTED | Gemini: APPROVED
Gemini APPROVED (confirmed the `byteCounter=0` reset fully closes the `writeFooter` overflow; auto-resume +
escape mechanics sound, no net-new blockers). Codex found one refinement of the SAME RAM-safety item.

### Codex finding
1-2. [BLOCKER] The `byteCounter=0`-in-`writeCache` only helps if `writeCache` runs before `closeSDfile`.
   `loop()`'s top-level abort path can call `closeSDfile()` directly (escape matched during normal RX) with a
   nonzero partial `pCache`. Make cache sanitation mandatory on EVERY abort-close path: force `byteCounter=0`
   there too and/or make the footer-skip mandatory so no `pCache` appender runs.

### Resolution (Claude) — ACCEPTED (single airtight abort path)
Defined ONE `performAbort()` helper invoked only from `loop()` top-level: `byteCounter=0` → `streamStop()` →
`closeSDfile()` with `writeFooter` MANDATORILY skipped → clear `abortRequested`. `closeSDfile` itself doesn't
append to `pCache`. So no oblivious appender runs on the abort path from any entry point — closes Codex's gap.

### prep.md changes this round
Decision 2: replaced "may skip writeFooter" + scattered resets with a single `performAbort()` helper
(byteCounter=0 + mandatory footer-skip + streamStop + closeSDfile + clear flag), called only from loop() top.

## Round 9 — Codex: APPROVED | Gemini: APPROVED ✅ (both, no BLOCKER/MAJOR remaining)
Both reviewers confirmed the single top-level `performAbort()` (byteCounter=0 + mandatory footer-skip +
closeSDfile-doesn't-append + writeStop's CMD12 BUSY-safe close) fully closes the abort-overflow concern from
every entry path, with no remaining holes for overnight recording, escape/abort, or auto-resume.

**FINAL: plan APPROVED by Codex AND Gemini after 9 rounds.** prep.md is the deliverable. Two workflow
decisions remain for the USER (not the panel): (1) keep auto-resume + escape vs disable auto-resume;
(2) accept that SD recording now requires %META before `b` (stock-GUI no-META SD start unsupported).
Next: user confirms those two, then /grill implements.
