# Prep: robust post-reset SD auto-resume (recover the night across chained slots)

## Goal
When an **external** event resets the Cyton MCU mid-night (intermittent power/connection
interruption — NOT the SD card, NOT a dead cell), the firmware must **recover the recording**:
correctly un-wedge the SD after a *warm* reset and **continue into a fresh slot**, so the user gets
the whole night across chained `OBCI_*.TXT` files (`collect_bci.build_session_chains` stitches them).
Today the boot-resume opens the next slot but fails to write it → a silent **0-byte slot** + no
forensic file. Fix that one fragility, correctly; change nothing else.

> **USER DECISION (locked):** "Delay-only recovery." Un-wedge + a stabilization delay + the existing
> resume-time slot allocation. The complex pre-allocation alternative is DROPPED. The user has
> **explicitly accepted** the small residual FAT-corruption risk the panel flagged (a 2nd glitch
> landing inside the resume's `createContiguous` FAT write could orphan the prior slot — empirically
> 0/3 real events; `executeSoftReset` stays disabled so there is no rapid-fire loop). The real
> prevention remains the hardware reseat of the JST connector.

## Context & constraints
- Board: OpenBCI Cyton (PIC32MX250F128B, 32 KB RAM, chipKIT bootloader, MIPS M4K). SD via the **DSPI**
  peripheral shared with the ADS1299 (`Sd2Card card(&board.spi, SD_SS)`); `board.begin()` brings DSPI
  up BEFORE `replaySessionFile()`. `board.csLow/csHigh` are **DSPI-coupled**, not raw GPIO.
- **Flash ~96 % full (~4.4 KB headroom).** Map+symbol-delta gate; helper is a small GPIO bit-bang.
- Final verification = the **user's flash + overnight sleep test**; bench proxy = reset injected
  *inside* the CMD25 write window. Mandatory **Codex+Gemini review**. User powers off via switch (no
  `--stop`); `collect_bci` removes `SESSION.TXT` + `REPLAYFL.TXT` on ingest each morning.

### Evidence (full detail in `work-log.md` + `…/tmp/obci_sd_forensics.md`)
- 6 nights: full nights run 7.5–8.5 h then power-off; failing nights stop at a **variable 4–6 h**,
  `e=0 r=0 n=0 x=0` (SD recovery never fires) incl. a new card with zero overruns → the stop is an
  **MCU reset**, not SD. Intermittent on the same firmware + cell confirmed fine → **external
  intermittent power/connection interruption** (leading candidate: JST battery connector / contact).
- After a glitch reset the card is left **stuck mid-CMD25** (powered, not cold); `card.init`'s 80 idle
  clocks don't abort CMD25 → `createContiguous`/`erase`/`writeStart` fail → **0-byte slot**;
  `writeReplayFail`'s own `card.init` also fails → **no REPLAYFL**.

### Why it "worked before" but fails now (the regression)
A **clean power-cycle** de-powers the card → cold init succeeds → resume works (the only path the
2026-06-24 bench verified). A **mid-write glitch reset** leaves the card powered + stuck mid-CMD25 →
`card.init` doesn't abort CMD25 → resume fails (0-byte). This fix issues the correct phase-safe
CMD25/CMD18 abort so the glitch path recovers like the clean path.

## Decisions
1. **Scope = RECOVERY (delay-only), not prevention.** Hardware reseat of the JST connector is the
   user's parallel action and the real prevention.
2. **Root fix = a phase-safe, bit-banged warm-reset bus recovery that PRECEDES the existing
   `card.init`, followed by a FORCED full init.** New helper `sdBusRecover()`. **All bit-bang is
   SPI mode 0** (CPOL=0/CPHA=0, MSB-first, SCK **idles LOW**, set MOSI then pulse SCK high→sample
   MISO on the rising edge→SCK low — R4 Codex#5/Gemini#3). Sequence:
   - **Take the SD pins off DSPI onto GPIO — PIC32 PPS-aware, glitch-free, exact register order
     (the single highest-risk step; /grill HARDWARE-verifies the exact sequence):**
     1. **Pre-load the output latches to safe idle FIRST:** `LAT` SCK=LOW, MOSI=HIGH, `SD_SS`=HIGH,
        ADS1299 CS=HIGH (`digitalWrite` before any direction change).
     2. **Set directions:** SCK/MOSI/`SD_SS`/ADS-CS = OUTPUT, MISO = INPUT.
     3. **Clear the PPS OUTPUT mapping for MOSI/SDO1 (`RPxnR = 0`)** so the `LAT` register actually
        drives the pin (R6 Gemini#2, BLOCKER): `SPI.end()` disables the module but does NOT release
        PPS, so without this the disabled peripheral keeps driving SDO1 and the bit-bang clocks out
        **all zeros** — `0xFD`/CMD12 never transmit and recovery silently no-ops. (SCK1 is a dedicated
        pin on the MX250, not PPS; MISO/SDI1 is read via `PORT`, unaffected.)
     4. **THEN disable the SPI/DSPI module** (`SPI.end()` / clear `ON` via the owning DSPI API — R4
        Codex#3) — AFTER 1-2 so disabling can't snap a stale `LAT`/`TRIS` into a phantom SCK edge
        (R5+R6 Gemini#1, BLOCKER). `card.init` re-establishes PPS + module + CMD0/ACMD41 afterward.
     The ADS1299 CS held HIGH keeps it off MISO/clocks (R4 Codex#2). (R2: `board.cs*` are DSPI-coupled
     — raw register access, not `board.cs*`, is required here.)
   - **Mask the ADS1299 DRDY interrupt (+ any other DSPI-using ISR) for the duration of
     `sdBusRecover`** (R6 Codex#1) so nothing re-enters DSPI while the pins are raw GPIO; re-enable on
     return (the normal stream path re-arms it). Recovery runs at **boot, pre-stream**, so this is
     belt-and-suspenders, but cheap. Mask the *specific* interrupt, NOT global `di()`, so `millis()`
     keeps advancing for the timeouts; /grill confirms no ISR touches DSPI in the window.
   - **Phase-safe CMD25/CMD18 abort. Every busy-wait ACTIVELY CLOCKS the bus** (loops `sdBbByte(0xFF)`
     while sampling MISO — polling MISO without toggling SCK never advances the card's state machine,
     R5 Gemini#2), and **every wait is `millis()`-bounded with a CONSERVATIVE seconds-scale bound for
     MISO-LOW programming waits** (align to SdFat's `SD_WRITE_TIMEOUT`, already raised to 1500 ms →
     use ~2 s; the ~250 ms initial drain stays short) so a legitimately-programming card is NEVER
     misread as dead (R6 Codex#2). **Fail-fast → "no recovery" (skip the FAT write; let
     `writeReplayFail` run) ONLY on a MISO-stuck-LOW timeout** (a genuinely dead/wedged card) —
     **never** fail just because no response token was seen:
     1. CS LOW → bounded busy-drain (clock `0xFF`, sample MISO, until high or ~250 ms).
     2. **Clock a FULL data packet of `0xFF` (≥514 bytes = 512 payload + 2 CRC, plus margin)** so a
        card stuck mid-payload finishes the block (R3: "a few" bytes get swallowed as payload).
     3. **Bounded busy-wait for MISO HIGH by continuously clocking `0xFF` (~500 ms), then send `0xFD`.**
        Do **NOT** parse/consume a data-response token (R5 Codex#1 + Gemini#1, both flagged): if the
        glitch was mid-payload the token already passed *during* the ≥514 drain; if it was in the
        token-wait phase *between* blocks no token ever appears. So: if MISO is already HIGH (between
        packets, or programming already done) proceed straight to `0xFD`; if MISO is LOW the flushed
        block is programming (BUSY) → keep clocking until it releases. A `0xFD` sent during BUSY is
        ignored → would stay wedged, so this wait is mandatory; but token-absence is normal, not a fail.
     4. Send the **stop-tran token `0xFD`** at a byte boundary → bounded busy-wait for MISO high
        (clocking `0xFF`, ~500 ms).
     5. **Best-effort** bit-banged **CMD12** (48-bit, valid CRC) to clear a stuck CMD18 read (card
        drives MISO, ignores `0xFD`). **Find R1 by clocking `0xFF` until a byte with MSB=0 arrives,
        bounded ~16 bytes** (a wedged card may exceed the spec's 8-byte N_CR flushing its pipeline —
        cheap margin, R7 Gemini#1; do NOT blind-discard exactly one stuff byte — cards send 0/1/many idle
        bytes; a blind discard can eat the real R1 — R6 Gemini#3), then wait R1b busy-release
        (clocking, bounded). CMD12 may legitimately be illegal/no-op after a CMD25 `0xFD` — **parse/
        log its R1 but NEVER fail recovery on a CMD12 illegal/no-response**; recovery fails ONLY on a
        MISO-stuck-low timeout (R6 Codex#4).
     6. CS HIGH → ~16 trailing idle clocks (CS high, clock `0xFF`) to release the bus.
   - After `sdBusRecover()` returns, **force the full protocol init**: set `cardInit=false` so the
     existing `if(!cardInit){ card.init(SPI_FULL_SPEED,SD_SS); volume.init(card); }` block ALWAYS runs
     (R3 Codex#1: a stale `cardInit==true` would skip it on the GPIO'd bus). `card.init` must re-take
     DSPI ownership cleanly — it re-runs DSPI/PPS setup + CMD0/ACMD41. **/grill MUST verify on
     hardware that PPS/peripheral state is fully restored after the raw-GPIO detour** (R4 Codex#3 — if
     `card.init` does NOT re-map PPS for the SD pins, `sdBusRecover` must re-run the DSPI/PPS init
     (re-assert peripheral pin-select + `ON`) before returning).
   - **PARK `SD_SS` (+ ADS1299 CS) HIGH as raw GPIO at the very START of `setup()`, BEFORE
     `board.begin()`** (R5 Codex#2): `board.begin()` brings DSPI up and clocks the ADS1299 *before*
     `replaySessionFile()`/`sdBusRecover()` run. If the wedged card's `SD_SS` is floating/low during
     that, the ADS init traffic on the shared bus reaches the still-CMD25-selected card and corrupts
     its state before recovery starts. A 2-line CS-park (latch high, then output) at the top of
     `setup()` guarantees the card is deselected until `sdBusRecover` deliberately selects it. Cheap,
     defensive, harmless on a cold boot.
3. **Bound EVERYTHING** — every wait in `sdBusRecover` has a `millis()` timeout + graceful abort; the
   helper provably terminates and can never hang the boot (soft-WDT is off).
4. **Keep `executeSoftReset(0)` + soft-WDT DISABLED** (rollback `6f6efe8` stays).
5. **`setupSDcard` FAIL-FAST on allocation failure (pre-existing latent bug — R2 Codex#3).** Today,
   `createContiguous`/`contiguousRange` failure only sets `cardInit=false` and **falls through** into
   `card.erase(bgnBlock,endBlock)` + `card.writeStart` with a STALE range → can erase/write WRONG
   blocks. After any create/range failure: **`openfile.close()` (close/reset the partially-created
   file object so no stale handle/dir state leaks — R6 Codex#3)** + `board.csHigh(SD_SS)` + `return
   fileIsOpen;` (the fn is `boolean`, `fileIsOpen` is false here — R3 Gemini#3 "void" claim REJECTED,
   source-verified L1143/L1403) **before** any `erase`/`writeStart`.
6. **UNIVERSAL `openfile.remove` guard (R2 Gemini#3) — preserve only NON-ZERO real-data files, and
   FAIL-FAST on exhaustion.** In `setupSDcard`, before `openfile.remove(root, currentFileName)`: open
   the target `O_READ`; if it exists with `fileSize() > 0`, bounded-loop `incrementFileCounter()` to a
   free/0-byte name; **`.close()` the O_READ test handle each iteration** (R3 Gemini#4 — else a handle
   leak breaks the next open/create). **Scan cap aligned to the resume/filename namespace, and after
   the loop RE-CHECK: if `currentFileName` STILL resolves to a `fileSize() > 0` file (scan exhausted
   without finding a safe slot), do NOT fall through to `remove` — **`.close()` the re-check handle**
   (R5 Gemini#4 — else the `SdFile` slot leaks in the error path), `board.csHigh(SD_SS)` + `return
   fileIsOpen;` (fail-fast)** (R4 Codex#4 + Gemini#2 — both flagged: a bare ≤16 loop that exhausts
   would otherwise blindly `remove` a valid 16th non-zero file = silent data loss). A non-zero file is
   NEVER removed (cold + resume). A 0-byte collision MAY be removed (audit trail is best-effort — R3
   Codex#5 acknowledged; "leave 0-byte slots" is best-effort, not guaranteed).
7. **FAT-corruption-race mitigation = a CONDITIONAL stabilization delay (~2-3 s)** placed **only after
   a valid resume intent is confirmed** (`SESSION.TXT` opened + `%PBEGIN`/`%PEND` validated) and
   **immediately before the first resume FAT write** — so a normal bedtime boot (no `SESSION.TXT`) is
   byte-for-byte unchanged (R3 Codex#6), and the FAT write only starts after stable power (a bounce
   during the delay just resets the MCU again before any FAT mutation). Residual risk **user-accepted**
   (see banner). Pre-allocation is the documented escalation if the connector fix doesn't hold.
8. **REPLAYFL: non-destructive + one-shot (R2 Codex#5).** In `writeReplayFail`, run `sdBusRecover()`
   **before** the existing `card.init` (keep `card.init` — R2 Gemini#2). Do **NOT** `SdFile::remove`
   an existing `REPLAYFL.TXT`: if it exists, skip (collect_bci unlinks it on ingest each morning, so
   it won't persist across nights — R3 Gemini#5). One attempt per boot.
9. **Resume always uses a FRESH slot; the prior slot's EARLIER data is untouched, its interrupted
   final block is SACRIFICIAL.** The ≥514-byte drain finalizes the one block the card was mid-writing
   into the prior slot's range with partial-real + `0xFF` fill (R5 Codex#3) — the card's CMD25 write
   pointer still targets that range across the warm reset. This is harmless: **every night is already
   footerless** (user powers off, no `--stop`), so the prior slot ALWAYS ends in an undefined tail and
   `collect_bci` already parses samples until the first malformed line. The drain just makes that last
   block deterministic instead of leaving stale card content. No NEW corruption vs the already-
   footerless norm. /grill **verifies `collect_bci` cleanly truncates a partial+`0xFF` tail block**
   before stitching the next slot. FAT-region race (the directory/FAT mutation for the NEXT slot) is #7.
10. **Keep immediate `incrementFileCounter`; reset-cause forensics = only `%BOOT seq=<bootSeq>`** (free).
    No RAM signature / exception handler / NV-write-in-fault / `SD.remove()` 0-byte deletion (rejected).
11. **Behaviour-identical on a normal night** — guaranteed by #6 (guard only acts on a non-zero
    collision, bounded) and #7 (delay is resume-only). `sdBusRecover` on a cold boot is bounded +
    harmless. Full recording + morning power-off unchanged.

## Plan
1. **Add `sdBbByte(uint8_t)→uint8_t`** (bit-bang one byte over GPIO, returns MISO) + **`sdBusRecover()`**
   per Decisions 2/3 (SD_Card_Stuff.ino near `writeReplayFail`). Resolve SD pin numbers
   (SCK/MOSI/MISO/`SD_SS`) from the DSPI/`Sd2Card` config. **Follow the exact Decision-2 handover
   order — latches → TRIS → clear MOSI PPS → disable module — NOT a loose "disable then pinMode"**
   (R7 Codex#1).
2. **`DefaultBoard.ino setup()`:** park `SD_SS` (+ ADS1299 CS) HIGH as raw GPIO at the **very top,
   before `board.begin()`** (Decision 2, R5 Codex#2) so pre-recovery DSPI/ADS traffic can't reach the
   wedged card. Then **`replaySessionFile`:** call `sdBusRecover()` then `cardInit=false` immediately
   before the `if(!cardInit){…}` block (~L806). Add the **conditional stabilization delay** after
   `SESSION.TXT` validation passes and before the K-command's first FAT write (Decision 7).
3. **`setupSDcard`:** (a) fail-fast `return fileIsOpen;` after create/range failure, before
   erase/writeStart (Decision 5); (b) universal remove-guard with per-iteration `.close()` (Decision 6).
   Success path stays byte-identical.
4. **`writeReplayFail`:** `sdBusRecover()` **before** the existing `card.init`; skip if `REPLAYFL.TXT`
   exists; one attempt (Decision 8).
5. **Map+symbol-delta flash gate** vs a `git stash` baseline; reject new `Print`/string pulls; trim a
   `!board.streaming` diagnostic string if tight.
6. **Do NOT touch** writeCache recovery, the rollback, RX-hardening, metaArmed, `%META`/`%CKPT`, the
   host protocol, or `collect_bci`.

## Files to touch
- `examples/DefaultBoard/SD_Card_Stuff.ino` — `sdBbByte` + `sdBusRecover`; edits to `replaySessionFile`,
  `setupSDcard`, `writeReplayFail`.
- `examples/DefaultBoard/DefaultBoard.ino` — the pre-`board.begin()` `SD_SS`/ADS-CS park in `setup()`
  (Decision 2); forward declarations if ordering needs them.
- `CLAUDE.md` — document after /grill. No SD-lib-fork change. No `collect_bci` change (verify only).

## Test plan
- **Build/size:** links under flash; record byte + symbol/map delta vs baseline.
- **Static safety (panel):** (a) `sdBusRecover` pin-handover is PIC32-correct: latches set → TRIS
  output → **PPS output map for MOSI/SDO1 cleared (`RPxnR=0`)** → THEN module disabled (LAT actually
  drives MOSI, no phantom clock); ADS1299 CS parked high; (b) the ADS DRDY/SPI ISR is masked across
  recovery (specific interrupt, not global `di()`, so `millis()` runs); (c) **all busy-waits clock
  `0xFF` while sampling MISO**; clocks ≥514 `0xFF`, then waits MISO-high (NO token parse) BEFORE
  `0xFD`; CMD12 best-effort (R1 found by MSB=0 scan, never fatal); MISO-low waits seconds-scale
  bound; **fail-fast ONLY on MISO-stuck-low** → cannot hang; (d) `SD_SS`/ADS-CS parked high before
  `board.begin()`; (e) `cardInit=false` forced after recovery so `card.init` re-runs + PPS/DSPI
  restored; (f) `setupSDcard` fail-fast **closes `openfile`** before erase/writeStart; (g) remove-guard
  never removes a non-zero file, `.close()`s each probe **and the exhaustion re-check handle**, and
  fail-fasts on scan exhaustion; (h) stabilization delay resume-only; (i) REPLAYFL never unlinks an
  existing file.
- **`collect_bci` tolerance (verify, no change):** confirm the host parser truncates/ignores a
  partial-real + `0xFF` tail block on the prior slot and still stitches the next slot (Decision 9).
- **Bench smoke (user, dongle, daytime) — hit the CMD25 window:** start a short recording; inject a
  reset *inside* a multi-block write (brief power interrupt / MCLR pulse timed into a write) leaving
  the card powered. Confirm reboot un-wedges, opens the **next** slot with `%BOOT … resume=1
  prev=OBCI_<prev>` + real samples (NOT 0 bytes), prior partial file intact, `collect_bci --reprocess`
  stitches the chain.
- **Field (user):** flash + sleep; a 4-6 h reset night yields a **chained pair** reconstructing the
  full night. `%BOOT seq` delta + REPLAYFL document the resets.

## Risks & open questions
- **Residual FAT-corruption race during resume `createContiguous`** — REDUCED by the conditional delay,
  not zero; **user-accepted**. Empirically 0/3 events. Escalation if it ever recurs after the connector
  reseat = pre-allocation (a bigger change, flash permitting).
- **DSPI release/re-acquire + PPS correctness (R4 Codex#3)** — `sdBusRecover` releases DSPI through the
  owning object and must leave the bus so `card.init` cleanly re-takes it (re-mapping PPS for the SD
  pins). If `card.init` does NOT re-assert PPS, `sdBusRecover` must re-run the DSPI/PPS init before
  returning. /grill verifies on hardware (bench reset-injection) that init + a real write succeed
  post-recover — this is the single highest-uncertainty implementation point.
- **ADS1299 on the shared bus (R4 Codex#2)** — its CS must be GPIO-high throughout recovery; /grill
  confirms the analog frontend re-initializes normally after `card.init` re-takes DSPI (resume keeps
  recording EEG, not just opens a file).
- **≥514-byte drain timing** — at the bit-bang clock rate this adds a few ms to the recovery; bounded
  + only on a resume boot. Acceptable.
- **Repeated mid-resume resets** burn names (5B,5C,…) bounded by `MAX_RESUMES=25` → REPLAYFL + idle.
- **Flash budget** — main build risk; bit-bang + map/symbol gate; if tight, trim diagnostics.
- **DEFERRED, named next mitigation:** gate `board.sendChannelData()` RF-TX off while `SDfileOpen` to
  cut peak current during SD-only logging (may PREVENT brownout-class resets). Out of THIS change.

## Out of scope
- Pre-allocation recovery (dropped per user decision; escalation-only). Re-enabling `executeSoftReset`/
  soft-WDT; deleting 0-byte slots; deferring the file counter; RAM reset-signature; exception handler;
  EEPROM/flash write in a fault context (panel-rejected).
- Changing writeCache recovery, RX-hardening, `%META`/`%CKPT`/protocol, or `collect_bci`.
- RF-TX gating (deferred). Hardware reseat of the JST connector + SD card is the **user's** parallel
  action (the real prevention); a bench `%CKPT`-watch run would confirm the connector cause.
