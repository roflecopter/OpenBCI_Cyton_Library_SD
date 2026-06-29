# Prep: Fix the intermittent Cyton freeSD MCU hang (ENH_BUFFER SPI overrun / SPIROV)

## Goal
Stop the intermittent overnight freeze that halts the Cyton SD recording mid-night
(variable 3.6–9.4 h) and leaves the board dead until a power-cycle. Root cause is pinned at
register level: the **512-byte SD block write uses DSPI's ENH_BUFFER (8-byte FIFO) bulk
transfer, whose `while(toWrite||toRead)` drain loop is unbounded**; when the DRDY ISR delays
the CPU mid-loop the RX FIFO overruns → **SPIROV** latches → received bytes are lost →
`toRead` never reaches 0 → infinite spin (board dead, clean data to the cut, no footer, no
reset, LED off, alive only after power-cycle). The hang occurs **inside `card.writeData()`**.
The fix makes the SPI primitives **FIFO-safe + bounded** so the bulk write *returns a failure
with a sticky fault flag* instead of hanging, then routes that fault through a proper recovery:
an **atomic SPI-module flush** (clears the host FIFO/SPIROV) followed by the firmware's
**already-correct `sdBusRecover()` CMD25 abort** (520-dummy flush + STOP_TRAN — promoted from
boot-only to the live cascade), `card.init`, and a **same-block retry** of the preserved
512-byte `pCache`. A safe, opt-in soft-WDT backstop (OFF by default) and a fork-side
synthetic-SPIROV self-test are added so the fix is provable on the bench.

## Context & constraints

- **Hardware**: OpenBCI Cyton, PIC32MX250F128B (32 KB RAM), chipKIT **DP32 bootloader**, SYSCLK
  40 MHz (CP0 Count = SYSCLK/2 = **20 MHz**). ADS1299 reads + SD writes share **one** SPI
  (`DSPI0 spi` → **SPI1**). **DRDY ISR only sets a flag** (`OpenBCI_32bit_Library.cpp:1339`);
  all SPI is in `loop()`, **sequentially: ADS read then SD write**. No SPI in any ISR.
- **SPI master = host drives the clock**, so a 512-byte block **shift** is deterministic
  (~205 µs at 20 MHz) and the card **cannot stall it mid-block**; the card signals busy by
  holding MISO **after** the block (programming), polled by `waitNotBusy`. ⇒ the per-block
  SPI-shift deadline and the card-busy timeout are **independent**.
- **Two DSPI transfer shapes (verified in `DSPI.cpp`):** single-byte `transfer(uint32_t)` (`:583`)
  = standard mode (`SPITBE`/`SPIRBF`), used by `spiRec`/`spiSend`; bulk `transfer(uint16_t,src)`
  (`:672`) = temporarily enables **ENH_BUFFER**, streams via `SPITBF`/`SPIRBE`, clears ENH_BUFFER
  on exit — used by `writeData` (`Sd2Card.cpp:552`); its `while(toWrite||toRead)` loop is the
  **primary hang**. ⚠ **PIC32 rule: `ENHBUF` is writable only when `ON=0`** — any module flush
  that re-clears ENHBUF MUST do it between `ON=0` and `ON=1` (Gemini R3).
- **The existing recovery infrastructure is sound — leverage it, don't reinvent:**
  - `writeCache()` (`SD_Card_Stuff.ino:1906`): on `card.writeData(pCache)` failure → `writeStop()`
    + `writeStart(bgnBlock+blockCounter,…)` (SAME block) + `writeData(pCache)` retry (the 512-byte
    `pCache` global is preserved); then skip-forward into a `card.erase()`-**pre-zeroed** block
    (counted in `sdRetries`/footer), then `card.init`, then the extended-recovery window, then
    `sdCardDead` + footer + `REPLAYFL` (fail closed). This boundary cascade is byte-identical for
    the non-SPIROV case; the **SPIROV case is routed differently** (Decision 8) because a
    mid-payload abort desyncs the card and bare `writeStop` can't fix it.
  - `sdBusRecover()` (`:751`, `static`): CS-low → **520 dummy bytes** (finishes ANY partial CMD25
    payload, position-agnostic) → bounded BUSY wait → **STOP_TRAN 0xFD** → BUSY wait → CS-high +
    idle clocks. This is the correct mid-CMD25 abort; it is currently **boot-only** and uses the
    **unbounded** `board.spi.transfer`. v4 promotes it to live recovery + bounds its pokes.
- **Repos / files**:
  - Sketch repo (version-controlled): `OpenBCI_Cyton_Library_SD/`, sketch
    `examples/DefaultBoard/{DefaultBoard.ino, SD_Card_Stuff.ino}`.
  - **SD fork (out-of-repo, patch-tracked)**: `~/Arduino/libraries/OpenBCI_32bit_SD/utility/Sd2Card.cpp/.h`
    (`<OBCI32_SD.h>`); already `#include <p32xxxx.h>` + `bnRbf=0`/`bnTbe=3`. Edits recorded in
    in-repo `patches/sd-fork-write-timeout.patch`.
  - **DSPI (toolchain, shared with ADS reads)** — **out of scope to edit** (user decision).
- **Build**: `arduino-cli compile --fqbn chipKIT:pic32:openbci …/examples/DefaultBoard --output-dir …/build-grill`.
  Debug build adds `--build-property compiler.cpp.extra_flags=-DSD_DEBUG_FAULT_INJECT` (reaches the
  library TU; verify the symbol in the `.map` — Codex R3#9).
- **Flash**: ~96 % full, **122880 B max**, FRAGMENTED; tip (`38b3896`) = **118600 B** (~4280 B
  free). Surgical diff; self-test compiled OUT of production. **Per-component size estimate in
  /grill before flashing** (GLM R2#7). Trim levers: debug prose (never host-contract strings).
- **Flash device**: `pic32prog -d /dev/ttyUSB0 -b 115200 <hex>` — **NEVER kill-timeout** (bricked
  Cyton A); run to "Verify flash … done."
- **MANDATORY** parallel **Codex + Gemini + GLM** review before ANY flash. **NEVER flash without
  explicit user OK** (overnight sleep test = final verification).
- **HOST-CONTRACT STRINGS** never trimmed: `"Size "`, `"META OK/FAIL/ERR"`, `"PERSIST OK/FAIL"`,
  `"TUNE OK/FAIL"`; lib-level `"Sample rate is …Hz"`, register dump, `"$$$"`. Size/%META restore
  (`38b3896`) is the tip and ships in this flash.
- **Forensics**: `collect-bci` STOPPED (slots 5A/5D/5E must survive); `collect-hyp` active.

## Decisions

1. **Scope** (user): root-cause SPI fix + **soft-WDT OFF by default behind a tune flag** +
   **fork-side synthetic-hang self-test** + **WDTPS readout**, **ONE flash** on `38b3896`. Keep
   the one-flash bundle (user minimizes flash COUNT; soft-WDT inert when off → no overnight
   attribution loss; rejects Codex R2#15 "stage it").
2. **Keep the fix in the patch-tracked OBCI32_SD fork; do NOT edit `DSPI.cpp`.**
3. **`sdSpiModuleFlush()`** (fork) — uses **atomic CLR/SET register ops**, no save/restore, with the
   ON and ENHBUF clears **SPLIT** so ENHBUF is only written when ON is already 0 (Gemini R6#1 — a
   single combined write may evaluate ENHBUF-writability on the *old* ON=1 and silently drop the
   clear; this supersedes R5's single-write): `SPI1CONCLR=_SPI1CON_ON_MASK` (ON→0) → **readback to
   settle** → `SPI1CONCLR=(_SPI1CON_ENHBUF_MASK|_SPI1CON_MODE16_MASK|_SPI1CON_MODE32_MASK)` (now
   legal) → `SPI1STATCLR=_SPI1STAT_SPIROV_MASK` → `SPI1CONSET=_SPI1CON_ON_MASK` (ON→1) → readback.
   **The mode/CKE/CKP/MSTEN bits and SPIBRG are NEVER touched** → the active SPI mode (MODE0 for SD,
   MODE1 for ADS) and the clock divider are preserved automatically — **no MODE re-assert anywhere**.
   Atomically flushes TX/RX FIFO + shift register + SPIROV. It **CLEARS `sdSpiFault`** (the "hardware
   handled" point — reconciles R3#2/R4: the PRIMITIVE only latches so the owner can SEE the fault;
   the OWNER calls this flush, which clears both hardware and the latch *before* running recovery
   primitives). CS (a GPIO) is preserved.
4. **FIFO-safe bounded bulk `spiBlockBounded(src,n)`** — local copy of DSPI's ENH_BUFFER bulk loop
   that **caps in-flight bytes** (`written-read < 7` with eager draining — GLM R6#1) so the RX FIFO
   (8 deep) **never overruns** under ISR jitter (PREVENT — Codex R2#5), **plus** a CP0 deadline +
   `SPIROV` check as the infinite-spin backstop. **The happy-path ENHBUF toggle MATCHES the stock
   DSPI bulk transfer (set/clear ENHBUF while `ON=1`)** — this is **empirically proven** on this
   MX250 (months of recordings) and, critically, does **NOT** toggle `ON` mid-transaction. The
   `ON=0` split form is **reserved exclusively for `sdSpiModuleFlush`** (recovery): toggling `ON=0`
   mid-CMD25 would release SCK/SDO to their PORT/TRIS state with CS still low and risk a spurious
   clock edge that desyncs the card (Gemini R8#1 — this is why the strict split must NOT be applied
   to the hot path; it is only safe in the recovery flush where the transaction is being abandoned).
   **Entry short-circuit**: `if (sdSpiFault) return false;` at the top (a known-poisoned bus aborts
   instantly instead of waiting the CP0 deadline; Gemini R8#3). On bail: **set `sdSpiFault=1` and
   return false** (does NOT itself flush — recovery owns that; Codex R3#2); a bail's ENHBUF state is
   fixed by the recovery's `sdSpiModuleFlush`. Keeps FIFO
   throughput (NOT a per-byte loop). Replaces `writeData`'s `_spi->transfer(512,…)`. **Drain
   eagerly** — read `SPI1BUF` every loop iteration whenever `SPIRBE` shows data, and gate the next
   write on `in-flight < 7` — to maximize ISR-jitter tolerance so the FIFO realistically never
   overruns (GLM R4#5).
5. **Bounded standard single-byte `spiByteBounded(out)`** — **entry short-circuit `if (sdSpiFault)
   return 0xFF;`** (a poisoned bus aborts instantly, no per-byte deadline stall for a naive caller
   like FAT ops / file close before the fault bubbles up; Gemini R8#3) → wait `SPITBE` (bounded,
   **mandatory** before write) → write `SPI1BUF` → wait `SPIRBF` (bounded) → read. On deadline: set
   `sdSpiFault=1`, return `0xFF`. `spiRec`/`spiSend` call it.
6. **Deadlines are generous INFINITE-SPIN BREAKERS via CP0 Count** (rollover-safe): per-byte a few
   ms, per-block tens of ms — above 512 B-shift + worst-case ISR jitter, far below "forever".
   **Card-busy stays on `waitNotBusy(SD_WRITE_TIMEOUT=1500 ms)`** (a legit GC stall is *between*
   blocks, never mid-shift — so no GC false-trip). `sdSpiFault` is set **only** by the CP0/SPIROV
   primitive, **never** by `waitNotBusy`.
7. **`sdSpiFault` is a STICKY latch**: set by the bounded primitives on bail (the primitive only
   latches — it does NOT flush, so the recovery owner can SEE the fault); **callers check it
   immediately after every `spiRec`/`spiSend`** (e.g. `waitNotBusy` tests `sdSpiFault` at the top
   of its loop and returns false) rather than trusting the ambiguous `0xFF` byte (Codex R3#5). It
   is cleared **only by `sdSpiModuleFlush()`** (Decision 3), which every recovery/init owner calls
   *before* running recovery primitives — so the latch never short-circuits the recovery itself
   (Codex/Gemini/GLM R4#1), while a NEW fault during recovery re-latches and is caught. The latch's
   sole job is to short-circuit the *remaining* in-flight primitives of an already-wedged op.
8. **SPIROV recovery (Layer 1b)** — in `writeCache`, when `card.writeData(pCache)` fails **and
   `sdSpiFault` is set**: `sdSpiModuleFlush()` (clears host FIFO/SPIROV **and the latch**, so the
   following primitives run) → **`sdBusRecover()` called LIVE** (the proper 520-dummy + STOP_TRAN
   that knocks the card out of mid-CMD25 — Gemini R3#2/3, Codex R3#1/6) → `card.init` →
   `writeStart(bgnBlock+blockCounter,…)` → `writeData(pCache)` (same-block retry of preserved data).
   **Check each return; if `card.init`/`writeStart`/`writeData` fails, FALL INTO the existing
   skip-forward / extended-window / `sdCardDead`+footer+`REPLAYFL` tail — never silent-exit** (else
   an infinite silent block-drop loop; Gemini R4#2). The `%E` discontinuity marker is **deferred**
   (set a flag/counter, emit AFTER the recovery critical section — never recurse SD writes through
   `pCache` mid-recovery; Codex R3#3). **Non-SPIROV** `writeData` failures keep the existing bare
   boundary cascade (byte-identical).
9. **`sdBusRecover()` made live-safe**: `sdSpiModuleFlush()` at entry (clears flag+hw) + route its
   `board.spi.transfer` pokes through the `extern`'d fork `spiByteBounded` (GLM R4#3) + **`if
   (sdSpiFault) break;` inside the 520-byte dummy loop and the BUSY waits** so a still-wedged bus
   bails immediately instead of paying 520×deadline (~5 s stall; Gemini R4#4).
10. **ADS-path guard = a fault-keyed flush at the top of `loop()`** (sketch, register-local; no
    `OpenBCI_32bit_Library.cpp` edit): `if (sdSpiFault) sdSpiModuleFlush();`. This catches **every**
    path that leaves the latch set — not just the `writeCache` SPIROV branch, but terminal/non-
    `writeCache` paths (`cardCommand`, FAT `writeBlock`, `writeFooter`, `closeSDfile`, `sdCardDead`,
    the soft-WDT close) that bubble up **without** flushing — so the next ADS read never runs on a
    poisoned bus (Gemini/GLM R5). It is **mode-safe now** because Decision 3's masked-CON restore
    preserves the active SPI mode (this is exactly why the round-4 MODE0-forcing version was unsafe
    and this one is not). It keys on the **fault latch**, not a raw `SPI1STAT` read, and is a no-op
    on a clean bus (the common case — `writeCache` already cleared the latch in its own phase).
11. **Soft-WDT behind `TUNE_KEY_SOFT_WDT_ENABLE=0x06`** (1 byte, default 0/OFF;
    `boolean tuneSoftWdtEnable`). 0x06 **IS carried in the session's `SESSION.TXT` `%TUNE` line**
    (session-scoped persistence) so it survives a within-night soft-WDT resume (else the watchdog
    could only ever fire once/night and the per-night cap is moot — Gemini R5#3); a **fresh** night's
    host-written `SESSION.TXT` simply omits it → default OFF → no leak to a future/attribution night
    (reconciles Codex R3#8). Our overnight test runs with it **OFF**. On fire: **`sdSpiModuleFlush()`
    FIRST** (a freeze likely already latched `sdSpiFault`, which would otherwise short-circuit the
    close to a no-op — Gemini R5#2) → **`sdBusRecover()`** (the freeze may be mid-CMD25; its 520-flush
    + STOP_TRAN is needed before any footer can land, else `writeStop`'s bare `0xFD` is eaten as a
    data byte — Gemini R8#2) → best-effort **bounded clean close** (`writeFooter`→`closeSDfile`) →
    **`executeSoftReset(0)` ALWAYS — even if the close fails** (mark the slot
    **suspect** via `REPLAYFL` if so) → fresh-slot resume. A watchdog that aborts its own reboot is
    useless (Gemini R4#3); resetting is safe here because: the 120 s floor means it only fires on a
    TRUE freeze (board already hung, not actively writing); the slot's dir entry was written at
    `createContiguous`, so a reset leaves **readable partial data** (like 5D/5E), not all-NUL; boot
    `sdBusRecover` knocks the wedged card out of CMD25; and the per-night cap + the 2.5 s
    resume-stabilize delay prevent the reset-storm that was the original all-NUL cause. Floor =
    `max(SOFT_WDT_FLOOR_MS=120 s, 2×tuneCkptIntervalMs)`. Provably inert when flag 0.
12. **Per-night soft-WDT cap** = a new EEPROM byte at an **audited-free address** + a **sticky
    "soft-WDT-reset-pending" record written as magic+complement** (torn-write/brownout safe;
    Codex R3#8). **On fire (before resetting): read the cap byte → if ≥ cap, do NOT reset (clean
    stop) → else increment + write it back, then set the sticky flag + `executeSoftReset`** (Gemini
    R6#3 — without the increment the watchdog fires endlessly = the reset storm it's meant to
    prevent). In `setupSDcard()` consume-and-clear the sticky flag once and **skip the cap reset when
    it indicates a soft-WDT reboot** (RCON is unreliable — the bootloader clears it; GLM R2#5). Reset
    the cap to 0 only on a genuinely fresh (non-replay, non-soft-WDT) session. Audit vs the per-chunk
    resume counter at EEPROM[7] + the file enumerator.
13. **WDTPS readout** in `setup()` — confirmed host-safe (`session_start.py:286 reset_input_buffer()`
    before `?`; banner consumed by `read_all()` :173). No gating.
14. **Synthetic-SPIROV self-test in the FORK** behind `#ifdef SD_DEBUG_FAULT_INJECT`: `extern
    volatile uint8_t sdInjectStuckOnce` **defined in `Sd2Card.cpp`**, set by an 8-byte debug-token
    matcher in the sketch. When set, the next bulk write **genuinely overruns** (writes ≥9 bytes
    without reading) to force a real SPIROV; logs `SPI1STAT/SPI1CON` pre/post; test early/mid/late
    block. Built via `--build-property …extra_flags=-DSD_DEBUG_FAULT_INJECT`; verify symbol in
    `.map`. Compiled OUT of production.
15. **Sample-loss in-band**: existing `sdErrs`/`sdRetries`/`%CKPT` account for dropped bursts;
    SPIROV recovery increments them + emits the **deferred** `%E` discontinuity marker; confirm the
    parser tolerates it (Codex R2#16).
16. **Normal (non-hang) nights stay byte-identical**: FIFO-safe primitives write identical SD bytes
    on the happy path; the non-SPIROV failure path is unchanged; soft-WDT OFF; self-test compiled
    out; only always-on addition is the host-safe `WDTPS=` boot line.

## Plan
Branch `grill/cyton-sd-recover` (tip `38b3896`).

**Layer 1 — bounded, FIFO-safe SPI primitives + module flush (fork `Sd2Card.cpp/.h`)**
1. File scope: `volatile uint8_t sdSpiFault=0;`, `volatile uint8_t sdRecoverEvents=0;`,
   `#ifdef SD_DEBUG_FAULT_INJECT volatile uint8_t sdInjectStuckOnce=0; #endif`. Confirm
   `_SPI1CON_ON_MASK`/`_SPI1STAT_SPIROV_MASK`/`_SPI1STAT_SPITBF`/`_SPI1STAT_SPIRBE`/ENHBUF/MODE bits
   in `…/proc/p32mx250f128b.h`; `#if !defined(_SPI1STAT_SPIROV_MASK) #error`. CP0 via `<cp0defs.h>`,
   `#define SD_CP0_PER_US 20u`, `cp0Past(t0,us)` helper (unsigned).
2. `sdSpiModuleFlush()` per Decision 3 — atomic split: `SPI1CONCLR=ON` → readback →
   `SPI1CONCLR=(ENHBUF|MODE16|MODE32)` → `SPI1STATCLR=SPIROV` → `SPI1CONSET=ON` → readback. Mode/BRG
   never touched (preserved). **CLEARS `sdSpiFault`.**
3. `spiByteBounded(out)` (Decision 5) → rewrite `spiRec`/`spiSend` to call it; keep non-`_spi`
   soft-SPI branches.
4. `spiBlockBounded(src,n)` (Decision 4) → replace `writeData`'s `_spi->transfer(512,…)`; on bail
   set `sdSpiFault`, return false.
5. Fault short-circuit in `cardCommand`/`waitNotBusy`/`waitStartBlock`/`writeData`/`writeBlock`/
   `readBlock`: check `sdSpiFault` after each `spiRec`/`spiSend` (not the byte) → fail (deselect,
   one path). Do NOT clear `sdSpiFault` here.
6. Update `patches/sd-fork-write-timeout.patch` (edits + ENHBUF/SPIROV/flush rationale).

**Layer 1b — SPIROV recovery wiring (sketch `SD_Card_Stuff.ino`)**
7. In `writeCache()` where `card.writeData(pCache)` fails, branch on the fault:
   `if (sdSpiFault) { sdSpiModuleFlush(); /* clears flag+hw */ sdBusRecover(); /* live CMD25 abort */
   bool ok = card.init(...) && card.writeStart(bgnBlock+blockCounter,…) && card.writeData(pCache);`
   - **on success** → `blockCounter++; byteCounter=0;` set the deferred `%E` flag → **return** (normal
     resume; do NOT fall through — GLM R5#3).
   - **on failure** → `sdSpiModuleFlush(); sdBusRecover();` (the retry's `writeStart` may have
     succeeded, leaving the card mid-CMD25 — the 520-flush+STOP_TRAN knocks it out; a bare tail
     `writeStop` would send one 0xFD = a data byte → desync; Gemini R6#2) then jump **directly to
     the skip-forward / extended-window / `sdCardDead`+footer+`REPLAYFL` tail**, **bypassing the
     tail's redundant same-block retry** (Gemini R5#4; never silent-exit, Gemini R4#2).
   `}` `else` → existing bare boundary cascade (non-SPIROV; byte-identical). Increment
   `sdRecoverEvents`; emit the deferred `%E` AFTER the critical section. Tail-entry invariants to
   document: `blockCounter`/`bgnBlock` unchanged, `pCache` intact, CMD25 aborted, bus clean.
   `extern` `sdSpiModuleFlush`/`sdSpiFault`/`spiByteBounded` from the fork.
8. `sdBusRecover()` (`:751`): `sdSpiModuleFlush()` at entry; route its pokes through the `extern`'d
   `spiByteBounded`; add `if (sdSpiFault) break;` inside the 520-byte dummy loop and the BUSY-wait
   loops so a still-wedged bus bails fast (Gemini R4#4).

**Layer 1c — fault-keyed ADS guard (sketch `DefaultBoard.ino`)**
9b. Top of `loop()`: `if (sdSpiFault) sdSpiModuleFlush();` (Decision 10 — mode-safe via masked-CON;
    catches terminal/non-`writeCache` paths before the next ADS read). `extern` the fork symbols.

**Layer 2 — safe soft-WDT, OFF by default (`SD_Card_Stuff.ino` + `DefaultBoard.ino`)**
10. `#define TUNE_KEY_SOFT_WDT_ENABLE 0x06`; `boolean tuneSoftWdtEnable=false`;
    `tuneKeyValueLength(0x06)=1`; `applyTune(0x06,v)→tuneSoftWdtEnable=(v!=0)`+`TUNE OK`. The host
    MAY include it in the session's `SESSION.TXT` `%TUNE` line (session-scoped; the `%TUNE` replay at
    boot re-applies it on a within-night resume); a fresh night omits it → OFF.
11. Per-night cap EEPROM byte + magic+complement sticky reset-pending record (audited addrs). On
    fire: read cap → if ≥ cap, clean stop (no reset); else increment + write back, set the sticky
    flag, `executeSoftReset`. In `setupSDcard()` consume-and-clear the flag, skip the cap reset on a
    soft-WDT reboot, reset cap to 0 only on a fresh non-replay session.
12. Re-enable `DefaultBoard.ino:~219-231` guarded by `tuneSoftWdtEnable`: stale `%CKPT` past floor +
    cap not hit → **`sdSpiModuleFlush()` then `sdBusRecover()` FIRST** (clear the latch+bus, then
    abort a possible mid-CMD25 so the footer can land; Gemini R5#2/R8#2) → best-effort bounded clean
    close (`writeFooter`→`closeSDfile`) → `executeSoftReset(0)` **always** (mark suspect via
    `REPLAYFL` if the close failed; Gemini R4#3). Inert when flag 0.

**Layer 3 / 4 / 5**
13. WDTPS readout in `setup()` after RCON capture.
14. Fork `#ifdef SD_DEBUG_FAULT_INJECT` real-overrun injection (≥9 bytes no read) + sketch token
    matcher; `SPI1STAT/CON` pre/post log.
15. Verify `38b3896` `"Size "` intact; build (prod + debug); **record per-component size + total <
    122880**. **Production trim levers if over budget** (GLM R4#4), in order: self-test is already
    `#ifdef`'d out; shorten verbose `%BOOT`/`%CKPT`/console diagnostic strings; collapse duplicated
    comments (no flash cost but keep code tight); last resort — reduce the soft-WDT suspect-marking
    elaboration to the minimal guarded reset. Host-contract strings are NEVER a trim lever.

## Files to touch
- `~/Arduino/libraries/OpenBCI_32bit_SD/utility/Sd2Card.cpp` + `.h` — bounded primitives,
  `sdSpiModuleFlush`, sticky `sdSpiFault`, fork-side self-test, recover counter.
- `OpenBCI_Cyton_Library_SD/patches/sd-fork-write-timeout.patch` — record fork edits.
- `examples/DefaultBoard/SD_Card_Stuff.ino` — SPIROV recovery branch (live `sdBusRecover`);
  `sdBusRecover` flush + bounded pokes; tune key 0x06 (session-scoped via SESSION.TXT %TUNE);
  per-night cap (read+increment+writeback) + sticky
  magic+complement flag; deferred `%E`; self-test token matcher.
- `examples/DefaultBoard/DefaultBoard.ino` — guarded soft-WDT (best-effort close then always
  reset); WDTPS readout; **top-of-loop `if (sdSpiFault) sdSpiModuleFlush();` guard** (Decision 10).
- `CLAUDE.md` — document ENH_BUFFER/SPIROV root cause + bounded-FIFO + module-flush + live
  `sdBusRecover` recovery + tune key 0x06; reinforce host-contract no-trim.
- `build-grill/DefaultBoard.ino.hex` — rebuilt artifact (NOT flashed until user OK).
- *(NOT touched: `OpenBCI_32bit_Library.cpp` — ADS guard moved to the sketch to avoid fork coupling.)*

## Test plan
1. **Build green & under budget** — per-component size + total < 122880 (prod + debug builds).
2. **Codex + Gemini + GLM panel** (read-only, deep). No flash until all three approve AND user OKs.
3. **Bench (only after explicit user OK; pic32prog un-timeout'd)**:
   - Power-cycle; boot prints incl. `WDTPS=…`; `session_start.py -a 18` (5 min) starts (proves
     `38b3896` `"Size "`/%META) → clean slot **with footer**.
   - **`-DSD_DEBUG_FAULT_INJECT` build**: fire the real-overrun token early/mid/late block → observe
     real SPIROV → `spiBlockBounded` bail (`sdSpiFault`) → `sdSpiModuleFlush` + live `sdBusRecover` →
     same-block retry → recording **continues** (deferred `%E` logged), OR clean `sdCardDead` +
     `REPLAYFL` — never a freeze. Inspect logged `SPI1STAT/CON` pre/post.
   - **Throughput**: `-a 22` (1000 Hz/8-ch) holds `B=4,864,000`, no sample loss (validates the
     in-flight cap + CP0 budget + that a normal night never trips recovery).
   - Re-flash the **production** (non-debug) hex for the overnight run.
4. **Final verification = the user's overnight `-a 20` sleep test** (12 h/500 Hz/ECG sleep). Success
   = one clean slot + footer, OR a logged in-place recovery (`%E`, bounded sample-loss) with no
   silent freeze.

## Risks & open questions
- **`sdSpiModuleFlush` correctness** — uses atomic CLR/SET, never touching mode/BRG, so the active
  SPI mode (SD MODE0 or ADS MODE1) is preserved automatically; verify ENHBUF/MODE16-32/SPIROV all
  cleared via the self-test's pre/post register dump; the ON-clear→readback→ENHBUF-clear split is
  required (ENHBUF writable only when ON=0); preserve CS.
- **In-flight-cap correctness** — keep `written-read ≤ headroom` AND drain eagerly so SPIROV can't
  latch; verify with self-test + `-a 22`.
- **Live `sdBusRecover` re-entrancy** — it's `static` in the TU and called from `writeCache`
  recovery (CS-low/high self-contained); confirm calling it mid-`writeCache` doesn't double-manage
  CS or recurse.
- **CP0 budget sizing** — generous infinite-spin breaker; verify no false trips at 1000 Hz (`-a 22`).
- **EEPROM map** — audit before assigning the per-night cap byte + sticky magic+complement record;
  minimize writes (wear) — only on actual reset.
- **Soft-WDT clean-close partial failure** — mark-suspect + `sdCardDead`; confirm fully bounded.
- **Flash overflow** at 96 % fragmented-full — per-component estimate before flashing; trims ready.
- **Pre-existing (out of scope)**: the boundary cascade's `writeStop` runs with CS high (Gemini
  R3#1) — a latent issue in the EXISTING non-SPIROV path; THIS bug (SPIROV) uses `sdBusRecover`'s
  own CS, so it's handled; not touched to keep non-SPIROV behaviour byte-identical.
- **Cannot reproduce the real multi-hour event** — the self-test forces a real SPIROV; final
  confidence is the overnight test.
- **Implementation MINORs (panel R7, fold in at /grill)**: (a) name the per-night EEPROM counter
  distinctly from its ceiling — `wdtRebootCount` vs `#define MAX_SOFT_WDT_REBOOTS` (Gemini); and give
  the counter byte the **same magic+complement torn-write protection** as the sticky flag (GLM); (b)
  write each register readback as a forced `volatile` read — `(void)SPI1CON;` — so the compiler can't
  optimize away the settle (Gemini); (c) the post-failure tail's `writeStop()` issues a second
  `STOP_TRAN` after `sdBusRecover()` already sent one — harmless (card ignores it in the `tran`
  state); add a comment so it doesn't read as a bug (Gemini).

## Out of scope
- Editing toolchain `DSPI.cpp` (user decision; shared with ADS path).
- A real hardware-WDT watchdog (infeasible: bootloader-locked ~1 s `WDTPS`, no ICSP).
- Changing `SD_WRITE_TIMEOUT` (unrelated; leave 1500 ms).
- Redesigning the existing same-block/skip-forward/extended-window cascade or the boundary-failure
  `writeStop` CS behaviour (leveraged/left as-is).
- Flashing / starting any recording without explicit user OK.
