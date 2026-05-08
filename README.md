# OpenBCI 32bit Library with SD improvements

This is a modified library for the OpenBCI 32bit Board firmware which adds additional SD card functionality.

Main purpose is to improve experience while using OpenBCI Cyton as a part of home based PSG sleep research. You can read more about my quantified self project and especially OpenBCI experience [here](https://blog.kto.to/hypnodyne-zmax-vs-openbci-eeg-psg) .

* short (50ms) flashing LED during writing, every 3 seconds. [Source](https://github.com/OpenBCI/OpenBCI_Cyton_Library/pull/80)
* increased sampling rate. [Source](https://github.com/OpenBCI/OpenBCI_Cyton_Library/pull/96)
* bugfixes. [Source](https://github.com/OpenBCI/OpenBCI_Cyton_Library/pull/93)
* it works for both Cyton and Cyton+Daisy
* i use it on a daily basis with 3 different Cyton/Cyton+Daisy and it work stable

## SD reliability and observability (2026-05)

Adds visibility, recovery, and self-describing metadata around SD writes. Motivated by the long-standing "1 in 50 silent empty recording" issue and by overnight slots that silently halted mid-night on aging cards.

### Per-block recovery on write failures

When `card.writeData()` fails on a block:
* Same-block retry via `writeStop` + `writeStart`.
* If that fails: skip-forward to the next block, retried up to 5x with 1 ms backoff.
* If skip-forward exhausts: one `card.init()` + `writeStart` recovery cycle (resets card-internal state, ~1-2 s gap on a degraded card, drops a corresponding burst of ADC samples but recording resumes).
* If even that fails: clean shutdown via a `sdCardDead` flag that immediately tears down the open-file flags so the main loop stops pumping bytes through a known-bad SPI bus. The previous fire-and-forget skip-forward could leave the multi-block context dead silently and produce an indefinite zombie halt; now the recording either resumes or stops cleanly.

### `ledSDError` fast strobe

When any SD write error fires, the on-board LED switches from the normal slow blink to a 100 ms / 300 ms fast strobe and stays in that mode for the rest of the session. Visible by morning if anything went wrong overnight.

### Inline `%E` markers and `%CKPT` heartbeat

The TXT recording is now self-describing for diagnostics. All markers are emitted on their own line (no fragmentation of CSV sample lines) at sample boundaries.

* `%E\n` — emitted on the line after each block-write error event. One per recovered block.
* `%CKPT t=<ms> b=<block> e=<errs> r=<retries> n=<reinits> o=<over>\n` — heartbeat line emitted approximately once per minute. Fields:
  * `t` — `millis()` since Cyton power-on at the moment of emission. Convert to wall-clock via the `dts` field in `%META` and the `%STOP AT` boot-relative timestamp.
  * `b` — `blockCounter` (which 512-byte SD block this is).
  * `e` — running count of `card.writeData()` failures.
  * `r` — running count of `writeStart` retry attempts.
  * `n` — running count of `card.init()` recovery cycles.
  * `o` — running count of block-write overruns (block took >`MICROS_PER_BLOCK`).

Gaps between consecutive `%CKPT t=` values that exceed ~60 s by more than sample-period jitter indicate a recovery-induced sample-drop window; post-processors should treat that interval as missing data.

### Footer

When a slot fills cleanly (recording reaches `BLOCK_COUNT-1`), the firmware writes a footer with all session counters:

```
%SamplingFreq:
%Total time mS:
%min Write time uS:
%max Write time uS:
%Over:
%Errors:
%Retries:
%Reinits:
%block, uS
[per-overrun lines]
```

Recordings that ended early via `sdCardDead` will not have the footer but will still have all `%CKPT` heartbeats up to the point of failure.

### Self-describing META protocol

Host can send a single-line ASCII `%META { ... }` JSON header before starting a recording, framed with a length-prefixed `M` opcode (`'M' <lenLo> <lenHi> <up to 1024 bytes>`). The firmware writes META atomically as the first 512-byte block of the file (newline-pad-flushed), then ACKs with `META OK <len> <sum>$$$` or `META FAIL$$$` if the SD write failed. Lets the recording be parsed without external sidecar metadata.

### `BLOCK_DIV` auto-detect

`BLOCK_DIV` (the slot-size divisor) is now auto-picked from `daisyPresent`: 1 for Cyton+Daisy (16 channels, longer per-line bytes), 2 for single Cyton. Previously a 12 H slot at 500 Hz Cyton-only was sized for the daisy line length and burned 2.49 GB of contiguous extent for ~half a usable recording; now it is correctly 1.24 GB. The host can still override explicitly via the `c` / `C` commands.

### Counter resets at session start

`setupSDcard` now resets all session-scoped counters (`sdErrs`, `sdRetries`, `sdReinits`, `sdPendingErrs`, `sdMetaCorrupted`, `sdLastCkptMs`, `sdCardDead`) so each recording starts with a clean diagnostic state.

## TODO

* add external trigger to start sd writing with increased sampling rate and hardcoded config, without using a dongle at all
* periodic checkpoint marker before/during META so a META FAIL has a more granular paper trail

## Instructions

* Follow OpenBCI Cyton official firmware upgrade [instructions](https://docs.openbci.com/Cyton/CytonProgram/)
* The only difference is that you clone [modded OpenBCI 32bit Library](https://github.com/roflecopter/OpenBCI_Cyton_Library_SD/) from this repo instead of default [OpenBCI_32bit_Library](https://github.com/OpenBCI/OpenBCI_32bit_Library)

## Important notice

* You do flashing at your own risk
* If something goes wrong you can potentially flash stock firmware back if hardware is not faulty.
* I don't take any responsibility for potential damage, firmware provided as is.
