# Roadmap

Planned firmware improvements not yet implemented. Items here are scoped + designed but parked behind something else (more urgent work, need data, or waiting on a quiet weekend to flash + validate).

---

## Runtime parametrisation of recovery / SD constants

**Why:** Today every tweak to recovery thresholds (`MAX_RESUMES`, `EXT_RECOVERY_WINDOW_MS`, `EXT_RECOVERY_CHUNK_MS`, `CKPT_INTERVAL_MS`, `SD_WRITE_TIMEOUT`) requires reflashing the cyton — bootloader dance, ~30 s per cycle, easy to miss the upload window. For *finding* the right values per card class (Industrial 16 GB vs Max Endurance 32 GB vs whatever next) we want to A/B values across nights, ideally same night, without touching the chip.

**Approach:** new `T <key_id> <value_lo> <value_hi> <value_b2> <value_b3>\n` wire command (mirrors `P` / `M` framing). Sets in-RAM versions of what are currently `#define` constants. Persistence: prepend a `%TUNE k=v k=v ...` line to `SESSION.TXT` so `replaySessionFile()` re-applies the tuning at boot — auto-resume after a silent halt naturally inherits the same values.

**Tunable params (5 total):**

| key_id | name | type | default | scope |
|---|---|---|---|---|
| 0x01 | `MAX_RESUMES` | uint8 | 25 | firmware |
| 0x02 | `EXT_RECOVERY_WINDOW_MS` | uint16 | 8000 | firmware |
| 0x03 | `EXT_RECOVERY_CHUNK_MS` | uint16 | 500 | firmware |
| 0x04 | `CKPT_INTERVAL_MS` | uint32 | 60000 | firmware |
| 0x05 | `SD_WRITE_TIMEOUT` | uint16 | 1500 | SD library fork (`Sd2Card.h`) |

For 0x05, `SD_WRITE_TIMEOUT` is currently `uint16_t const` — needs converting to a non-const variable in the fork plus a setter. Affects every `waitNotBusy(SD_WRITE_TIMEOUT)` call site (writeBlock, writeData, writeStop). Document the fork edit in `patches/sd-fork-write-timeout.patch` alongside the existing one.

**Wire protocol:**

```
T <key_id:1 byte> <val:1-4 bytes LSB-first>\n
```

Length is implicit by key_id (mapping table above). Firmware acks `TUNE OK <key_id>$$$` or `TUNE FAIL <reason>$$$`. Same gating as `P`: `!board.streaming && !replayingSession && sdMetaState == 0`.

**SESSION.TXT format extension:**

```
%PBEGIN
%TUNE max_resumes=25 ext_recovery_ms=8000 ext_chunk_ms=500 sd_write_timeout=1500
x1060110X...
~5
/0
K
b
%PEND
```

`replaySessionFile()` parses `%TUNE` lines at the top of the body (before the first non-`%` byte), applies key=value pairs to in-RAM tuning, then proceeds with the existing byte feed. Unknown keys skipped silently (forward-compat).

**Host side (`session_start.py`):**

- New yml block:
  ```yaml
  tune:
    max_resumes: 25
    ext_recovery_ms: 8000
    ext_chunk_ms: 500
    ckpt_interval_ms: 60000
    sd_write_timeout: 1500
  ```
- CLI overrides: `--tune key=value` (repeatable) so a quick A/B doesn't need editing yml
- Send `T` commands BEFORE `P` so the SESSION.TXT write itself uses fresh tuning state (mostly cosmetic — only `MAX_RESUMES` gates anything in the SESSION.TXT-write path)
- `%TUNE` line included automatically in the SESSION.TXT payload

**Forensic visibility:**

- `%CKPT` extended once more: `t=… b=… e=… r=… n=… o=… x=… T=<key_summary_hash>` — short hash so morning files self-document which tuning was active
- `%META` augmented with a `tune` block: `"tune": {"max_resumes": 25, "ext_recovery_ms": 8000, ...}` — primary record per session

**Effort estimate:** ~80 lines firmware + ~20 lines `session_start.py` + ~10 lines SD library fork. Plus one parallel review pass (claude + codex) per the standing rule. Maybe 30–45 min implementation, plus reflash + 5-min validation test. Park until tonight's data tells us which params are most worth iterating.

**Risks / open questions:**

- Adding a 5th wire protocol means another P-byte-stealing-from-M-payload hazard if META JSON ever contains the literal `T` byte mid-payload — need the same `sdMetaState == 0` gate and confirm META payloads don't contain T as a top-level command character (they shouldn't — META payloads are opaque JSON nested inside `M <len> <payload>`).
- `SD_WRITE_TIMEOUT` is read inside the SD library on EVERY waitNotBusy call. Changing it from const to volatile uint16 may cost an extra memory load per call — measure whether this affects sample throughput at 500 Hz. Probably negligible; document just in case.
- Storing tuning in `%TUNE` line of SESSION.TXT means manual edits to SESSION.TXT (e.g. `vi` on the SD card) become a way to tune without going through `session_start.py` — useful debug path, but watch for accidental tampering.
- `MAX_RESUMES` lowered at runtime to a value below the *current* `EEPROM[7]` would lock out auto-resume immediately. Clamp the runtime setter so `EEPROM[7] >= new_max` resets `EEPROM[7]=0` (preserves intent of "raise the cap").

---

## Morning SD health check (post-processing step)

**Why:** a dying card surfaces as elevated `e/r/n/x` counters in `%CKPT` lines plus occasionally a silent halt. Catching the degradation *before* the next night's recording avoids losing irreplaceable EEG data. Morning is the natural place: the SD is already mounted on the laptop, the processing pipeline (`sd_convert.py` → BDF → ingestion) runs anyway, and any "card is dying" verdict has all day to act on (swap card, reformat, ship a fresh one to the bedside) before bedtime.

**Approach:** add a short self-test step to the morning processing flow — write+verify a 1 MB pattern file on the SD, measure write-latency distribution, confirm zero errors. Cheap (~5–10 s on a healthy card vs the original 30 s evening proposal). Combine the live-write result with the parsed `%CKPT` counters from last night's TXT to produce a single "card health" verdict per morning. Persist verdicts in `sessions.db` so trends are visible (e.g. "Industrial 16 GB latency p95 has crept from 80 ms to 240 ms over 30 nights → schedule replacement").

**Where it lives:** the post-processing pipeline on p14s (alongside or inside `sd_convert.py`'s wrapper), not on the Cyton. No firmware change required.
