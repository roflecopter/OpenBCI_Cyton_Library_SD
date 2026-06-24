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
  kept). Trim more from there or from `!board.streaming`-gated diagnostic strings if needed.
- **Flash** (bootloader/OTA, no ICSP readback — bootloader is protected, not brickable, but
  a partial write needs a re-flash): connect the dongle, then as root
  `/home/lst/.arduino15/packages/chipKIT/tools/pic32prog/v2.1.46/pic32prog -d /dev/ttyUSB0 -b 115200 <hex>`.
  **NEVER wrap pic32prog in a kill-timeout** — cutting it mid-write is how Cyton A got bricked.
  Let it run to `Verify flash … done`. Build artifacts go to `build-grill/` (gitignore-worthy).
- **git: github `roflecopter` remote, but z13's key isn't on github** → `git push` fails
  (publickey). Commit locally; rsync to p14s to push (same as paperlike/zmax). git.kto.to works.

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

## Verified behaviour (2026-06-24, on hardware)
Stray `F`/`j`/`s`/`1` ignored mid-stream (`B` held); auto-resume after power-cycle (×3, clean);
`session_start.py --stop` round-trip; 45-min endurance run cleared block 101,340 with `B=2432000`
and **zero SD errors** (`%CKPT e=0 r=0 n=0`); 1000 Hz × 8-ch sustained (write 326–332 µs, 0 overruns).
