# Prep: Cyton freeSD diagnostics + tunable-knobs layer (bundled into the SPIROV-fix flash)

> v2 — revised after panel round 1 (Codex + Gemini). Grounded against the actual firmware:
> RCON is already captured at `setup()` entry (bootloader pre-clears it → `rcon=0x00` is expected,
> not fixable); the MX250 has **no HLVD module** (only `NVMCON.LVDSTAT/LVDERR`, flash-write-window
> only); EEPROM used addrs are 0,1,4,5,6,7,10,11 (free low slots 2,3,8,9); the diag EEPROM
> breadcrumb uses its **own standalone, redundancy-checked bytes** (`pf`@9, `ps`@8 + `~ps`@12),
> **decoupled** from `prep.md`'s magic+complement record (which keeps slots 2,3).

## Goal
Bake a thin **observability + host-tunable** layer into the **same single flash** as the SPIROV
fix (`prep.md`) so that, after this one flash, **future debugging needs no reflash**: the next
failure's SD file (plus the next boot's `%BOOT`) self-identifies whether the freeze was a
**SPIROV/SPI-bus hang** (which the fix recovers + logs) or **not the bus** (power/other → it
re-freezes with no SPIROV activity near the cut). The fix's parameters become host-tunable via the
existing `T` protocol. The VDD/brownout monitor is **dropped** (user decision); the synthetic-hang
self-test is **runtime-gated** so it validates the actual production hex.

## The discriminator (host-side morning script — corrected per R1–R8; post-flash-revisable)
> **Scope note (R8):** this is the spec for a **host-side Python classifier** that reads the SD file
> + the next `%BOOT`. It is **revisable on real data AFTER the flash** — only the *firmware-emitted
> data* it consumes (`s=` count, the `ps`/`~ps`/`pf` EEPROM breadcrumb, `%E` markers at their byte
> offsets, the `pf` states) is flash-gating, and that has been stable since v4. The `%E` marker's
> **file byte-offset IS its location** (no extra firmware field needed for adjacency).

A *recovered* SPIROV is **benign** (the fix logs a `%E`, bumps the counter, continues). The question
is **"was SPIROV active at the cut?"** — answered by **combining three independent signals** (NOT a
strict SD-then-EEPROM order — R8 Codex#1/Gemini#1 showed ordering either masks a valid `ps>s` or makes
the contradiction guard dead code). Decode `pf`∈{clean `0x00`, unclean `0xA5`, else UNKNOWN} and `ps`
(valid iff `EEPROM[8]==~EEPROM[12] && ≤250`, else UNKNOWN). Let `BLK` = 512-byte blocks of *valid*
data; adjacency uses the **last `%E` marker's byte-offset** (NOT the `s=` increment's position — R8
Codex#2: `%CKPT`/`s=` lands at the 60 s checkpoint boundary, far from the SPIROV; only `%E` sits at
the recovery point). **Procedure:**
1. **Footer present** → CLEAN.
2. **Contradiction guard FIRST (R8 Gemini#1):** **no footer + `pf` decodes CLEAN (`0x00`)** →
   **UNKNOWN** (a clean breadcrumb under an unclean file = stale/prior-session, e.g. power lost in the
   `%BOOT`-flush→arm window before `pf=0x3C`). Must run before any SD/EEPROM trust, else it's dead code.
   > **`ps` TRUST GATE (R9 Gemini#1 + R10 Codex#1 + R11 Gemini#1):** `ps` is trusted ONLY when ALL
   > hold: (a) **`pf` decodes `0xA5`**; (b) **the analyzed file is the latest on the card and was not
   > followed by intervening sessions run on a *different* card** — checked as `analyzed_file.seq ==
   > next_file.prev` (or, when it IS the newest file, the live EEPROM `sessionSeq == analyzed_file.seq
   > + 1` — `seq` increments *before* arm, so the live counter is one ahead of the latest file's `seq`,
   > R12 Gemini#1); and
   > (c) **the file shows substantial recording progress past `%BOOT`** (≥ a few KB of samples, not a
   > `%BOOT`-only/pre-arm casualty). **Which condition does what (R11 Gemini#1 — important):** the
   > global `sessionSeq` increments *before* the arm step, so (b) **does NOT** by itself exclude a
   > pre-arm casualty (a session that died pre-arm still incremented `seq` and still reports `prev`) —
   > (b) only detects intervening/foreign-card sessions. **It is condition (c) that actually protects
   > the `%BOOT`-flush→`pf=0x3C` pre-arm window** (R10 Codex#1): a pre-arm casualty is a near-empty
   > `%BOOT`-only file → fails (c) → `ps` untrusted → SD-only signals → typically UNKNOWN. *(Bulletproof
   > firmware alternative, deferred: store the armed `sessionSeq` INSIDE the breadcrumb record, written
   > atomically at the `pf=0xA5` step, so trust is a pure EEPROM-internal `storedSeq == file.seq` check
   > — costs an EEPROM word + a write, eliminates reliance on (c)'s heuristic; this is a
   > post-flash-revisable host script so it can wait.)*
3. **Positive BUS signals — fire if ANY (the `ps` signal is checked regardless of whether the SD tail
   looks available — R8 Codex#1 — but only when trusted per the gate above):**
   - **a flushed `%E` within ≤4 BLK of EOF** (by `%E` byte-offset) → **BUS** (SPIROV right at the cut).
   - **SD `s ≥ 250`** (uncapped) or trusted `ps == 250` → **BUS** (fundamentally unstable — R3 Gemini#1;
     absolute, overrides any clean-run check below).
   - **trusted `ps > last-durable-%CKPT s=`** → a SPIROV occurred *after* the last durable checkpoint
     (the ONLY evidence of a fatal SPIROV that bumped `ps` but hung before its `%E`/next `%CKPT`
     flushed — R8 Codex#1). **BUT (R9 Gemini#2 ↔ R8 Codex#1 split — resolved):** if the last flushed
     `%E` is followed by **≥20 BLK of clean data**, that SPIROV demonstrably recovered, so `ps>s=`
     could be EITHER that recovered SPIROV (then a coincidental power cut) OR a *newer* fatal SPIROV
     that didn't log a `%E` — **genuinely indistinguishable with an uncounted `%E` marker → classify
     UNKNOWN, not BUS** (neither forced-bus per Codex nor forced-not-bus per Gemini; same operational
     response). Otherwise (no ≥20 BLK clean run after the last `%E`) → **BUS** (flag low-confidence/
     SPIROV-near-tail when the tail is otherwise clean — R3 Codex#3).
4. **No positive bus signal → classify NOT-bus / UNKNOWN:**
   - **last `%E` ≥20 BLK before EOF** (substantial clean run after the last logged SPIROV), **OR**
     trusted `0 < ps ≤ last-durable s=` (every recorded SPIROV was already in a flushed `%CKPT`) →
     **recovered-then-other (NOT bus)**. *(`≤` not `==` — R7 Gemini#1: once-per-epoch `ps` can lag `s`.)*
   - **trusted `ps == 0`** and no `%E` → **no durably-recorded SPIROV** → power-loss **or** non-SPI hang
     (indistinguishable here, Decision 1; the torn-`ps`-write-during-power-loss blind spot, R5 Codex#3).
   - **last `%E` in the 5–19 BLK gray band**, **or** `ps` untrusted/UNKNOWN with no decisive `%E`/`s`
     signal → **UNKNOWN**.
**Operational response** for NOT-bus / UNKNOWN / low-confidence is identical: enable the OFF-by-default
soft-WDT next night while chasing the non-bus cause — so an ambiguous classification never blocks
progress. Threshold bands are **mutually exclusive: ≤4 / 5–19 / ≥20 BLK** (R8 Gemini#2). *(A clean
disambiguator for the `ps>s`+clean-run case — tagging each `%E` with the running SPIROV count so the
host can compare `ps` to the last `%E`'s count — is a possible future firmware enhancement, deferred:
it changes the `%E` host-contract + costs flash, and the ambiguous case has the same response anyway.)*

## Context & constraints
- **Builds into ONE flash with `prep.md`** (the SPIROV fix). `/grill` builds both together and
  flashes once. The only fix-prep decision this plan changes is the self-test gating (Decision 8).
- **`%CKPT` is SD-only** (`memcpy` into `pCache`, never Serial0); **no live host link at 500 Hz**
  (RF off above 250 SPS). Telemetry lands in the SD file or survives the power-cycle in EEPROM.
- **`%CKPT` is a HOST CONTRACT** — `%CKPT t=… b=… B=… e=… r=… n=… o=… x=… T=<hex8>` (snprintf at
  `SD_Card_Stuff.ino:1812`). Additions are **append-only `k=v`**, never reorder/rename/remove
  existing keys.
- **`%BOOT`** (`%BOOT seq=… rcon=… session=… prev=… resume=… rcv=… g=0x…` + resume-only trailing
  fields) is host-parsed and append-only.
- **`T=<hex8>`** is an FNV-1a hash over the runtime tunables (`tuneSummaryHash()`).
- **EEPROM** used addrs (audited): 0,1 (file enumerator), 4 (legacy `sessionActive` — entangled with
  the resume chain, NOT reused here), 5,6 (sessionSeq), 7 (resume cap), 10,11 (slotChar/rate). Free
  low slots: 2,3,8,9,12,13. `prep.md`'s magic+complement sticky record (reset-pending + per-night cap)
  takes slots **2,3**; this plan's breadcrumb takes **`pf`@9 (sentinel) + `ps`@8 with its complement
  `~ps`@12** (R2+R5: decoupled from prep.md's record; the `ps`+`~ps` pair gives the redundancy a raw
  count byte lacks — a torn read fails to UNKNOWN, see Decision 4).
- **Flash budget ~4280 B free at 96 % fragmented-full is THE gating risk** (shared with the fix);
  per-component size measured in `/grill`; drop order in Decision 7.
- Mandatory Codex+Gemini+GLM panel; NEVER flash without explicit user OK.

## Decisions
1. **No power-loss hardware signal exists on this part — accept the limitation** (refines the
   user's "drop the VDD monitor"). Verified against the firmware/proc-header: (a) **RCON is already
   captured at `setup()` entry** (`bootResetCause=RCON; RCONCLR=0xFFFF`, DefaultBoard.ino:58) — as
   early as user code can run — but the **chipKIT bootloader pre-clears RCON**, so `rcon=0x00` is
   *expected* and POR/BOR/WDTO bits are **not recoverable** in the sketch (no fix possible). (b) The
   **PIC32MX250F128B has no HLVD/LVDCON module** — only `NVMCON.LVDSTAT/LVDERR`, which flag a
   low-voltage event *during a flash write* only (not a continuous brownout monitor), so the
   panel-suggested HLVD interrupt is **infeasible here**. (c) The ADC band-gap path is
   feasibility-uncertain (4-bit `CH0SA` mux; A5–A7 taken by the accelerometer) and the user dropped
   it. → **Conclusion:** power-loss vs non-SPI-hang stays *undistinguished*; documented as an
   accepted limitation, handled operationally by the soft-WDT escalation (above). `NVMCON.LVDSTAT`
   is noted as a *possible future* bonus (flash-write-window only) — **not added now**.
2. **`%CKPT` gains ONE append-only field `s=%lu`** = cumulative **hardware-SPIROV-confirmed** events
   this session. **Critical (R1):** `s` increments **only when the SPI `SPIROV` status bit was
   actually observed** (in the fork primitive, gated on the real `SPI1STAT.SPIROV`), **never** on a
   generic CP0-deadline / bounded-timeout fault — those are a *different* failure (card-busy, bad CS,
   deadline-too-low) and would conflate the signal. Deadline/timeout faults reuse the **existing**
   `x=` (`sdExtRetries`) counter (no new field) so `s=` stays a clean SPIROV-only signal. Source =
   a `volatile uint32_t sdSpirovSeen` (`extern`). **Field placement (R7 Gemini#2):** insert `s=`
   **immediately before `T=`** (`… x=… s=… T=<hex8>`), keeping `T=` the line terminator. *Grounded:*
   the canonical host parser `sd_convert.parse_ckpt_line` is whitespace-split + unknown-key-skip +
   **order-independent** and doesn't even read `T=`, so order isn't strictly required — but placing
   `s=` before `T=` is free insurance for any ad-hoc `T=<hex8>$` end-anchored regex (Test 1 confirms
   none exists). It does NOT reorder existing keys (all of `t,b,B,e,r,n,o,x,T` keep relative order).
   **Cap clarity (R5 Codex#6):** the SD `s=` is the
   **true uncapped** count (`uint32_t`, `%lu`); only the EEPROM `ps` byte is capped at 250. The
   classifier's saturation rule therefore reads **`s ≥ 250` (SD) OR `ps == 250` (EEPROM)** — stated
   consistently wherever saturation appears.
3. **`%CKPT` buffer hardening (Codex BLOCKER) — corrected vs the existing code (R2):** the existing
   writer (`SD_Card_Stuff.ino:1822`) already does `if (n > 0 && byteCounter + n <= 512) memcpy(...,n)`
   and, crucially, **does NOT update `sdLastCkptMs` when it doesn't fit → the `%CKPT` is RETRIED at
   the next sample boundary** (landing at `byteCounter==0` after a normal block flush, where a
   ~135-byte line always fits). So a `%CKPT` is **never dropped** and **no mid-stream forced flush is
   needed** (Gemini#2's "26% loss" reads a drop into the plan that isn't there — keep the
   defer-and-retry). The **real** bug Codex#1 catches: if `snprintf` *truncates* (`n >= sizeof(tmp)`),
   the existing `byteCounter+n<=512` test uses the inflated `n` and `memcpy(...,n)` **reads past
   `tmp[]`**. Fix = enlarge `tmp[160]` and **split the two failure cases** (R3 Gemini#3 — a naive
   `n < sizeof(tmp)` guard that *skips* would never advance `sdLastCkptMs` → infinite per-sample
   retry spin):
   ```
   int n = snprintf(tmp, sizeof(tmp), …);
   if (n <= 0 || n >= (int)sizeof(tmp)) {    // encoding error / empty / truncated — defensive (R4 Gemini#1; R5 Gemini#3: n==0)
       sdLastCkptMs = millis();              // ADVANCE the timer (drop this one %CKPT) so we never spin
   } else if (n > 0 && byteCounter + n <= 512) {
       memcpy(pCache + byteCounter, tmp, n); byteCounter += n; sdLastCkptMs = millis(); … // (existing)
   }                                          // else: doesn't fit THIS block → leave timer → retry next
   ```                                        //       sample boundary (existing, BOUNDED: fits at byteCounter==0)
   So: the over-read is prevented (memcpy only when `n < sizeof(tmp)`); the existing **defer-and-retry
   never drops** a `%CKPT` (Gemini#2's "26% loss" misreads a drop into the plan that isn't there — the
   no-fit branch retries at the next sample boundary and always fits after a block flush); and the
   should-never-happen truncation advances the timer (drops exactly one) instead of spinning. Adding
   `s=%lu` lengthens the worst case to ~111–125 B (still ≪ 160 and ≪ 512-after-flush).
4. **EEPROM freeze-breadcrumb — standalone, redundancy-checked, synchronous per-epoch (R2 Codex
   1/3/4 + Gemini 1/3; R5 Codex 2/3 + Gemini 2):**
   - **Storage = standalone bytes, NOT in `prep.md`'s complement record** (sharing risks tearing both
     the evidence and the WDT/reset-pending state). `prep.md` keeps its own record at free slots 2,3;
     this plan uses:
     - **`pf` @ `EEPROM[9]`** — sentinel-encoded `0x00`=clean / `0xA5`=unclean-active; **any other
       byte (incl. `0xFF` virgin or a torn value) → UNKNOWN** (only 2/256 values decode, so a torn
       `pf` fails safe).
     - **`ps` @ `EEPROM[8]` + its complement `~ps` @ `EEPROM[12]`** (R5 Gemini#2 — a raw count byte
       has NO redundancy; a torn DEE page leaves a random byte that is a valid `0..250` ~98 % of the
       time, *fabricating* SPIROV evidence). On read, `ps` is trusted **only if `EEPROM[8] ==
       (uint8_t)~EEPROM[12]` AND `EEPROM[8] ≤ 250`; otherwise → UNKNOWN**. Virgin `0xFF/0xFF` →
       `0xFF != ~0xFF` → UNKNOWN on a never-armed cell, so the **arm** (writing `ps=0`,`~ps=0xFF`) is
       what establishes a valid baseline (see lifecycle).
   - **Backend = Microchip DEE** (verified, `libraries/EEPROM/utility/Deeprom.c`): a write
     **invalidates the old record and appends a new last-valid-wins record** — a torn *normal* write
     degrades to old-or-absent (not silent corruption); only a page **pack/erase** (rare, page-full)
     is a true non-atomic window. So a write is **record-transactional**, not byte-RMW — but NOT a
     guarantee, hence the `pf` sentinel + `ps` complement above (the honest replacement for the dropped
     "single-byte atomic" claim).
   - **Synchronous per-EPOCH SPIROV write — fixes the R2+R3 BLOCKER (fatal first AND N-th SPIROV):**
     write `ps` **synchronously in the SPI fault path, BEFORE the risky recovery cascade**, on the
     **first SPIROV of each checkpoint-epoch** — i.e. the first SPIROV since the last *durable* `%CKPT`
     (track `psWroteThisEpoch`, set on write, cleared whenever a `%CKPT` is committed to the cache).
     write **both** `ps = min(sdSpirovSeen,250)` @8 **and** `~ps` @12. This is the R3 synthesis of
     Codex#2 (epoch-bounded) + Gemini#1 (every fatal SPIROV captured): a fatal **first** SPIROV is
     recorded before recovery can hang (`ps≥1` while the last durable `%CKPT s=0`), and a fatal
     **N-th** SPIROV in a later epoch is also recorded (its epoch's first SPIROV bumps `ps` above the
     last durable `s=`), so `ps > last-ckpt s=` holds in both. Wear is **≤2 EEPROM writes per ~60 s
     epoch** (value+complement; a stuck-bus storm within one epoch still costs one pair; worst case ≪
     DEE endurance), and the ~ms DEE write is negligible in the already-faulted path. Subsequent
     SPIROVs in the same epoch only bump the RAM `sdSpirovSeen` (for the SD `s=`).
     - **Torn / silently-failed `ps`-write blind spot (R5 Codex#3 + R12 Codex#2 — acknowledged):** if
       the power-loss that IS the failure tears this `ps`/`~ps` write — OR the DEE write silently fails
       and leaves the old valid pair — next boot reads the stale `ps` (`ps=0` if this was the first
       SPIROV) → "no durably-recorded SPIROV" (NOT-bus). This is the SAME inherent blind spot and is
       self-consistent (a power/EEPROM event ate the write → pointing at power/other is defensible). A
       readback of the per-epoch pair (forcing `pf=0x3C` on mismatch) or a `ps-write-pending` sentinel
       would promote it to UNKNOWN — **optional /grill hardening if flash/latency allow; not required**
       (this is in the already-degraded fault path, so a readback adds latency exactly when the bus is
       sick).
   - **No EEPROM writes during steady-state 500 Hz sampling:** the only in-recording write is the
     per-epoch `ps`/`~ps` pair on a SPIROV (already-degraded path); the `pf` arm/clear writes bracket
     the recording (session start / clean stop) when no samples are flowing (Codex-R2#5 resolved).
   - **Lifecycle — crisp, no self-contradiction (R2 Codex#4):**
     - **Boot = CONSUME only, no mutation:** read prior `pf`/`ps` into locals; they are emitted in the
       next session's `%BOOT` (Decision 5). Nothing is reset at boot, so a boot that never records
       cannot fabricate a false state and a failed flush/open cannot erase evidence.
     - **Recording-session START = ARM, but only AFTER a CHECKED full-write + SD-sync of the new
       `%BOOT`** (carrying the prior `pf`/`ps`) (R1 Codex#5 + R3 Codex#4): the `%BOOT` line is
       bounded-formatted; if its truncation guard trips OR its write/sync is not confirmed, **abort
       the `openSDfile` sequence** and leave the prior EEPROM evidence intact — do **not** arm
       (R5 Gemini#4: never arm `pf=0xA5` without a durable `%BOOT` on disk). **On confirmed success,
       FIRST unconditionally init the RAM tracking state — `sdSpirovSeen=0` AND `psWroteThisEpoch=false`
       — before any EEPROM write (R8 Gemini#3: don't leak a stale `psWroteThisEpoch=true` into the new
       session if a later arm step fails), THEN arm in an INVALIDATE-FIRST order so any interruption
       leaves a durable UNKNOWN, never stale-valid (R6 Codex#3 + Gemini#2):**
         1. write `pf = 0x3C` (an explicit "arming/invalid" sentinel — decodes to `pf=?`/UNKNOWN) and
            **read back; this is a HARD PRECONDITION (R7 Codex#1)** — it must overwrite any stale prior
            `pf=0xA5`/`0x00`, else that stale-valid byte would masquerade as this session's outcome next
            boot. **Bounded retry (e.g. 3×); if `pf=0x3C` STILL can't be verified, ABORT `openSDfile`**
            (don't record) — a DEE that can't even invalidate is already failing the firmware's existing
            EEPROM bookkeeping (resume cap etc.), so this is not a new failure mode. (`pf=0x3C` confirmed
            is the gate; only the LATER steps may degrade-and-proceed.)
         2. write the pair `ps=0x00`@8 **and** `~ps=0xFF`@12; read back and verify **with the
            complement-aware getter** (`EEPROM[8]==0x00 && EEPROM[12]==0xFF`) — R6 Gemini#2: verifying
            only `ps==0` would miss a torn `~ps`. (`sdSpirovSeen`/`psWroteThisEpoch` already RAM-init'd.)
         3. write `pf=0xA5`; read back to confirm.
       If a readback fails at **step 2 or 3** (after `pf=0x3C` is confirmed), **leave the durable
       `pf=0x3C`** (next boot reads UNKNOWN, never stale evidence) and proceed recording with the
       breadcrumb flagged untrusted — the recording is the priority; the breadcrumb degrades to a
       *durable* UNKNOWN, not a RAM-only one (R6 Codex#3). The per-epoch SPIROV write (above) likewise
       writes **both** `ps` and `~ps`.
     - **Genuine clean stop = CLEAR:** `pf=0x00` only after the footer is written AND FAT/file
       metadata flushed — **never** on rollover/auto-resume/retry/failed/partial close (Codex-R1#8).
       A hang-before-clean-close therefore reliably leaves `pf=0xA5`.
5. **`%BOOT` append at the ABSOLUTE END (Codex 9):** append `pf=<0|1>` + `ps=<dec>` (the prior
   session's consumed values) after the existing resume-only trailing fields of **every** `%BOOT`
   variant (not "after `g=0x…`", which would reorder the trailing fields). Append-only; existing
   parsers skip unknown `k=v`. **Decode/print mapping (R3 Gemini#4 + R5 Codex#5)** — both fields are
   decoded from the raw EEPROM bytes in the `snprintf`, with explicit UNKNOWN cases (so a torn byte
   never masquerades as evidence):
   - **`pf`:** `0xA5→1`, `0x00→0`, **any other byte → `pf=?`** (UNKNOWN; `0xFF` virgin lands here too).
     The host sees `pf=1`, never raw `pf=165`.
   - **`ps`:** print the decimal **only if `EEPROM[8]==(uint8_t)~EEPROM[12]` AND `≤250`**; **anything
     else → `ps=?`** (UNKNOWN). This includes `0xFF/0xFF` (virgin OR torn/erased — both `ps=?`, R6
     Codex#1/Gemini#1: a special `0xFF/0xFF→0` case would let a torn erased cell masquerade as "no
     SPIROV"), a complement mismatch, and `251..254`. There is **no `ps=0` shortcut** — a genuine
     `ps=0` only ever appears as the explicitly-armed pair `0x00`/`0xFF` (which passes the complement
     check).
   - Add **parser fixtures for `pf=?` / `ps=?`** alongside the `s=`/`pf=`/`ps=` fixtures (Test 1).

   **Audit the `%BOOT` snprintf buffer (R4 Gemini#3):** the appended fields add ~12–15 chars — enlarge
   the `%BOOT` buffer if needed and apply the same spin-proof `n<=0 || n>=sizeof` guard as `%CKPT`
   (Decision 3). **If the `%BOOT` truncation guard trips, treat it as a write failure and ABORT the
   open sequence** (R5 Gemini#4) — never arm `pf=0xA5` without a durable `%BOOT` (Decision 4).
6. **Tune keys 0x07–0x0A** (host-tunable, no reflash) — explicit wire contract (R2 Codex#7); each
   gets a `tuneKeyValueLength` entry, an `applyTune` case that **validates the range and rejects
   before mutating** (out-of-range → `TUNE FAIL`, value unchanged), a `%TUNE` text keyword, and
   inclusion in `tuneSummaryHash()`:

   | key | name | width/endian | default | range | persist (resume `%TUNE`) | in `T=` hash |
   |-----|------|--------------|---------|-------|--------------------------|--------------|
   | 0x07 | `spi_deadline_us` | u16 LE | (fix const) | 50–5000 | yes | yes |
   | 0x08 | `fifo_headroom` | u8 | (fix const) | 1–7 | yes | yes |
   | 0x09 | `soft_wdt_floor_ms` | u32 LE | 120000 | 30000–600000 | yes | yes |
   | 0x0A | `soft_wdt_max_reboots` | u8 | (fix const) | 0–25 | yes | yes |
   | 0x0B | `fault_inject_arm` | u8 | 0 | 0–1 | **NO** | **NO** |

   (Exact widths/defaults reconciled against the fix's constants at /grill; the table is the binding
   contract.) Each tunable is wired to the fix's constant so the runtime value (not the `#define`) is
   used. **`T=` is NOT byte-identical for a no-tune session (Codex-R1#12):** extending the hash basis
   changes the default `T=` value; `T=` is a per-firmware-version fingerprint (cross-version
   comparison is meaningless). Bench expectation: a no-tune `-a 18` differs from the pre-flash
   firmware only in the new appended fields **and** the `T=` value.
7. **Flash-budget drop order (Codex 13), if `prep.md`+`prep-diag.md` > 122880 B:** (1) the
   fault-injector (arm key + token + injection); (2) the **`%TUNE` *text* keyword aliases** (keep the
   cheap binary tune keys); (3) verbose bench-log strings; (4) the least-useful tune keys
   (`soft_wdt_floor_ms`/`max_reboots` before `spi_deadline_us`/`fifo_headroom`). **Never drop:** the
   bounded `%CKPT s=` field, the `pf`/`ps` breadcrumb, the magic-protected record, or any
   host-contract string. (RCON capture is already in the firmware — not a budget item.) Record
   per-component sizes + any drop in the `/grill` log.
8. **Runtime-gated synthetic-hang self-test — user's choice, HARDENED; panel dissents (see below).**
   The injection ships in the **production** hex, gated by ALL of: (a) tune key `0x0B`
   `fault_inject_arm=1` (u8, validated `0–1` reject-before-mutate; session-scoped, default 0, **never
   persisted** to the resume `%TUNE`, **excluded from `tuneSummaryHash`** — Decision 6 table); **and**
   (b) the dedicated **8-byte magic token** matched by its **OWN independent state index
   `injectState`** — NOT the shared `ESC_TOKEN` matcher (R2 Gemini#4: a single-index matcher can't
   track two tokens — a partial match on one resets the other), so a parallel `injectState` (or an
   8-byte sliding compare) runs alongside the escape matcher; **and** (c) **one-shot, bounded**: when
   armed AND the token completes, set `sdInjectStuckOnce`; the **next bulk write consumes and clears
   it whether or not it fired** (no indefinitely-pending injection — R2 Codex#6), and **both
   `fault_inject_arm` and `sdInjectStuckOnce` are force-cleared at every session boundary (start AND
   stop)**. The single injection forces a genuine `SPI1STAT.SPIROV` → exercises the real
   detect→flush→`sdBusRecover`→retry path, logging `SPI1STAT/CON` pre/post over Serial0 on the bench.
   **Production-night guarantee + HONEST residual (R3+R5+R6 Codex#6/#5):** the injector
   (arm-accept + token-match + fire) is **hard-gated on (i) `curSampleRate ≤ 250 SPS` AND (ii) RF up**.
   The **production overnight runs at 500 SPS / RF OFF → the injector is PROVABLY unreachable there**
   (both gates false) — the case that matters for the recorder. **Honest residual (R6 Codex#5 — I do
   NOT claim a bench-vs-normal "predicate"):** any **≤250 SPS RF-up session** (a daytime/QA RF run) is
   *not* distinguishable from a deliberate bench test by the firmware, so there a *host bug* could in
   principle arm `0x0B` and emit the exact 8-byte token within the short arm-expiry window. Bounds on
   that: (a) it needs the precise reserved token by accident, (b) the arm self-clears quickly and is
   one-shot, and crucially **(c) an accidental fire is NON-DESTRUCTIVE** — it forces one SPIROV the fix
   *recovers from* (a logged `%E` + `ps` bump, ~ms–s glitch), never data loss. This **supersedes**
   `prep.md`'s compile-time `#ifdef SD_DEBUG_FAULT_INJECT`. The reserved token + a parser
   non-collision/non-consumption test are bench-verified (Test plan); the token must not be a
   prefix/substring overlap with `ESC_TOKEN` or any `%` protocol, and the matcher uses its **own**
   `injectState` (never the shared `ESC_TOKEN` index). **The `#ifdef` (deferred-to-user) eliminates
   this residual entirely** — see the dissent note.
   > **PANEL DISSENT (Codex + Gemini) — flagged in R1, R3, R5 AND R6 (FOUR rounds); DEFERRED TO USER,
   > now a STRONG recommendation to switch.** Every round the reviewers argued this should be a
   > compile-time `#ifdef` (0 production bytes, no hidden corruptor in the shipped hex, and it makes the
   > entire RF-up-reachability residual — R5/R6 Codex — *moot*). Gemini-R6 put it bluntly: with no plan
   > to fire the injector on a live subject, shipping the trigger in the production hex is unnecessary
   > flash + risk. **The recovery PATH is byte-identical in a `#ifdef`-out production build and a
   > one-time `-DSD_DEBUG_FAULT_INJECT` bench build, so a debug-build bench test still validates the
   > production recovery** — which substantially meets the user's "exact hex" goal. It is kept as the
   > runtime gate **per the user's explicit prior decision**, hardened as above and **drop #1** under
   > flash pressure, BUT given four independent flags I recommend the user choose the `#ifdef` at
   > /grill (also reclaims flash toward the ~4280 B budget). The user's call.

## Plan
Same branch as the fix (`grill/cyton-sd-recover`); edits interleave with `prep.md`'s.
1. **`%CKPT`** (`SD_Card_Stuff.ino:1812`): enlarge `tmp[160]`, add `s=%lu` **immediately before the
   `T=%08lx` field** (arg `sdSpirovSeen`; keep `T=` the terminator — R7 Gemini#2), bound the copy
   (Decision 3). Re-verify worst-case length.
2. **Fork primitive** (`Sd2Card.cpp`): increment `sdSpirovSeen` **only** on a confirmed
   `SPI1STAT.SPIROV` (Decision 2); on the **first SPIROV of each checkpoint-epoch**, write the **PAIR
   `ps=min(sdSpirovSeen,250)`@`EEPROM[8]` AND `~ps`@`EEPROM[12]` SYNCHRONOUSLY, right here in the fault
   path, before the recovery cascade** (NOT a RAM flag, NOT deferred to `loop()`) — gated by
   `psWroteThisEpoch` (Decision 4); the always-compiled, runtime-gated injection hook (Decision 8).
   Record in `patches/sd-fork-write-timeout.patch`.
3. **EEPROM breadcrumb = THREE standalone bytes** (`ps`@`EEPROM[8]` + complement `~ps`@`EEPROM[12]` +
   `pf`@`EEPROM[9]`, sentinel/complement-checked) — **decoupled from `prep.md`'s record at slots 2,3**
   (Decision 4): after the checked `%BOOT` write+sync, **unconditionally RAM-init `sdSpirovSeen=0` +
   `psWroteThisEpoch=false` FIRST** (R8 Gemini#3), then arm in the **invalidate-first order** (write
   `pf=0x3C`+verify = **HARD precondition**, bounded-retry-else-abort R7 Codex#1 → write
   `ps=0`/`~ps=0xFF`+complement-aware-verify → write `pf=0xA5`+verify); on a step-2/3 readback failure
   leave the durable `pf=0x3C` UNKNOWN sentinel + flag untrusted (R6 Codex#3); clear `pf=0x00` only on a
   genuine clean stop; clear `psWroteThisEpoch` **the moment a `%CKPT` is copied into `pCache`** (R4 Gemini#5).
   A read decodes `ps` only if `EEPROM[8]==(uint8_t)~EEPROM[12] && ≤250`, else UNKNOWN (Decision 5).
4. **`%BOOT` emit** (`SD_Card_Stuff.ino` ~1442+): read the record, append `pf=`/`ps=` at the
   absolute end; flush `%BOOT` to SD **before** resetting/re-arming (Decision 5).
5. **Tune keys 0x07–0x0A** + `%TUNE` keywords + `tuneSummaryHash` (Decision 6); wire each tunable to
   the fix's constant.
6. **Fault-injector** (Decision 8): `TUNE_KEY_FAULT_INJECT_ARM 0x0B`, 8-byte token matched by its
   **own independent `injectState`** (NOT the shared `ESC_TOKEN` matcher), one-shot auto-disarm,
   ≤250 SPS+RF-up gate, short arm expiry, cleared at every session boundary. **Per the user's
   /grill choice: keep runtime OR (panel-recommended) `#ifdef SD_DEBUG_FAULT_INJECT` it out of the
   production hex** (Decision 8 — the recovery path is byte-identical either way, so a one-time debug
   build still validates production recovery).
7. **Build + size** (Decision 7): combined build, per-component sizes, apply drop order only if
   > 122880 B.

## Files to touch
- `examples/DefaultBoard/SD_Card_Stuff.ino` — `%CKPT` `s=`+buffer bound (Decision 3); `%BOOT`
  `pf=`/`ps=` with sentinel/complement decode→`0|1|?`/`dec|?` + buffer audit + truncation-aborts-open
  (Decision 5); standalone EEPROM **`ps`@8 + `~ps`@12 + `pf`@9** invalidate-first arm + complement-aware
  readback + clean-close clear + `psWroteThisEpoch` reset on `%CKPT` pCache-copy (Decision 4); tune
  keys 0x07–0x0A + `%TUNE` keywords + `tuneSummaryHash` (Decision 6); fault-inject arm key + independent
  token matcher (Decision 8).
- `~/Arduino/libraries/OpenBCI_32bit_SD/utility/Sd2Card.cpp` — `sdSpirovSeen` increment gated on
  `SPI1STAT.SPIROV`; the SYNCHRONOUS per-epoch **`ps`@8 + `~ps`@12 PAIR** write in the fault path (no
  RAM flag, no `loop()` deferral); runtime-gated injection hook. Record in
  `patches/sd-fork-write-timeout.patch`.
- `examples/DefaultBoard/DefaultBoard.ino` — only if the breadcrumb read/EEPROM-write lands in
  `setup()`/`loop()` glue.
- `CLAUDE.md` — document `s=`/`pf=`/`ps=`, the discriminator truth table, the new tune keys, the
  runtime fault-injector, that `T=` is opaque per-version, and the accepted power-vs-other-hang
  limitation.
- `prep.md` — one-line pointer that the diagnostics layer ships in the same flash and supersedes its
  compile-time self-test; note this plan's breadcrumb uses SEPARATE standalone bytes (ps@8/~ps@12/
  pf@9), decoupled from prep.md's record at slots 2,3 (just coordinate the address map).

## Test plan
1. **Host-contract fixtures FIRST (Codex 10):** grep `session_start.py` + the SD post-processors for
   regexes that anchor `%CKPT`/`%BOOT`/`T=` layout (fixed field order, `T=<hex8>$` end-anchors);
   add/confirm fixtures that the appended `s=`/`pf=`/`ps=` fields parse and that no anchor breaks —
   **before** the firmware change is accepted. **Also (R10 Codex#3):** the `%E` adjacency signal must
   parse `%E` as a **line-framed marker** (e.g. `^%E$` per line), NOT a raw substring scan — add a
   fixture with literal `%E` bytes embedded in non-marker EEG payload to confirm they're not
   miscounted as recovery markers.
2. Build combined; per-component size; total < 122880 (record; apply drop order if needed).
3. Codex + Gemini panel (read-only; GLM not installed on z13). No flash until both approve AND the user OKs.
4. **Bench (after user OK):** clean `-a 18` → `%CKPT s=0` + `%BOOT` with `pf=`/`ps=`; set each new
   tune key → `TUNE OK`, out-of-range → `TUNE FAIL` (value unchanged); **token-collision test:** send
   the 8-byte magic token **while disarmed** and confirm it does NOTHING and is not consumed as/by
   `ESC_TOKEN` or a `%` protocol; **arm `0x0B` + send the token** → one real SPIROV → fix recovers,
   `s=` increments, `%E` logs, `SPI1STAT/CON` pre/post printed, then a **second** token does NOTHING
   (one-shot consumed) and a `--stop` confirms the arm was force-cleared at the session boundary;
   power-cycle mid-`-a 18` (after at least one injected SPIROV) → next `%BOOT` reports `pf=1` + `ps≥1`
   (with `seq=` matching), proving **the breadcrumb persists across a power-cycle after a *recovered*
   SPIROV** (R10 Codex#2: the injector recovers by design, so this validates persistence + the
   `ps`/`pf`/seq path — NOT a fatal pre-`%E` hang, which the injector can't stage). Confirm a no-tune
   `-a 18` differs from the pre-flash firmware **only** in the appended fields + the `T=` value (Decision 6).
5. **Final = the overnight `-a 20`** — run the morning classifier **exactly as the discriminator
   section above (the full v12 procedure — do NOT re-specify it here; R12 Codex#1: a stale duplicate
   could reintroduce the pre-arm false-BUS).** In brief: clean footer → done; **no footer +
   `pf=0x00` → UNKNOWN** (contradiction, first); trust `ps` ONLY per the full TRUST GATE (`pf==0xA5`
   **and** seq-correspondence **and** substantial recording-progress); then **BUS** if `%E`
   ≤4 BLK to EOF, or `s≥250`/trusted `ps==250`, or trusted `ps>last-durable-s=` **without** a ≥20 BLK
   clean run after the last `%E` (with such a clean run → **UNKNOWN**); **NOT-bus** if ≥20 BLK clean
   after the last `%E` or trusted `0<ps≤s=`, or trusted `ps==0`+no `%E`; else **UNKNOWN** (5–19 BLK
   gray band, or untrusted/UNKNOWN `ps` with no decisive SD signal). Every not-bus/UNKNOWN/low-confidence
   outcome → enable the OFF-by-default soft-WDT for the next night while chasing the non-bus cause.

## Risks & open questions
- **Power-vs-other-hang stays undistinguished** (Decision 1) — accepted; no cheap hardware signal on
  this part (RCON bootloader-cleared, no HLVD). The soft-WDT escalation is the operational answer.
- **EEPROM layout coordination with `prep.md`** — prep.md's complement record at slots 2,3; this
  plan's standalone `pf`@9, `ps`@8 + `~ps`@12 (decoupled). Both plans confirm the final map at /grill.
- **DEE non-atomicity at pack/erase** — the rare page-pack is the only true non-atomic window; a
  power loss there can scramble a record. Mitigated by the `pf` sentinel (2/256 valid) + the `ps`
  value+complement check → a non-decodable read is classified **UNKNOWN**, never proof (Decision 4 +
  discriminator). Residual torn-`ps`-write-during-power-loss blind spot is acknowledged (Decision 4).
- **Discriminator has an honest UNKNOWN bucket** — SPIROV-at-tail and corrupt-evidence cases are
  UNKNOWN, not forced into bus/not-bus; the operational response (enable the soft-WDT next night) is
  the same as NOT-bus, so an UNKNOWN never blocks progress.
- **`%CKPT`/`%BOOT` length** — verify the worst-case appended line vs `tmp[160]` + the 512-cache
  guard (Decision 3) and against the host fixtures (Test 1).
- **`tuneSummaryHash` basis change** breaks cross-version `T=` comparison (intended; documented).
- **Runtime fault-injector** — gated (arm key + token + one-shot auto-disarm + arm-not-persisted +
  ≤250 SPS/RF-up bench predicate + arm expiry, **independent `injectState` matcher**); provably
  unreachable in the 500 SPS/RF-off production night, accidental fire is non-destructive. Panel
  flagged it R1/R3/R5 and prefers `#ifdef` — **recommended deferred-to-user reconsideration**.
- **Combined flash budget** — the real risk; measured at /grill, drop order pre-agreed (Decision 7).

## Out of scope
- VDD/brownout ADC monitoring and HLVD (dropped/infeasible — Decision 1).
- Improving RCON capture (already as-early-as-possible; bootloader-cleared — Decision 1).
- Any live-telemetry mechanism at 500 Hz (no RF link there).
- The SPIROV fix itself (that's `prep.md`).
- Flashing / starting any recording without explicit user OK.
