# Roadmap

Planned firmware improvements not yet implemented. Items here are scoped + designed but parked behind something else (more urgent work, need data, or waiting on a quiet weekend to flash + validate).

---

## Patch F (deferred): periodic forced idle

**Why:** even with `SD_WRITE_TIMEOUT` raised to 1500 ms, modern controllers might benefit from explicit idle windows for background GC. At 500 Hz the SPI bus is never idle long enough for the card to enter idle-GC mode → all GC happens inline.

**Approach:** insert a brief pause (~100 ms) every N seconds (~30 s) inside `writeCache()`, gated behind a `FORCED_IDLE_INTERVAL_MS` constant (or runtime tunable per the section above).

**When to ship:** after collecting a few nights of data with Patches A–E in place. If `e/r/n/x` counters in `%CKPT` are consistently near zero, F is unnecessary noise. If errors persist, size the idle window from the observed GC stall distribution rather than guessing.

---

## Patch G (deferred): pre-erase / TRIM at session start

**Why:** SD cards perform better when the controller knows which clusters are free. SdFat doesn't issue `CMD33`/`CMD38` ERASE; cards see all old slots as "still data" until overwritten. Erasing the next slot's clusters at session start (or as a `session_start.py` pre-step) primes the controller for fast linear writes.

**Effort:** ~50 lines in the SD library fork. Defer until we see whether `%CKPT` counters stay low across multiple nights — if they do, G is premature optimisation.

---

## Patch H (deferred): SDXC controller-mode detection + adaptive SPI clock

**Why:** the SD library hardcodes 20 MHz SPI on the DSPI path (`SPI_HALF_SPEED` is a no-op). Some SDXC cards have signal-integrity issues at 20 MHz over the Cyton's 2013-era SD-slot traces. Memory's note at line 50: real half-speed support requires library-level changes.

**Approach:** convert the hardcoded `_spi->setSpeed(20000000UL)` in `Sd2Card::init` and `csLow(SD_SS)` to read a runtime `_sckSpeedHz` member. Add `setSckSpeed(uint32_t hz)` setter. Wire to a tunable per the parametrisation section above.

**When to ship:** if a specific card shows persistent CRC errors that the GC-busy hypothesis can't explain. So far Industrial 16 GB and Max Endurance 32 GB have no signal-integrity smell — pure GC behaviour.

---

## Long-term: per-night pre-flight self-test

Imagine session_start.py running a 30-second SD self-test before declaring the night ready: write+verify a 1 MB file, measure latency distribution, confirm no errors. Catches a dying card BEFORE the night starts instead of finding out at sunrise.

Out of scope until the basic recovery + tuning loop is solid. Add a backlog ticket for Q3.
