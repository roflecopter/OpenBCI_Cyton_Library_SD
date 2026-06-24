# Prep: Fix OpenBCI Cyton freeSD recording truncation — unguarded `setupSDcard()` clobbers `BLOCK_COUNT` mid-session

## Goal
Permanently fix the firmware defect that silently truncates 12 h sleep recordings.
**Root cause (proven by on-card arithmetic, this session):** `sdProcessChar()` dispatches the
slot-duration command chars (`A S F G H J K L a`) to `setupSDcard()` **with no `!board.streaming`
guard** (unlike the `P`/`T`/`M` commands, which ARE guarded — lines 288/577/1422), and
`setupSDcard()` **overwrites the global `BLOCK_COUNT` via its duration switch (1098-1128) before any
guard or file op**. A single **stray slot-char byte** on the cyton's serial RX during a live session
silently rewrites `BLOCK_COUNT` to a *shorter* duration, so the recording footers and closes early
when `blockCounter` reaches the new, smaller value. Rounds 2-3 generalized this to the whole class —
ANY stray RX byte (`j` close, `s` stop, `1`-`8` channel-off, slot char) can damage an open recording,
because `loop()` dispatches every inbound byte to the command handlers even while recording. The fix is
a **state-keyed RX policy** (ignore all inbound commands while recording; a tiny allowlist during the
arming handshake) + a `setupSDcard()` entry guard + an in-band `BLOCK_COUNT` canary — minimal, one flash.

## Evidence (OBCI_4E, 2026-06-23, firmware `6f6efe8`) — re-derived from the raw card this session
The summary's earlier "`%META` pCache-overflow corrupts `BLOCK_COUNT`" theory was **disproven** by a
full static audit + raw-card forensics. The actual facts:
- File `OBCI_4E.TXT` is **1,245,184,000 B = 2,432,000 blocks** = exactly the **`K` / 12 H** slot
  (`createContiguous` ran with `BLOCK_COUNT=2,432,000`). Host `session_start -a 20` sent `K`. ✓
- Recording **footered + closed at block 100,999/101,000**. The footer is the real one
  (`%max Write 0x45EC=17,900 µs`, `%Errors 0 %Retries 0 %Reinits 0 %ExtRetries 0 %Over 1`).
- **101,000 = exactly the `F` / 30-MIN slot value** under the firmware's *integer* scaling:
  `BLOCK_30MIN(101340)/BLOCK_DIV(2)=50670; 50670/250=202 (int trunc); 202*500=101,000`.
  (For comparison `K`: `…/250=4864 (trunc) *500 = 2,432,000` = the file.) Landing on the **exact
  `F` arithmetic result** rules out random memory corruption — the `F` switch case *executed*.
- A single `setupSDcard` call computes `BLOCK_COUNT` once and uses it for BOTH `createContiguous`
  AND the fill check, so it cannot be 2,432,000 for one and 101,000 for the other **in the same
  call**. ⇒ `setupSDcard` ran a **second time with `limit='F'`**, clobbering `BLOCK_COUNT`, **without
  creating a new file** (its mid-stream `createContiguous` failed / the bump targeted an absent slot).
- Head of `OBCI_4E` = `%BOOT seq=0029 rcon=0x00 … resume=0\n` immediately followed by sample data:
  **`%META` and `%START AT` are absent** (whole-file scan: 1×`%BOOT`, 0×`%META`, 0×`%START`,
  28×`%CKPT`). Sample counter is **continuous** (no gaps). The missing `%META`/`%START` (both written
  *pre-streaming*, after `%BOOT`) means the stray slot char struck in the **pre-streaming handshake
  window** (after `K` opened the file, before `b` started streaming) — so the guard must reject a
  slot char **once a file is already open**, not only once streaming has started.
- **Empty `OBCI_46/4B/4C.TXT` (0 B)** corroborate spurious `incrementFileCounter()` calls from stray
  slot chars that bumped the counter but never recorded.
- **One root cause explains the whole 6-night pattern** (not 4 separate bugs): a stray **slot char**
  → truncated/empty night; a stray **`j`** → instant clean close; a stray slot char whose
  `createContiguous` *succeeds* mid-stream → orphaned/all-NUL night (06-21/22 erase). The 06-20 hard
  freeze (`DSPI` spin) is the one genuinely separate failure (parked).

## Context & constraints
- **Hardware:** PIC32MX250F128B, 32 KB RAM, chipKIT/Arduino. Sketch
  `examples/DefaultBoard/{DefaultBoard.ino,SD_Card_Stuff.ino}`. Build FQBN `chipKIT:pic32:openbci`.
  **Flash ~96 % full** (last build 118,420 / 122,880 B → ~4,460 B free) — size-check every build.
- **No ICSP** → OTA bootloader flash only; bad flash = re-flash (bootloader protected, not brickable).
  **Mandatory Codex+Gemini review of the full diff before the flash.** Let `pic32prog` run to
  completion (NO kill-timeout). Keep the `6f6efe8` `.hex` as rollback.
- **Host** `~/Storage/Dev/openbci-session/session_start.py`: sends `P` (SESSION.TXT) → slot char (`K`)
  → `M` (%META) → `b` (stream). One slot char per session. Don't break this contract.
- **Current firmware on the board** = `6f6efe8`. The unguarded-`setupSDcard` defect predates the 2026-05
  campaign and is in every recent build. **Scope of THIS fix = the slot-char/`BLOCK_COUNT` clobber**
  (the common truncate/erase failures). The 06-20 `DSPI` hard freeze is separate (parked, hardware-WDT prep).

## Decisions
> Defect CLASS (rounds 2-3): "any stray RX byte mutating an open/active SD recording." RF noise from the
> on-board radio injects ARBITRARY bytes — a stray slot char (truncate), `j` (close), `s` (stop), `1`-`8`
> (drop a channel), or a slot char in the brief pre-stream handshake (the actual 06-23 fault). Fix = one
> **state-keyed RX policy** + a `setupSDcard` entry guard. Confirmed from code: `loop()` (DefaultBoard.ino
> 159-169) runs the `P`/`T`/`M` frame processors first (already gated on `!streaming`), then dispatches
> leftover single-byte commands to `sdProcessChar`+`board.processChar` (163-166) **with no recording-state
> check**. Round 3 RETIRED the in-band stop and the host `j`-belt (both introduced new BLOCKERs) — this is
> the converged design.

1. **LAYER A — one centralized, state-keyed RX command policy (PRIMARY fix).** Every raw RX byte, at EVERY
   ingress (Serial0 144, Serial1 174, wifi, AND the `writeCache` recovery drain 1770), FIRST feeds the
   hardened-escape matcher (Decision 2) — BEFORE any parser, since `P`/`T`/`M` consume their frame bytes and
   would otherwise hide the token (Codex). THEN, replace the bare `sdProcessChar(c); board.processChar(c);`
   dispatch (after the P/T/M chain) with a helper `dispatchCommandByte(c)`:
   - **RECORDING = `board.streaming && SDfileOpen`:** drop ALL command bytes (only the escape acts). NB the
     `&& SDfileOpen` is REQUIRED — a PC-only GUI stream (streaming, no SD file) must fall through to IDLE so
     its `s`/stop still works (Gemini). Nothing else inbound is legitimate during an SD recording.
   - **HANDSHAKE = `SDfileOpen && !streaming`** (slot char done → before `b`): default-DENY. Allow ONLY `b`,
     and ONLY once `metaArmed` is set — i.e. AFTER the `%META` frame completed (premature-`b` fix: a stray
     `b` before `M` would start streaming → real `M` then blocked → no `%META`, the OBCI_4E symptom). Drop
     slot chars, `j`, `s`, `c`/`C`, channel toggles, `~`, `?`, everything else. **`~` NOT allowed** (it's the
     multi-byte rate prefix — `~6`→`~` passes, `6` dropped, next `b` eaten as the rate param — Gemini); rate
     is set in IDLE pre-slot, host drops its post-slot `~~` (Decision 4). **`P`/`T` gated IDLE-only**
     (`&& !SDfileOpen` at 288/578) and **`M` gated to `SDfileOpen && !streaming && !metaArmed`** (one `M`
     only — a stray `M` after arming would re-enter `sdMetaProcess` and eat the `b`; Gemini/Codex).
   - **IDLE = `!SDfileOpen && !streaming`:** normal dispatch — all config commands work (rate, channels,
     `P`, `T`, slot char). IDLE stays config-mutable by design; the host verifies final rate/channel state
     in IDLE **before sending the slot char** (Decision 4 / Codex) — NOT in handshake (handshake denies
     them) — since a stray IDLE byte could mis-set config (not truncate).
   - **REPLAY (auto-resume) is a TRUSTED path that BYPASSES `dispatchCommandByte`.** `replaySessionFile`
     (951-959, CONFIRMED) feeds SESSION.TXT bytes straight to `sdProcessChar`+`board.processChar` (its bytes
     are host-written config, not RF), so its replayed `b` is NOT subject to the `metaArmed` handshake gate
     → a power-blip resume starts normally even though replay sends no `%META`. **Defensively set
     `metaArmed=true` inside `replaySessionFile`** (before its `b`) so state is consistent (Codex/Gemini:
     "the replayed `b` is dropped" — resolved by the bypass + this flag). The escape matcher still fires for
     a resumed session (it's fed at the raw-byte ingress, independent of `metaArmed`).
2. **Hardened escape via a DEFERRED abort flag (replaces round-3's "no stop"; avoids the lockout).**
   Removing the stop entirely created an auto-resume LOCKOUT: boot-time `replaySessionFile()`
   (DefaultBoard.ino:107 — CONFIRMED active) auto-resumes a still-present SESSION.TXT → RECORDING drops all
   → locked out until the 12 h fills. Fix: a fixed **≥8-byte NON-PRINTABLE token** (e.g. `1B 00 FF 55 …` —
   non-printable so it can never appear in a `P`/`%META` *text* payload the matcher also sees; the host
   strips such bytes from session names/notes; ≥8 + distinct ⇒ immune to repeated-byte bursts and
   astronomically unlikely in RF garbage — Codex/Gemini). Implement as a **single non-inlined helper**
   (`uint8_t token[]` with explicit length — NOT a C string, since it contains `0x00`/`0xFF`; signed-char
   pitfalls avoided), called from all ingress sites (no 4× inlined copies — flash, Gemini r6). On full match
   it ONLY sets a global **`abortRequested` flag** — never closes inline.
   **Safe-abort contract (Codex/Gemini r6-7 — the SD is mid-CMD25 multi-block write):**
   - `abortRequested` is acted on ONLY at an SD **block boundary / idle point**, never mid-data-phase: check
     it at the TOP of `writeCache` (before `csLow`/`writeData`, SD between blocks) and at the TOP of `loop()`.
   - **`writeCache`'s abort early-return MUST set `byteCounter = 0;` before returning** — mirroring the
     existing `sdCardDead` early-return (line 1666). This is the BULLETPROOF guard (Gemini r7 BLOCKER):
     `closeSDfile`→`writeFooter()` (and its `convertToHex`/pad loops) append ~218 B to `pCache` via the bare
     `pCache[byteCounter++]` pattern (no bounds check, 1943-2017); if the abort fires at a non-zero
     `byteCounter`, a `writeCache` early-return that did NOT reset `byteCounter` would let `writeFooter` walk
     past `pCache[512]` → RAM corruption. Resetting `byteCounter=0` makes every oblivious appender harmless
     (it never re-crosses 512 in one ≤512 B run). Do NOT rely on "suppress appends in the sample path" alone.
   - **ONE abort-close path, a single `performAbort()` helper, invoked ONLY from `loop()` top-level**
     (Codex r8 — the `writeCache` `byteCounter=0` reset alone is necessary-but-not-sufficient because an
     escape matched during normal RX reaches `loop()`'s top-level close without `writeCache` running). It
     does, in order: `byteCounter = 0;` (sanitize the cache on THIS path too) → `board.streamStop();` →
     `closeSDfile();` with **`writeFooter` MANDATORILY SKIPPED** on abort (a footer-less partial file is
     valid; `closeSDfile` itself does not append to `pCache`, only `writeStop`/close/remove + Serial
     prints) → `abortRequested = false;`. With the footer skipped AND `byteCounter=0`, no `pCache` appender
     runs on the abort path from any entry point.
   - **CLEAR `abortRequested` after the close** so the next session can't inherit it (Codex).
   - **Never** call `closeSDfile()` from inside `writeCache`/the drain (CMD12/FAT mid-CMD25 → SPI hang / FAT
     corruption / re-entrancy — Gemini/Codex).
   The recovery drain feeds the matcher from **every active ingress transport** (Serial0 for this user; also
   Serial1/wifi if active — Gemini r6) so a stall can't hide the escape. This token is the ONLY thing honored
   during RECORDING; `session_start` sends it to stop / clear a stale or resumed session. Proven to fire in a
   resumed session AND mid-stall (P3.2).
3. **LAYER B — reject the stray slot char AT THE CALLER (Gemini return-value fix).** Guard in
   `sdProcessChar`'s slot-case, NOT inside `setupSDcard`: `if (board.streaming || SDfileOpen) break;` — so
   it does NOT call `setupSDcard`, does NOT reassign `SDfileOpen`, and does NOT print "Size N SD file".
   (Guarding *inside* `setupSDcard` with `return fileIsOpen` would make the caller set `SDfileOpen=true` +
   print success → SILENT destructive merge; `return false` would wrongly clear `SDfileOpen` while a
   recording is live. Caller-side `break` is the only correct spot.) ⇒ a rejected slot char gives NO
   "Size N" → `session_start` times out → **fails LOUD**. Redundant with Layer A's handshake/streaming drop
   of slot chars (defense-in-depth); also protects the trusted replay path (runs with `SDfileOpen==false`
   at its slot char → allowed). First legit slot char per session has `SDfileOpen==false` → allowed (P0.1).
4. **No host `j`-belt; host drops its post-slot `~~`; host fail-loud is MANDATORY.** Dropped the `j`-belt
   (it would delete the just-written SESSION.TXT — Gemini). The host's post-slot `~~` rate-query (line 451)
   is removed (rate already set pre-slot; keeps `~` out of the handshake). `session_start` MUST **hard-fail
   (not warn)** with STRONG verification (Codex): drain stale RX before the slot char; **re-assert the final
   rate/channel state in IDLE BEFORE the slot char** (NOT before `b` — handshake denies config commands, so
   a pre-`b` re-assert would be dropped — Codex r6); require the EXACT expected block count (2,432,000) +
   file id in the "Size N SD file" line; require `META OK` + `%START` + the post-`b` start marker before
   declaring the night started. This loud, exact verification replaces the belt. P0.1 proves
   `SDfileOpen==false` at the legit slot char in the real workflow.
4b. **GUI-compat trade-off (Gemini): SD recording now REQUIRES the host to send `%META` before `b`** (the
   `metaArmed` gate). `session_start` always does; the **stock OpenBCI GUI's no-`%META` SD start is no
   longer supported** on this build. Accepted because this is the user's dedicated sleep-recording firmware
   driven solely by `session_start` — flagged in the assumption for the user to confirm. (Removing the gate
   would re-open the premature-`b` hole, so they're coupled.)
5. **`%CKPT` gains `B=<BLOCK_COUNT>`** — permanent in-band canary. Had this existed, the 28 `%CKPT` lines
   would have shown `BLOCK_COUNT` flip 2,432,000→101,000 and pinned the bug in minutes. ~12 B of code, fits
   the existing 128-B `tmp` snprintf buffer. (Canary, not proof — the guards are the fix.)
6. **DROP the old `sdMetaProcess` `byteCounter/blockCounter` "fix"** — disproven root cause; would
   **double-increment `blockCounter`** (`writeCache()` already resets `byteCounter`/advances `blockCounter`
   on its success path 1856-1857, *inside* `writeCache`). The RAM-stage `%META` path (1461-1492) is already
   correct on the happy path. No change. **DROP forensic `SESS_<NN>.TXT`** (flash bloat; `B=` is the record).
7. **INCLUDE Gemini's latent `writeCache()` error-path fix** (separate real bug, cheap): the
   `if(!resumed){ sdCardDead=true; … return; }` fatal path (≈1833) returns **without `byteCounter=0`**, so
   a caller mid-`pCache[byteCounter++]` loop would walk past 512 on a real SD write failure. Add
   `byteCounter = 0;` there. Inert on the happy path (P0.4). First firmware item to cut if flash is tight.
8. **`meta verified` honesty — documented, no code.** The host sum is over the RAM `sessBuf` receive
   (host→board transfer), not the SD write — that's why it passed while `%META` was absent. The guards +
   `B=` canary are the fix; documented as a known host-side limitation.
9. **Scope (narrowed per Codex round 3):** this fix closes the **stray-RX-byte class** (truncate / close /
   erase / channel-drop) — the failures that cost the recent nights. It does **NOT** fix the rare 06-20
   `DSPI::transfer` hard freeze (separate hardware-WDT prep). "Trust it for a sleep night" applies to the
   stray-RX class; the DSPI freeze remains separately tracked and is NOT claimed fixed here.

> ASSUMPTION (auto-resolved): (a) a **state-keyed RX policy** dropping all commands while recording +
> default-deny handshake (only `b`-after-`metaArmed`); (b) a **hardened ≥8-byte non-printable escape** (via
> a deferred `abortRequested` flag) as the ONE honored stop, so power-cycle-then-resume can't lock the user
> out; (c) **KEEP boot-time auto-resume** — the user explicitly values power-blip recovery ("will
> SESSION.TXT make recording resume?").
>
> **USER-CONFIRMED (2026-06-24):**
> 1. ✅ **Keep auto-resume + hardened escape token** (user chose this over disabling auto-resume) — power-blip
>    recovery is retained; the escape token is the clean stop/abort that prevents the lockout.
> 2. ✅ **Accept `%META`-required** (Decision 4b) — the stock-GUI no-`%META` SD start is unsupported on this
>    build; the user drives everything via `session_start`. Keeps the premature-`b` defense.

## Plan
### Phase 0 — Static confirmation + full command-byte audit (NO FLASH). Gates the fix.
- **P0.1 Prove the Layer-B guard is safe for every caller of `setupSDcard`** (Codex #2). From code:
  `SDfileOpen` is set ONLY by `setupSDcard` (1012) and `closeSDfile` (1018), and cleared (=false) on
  EVERY session-end path — clean close (`closeSDfile`→`fileIsOpen=false` 1357), replay-fail (988),
  `writeCache` `sdCardDead` early-return (1668), `writeCache` fatal `!resumed` (1835); fresh-boot global
  init = false. **`P`/`T`/`M` do NOT set `SDfileOpen`** (verified — not in the ref list). ⇒ the first
  legit slot char per session has `SDfileOpen==false`; auto-resume `replaySessionFile` runs at boot with
  it false. Test the two-sessions-in-one-uptime case after BOTH a natural fill-close and a `j` close
  (Codex #2). The no-power-cycle/still-open edge fails LOUD (slot char rejected → no "Size N SD file"),
  not silently (Decision 4) — confirm `session_start` surfaces it. **Also prove the ONLY callers of
  `setupSDcard` are the guarded `sdProcessChar` slot-case and the boot replay path** (no direct call
  bypasses the caller-side guard — Codex r5 #7).
- **P0.2 Specify the centralized `dispatchCommandByte` policy + cover every ingress.** Enumerate every byte
  `sdProcessChar` (slot chars, `j`, `s`, `b`, `c`/`C`) and `board.processChar` (`s`/`b` stream, `1`-`8` &
  `!`-`*` channels, register/setting cmds) act on; confirm the ONLY legitimate post-slot-char byte is `b`
  (host order verified: rate/channels/`T`/`P` precede the slot char; only the dropped `~~`, the `M` frame,
  and `b` follow it). Define IDLE/HANDSHAKE/RECORDING (Decision 1) called from all three ingress sites.
  Verify: (a) `M`/`P`/`T` frame processors run BEFORE the helper (so `%META` lands) — gate `P` (288)/`T`
  (578) IDLE-only (`&& !SDfileOpen`) and `M` (1422) to `SDfileOpen && !streaming && !metaArmed` (one `M`;
  a stray `M` after arming would re-enter `sdMetaProcess` and eat `b` — Gemini/Codex r5); (b) `metaArmed`
  set exactly at `%META`-complete ("META OK" path), reset in `setupSDcard`, gates `b`; (c) RECORDING is
  `streaming && SDfileOpen` (PC-only stream falls through to IDLE so its `s` works — Gemini r5); (d) the
  byte is consumed from the FIFO (radio never backs up); (e) the **REPLAY path (951-959) keeps its DIRECT
  `sdProcessChar`+`board.processChar` dispatch (bypasses `dispatchCommandByte`, trusted source) and sets
  `metaArmed=true`** so the resumed `b` is never gate-dropped (Codex/Gemini r6 BLOCKER).
- **P0.2b Specify the hardened escape matcher (Decision 2).** Choose a fixed **≥8-byte NON-PRINTABLE
  token** (host strips such bytes from `P`/`%META` text payloads). The matcher is fed every raw ingress
  byte **BEFORE any parser** (so `P`/`T`/`M` frame bytes don't hide it — Codex r5 #2) AND in the recovery
  drain, in ALL states. On full match it ONLY sets `abortRequested=true`; **`writeCache` checks the flag
  and returns early, and `loop()` does `streamStop()`+`closeSDfile()` at the top level** — never close from
  inside `writeCache`/the drain (CMD12/FAT mid-CMD25 re-entrancy — Gemini/Codex r5 #1/#3). Test
  token-in-payload + partial-overlap + repeated-byte-burst non-triggering.
- **P0.3 Missing-`%META` mechanism (CORROBORATION only, exact path not relied upon — Codex/Gemini).** Most
  likely: a stray slot char in the HANDSHAKE window (after the legit slot char, before block 0 flushes)
  re-enters `setupSDcard`, whose `volume.cacheClear()` (1153) discards the unflushed `%BOOT`+`%META` and
  resets `BLOCK_COUNT`/`blockCounter`; the new `%BOOT` is then written with no `%META`. (Gemini round 2's
  "mid-stream block-0 overwrite" was retracted in round 3 as physically inconsistent with the CMD25 lock.)
  The SD-level write-pointer details are uncertain and NOT load-bearing — the Layer-B guard + Layer-A
  handshake-drop prevent the entire re-entry regardless.
- **P0.4 Confirm Decision-7 error-path fix is inert on the happy path** (only adds `byteCounter=0` on the
  already-fatal `!resumed` branch; no double-reset).
- **P0.5 Stray-byte source (context only).** Dongle unplugged ⇒ the on-board RFduino injected the byte
  (RF noise / unpaired-radio garbage). We harden the cyton to SURVIVE stray bytes; we don't fix the radio.
- **P0.6 Note for /grill (panel can't read source):** both reviewers reported sandbox `/home/lst` access
  was DENIED, so their review is plan-text only — the line-cited code facts here are from Claude's direct
  reads and MUST be re-verified against the actual build in /grill before the flash.
- **Acceptance gate:** P0 produces (a) the `SDfileOpen`-lifecycle proof, (b) the `dispatchCommandByte`
  spec covering all ingress, (c) sign-off that Decisions 6 (no `sdMetaProcess`/`SESS`) is correct,
  (d) error-path one-liner inert.

### Phase 1 — The fix (NO FLASH yet; build + review).
- **P1.1 Layer A** — `dispatchCommandByte(c)` (IDLE/HANDSHAKE = `SDfileOpen && !streaming` / RECORDING =
  `streaming && SDfileOpen`) called from all ingress sites. Add the `metaArmed` flag (set on `%META`
  complete, reset in `setupSDcard`, **and set `=true` in `replaySessionFile`**) gating `b`. Gate `P`/`T` to
  IDLE (`&& !SDfileOpen`) and `M` to `… && !metaArmed`. Keep the replay dispatch DIRECT (bypasses the helper).
- **P1.1b Hardened escape matcher + deferred abort** (Decision 2) — single non-inlined helper, `uint8_t
  token[]` (explicit length, ≥8 non-printable bytes), fed every raw ingress byte (before parsers) + the
  recovery drain (all active transports); full match → `abortRequested=true` ONLY. `writeCache`'s abort
  early-return **sets `byteCounter=0`** (mirrors the `sdCardDead` return at 1666) so `closeSDfile`/`writeFooter`
  can't overflow `pCache` (Gemini r7); `loop()` does `streamStop()`+`closeSDfile()` once at top level (may
  skip `writeFooter`), then CLEARS `abortRequested`. Never close in `writeCache`.
- **P1.2 Layer B** — caller-side slot-char guard in `sdProcessChar`: `if (board.streaming || SDfileOpen)
  break;` (Decision 3 — NOT inside `setupSDcard`; preserves fail-loud).
- **P1.3** `%CKPT` `B=<BLOCK_COUNT>` canary (Decision 5) — extend the existing snprintf format + arg.
- **P1.4** `writeCache()` `!resumed` `byteCounter = 0;` (Decision 7) — latent-overflow hardening.
- **P1.5 (host, MANDATORY)** `session_start.py`: drain stale RX before the slot char; require the EXACT
  block count (2,432,000) + file id; require `META OK` + `%START` + the start marker before declaring the
  night started; re-assert final rate/channels before `b`; hard-FAIL (not warn) on any miss (Decision 4).
  Drop the post-slot `~~`; add the escape-send "stop"/clear path. No flash cost.
- **P1.6 Flash-fit check.** Rebuild; assert `Sketch uses N bytes ≤ 122,880` with margin. **Firmware-only**
  trim order if over (Codex r5 #8 — `byteCounter=0` saves ~nothing): cut `B=` width/format + any new debug
  strings FIRST, then P1.4. **Never cut P1.1/P1.1b/P1.2** (the night-savers).
- **P1.7 Mandatory Codex+Gemini review** of the full diff before any flash.

### Phase 2 — ONE flash.
- Build (FQBN `chipKIT:pic32:openbci`), **verify size**, flash via `pic32prog` (no kill-timeout; wait for
  `Verify flash … done`, stream the `###` progress). Keep `6f6efe8.hex` as rollback.

### Phase 3 — Verification (deterministic fault injection, not just a clean bench).
- **P3.1 Legit-path sanity (fast).** `-a 21` (5-min burn-in) fills at the CORRECT block count and now
  shows `B=<5min value>` in `%CKPT`; `-a 20` opens a 2,432,000-block file with `%META` + `%START AT`
  present and `B=2432000` in every `%CKPT`. Confirms the guard didn't break the one legit slot char.
- **P3.2 DIRECT fault injection (the real proof; expanded rounds 2-4).** Start a 12 H `-a 20` recording,
  dongle plugged, and inject **mid-stream** — separately AND as a repeated-byte burst — each destructive
  stray byte: a **slot char (`F`, `K`)**, a **`j`** (close), an **`s`** (stop), a **channel toggle (`1`)**,
  and a forged **`P`**. PASS for ALL: `B=` stays `2432000`, recording continues, all channels stay on, no
  early footer, no new/empty `OBCI_` file, SESSION.TXT untouched. Then inject the same set + a **premature
  `b`** in the **handshake window** (between slot char and `b`): the legit `b`-after-`%META` still starts
  streaming, `%META`/`%START AT` present, file counter unchanged, and a premature `b` is REJECTED (does not
  start streaming before `%META`). Inject during an **artificial SD stall** (recovery drain) — only the
  escape may act. Finally verify the **hardened escape**: the full ≥8-byte token cleanly stops + removes
  SESSION.TXT via the deferred `abortRequested`+top-level-close path, in **RECORDING, a resumed session,
  AND mid-SD-stall** (proving no re-entrant close); confirm a repeated-byte burst, a partial token, and the
  token-as-text-in-a-`P`/`%META`-payload do NOT trigger it.
- **P3.2b SESSION.TXT survival + auto-resume.** SESSION.TXT correct after a normal start; a clean fill-close
  OR an escape-stop removes it; a power-cut mid-session leaves it and the next boot auto-resumes (intact).
- **P3.2c Acquisition-integrity invariant (the 2.73 h anomaly — Codex).** `B=` won't catch slow/paused
  acquisition, so assert: footer `Total time` ≈ (decoded sample count ÷ sampling rate) within tolerance,
  and `%CKPT` `t=` deltas stay ~60 s with monotonic `b=`. (The anomaly is now explained as a stray `s`
  pausing streaming — Gemini — and is covered by Layer A dropping `s`; this invariant verifies no residual.)
- **P3.3 Long bench (multi-hour daytime).** ≥3 h unattended; `B=2432000` throughout, no early close,
  `%META` intact, acquisition invariant holds. Only after P3.1-3.3 pass does the user trust it for a real
  sleep night (stray-RX class; the DSPI freeze is out of scope — Decision 9).

## Files to touch
- `examples/DefaultBoard/DefaultBoard.ino` — **Layer A**: `dispatchCommandByte(c)` (IDLE/HANDSHAKE/RECORDING
  + `metaArmed` gate) called from the Serial0/Serial1/wifi ingress sites, replacing the bare dispatch
  (P1.1); the **hardened-escape matcher** fed from there + the recovery drain (P1.1b).
- `examples/DefaultBoard/SD_Card_Stuff.ino` — **Layer B**: caller-side slot-char guard in `sdProcessChar`
  (`break`, P1.2); gate `P`/`T` entry on `!SDfileOpen` (288/578); set/reset `metaArmed`; feed the escape
  matcher from the `writeCache` recovery drain; `%CKPT` `B=` field (P1.3); `writeCache` `!resumed`
  `byteCounter=0` (P1.4).
- `~/Storage/Dev/openbci-session/session_start.py` — **MANDATORY**: hard-fail if the slot char is rejected
  (P1.5); **drop the post-slot `~~`**; add an escape-send "stop" path; optionally a stray-byte injector +
  a checker (`%META` present, `B=` constant, acquisition invariant). (Host-only — no PIC32 flash cost.)
- `README.md` / `changelog.md` — document the root cause, the RX policy + caller-side guard, the hardened
  escape (the ONLY stop), the `B=` field, and the auto-resume/escape interaction.

## Test plan
P0 static gate (guard-safety + `dispatchCommandByte`/escape spec + lifecycle proof) → build fits flash →
Codex+Gemini approve → ONE flash → P3.1 legit sanity → **P3.2 inject stray slot/`j`/`s`/`1`/`P`/premature-`b`
mid-stream AND in the handshake window, prove `B=2432000` holds + recording survives + escape works** →
P3.2b SESSION.TXT/auto-resume → P3.2c acquisition invariant → P3.3 multi-hour bench. "Done" = a multi-hour
recording with `B=2432000` + acquisition invariant throughout AND live injection of every destructive byte
that the firmware ignores, AND a working hardened escape.

## Risks & open questions
- **[Layer-B guard breaks a legit start]** Resolved: P0.1 proves `SDfileOpen==false` at the first legit
  slot char (every session-end path clears it; `P`/`T`/`M` don't set it). The no-power-cycle/still-open
  edge fails LOUD (slot char rejected, no "Size N SD file"), not silently. Stress-test two-sessions/uptime.
- **[Handshake too strict/loose]** Layer A's HANDSHAKE allows ONLY `b`-after-`metaArmed`; host order
  verified to send only the dropped `~~`, the `M` frame, then `b`. P0.2 confirms no other legit
  post-slot-char byte exists and that stray `j`/`s`/`1`/`P`/premature-`b` are all dropped while the real
  `b` passes. `~` deliberately excluded (multi-byte rate desync — Gemini).
- **[Hardened-escape false-trigger / lockout]** The escape token must be ≥4 distinct bytes (immune to
  repeated-byte bursts) and absent from normal command/`%META`/sample traffic (P0.2b). It is the ONLY way
  out of a resumed RECORDING session, so it MUST also fire from the recovery drain. If the token is ever
  unsendable (host bug), the only fallback is fill/BIOS — so P3.2 must prove it fires in a resumed session.
- **[Auto-resume vs escape — workflow fork]** Keeping auto-resume (user's stated preference) requires the
  escape to avoid lockout. If the user instead prefers **disabling auto-resume** (power-cycle = clean stop,
  no escape needed, simpler/less flash), that's a smaller firmware change — flagged in the assumption for
  the user to pick before /grill.
- **[Stray-byte source unmitigated]** We harden the firmware to *survive* stray bytes, not stop the RFduino
  emitting them. The state-keyed policy + caller-side guard close the class; the `B=` canary catches residue.
- **[`%META`-absence mechanism uncertain]** Two candidate paths (handshake-window `setupSDcard` re-entry
  clearing pCache; premature-`b` blocking the real `M`). Both are prevented by the fix; corroboration only,
  not load-bearing.
- **[2.73 h-vs-29 min anomaly — now explained, verified by invariant]** Most likely a stray `s` paused
  streaming (clock runs, samples halt) + later `b` resumed — covered by Layer A dropping `s`. P3.2c's
  acquisition invariant (footer elapsed ≈ sample-count/rate) is the pass/fail gate so a residual can't hide.
- **[Flash fit]** ~4,460 B free. `dispatchCommandByte` + escape matcher + caller guard are the firmware
  adds (~low-hundreds B); P1.6 is a hard gate. Firmware-only trim: P1.4 → `B=` width. Never cut Layer A/B/escape.
- **[`B=` canary cost]** Stays inside the existing `%CKPT` SD line (safe sample boundary); one extra `%lu`
  in the existing snprintf — verify the 128-B `tmp` still fits the longest line.
- **[Scope honesty]** Does NOT fix the 06-20 `DSPI` hard freeze (parked, Decision 9). DOES close the entire
  stray-RX-byte class (truncate/close/erase/channel-drop) — the failures costing nights.
- **[Panel couldn't read source]** Both reviewers were sandbox-denied repo access (plan-text review only);
  the line-cited code facts are Claude's direct reads and are re-verified on the real build in /grill (P0.6).
- **[Intermittency honesty]** The trigger is a probabilistic stray byte, so a clean bench can't prove the
  fix — **P3.2's deliberate injection is required**, since a passive bench may never receive a stray byte.

## Out of scope
- The 06-20 hard freeze / `DSPI::transfer` unbounded spin (separate hardware-WDT prep).
- Stopping the RFduino from emitting stray bytes (radio-firmware level; we harden the cyton to survive).
- `sdMetaProcess` rewrite, `SESS_<NN>.TXT`, SD-readback `%META` verify (Decisions 5/6/8 — not needed).
- Full rollback to pre-2026-05 firmware / host-protocol rework.
