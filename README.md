# OpenBCI 32bit Library with SD improvements

This is a modified library for the OpenBCI 32bit Board firmware which adds additional SD card functionality.

Main purpose is to improve experience while using OpenBCI Cyton as a part of home based PSG sleep research. You can read more about my quantified self project and especially OpenBCI experience [here](https://blog.kto.to/hypnodyne-zmax-vs-openbci-eeg-psg) .

* short (50ms) flashing LED during writing, every 3 seconds. [Source](https://github.com/OpenBCI/OpenBCI_Cyton_Library/pull/80)
* increased sampling rate. [Source](https://github.com/OpenBCI/OpenBCI_Cyton_Library/pull/96)
* bugfixes. [Source](https://github.com/OpenBCI/OpenBCI_Cyton_Library/pull/93)
* it works for both Cyton and Cyton+Daisy
* i use it on a daily basis with 3 different Cyton/Cyton+Daisy and it work stable

Future tunable-parameter design + planned patches (F: periodic forced idle, G: pre-erase/TRIM, H: adaptive SPI clock for SDXC, long-term per-night SD self-test): see `ROADMAP.md`. Local edits to the SD library fork (e.g. `SD_WRITE_TIMEOUT 600 → 1500`) are documented for re-application in `patches/`.

## SD reliability and observability (2026-05)

Adds visibility, recovery, and self-describing metadata around SD writes. Motivated by the long-standing "1 in 50 silent empty recording" issue and by overnight slots that silently halted mid-night on aging cards.

### Per-block recovery on write failures

When `card.writeData()` fails on a block, the firmware tries — in order, with each tier engaging only if the previous fails:

1. **Same-block retry** via `writeStop` + `writeStart`.
2. **Skip-forward to next block**, `writeStart` retried up to 5x with 1 ms backoff between attempts.
3. **One `card.init()` + `writeStart` recovery cycle** (resets card-internal state, ~1-2 s gap, drops a corresponding burst of ADC samples but recording resumes in same slot).
4. **Extended in-place recovery wait window** (added 2026-05-13) — `EXT_RECOVERY_WINDOW_MS = 8000` ms total, retried every `EXT_RECOVERY_CHUNK_MS = 500` ms. Each retry = `card.init() + writeStart`. Host serial is drained between attempts so the RFduino link stays alive across the wait. Sample stream is paused implicitly (loop() suspended), gap recorded as `sdExtRetries++`. Targets three real-world failure modes: (a) SD-sniffer micro-movement causing brief contact loss, (b) high-endurance / pSLC cards stalling SPI for hundreds of ms during background GC bursts, (c) AA-battery brown-outs during `card.init()` inrush.
5. **Slot recreation via `executeSoftReset(0)`** (added 2026-05-12) — only if the extended window also fails. Triggers an MCU software reset; the next boot enters `replaySessionFile()`, re-runs the original `K`/`L`/etc command, and `setupSDcard()` allocates the NEXT `OBCI_<N+1>.TXT` slot in a fresh extent. Recording resumes there. EEPROM[7] (`resumeCount`) bounds the chain at `MAX_RESUMES = 25` attempts (raised from 3 on 2026-05-13 after a night where one slot was created but recorded zero samples, burning a full budget unit on nothing useful). A successful first-`%CKPT` resets EEPROM[7]=0 — so a healthy night gets effectively unlimited budget; only sustained card death exhausts the cap.
6. **Clean shutdown via `sdCardDead`** — last-resort fallback if `executeSoftReset` somehow returns (shouldn't on PIC32MX). Tears down open-file flags so the main loop stops pumping bytes through a dead SPI bus.

Combined with the SD-library `SD_WRITE_TIMEOUT` raised from 600 ms → 1500 ms in the local fork (patch documented at `patches/sd-fork-write-timeout.patch`), most modern card GC behaviour is now absorbed silently with no visible gap. Verified on SanDisk Max Endurance 32 GB (2026-05-13) — went from ~6 errors/min to 0 errors over a 5 min clean baseline run; even with deliberate sniffer-bump abuse the recovery loop kept everything in a single slot file.

### `ledSDError` fast strobe + auto-clear

When any SD write error fires, the on-board LED switches from the normal slow blink to a 100 ms / 300 ms fast strobe. Since 2026-05-11 the strobe **auto-clears** at the next `%CKPT` if no new errors have occurred since the previous `%CKPT` — so the morning user can distinguish "recording fine, had a glitch six hours ago" from "recording is currently broken". Forensic history is preserved in the `%CKPT` and footer counters in the file.

A separate `ledReplayFail` LED state (added 2026-05-11) emits a distinct double-flash pattern (100/100/100/1000 ms) when the boot-time `replaySessionFile()` attempts a SESSION.TXT replay and aborts before streaming starts — see "SESSION.TXT auto-resume" below.

### Inline `%E` markers and `%CKPT` heartbeat

The TXT recording is now self-describing for diagnostics. All markers are emitted on their own line (no fragmentation of CSV sample lines) at sample boundaries.

* `%E\n` — emitted on the line after each block-write error event. One per recovered block.
* `%CKPT t=<ms> b=<block> e=<errs> r=<retries> n=<reinits> o=<over> x=<extretries>\n` — heartbeat line emitted approximately once per minute. Fields:
  * `t` — `millis()` since Cyton power-on at the moment of emission. Convert to wall-clock via the `dts` field in `%META` and the `%STOP AT` boot-relative timestamp.
  * `b` — `blockCounter` (which 512-byte SD block this is).
  * `e` — running count of `card.writeData()` failures.
  * `r` — running count of `writeStart` retry attempts.
  * `n` — running count of `card.init()` FAST-path recovery events (1 per failure event — semantics unchanged across firmware versions for cross-version comparability).
  * `o` — running count of block-write overruns (block took >`MICROS_PER_BLOCK`).
  * `x` — running count of EXTENDED-window recovery attempts (added 2026-05-13 with the per-event 8 s wait loop). Up to ~16 per failure event. `x=0` for an entire slot is the desired steady state — non-zero means at least one event needed the extended window to recover. See "Per-block recovery on write failures", tier 4.

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
%ExtRetries:
%block, uS
[per-overrun lines]
```

`%ExtRetries:` was added 2026-05-13 alongside the `%CKPT x=` field. Recordings that ended early via `sdCardDead` will not have the footer but will still have all `%CKPT` heartbeats up to the point of failure.

### SESSION.TXT auto-resume + `P` protocol + `ledReplayFail`

Added 2026-05-11. Lets a recording survive a silent MCU halt by automatically replaying the original session-config command stream on next boot and continuing in a new continuation file (chained back to the original via `%BOOT prev=...`).

* **Wire protocol — `P` command**: host sends `'P' <lenLo> <lenHi> <up to 1024 bytes payload>`. Firmware writes the payload to SD root as `SESSION.TXT`, framed `%PBEGIN\n` + payload + `%PEND\n`. ACKs with `PERSIST OK <len> <sum>$$$` or `PERSIST FAIL <pfail>$$$` (codes 1-6 for which SD step failed). Same gating model as the `M` META protocol: `!board.streaming && !replayingSession && sdMetaState == 0`.
* **Boot-time replay** (`replaySessionFile()` in `setup()`): reads SESSION.TXT, validates `%PBEGIN`/`%PEND` framing, pre-scans for `~ < K < M < b` ordering, bumps `EEPROM[7]++` (resume cap), then feeds each byte through the same `sdPersistProcess → sdMetaProcess → sdProcessChar → board.processChar` dispatch chain that `loop()` uses for host serial bytes. The `K` command in the file opens a new continuation slot, `M` writes META, `b` starts streaming. The new file's `%BOOT` line carries `prev=OBCI_<NN>.TXT resume=N` so a post-processor can chain the multi-file session.
* **`ledReplayFail` indicator**: distinct double-flash pattern (100/100/100/1000 ms) emitted when `replaySessionFile()` aborted before streaming started. Distinguishable by eye from the steady `ledSDWrite` (mostly solid + brief dim every ~3.5 s) and the `ledSDError` strobe (continuous 100/300 ms).
* **`REPLAYFL.TXT` forensic file** (added 2026-05-12): when `ledReplayFail` fires the firmware writes `code=N t=<ms>\n` to SD root. Codes 1-12 narrow down the failure point (cap exhausted, SESSION.TXT corrupt, replay-feed timeout, streaming-never-started, etc). The write itself was hardened on 2026-05-13 to do a fresh `card.init()` first, so the forensic file lands even when the card has degraded enough that the prior path silently dropped the write.
* **`MAX_RESUMES`** = 25 (raised from 3 on 2026-05-13). EEPROM[7] is uint8 with 0xFF reserved as virgin sentinel. The first `%CKPT` in a successfully-resumed session resets EEPROM[7]=0 — so a healthy night gets effectively unlimited budget; only sustained card death exhausts the cap. Per-slot `resume=N` field in `%BOOT` is now 1-2 ASCII digits (back-compat: parsers using `int(value)` accept either width).
* **Manual stop semantics**: host `j` command does a clean close — deletes SESSION.TXT and resets EEPROM[7]=0 so the next boot is a normal idle, not an auto-resume. Power-off without `j` leaves SESSION.TXT in place — next power-on will replay it. The recommended morning workflow is to either pull the SD before next evening (the natural sleep-recording rhythm), or to run the host-side session-start tool again — it overwrites SESSION.TXT.

### Self-describing META protocol

Host can send a single-line ASCII `%META { ... }` JSON header before starting a recording, framed with a length-prefixed `M` opcode (`'M' <lenLo> <lenHi> <up to 1024 bytes>`). The firmware writes META atomically as the first 512-byte block of the file (newline-pad-flushed), then ACKs with `META OK <len> <sum>$$$` or `META FAIL$$$` if the SD write failed. Lets the recording be parsed without external sidecar metadata.

### Runtime tunable recovery / SD constants — `T` protocol + `%TUNE`

Added 2026-05-15 (ROADMAP item 1). Five previously-`#define`d recovery thresholds — `MAX_RESUMES`, `EXT_RECOVERY_WINDOW_MS`, `EXT_RECOVERY_CHUNK_MS`, `CKPT_INTERVAL_MS`, `SD_WRITE_TIMEOUT` — are now RAM-backed and rewritable mid-session. The host can A/B values per card class across nights without reflashing the cyton (bootloader dance is ~30 s/cycle and easy to miss the upload window).

* **Wire protocol — `T` command**: host sends `'T' <key_id:1B> <value bytes, LSB-first, count implied by key>`. Firmware acks `TUNE OK <key_id>$$$` or `TUNE FAIL <code>$$$` (code 1 = unknown key, 2 = value out of range). Same gating model as `P` and `M`: `!board.streaming && !replayingSession && sdMetaState == 0 && sessState == 0`. Key IDs and value widths:

  | key_id | name | C type | default | valid range |
  |---|---|---|---|---|
  | 0x01 | `MAX_RESUMES` | uint8 | 25 | 1..254 |
  | 0x02 | `EXT_RECOVERY_WINDOW_MS` | uint16 | 8000 | 1..60000 |
  | 0x03 | `EXT_RECOVERY_CHUNK_MS` | uint16 | 500 | 10..5000 (and ≤ window) |
  | 0x04 | `CKPT_INTERVAL_MS` | uint32 | 60000 | 1000..3600000 |
  | 0x05 | `SD_WRITE_TIMEOUT` | uint16 | 1500 | 100..5000 |

  For 0x05, `SD_WRITE_TIMEOUT` lives in the SD library fork — see `patches/sd-fork-write-timeout.patch` for the const → extern conversion. Lowering `MAX_RESUMES` below the current `EEPROM[7]` value resets `EEPROM[7]=0` (preserves "raise the cap" intent).

* **Persistence — `%TUNE` line in SESSION.TXT**: `session_start.py` prepends `%TUNE max_resumes=25 ext_recovery_ms=8000 ext_chunk_ms=500 ckpt_interval_ms=60000 sd_write_timeout=1500\n` to the SESSION.TXT payload. `replaySessionFile()` parses `%TUNE` lines line-by-line during the byte-feed loop BEFORE dispatching the rest of the body — so an auto-resumed continuation file inherits the same tuning state that produced the silent halt. Unknown keys are silently skipped (forward-compat with future additions). The `%TUNE` line bypasses the normal dispatch chain because some of its bytes (`T`, `U`, `E`) are valid OpenBCI command chars that would fire spurious `CHANNEL_ON_*` commands otherwise.

* **Forensic visibility**: every `%CKPT` line now ends with `T=<hex8>` — an FNV-1a hash over the 5 tunables. Same tunable set → same hash, so each morning file self-documents which tuning was active. Pair with the `tune` block in `%META` JSON for the primary record. Hash is stable as long as the tunable set doesn't change shape; if a 6th tunable is added later, the hash will shift but old recordings remain parseable.

* **Host knobs (`session_start.py`)**: optional `tune:` block in `session_start.yml`, plus `--tune key=value` CLI flag (repeatable) overlaying yml on top of firmware defaults. Both go through `tune_helpers.merge_tune()` which validates ranges before any serial I/O. Default behaviour (no yml block, no CLI) = firmware defaults; no T commands sent, and the `%TUNE` line still appears in SESSION.TXT documenting "everything at default".

### `BLOCK_DIV` auto-detect

`BLOCK_DIV` (the slot-size divisor) is now auto-picked from `daisyPresent`: 1 for Cyton+Daisy (16 channels, longer per-line bytes), 2 for single Cyton. Previously a 12 H slot at 500 Hz Cyton-only was sized for the daisy line length and burned 2.49 GB of contiguous extent for ~half a usable recording; now it is correctly 1.24 GB. The host can still override explicitly via the `c` / `C` commands.

### Counter resets at session start

`setupSDcard` resets all session-scoped counters (`sdErrs`, `sdRetries`, `sdReinits`, `sdExtRetries`, `sdPendingErrs`, `sdMetaCorrupted`, `sdLastCkptMs`, `sdCardDead`, `firstCkptResetDone`) so each recording starts with a clean diagnostic state. EEPROM[7] (resume cap) is reset to 0 on every fresh non-resume `K` command (i.e. host-driven new session, not auto-resume) and on every successful `P` write — so a stale value from yesterday never carries over.

### Card sizing notes

Empirically validated card classes for nightly sleep recording:

| Card | Verdict | Notes |
|---|---|---|
| SanDisk Industrial 16 GB (pSLC) | recommended | clean badblocks scan; quiet `%CKPT` counters across multiple nights |
| SanDisk Max Endurance 32 GB | works after 2026-05-13 patches | `f3probe -d` confirmed genuine. pSLC GC bursts up to ~750 ms hit the original `SD_WRITE_TIMEOUT=600 ms` ceiling and produced ~6 errors/min until the timeout was raised to 1500 ms. With current firmware: 0 errors over a 5 min clean baseline; ~1 in ~7 sniffer-bumps needs the extended-window recovery |
| SanDisk Extreme Pro 32 GB | dying — debug only | spare-pool exhaustion produces silent halts on aging units |

Modern controllers run two GC modes: inline (forced during writes when the SLC cache fills) and idle (background, only kicks in after ~100-200 ms of SPI idle). At 500 Hz EEG with ~56-byte sample lines arriving every 2 ms the SPI bus is never idle long enough for idle-GC mode → all GC happens inline. The 1500 ms `SD_WRITE_TIMEOUT` lets inline GC bursts up to ~1.5 s complete silently. If a card class needs more headroom or shows persistent errors with this ceiling, see Patch F (periodic forced idle) and Patch G (pre-erase / TRIM) in `ROADMAP.md`.

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
