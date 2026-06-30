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
