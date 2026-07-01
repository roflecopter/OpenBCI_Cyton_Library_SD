# OpenBCI Cyton freeSD firmware (z13 fork)

SD-recording sleep-EEG firmware for the OpenBCI **Cyton** (PIC32MX250F128B, 32 KB RAM).
Sketch: `examples/DefaultBoard/{DefaultBoard.ino, SD_Card_Stuff.ino}`. The board records
12 h sleep PSG to the SD card alone (no RF stream at ≥500 Hz). Host driver = the
**`openbci-session`** repo (`session_start.py`). Board hardware facts + nightly workflow
live in the memory note `project-openbci-cyton-b-working-board`.

## Build & flash (z13)
- **Build as `lst`** (arduino-cli at `/home/lst/bin/arduino-cli`; needs the lst env):
  `sudo -u lst -i bash -lc 'arduino-cli compile --fqbn chipKIT:pic32:openbci <abs-path>/examples/DefaultBoard'`
  FQBN is **`chipKIT:pic32:openbci`** (NOT `HelvePic` — that overflows the link).
- **⚠ Flash is ~96 % full** (~118.5 KB / 122.88 KB). Any new code must be size-checked; the
  "Sketch uses N bytes" line is a hard gate. The dead WiFi path (`wifi.loop()` + wifi RX block)
  was dropped to make room — gc-sections reclaims the unreferenced WiFi methods (`wifi.begin`
  kept). Trim more from there or from `!board.streaming`-gated diagnostic strings if needed —
  **BUT FIRST read the "⛔ HOST-CONTRACT STRINGS" section below.** Several `Serial0.print` lines
  look like diagnostics but are PARSED BY THE HOST and must NEVER be trimmed (this exact mistake
  broke every recording start on 2026-06-28).
- **Flash** (bootloader/OTA, no ICSP readback — bootloader is protected, not brickable, but
  a partial write needs a re-flash): connect the dongle, then as root
  `/home/lst/.arduino15/packages/chipKIT/tools/pic32prog/v2.1.46/pic32prog -d /dev/ttyUSB0 -b 115200 <hex>`.
  **NEVER wrap pic32prog in a kill-timeout** — cutting it mid-write is how Cyton A got bricked.
  Let it run to `Verify flash … done`. Build artifacts go to `build-grill/` (gitignore-worthy).
- **git: github `roflecopter` remote, but z13's key isn't on github** → `git push` fails
  (publickey). Commit locally; rsync to p14s to push (same as paperlike/zmax). git.kto.to works.

## ⛔ HOST-CONTRACT STRINGS — NEVER TRIM (these are NOT diagnostics)

**`%META` (self-describing recordings) is one of this repo's core MUST features. The host
handshake that writes it is carried over plain `Serial0.print` lines — so those lines are a
CONTRACT with `openbci-session/session_start.py`, not debug chatter. NEVER remove or reword them
to save flash.** `session_start.py` drives the board entirely by *parsing these exact strings*; if
one goes missing the start silently fails (or %META/persist/tune silently don't verify) even though
the board is fine. Each line below maps to a `re.findall`/`re.search` in the host — keep them
byte-exact (the space after `Size`, the field order, the `OK`/`FAIL` tokens):

| Firmware emits (`SD_Card_Stuff.ino`, `!board.streaming`-gated) | Host parses (`session_start.py`) | If trimmed |
|---|---|---|
| `Size <BLOCK_COUNT> SD file OBCI_NN.TXT` | `re.findall('Size ', res)` + `r'I\_.*\.T'` (filename) | **every start → "SD init failed"** (the 2026-06-28 regression) |
| `META OK <len> <sum>` / `META FAIL` / `META ERR` | `parse_meta_ack` → `meta_status` | %META silently unverified → recordings lose channel labels |
| `PERSIST OK <total> <sum>` / `PERSIST FAIL` | `re.search(r'PERSIST OK (\d+) (\d+)')` | SESSION.TXT (auto-resume) silently not confirmed |
| `TUNE OK <id>` / `TUNE FAIL <code>` | `re.search(r'TUNE OK <id>\b')` | runtime-tunable overrides silently dropped |

Library-level (in `OpenBCI_32bit_Library`, NOT this sketch — don't touch): `Sample rate is <N>Hz`
(host line 350), the `?` register dump (`ADS`/`Registers`/`CONFIG`, `assert_board_idle`), and the
`$$$` EOT (`board.sendEOT`). **Rule: before trimming ANY `Serial0.print`, grep
`openbci-session/session_start.py` (and `collect_*.py`) for the literal — if the host matches it,
it stays.** Strings that are genuinely safe to trim are the *failure-detail* ones the host does NOT
parse (`"erase block fail"`, `"writeStart fail"`, `"initialization failed…"`, the closeSDfile
write-time/overrun stats) — the host infers failure from the *absence* of the success string, so
losing the failure text only costs human readability, never the handshake. The restored `Size`
print carries a loud `⚠ HOST CONTRACT — DO NOT trim` comment in-code; add the same to any line here.

## Stray-RX hardening (2026-06-24) — flashed + hardware-verified
Root cause of the repeated mid-night truncations/erased nights: the firmware dispatched
inbound radio command bytes to the command handlers **even mid-recording**, so a stray byte
(dongle unplugged → unpaired-radio noise) could rewrite `BLOCK_COUNT` (slot char → 30-min
truncation; OBCI_4E footered at block 101,000 = the exact `F`/30-min value), close the file
(`j`), or re-enter `setupSDcard`. Fix (branch `grill/cyton-rx-hardening`, commit `3b91a89`):
- **`dispatchCommandByte()` RX policy** (DefaultBoard.ino `loop()`): RECORDING
  (`streaming && SDfileOpen`) drops ALL command bytes; HANDSHAKE (`SDfileOpen && !streaming`)
  allows only `b`-after-`metaArmed`; IDLE = normal. Called from every ingress.
- **Caller-side slot-char guard** in `sdProcessChar` (`if (board.streaming || SDfileOpen) break;`)
  — fail-loud, no silent destructive merge.
- **`metaArmed` gate** → SD recording now **REQUIRES `%META` before `b`** (blocks a premature/stray
  `b`). The stock OpenBCI GUI's no-`%META` SD start no longer works — accepted (z13 uses session_start).
  `P`/`T` gated IDLE-only; `M` gated one-shot.
- **Hardened escape token** `DE AD BE EF 01 02 03 04` (8 non-printable bytes) is the **ONLY** in-band
  stop: caught by `feedEscape()` on every raw byte (before the command policy, in all states incl. the
  `writeCache` recovery drain) → sets `abortRequested` → `loop()` runs `performAbort()` (streamStop +
  closeSDfile + removes SESSION.TXT + unconditional `$$$`). **`s`/`j` no longer stop a recording.**
  The deferred flag (never closing inside `writeCache`) avoids CMD12/FAT mid-CMD25 re-entrancy.
- **`%CKPT B=<BLOCK_COUNT>`** in-band canary (watch it stays constant to catch any future clobber).
- `writeCache` `!resumed`/abort early-returns reset `byteCounter=0` (pCache overflow safety).
- **Pending, NOT yet flashed** (commit `6bf4da0`): cosmetic `resume=` mislabel fix (gate the
  fresh-session reset on `replayingSession`, not the boot-level `autoResume`). Bundle with next flash.

Full audit trail in this repo: **`prep.md`** (9-round plan), **`prep-review-log.md`**,
**`grill-review-log.md`** (implementation + 3-round Codex+Gemini review + the hardware test matrix).
Don't re-litigate those decisions; read them first.

## SD bus-recovery + robust auto-resume (2026-06-28) — built, NOT yet flashed/verified
Branch `grill/cyton-sd-recover` (off `grill/cyton-rx-hardening`). Fixes the **silent 0-byte
next-slot** failure: on the failing nights an **intermittent external power glitch** (leading
candidate: the JST battery connector / a movement-dependent contact — the user is hardwiring/reseating
it, which is the real *prevention*) resets the MCU **mid-CMD25**, leaving the card stuck holding the
bus. The boot-level auto-resume (`replaySessionFile`) then can't `card.init()` the wedged card → opens
the next slot but never writes it → 0-byte file, and `REPLAYFL.TXT` never lands either. A full
power-cycle de-powers the card so cold init works — which is why resume "worked before." This change
is the **SALVAGE path** (recover the night across chained slots; `collect_bci.build_session_chains`
stitches `OBCI_5A`+`OBCI_5B`), not prevention. Key facts that shaped it:
- **OBCI32_SD talks to the card over the HARDWARE DSPI** (`Sd2Card card(&board.spi, SD_SS)` →
  `_spi->transfer()`), NOT software bit-bang — the Sd2PinMap `IOPORT_G` bit-bang branch is dead code.
  So recovery uses the public DSPI API (`board.csLow/csHigh(SD_SS)`, `board.spi.transfer()`), no
  PPS/LAT/TRIS poking. `board.begin()` fully inits the DSPI **before** `replaySessionFile()` runs.
- **`sdBusRecover()`** (new): hardware-DSPI CMD25 abort — `csLow` → drain 520× `transfer(0xFF)`
  (512 data + CRC + buffer) → bounded busy-wait MISO-high → `transfer(0xFD)` stop-tran → bounded
  busy-wait → `csHigh` + 16 trailing clocks. All waits bounded by `millis()` deltas
  (`SD_RECOVER_BUSY_MS=2000`), rollover-safe. **CMD12 deliberately dropped** — this firmware never
  issues CMD18 (multi-block READ), so the card is never in a read-multiple state; saves flash.
- **`replaySessionFile()`**: init-first → on failure `sdBusRecover()` → retry `card.init`+`volume.init`
  (was unconditional recovery). Plus a resume-only `delay(SD_RESUME_STABILIZE_MS=2500)` before the
  feed loop to let a just-power-glitched card settle. Non-resume path untouched.
- **`writeReplayFail()`**: init-first/recover/retry + **non-destructive self-healing REPLAYFL** —
  trusts an existing marker only if it parses (`code=<digit>…\n`), else removes + recreates with
  `O_TRUNC`; snprintf clamped to `sizeof-1`.
- **`setupSDcard()`**: fail-fast on `card.init` failure (no fall-through); a **remove-guard** before
  `openfile.remove` skips deleting a *non-zero* prior slot (only reuses a genuinely 0-byte/free name),
  bounded by a **numeric `attempts < 256`** scan (decoupled from the filename cycle — soft-WDT is OFF,
  so an unbounded scan would brick the night); all 4 allocation failures route through new
  **`setupSDfail()`** which removes the orphaned full-size file.
- **`DefaultBoard.ino`**: park `SD_SS` HIGH as a raw GPIO **before** `board.begin()`
  (latch-before-TRIS) so ADS init clocks can't reach a stuck card.
- **Safety invariants held**: `executeSoftReset(0)` + soft-WDT self-reset stay **DISABLED** (their
  rapid-fire loop was the night-eraser, rollback `6f6efe8` — do NOT re-enable); resume allocates a
  **fresh contiguous slot**, never touches the prior slot's FAT/clusters; **normal no-reset night is
  byte-for-byte identical** (recovery + stabilize delay both gated behind a failed `card.init`).
- **Resume-diagnostics breadcrumb (same flash, +2 review rounds):** the resumed slot's `%BOOT` now
  carries `rcv=<0-2>` (0 = no bus-recovery needed / 1 = `sdBusRecover` ran AND the retry init succeeded
  = the fix salvaged the resume / 2 = ran but failed, ~never reaches %BOOT) and `g=0x<HEX16>` = low-16
  bits of `millis()` at slot creation = boot→resume latency = a **lower bound** of the inter-slot
  recording gap (no RTC, so the unpowered/in-reset duration is unmeasurable; for a brief glitch
  off-time≈0 so this IS the gap estimate the host stitcher inserts). **Both gated behind `if
  (autoResume)`** → a normal no-reset night's `%BOOT` is byte-for-byte unchanged. To fit it on the
  96%-full flash, the closeSDfile `!board.streaming` console write-time/overrun stats were trimmed
  (kept `board.sendEOT()` — the `$$$` the host waits on; `%CKPT` still reports e/r/n live). ⚠ Gemini
  caught a real MAJOR: `EMIT_HEX16` is a macro that evaluates its arg 4× (once per nibble), so passing
  `millis()` directly tore the hex — fixed by snapshotting to a local `gapMs` first.
- **⚠ REGRESSION caught on hardware 2026-06-28 (now fixed):** the recovery-fix flash trim removed
  setupSDcard's `Size <N> SD file OBCI_NN.TXT` success print — which `session_start.py` PARSES to
  confirm the slot opened. Result: the board opened the slot fine but the host reported "SD init
  failed" and exited before `b`, so a fresh start never recorded (the night was salvaged only because
  a power-cycle made the firmware AUTO-RESUME from SESSION.TXT, which needs no host). Fixed by
  restoring the exact print with a loud `⚠ HOST CONTRACT — DO NOT trim` in-code comment; see the new
  "⛔ HOST-CONTRACT STRINGS" section above. Audit confirmed `Size` was the ONLY host string trimmed
  (META/PERSIST/TUNE all intact). NOT yet flashed (recording was live) — goes out next flash.
- Build **118600 B (96%)** (Size print restored; still below the original 118716), RAM 11852 B (36%).
  Audit: `prep.md` (delay-only scope, 7-round plan panel) + `prep-review-log.md` + `grill-review-log.md`
  (1 architecture consult + **5 review rounds for the recovery fix, both APPROVED at R5; +2 rounds for
  the breadcrumb, both APPROVED at R2** — all Codex+Gemini). ⚠ **NOT flashed** — final verification is
  the user's flash + an overnight sleep test (a mid-night external reset cannot be reproduced on the
  bench). Flash with the normal `pic32prog` invocation above (NEVER a kill-timeout).

## SPIROV mid-write FREEZE fix (2026-06-29) — built + Codex+Gemini APPROVED, NOT yet flashed
Branch `grill/cyton-sd-recover`. Cures the **intermittent overnight FREEZE** (recording halts
mid-night, board dead until power-cycle; the file ends mid-sample with CLEAN normal-range EEG — NOT
power loss). **Root cause (register-level):** the 512-byte SD block write uses DSPI's **ENH_BUFFER**
(8-byte RX FIFO) bulk transfer whose `while(toWrite||toRead)` drain loop is **UNBOUNDED**. A DRDY-ISR
delay mid-loop overruns the RX FIFO → `SPI1STAT.SPIROV` latches → received bytes are lost → `toRead`
never reaches 0 → **infinite spin inside `card.writeData()`**. This is a *different* failure from the
2026-06-28 0-byte-next-slot (that was an external mid-CMD25 reset; the salvage path above handles it).

**The fix — bounded, FIFO-safe SPI in the OBCI32_SD fork (out-of-repo `~/Arduino/libraries/
OpenBCI_32bit_SD/utility/Sd2Card.cpp`; archived in this repo as `patches/sd2card-spirov-fix.cpp` +
`patches/sd2card-spirov-fix.patch`):**
- `spiByteBounded(out)` — single-byte (standard mode, ENHBUF=0) transfer, CP0-deadline bounded; on
  deadline latches sticky `sdSpiFault` + returns 0xFF. `spiRec`/`spiSend` route through it.
- `spiBlockBounded(src,n)` — replaces writeData's `_spi->transfer(512,src)`. DSPI ENH_BUFFER bulk loop
  but **caps in-flight bytes** (`(toRead-toWrite) < sdFifoHeadroom`, default 7, on the 8-deep RX FIFO)
  with eager draining so the FIFO can't overrun under ISR jitter, PLUS a `SPI1STAT.SPIROV` check + a
  no-progress CP0 deadline as the infinite-spin backstop. On bail: latch `sdSpiFault`, return false.
- `sdSpiModuleFlush()` — atomic SPI1 flush: ON→0 (readback) → clear ENHBUF/MODE16/MODE32 (only legal
  while ON==0) → clear SPIROV → ON→1 (readback). Preserves mode/CKE/CKP/MSTEN/BRG; clears `sdSpiFault`.
- Sticky `sdSpiFault` latch + fault short-circuits in cardCommand / waitNotBusy / waitStartBlock /
  readData (bounded per-byte) / readRegister / writeData.
- **writeCache SPIROV recovery branch** (`SD_Card_Stuff.ino`): on a faulted `writeData`, `sdBusRecover()`
  (made live-safe — flushes first, bounded pokes) does the live CMD25 abort → `card.init` → `writeStart`
  → same-block retry of the preserved `pCache`. Non-SPIROV failures take the existing bare retry
  (byte-identical). A unified `if(!ok && sdSpiFault) sdBusRecover()` guards the skip-forward tail.
- **Layer 1c** (`DefaultBoard.ino`): a top-of-loop `if (sdSpiFault) sdSpiModuleFlush();` ADS-path guard.

**⚠ THE REAL FLASH CEILING IS 118784 B, NOT 122880.** arduino-cli reports "122880 max", but the linker
script `chipKIT-application-32MX250F128.ld` defines `kseg0_program_mem = 0x1D000 = 118784 B` — a 4 KB
DEE-EEPROM page (0x1E000) + a 4 KB splitflash page (0x1F000) are carved off the top. So the `38b3896`
baseline (118600 B) had only **~184 B free**, not the ~4280 the prep assumed. At 96% **fragmented**-full
the chipKIT `-ffunction-sections` allocator can't grow any function's section past its gap → even a
~10 B add fails to link.

**SCOPE CUT (user decision — flash budget):** shipped the **SPIROV fix ONLY**. **DROPPED** the entire
`prep-diag.md` diagnostics layer (`%CKPT s=`, the `ps`/`pf` EEPROM freeze-breadcrumb, `%BOOT` decode,
tune keys 0x07–0x0A, the synthetic-SPIROV self-test), **`prep.md` Layer 2 soft-WDT**, and **Layer 3
WDTPS readout** — none fit, and none is the diagnosed cause. EEPROM map UNCHANGED from `38b3896`.

**HOST-CONTRACT-SAFE flash reclamation (verified, NOT assumed):** trimmed `writeFooter()` to emit ONLY
the `\n%Total time mS:\n` clean-stop marker. **Codex round-2 correctly caught** that the old footer
wrote `%SamplingFreq` *before* `%Total time`, so "host breaks at %Total time" didn't cover it — traced
BOTH host parsers (`py-qs-data/openbci_functions.py` = the live collect-bci ingest; `openbci-session/
sd_convert.py`): the ingest loop **breaks at a line starting `%Total time`**, so every other dropped
field (min/max/over/err/retry/reinit/extRetry/block, all *after* the break) was never parsed; and **no
host code reads the `%SamplingFreq` token** (its old before-break line was only mis-ingested as a benign
phantom EOF "stop"). EEG samples are byte-identical. The `%CKPT t/b/e/r/n/o/x` parser is
order-independent key=value — `o=` (`overruns`) is KEPT (sd_health reads it).

**Build 118360 B (96%, ~424 B free under the 118784 ceiling), RAM 11552 B (35%).** Normal no-fault
recordings are byte-identical except the trimmed footer (host never parsed it). Audit:
`grill-review-log.md` "Grill #2" — 4 grill-time decisions + **3 Codex+Gemini review rounds, both
APPROVED at R3**. Notable catches: R1 unbounded readData/readRegister/waitNotBusy fault gaps (fixed);
R2 the `%SamplingFreq` justification gap (resolved by host-parser proof) + a redundant-flush MINOR
(fixed) + a Gemini BLOCKER that was a diff-generation artifact (the round-1 fixes were present; the
reviewed baseline diff was incomplete — regenerated). ⚠ **NOT flashed** — final verification is the
user's flash + overnight `-a 20` sleep test + an `-a 22` 1000 Hz throughput test (the multi-hour freeze
is only reproducible on a real overnight run). Flash with the normal `pic32prog` invocation above
(**NEVER a kill-timeout** — that bricked Cyton A; let it run to "Verify flash … done").

## Verified behaviour (2026-06-24, on hardware)
Stray `F`/`j`/`s`/`1` ignored mid-stream (`B` held); auto-resume after power-cycle (×3, clean);
`session_start.py --stop` round-trip; 45-min endurance run cleared block 101,340 with `B=2432000`
and **zero SD errors** (`%CKPT e=0 r=0 n=0`); 1000 Hz × 8-ch sustained (write 326–332 µs, 0 overruns).

## Mid-recording HANG re-diagnosis + universal SPI bounding (Stage A, 2026-06-30) — built + Codex+Gemini APPROVED, NOT yet flashed
Branch `grill/cyton-hang-recovery` (off `grill/cyton-sd-recover` HEAD `3e93cde`). **The SPIROV fix
(`3e93cde`) was flashed 2026-06-29 and STILL FROZE at ~7.1h** (slot OBCI_61, mid-block halt, **e=0 r=0
across all 425 %CKPT** — identical signature to the pre-fix OBCI_5A). Proof the SPIROV diagnosis was the
WRONG bug: the bounded writeData's recovery increments e/r and they stayed 0, and bounding the SD write
changed nothing. **Re-diagnosis = an unobserved mid-recording MCU HANG; prime suspect = the ADS1299
per-sample read, which went through the chipKIT `DSPI::transfer()` whose internals are UNBOUNDED**
(`while(!SPITBE)/while(!SPIRBF)`, no timeout). Forensics: `obci_61_freeze_forensics.md`. Full plan:
`prep.md` (8-round dual-signed-off) + `prep-review-log.md`.

**Stage A (this build) — what changed:**
- **Universal ADS+accel SPI bounding** (`OpenBCI_32bit_Library.cpp`, archived `patches/ads-accel-spi-bounding.cpp`):
  `xfer()` (the chokepoint for ALL ADS reads/writes/commands) + the 3 LIS3DH accel helpers now route
  through the OBCI32_SD fork's bounded `spiByteBounded` instead of the unbounded `DSPI::transfer()`. On a
  CP0-deadline stall it latches the shared sticky `sdSpiFault` + returns 0xFF; the EXISTING top-of-loop
  `if (sdSpiFault) sdSpiModuleFlush()` guard recovers the bus next iteration → a faulted sample reads 0xFF
  (one garbage sample per rare wedge) instead of a frozen board. **If the freeze IS the ADS DSPI unbounded
  wait (the diagnosis), this fix alone makes it recover.** Safe because `spiByteBounded`'s entry
  short-circuit (returns 0xFF without touching SPI1STAT when `sdSpiFault` is set) means an ENHBUF-set bus
  is never mis-read. **NET FLASH −212 B** (dedup — shared free function replaces 8 inlined DSPI calls).
- **DEVCFG1 readout in every %BOOT** (`SD_Card_Stuff.ino`): ` wd=0x<DEVCFG1>` (raw 32-bit @0xBFC00BF8).
  The hardware-WDT (Phase 1) has a HARD pre-flash gate on FWDTEN (bit 23, must be 0) + WDTPS (bits 20:16)
  which **can't be read with the board offline** — so surface it in %BOOT; the next flash's card reveals
  WDTPS and makes the Stage-B WDT decision data-driven. Decode formula is in-code beside the readout.
  Parser-safe (host ignores unknown trailing %BOOT tokens).
- **Build 118452 B (96%, 332 B free under the REAL 118784 ceiling), RAM 11552 B.** Codex+Gemini both
  APPROVED at review round 2 (`grill-review-log.md`); both ENDORSED the staging. Round-1 BLOCKER fixed: a
  latent unspecified-eval-order byte-swap in `LIS3DH_read16` (sequenced into locals).

**DEFERRED to Stage B (NOT a value-judgment skip — a hard data dependency):** the hardware WDT (Phase 1) +
the persistent breadcrumb. The WDT's pre-flash gate (WDTPS) is unreadable until Stage A's `%BOOT` reveals
it, AND the persistent-RAM breadcrumb is INERT without the WDT (a Stage-A freeze hangs until a manual cold
power-cycle = RAM drained = nothing captured; the breadcrumb only records across a WARM reset, which only
the WDT produces). Stage B is unblocked once the user flashes Stage A and reports: (a) the `wd=0x` value,
and (b) whether the next overnight slot is CLEAN (Phase 0 fixed it → WDT may be unnecessary) or truncates
again (the hang is NOT the ADS path → WDT+breadcrumb needed to locate it).

**⚠ NOT flashed** — board was offline this session; final verification is the USER's flash + overnight
`-a 16` sleep soak (the multi-hour freeze only reproduces on a real overnight run). Flash with the normal
`pic32prog` invocation (**NEVER a kill-timeout** — that bricked Cyton A; let it run to "Verify flash … done").
Artifact: `build-grill/DefaultBoard.ino.hex`.

## Mid-recording HANG — Stage B: hardware WDT auto-recovery (2026-07-01) — built + Codex+Gemini APPROVED, NOT yet flashed
Branch `grill/cyton-hang-stage-b` (off Stage A). **Stage A's SPI bounding did NOT stop the freeze** —
the 2026-06-30 soak (`OBCI_62`) froze AGAIN at **4.9h, e=0 r=0**, same signature -> the hang is a
**non-SPI MCU lockup**. Stage A's `wd=` readout delivered the gate: **wd=0xFF6A0D5B -> FWDTEN=0**
(WDT off at reset, app can arm it, NO infinite-reset brick — on any reset WDTCON.ON reverts to FWDTEN=0)
**+ WDTPS=0x0A (~1.02s) + WINDIS=1**.

**What Stage B adds — a hardware watchdog that resets on the hang -> setup() -> the EXISTING
`replaySessionFile()` auto-resume salvages the night across slots (dies-at-4.9h becomes a full salvaged
night).** Design (WDT-primitive in the OBCI32_SD fork, archived `patches/wdt-stage-b.cpp`; driven by
`DefaultBoard.ino`):
- **Armed ONLY while recording** (arm on the stream-start transition, **disarm on stop** -> WDT hardware
  OFF at idle so a long idle command can't false-reset). `WDTCONCLR` at `setup()` start keeps it off
  through the whole setup/resume path. Gated on **FWDTEN==0 && WDTPS>=0x0A** (fail-closed: a board with a
  shorter postscale ships as no-WDT = Stage A behaviour, never risks a false-reset).
- **`petWDT()`** (top-of-loop + INSIDE `waitNotBusy`, so a legit 1.5s block write — > the ~1s WDTPS —
  keeps the WDT fed) pets ONLY while recording progress is recent: `(CP0 - wdtLastProg) < 4s`. `CP0` is a
  HARDWARE timer that ticks even under an interrupt lockup (which halts `millis()`), so the no-progress
  gate catches THAT too. `wdtLastProg` bumped on each ADS sample **and each completed block**
  (`waitNotBusy` success) so a multi-block flush can't false-trip. A genuine hang stops the bumps ->
  petWDT stops -> the WDT fires (~5s: 4s deadline + ~1s WDTPS). `resumeCount` cap (25, clears on first
  `%CKPT`) bounds any rapid loop; the real multi-hour hang salvages into 2-3 slots/night.
- **Breadcrumb DEFERRED** (` hp=<phase>` didn't fit the 96%-full fragmented flash). Free substitute: the
  EXISTING resumed-`%BOOT` **`rcv=`** field (rcv=1 => card was wedged => SD-write-path hang; rcv=0 =>
  non-SD hang) + **`resume=N`** counts the WDT recoveries. Removed Stage A's ` wd=` %BOOT readout (~304 B,
  job done) to make room.
- **Build 118664 B (96%, 120 B free), RAM 11572 B.** Codex+Gemini both APPROVED (3 review rounds; R1
  converged on a WDTPS-gate BLOCKER + a block-progress MAJOR, both fixed; R2 a disarm-at-idle MAJOR,
  fixed + simplified). Audit: `grill-review-log.md` "STAGE B".
- **⚠ NOT flashed** — user-gated. Verification = the user's overnight soak: SUCCESS = the night salvages
  across chained slots (resume=N counts recoveries) instead of dying at one 4.9h slot; a HEALTHY night must
  still return as ONE clean slot (no spurious WDT reset). Flash with the normal `pic32prog` (NEVER a
  kill-timeout). Artifact: `build-grill/DefaultBoard.ino.hex`.
