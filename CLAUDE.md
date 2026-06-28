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

## Verified behaviour (2026-06-24, on hardware)
Stray `F`/`j`/`s`/`1` ignored mid-stream (`B` held); auto-resume after power-cycle (×3, clean);
`session_start.py --stop` round-trip; 45-min endurance run cleared block 101,340 with `B=2432000`
and **zero SD errors** (`%CKPT e=0 r=0 n=0`); 1000 Hz × 8-ch sustained (write 326–332 µs, 0 overruns).
