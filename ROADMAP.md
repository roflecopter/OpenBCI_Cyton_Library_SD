# Roadmap

Planned firmware improvements not yet implemented. Items here are scoped + designed but parked behind something else (more urgent work, need data, or waiting on a quiet weekend to flash + validate).

---

## Morning SD health check (post-processing step)

**Why:** a dying card surfaces as elevated `e/r/n/x` counters in `%CKPT` lines plus occasionally a silent halt. Catching the degradation *before* the next night's recording avoids losing irreplaceable EEG data. Morning is the natural place: the SD is already mounted on the laptop, the processing pipeline (`sd_convert.py` → BDF → ingestion) runs anyway, and any "card is dying" verdict has all day to act on (swap card, reformat, ship a fresh one to the bedside) before bedtime.

**Approach:** add a short self-test step to the morning processing flow — write+verify a 1 MB pattern file on the SD, measure write-latency distribution, confirm zero errors. Cheap (~5–10 s on a healthy card vs the original 30 s evening proposal). Combine the live-write result with the parsed `%CKPT` counters from last night's TXT to produce a single "card health" verdict per morning. Persist verdicts in `sessions.db` so trends are visible (e.g. "Industrial 16 GB latency p95 has crept from 80 ms to 240 ms over 30 nights → schedule replacement").

**Where it lives:** the post-processing pipeline on p14s (alongside or inside `sd_convert.py`'s wrapper), not on the Cyton. No firmware change required.

**Status (2026-05-15):** shipped on the host side in `openbci-session/sd_health.py` (see openbci-session main branch). This roadmap entry kept for historical context until the firmware-side counters baseline a few weeks of real data — the verdict thresholds in sd_health are conservative starting values and may need recalibration.
