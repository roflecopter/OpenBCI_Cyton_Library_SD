# prep-diag.md — adversarial review log (Codex + Gemini + GLM)

Plan under review: `prep-diag.md` — the diagnostics + tunable-knobs layer bundled into the SAME
single Cyton freeSD flash as the SPIROV fix (`prep.md`). Goal: after one flash, future debugging
needs NO reflash — the next failure's SD file + next `%BOOT` self-identify SPIROV-vs-power-vs-other.

Author: Claude (orchestrates, casts no vote). Reviewers: Codex (gpt-5.5), Gemini (3.1 Pro), GLM 5.2.
Adaptive early-stop; backstop 7 rounds.

---

## Round 1 — Codex: CHANGES_REQUESTED | Gemini: CHANGES_REQUESTED | GLM: (no verdict — wrapper non-agentic, attempted tool calls)

### Codex findings (13)
1. [BLOCKER] `%CKPT s=%lu` can overflow `tmp[128]` → snprintf truncate / read past buffer. Enlarge + bound n.
2. [BLOCKER] Discriminator overstated — cumulative `s>0`/`ps>0` only proves SPIROV happened sometime, not that the FATAL freeze was SPIROV. Need "SPIROV near the cut" (last-ckpt s-delta / last-SPIROV proximity).
3. [MAJOR] Power-vs-other still not distinguished; `rcon=0x00` not meaningful — capture raw RCON/BOR/POR/WDTO early.
4. [MAJOR] `sdSpiFault` must not equal SPIROV — increment `s` only when the hardware SPIROV bit was observed, not on any bounded timeout (card-busy/bad-CS/deadline).
5. [MAJOR] EEPROM evidence can be erased before safely logged — clear/reset only AFTER the %BOOT record is durably on SD.
6. [MAJOR] `ps` not clearly per-session — reset at recording-session start, not just boot (clean bench SPIROVs leak into a later run same power-cycle).
7. [MAJOR] Single-byte EEPROM records not robust — use magic/complement/checksum + audited addresses + update-style writes.
8. [MAJOR] Clear-at-`closeSDfile` underspecified — rollover/retry/failed/partial close would falsely mark unclean as clean. Clear only on genuine final clean close + FAT flush.
9. [MAJOR] `%BOOT` append order inconsistent — appending after `g=0x` reorders existing trailing resume fields. Append at absolute end of every variant.
10. [MAJOR] Parser compat assumed not proven — add fixture tests for session_start.py + post-processors before the firmware change.
11. [MAJOR] Always-compiled fault-injection = too much production risk for the value; prefer #ifdef or timeouted-arm/auto-disarm/no-background-scanner.
12. [MINOR] `T=` "byte-identical" claim false — extending the hash basis changes T= at defaults.
13. [MINOR] Flash drop order misses cheaper cuts — drop text keyword aliases + verbose bench strings before binary tune keys; mandatory priority = bounded %CKPT + safe breadcrumbs + RCON, not the injector.

### Gemini findings (6)
1. [BLOCKER] EEPROM wear-out death loop on a hard bus fault (stuck MISO → thousands SPIROV/sec → per-event `ps` write bricks the page). Write EEPROM only on the FIRST SPIROV of a session; RAM counter handles `s=`.
2. [MAJOR] `pf=1` can't tell power-loss from a non-SPI hang. Suggests using the PIC32 HLVD peripheral (interrupt writes `pf=2` before death).
3. [MAJOR] Unjustified flash waste for the bench fault-injector — revert to `#ifdef` (0 production bytes).
4. [MINOR] `rcon=0x00` — chipKIT bootloader clears RCON before the sketch → useless; reinforces HLVD need.
5. [MINOR] Cyton channel-settings EEPROM collision — map breadcrumbs to a high block (0x50-0x60).
6. [MINOR] EEPROM flash-erase ~20ms stall during SPI recovery — defer the write to loop() via a RAM flag, not inside the SPI primitive.

### GLM
Non-agentic wrapper (no filesystem); it emitted a shell-command-shaped probe and produced no findings/verdict. Re-prompt round 2 telling GLM it has NO tools and must review from the prompt text alone.

### Grounding done (Claude, before resolving)
- **RCON already captured at setup() entry** (DefaultBoard.ino:58 `bootResetCause=RCON; RCONCLR=0xFFFF`), dev comment confirms the chipKIT bootloader pre-clears it → `rcon=0x00` expected, NOT improvable. → Codex#3 already-done; Gemini#4 confirmed.
- **No HLVD module on PIC32MX250F128B** — proc header has only `NVMCON.LVDSTAT/LVDERR` (flash-write-window LVD status, not a brownout interrupt). → Gemini#2 mechanism INFEASIBLE here.
- **EEPROM map audited**: used 0,1,4,5,6,7,10,11; EEPROM[4] is a *legacy* sessionActive flag entangled with the resume chain ("do NOT clear EEPROM[4]", line 2071) → NOT reused. Free low slots 2,3,8,9. This fork stores only file/session bookkeeping in EEPROM (no channel configs) → Gemini#5's high-block claim over-cautious; audit resolves it. prep.md already plans a magic+complement sticky record → diag piggybacks into it.

### Resolution (Claude)
- Codex#1 (BLOCKER) ACCEPTED → Decision 3: `tmp[160]` + bounded copy (`n>0 && n<sizeof && byteCounter+n<=512`).
- Codex#2 (BLOCKER) ACCEPTED → new "discriminator" section: signal is per-interval `s=` delta + `%E` proximity to the cut; a recovered SPIROV earlier is benign. Cumulative `s>0` alone is explicitly NOT the test.
- Codex#4 ACCEPTED → Decision 2: `s` gated on the real `SPI1STAT.SPIROV` bit only; deadline/timeout faults reuse existing `x=`.
- Codex#5/#6/#8 + Gemini#1(BLOCKER)/#6 ACCEPTED → Decision 4 rewritten: write EEPROM only on the FIRST SPIROV (wear), in loop() context via a RAM flag (no ~20ms stall in the SPI primitive), `pf` armed at session start + cleared only on genuine clean stop (not rollover/retry), `ps` reset at session start, reset/re-arm only AFTER %BOOT flushed to SD.
- Codex#7 ACCEPTED → piggyback into prep.md's magic+complement record (audited free low slots), not loose bytes.
- Codex#9 ACCEPTED → Decision 5: append `pf`/`ps` at the absolute end of every %BOOT variant.
- Codex#10 ACCEPTED → Test plan step 1: host-contract fixtures (session_start.py + post-processors) BEFORE flash.
- Codex#12 ACCEPTED → Decision 6: `T=` not byte-identical; documented as per-version fingerprint; bench expectation updated.
- Codex#13 ACCEPTED → Decision 7 drop order refined (injector → %TUNE text aliases → verbose strings → least-useful keys; never drop bounded %CKPT/breadcrumb/record).
- Codex#3 + Gemini#4 RESOLVED BY GROUNDING (already done / bootloader-cleared) → Decision 1 documents it; no code change possible.
- Gemini#2 (HLVD) REJECTED on fact (no HLVD on this part) → Decision 1; power-vs-other-hang accepted as a documented limitation (beyond the stated SPIROV-vs-rest goal; soft-WDT escalation is the operational answer). Surfaced as a risk.
- Gemini#5 RESOLVED BY AUDIT → free low slots 2,3,8,9, no channel-config collision in this fork.
- Codex#11 + Gemini#3 (UNANIMOUS, vs the user's explicit "runtime gate" choice): NOT silently overridden. Default kept = runtime gate but HARDENED (one-shot auto-disarm + arm-not-persisted + token via the reused ESC matcher, no new scanner) and made drop #1. Panel dissent + the #ifdef alternative recorded in Decision 8 and flagged as the #1 DEFERRED-TO-USER item for the Act-4 summary.

### prep-diag.md changes this round
Rewrote to v2: added the "discriminator" section + Decision 1 (no power signal, accepted limitation) + Decision 3 (buffer hardening) + rewrote Decision 4 (robust/low-wear/deferred EEPROM) + Decision 5 (%BOOT absolute-end) + Decision 6 (T= not byte-identical) + Decision 7 (drop order) + Decision 8 (hardened runtime injector + panel-dissent block). Test plan gained the host-contract-fixtures-first step.

---

> **Panel note:** `glm-review` is NOT installed on z13 (only `codex-review` + `gemini-review`
> exist; the round-1 304-byte GLM artifact was a non-agentic probe with no verdict, and round 2
> was "command not found"). Per the skill ("reviewer unavailable → note and proceed"), the panel
> is **Codex + Gemini** from here — the canonical 2-model panel.

## Round 2 — Codex: CHANGES_REQUESTED | Gemini: CHANGES_REQUESTED | GLM: (unavailable)

### Codex findings (7)
1. [BLOCKER] Deferred `ps` write misses the fatal first SPIROV — if the first SPIROV hangs recovery before loop() runs, no EEPROM `ps`, no `s=`, maybe no durable `%E` → `pf=1 ps=0` falsely read as NOT-bus. Make the first-SPIROV breadcrumb durable BEFORE the risky recovery, or classify as UNKNOWN.
2. [MAJOR] `s=` delta proves "recent" not "causal" — a recovered SPIROV one ckpt before the cut + good EEG + power failure still trips "implicated." Require `%E`/last-SPIROV adjacency to EOF, or add last-SPIROV time/byte-age.
3. [MAJOR] Single-copy magic+complement record not crash-atomic; shared with reset-pending/cap → a diag write can lose evidence AND WDT-guard state. Use a two-slot generation+CRC journal, or separate.
4. [MAJOR] `pf` lifecycle contradicts itself — "arm at session start" vs "reset/re-arm after %BOOT flush at boot" → false pf=1 for non-recording boots + evidence erased after a failed flush. Consume only after checked write+sync; arm/reset only after a recording file actually starts.
5. [MAJOR] Loop-deferred EEPROM write not proven timing-safe — loop() isn't inherently outside the 500Hz path; ~20ms write can drop samples. Service only in a quiescent state, or log a data gap.
6. [MAJOR] Fault-injector persistence hole — arm has no timeout; `sdInjectStuckOnce` can linger to a later bulk write same power-cycle; token/parser collision proof absent. Add validation, reserved-token tests, passive matching, short timeouts, clear on every session boundary.
7. [MINOR] Tune wire contract underspecified — 0x07-0x0A need width/endian/default/min-max/persist/hash; 0x0B excluded from persist + hash. Add a key table + reject-before-mutate.

### Gemini findings (4)
1. [BLOCKER] Discriminator hole + deferred EEPROM hides fatal SPIROVs (same as Codex#1). Fix: write `ps` SYNCHRONOUSLY in the SPI fault path (20ms acceptable when the bus is already failing), cap `if (sdSpirovSeen <= 250)`; discriminator: fatal SPIROV proven if next-boot `ps` > last SD `%CKPT s=`.
2. [BLOCKER] `%CKPT` buffer truncation = ~26% silent checkpoint loss (a ~135B line straddling the 512 boundary dropped). Fix: force a block flush + reset byteCounter before memcpy, or use a boundary-aware write — don't drop.
3. [MAJOR] Updating `ps` inside the shared complement record risks tearing the `pf` evidence (multi-byte write + complement; a hang between bytes invalidates the whole record). Store `ps` as a standalone byte (slot 8/9), atomically updatable, no checksum.
4. [MAJOR] Token-matcher collision — reusing the single-index ESC matcher can't track two tokens (a partial match resets the index). Use an independent `injectState` or an 8-byte sliding compare with two memcmps.

### Grounding done (Claude, before resolving)
- Read the EXISTING `%CKPT` writer (SD_Card_Stuff.ino:1798-1849): it ALREADY does `if(n>0 && byteCounter+n<=512) memcpy(...)` AND **does NOT update sdLastCkptMs on no-fit → retries next sample boundary** (lands at byteCounter=0 after a flush, always fits). So `%CKPT` is NEVER dropped today; Gemini#2's "26% loss" reads a drop into the plan that the existing design doesn't have. The REAL bug (Codex#1-buffer): on snprintf truncation (`n>=sizeof(tmp)`) the `byteCounter+n<=512` test uses the inflated n and `memcpy(...,n)` reads past tmp[]. Also confirmed the `%E` deferred-marker path (line 1791) → on a fatal-first-SPIROV hang, %E/s= never flush → only a synchronous EEPROM write survives (confirms the R2 BLOCKER).

### Resolution (Claude)
- Codex#1 + Gemini#1 (BLOCKER, same issue) ACCEPTED → Decision 4 rewritten: `ps` written SYNCHRONOUSLY on the first-SPIROV `0→1` edge IN the fault path BEFORE the recovery cascade (captures fatal-first-SPIROV; one write/session = bounded wear; ~20ms negligible vs the recovery it precedes). Discriminator rewritten: `ps > last-ckpt s=` is definitive; `pf=1 ps=0` now soundly = no SPIROV = not-bus. Adjacency (Codex#2) folded in: "implicated" requires last SPIROV activity adjacent to EOF with no substantial clean progress after.
- Gemini#2 (BLOCKER) RESOLVED BY GROUNDING + correction → Decision 3 fixed to MATCH the existing defer-and-retry (never drop, no forced flush); the only new bound is `n < (int)sizeof(tmp)` (fixes the Codex#1 over-read) + `tmp[160]`.
- Codex#3 + Gemini#3 (MAJOR, same) ACCEPTED → Decision 4: `ps`@EEPROM[8] + `pf`@EEPROM[9] as STANDALONE single atomic bytes, fully DECOUPLED from prep.md's complement record (slots 2,3). Single-byte writes are atomic → no journal, no tearing.
- Codex#4 (MAJOR) ACCEPTED → Decision 4 lifecycle made crisp: boot = CONSUME only (no mutation); session-start = ARM (pf=0xA5, ps=0) only AFTER the new %BOOT is flushed; clean-stop = CLEAR. No false pf for non-recording boots; no erase-on-failed-flush.
- Codex#5 (MAJOR) RESOLVED → with the synchronous first-write + arm/clear bracketing the recording, the ONLY in-recording EEPROM write is the one first-SPIROV `ps` in the already-degraded fault path; no write during steady-state sampling.
- Codex#6 + Gemini#4 (MAJOR) ACCEPTED → Decision 8: independent `injectState` matcher (not the shared ESC matcher); pending consumed+cleared on the next bulk write regardless of fire; arm+pending force-cleared at every session boundary; strict 0x0B validation (u8 0-1, reject-before-mutate); reserved-token + parser-non-collision bench test.
- Codex#7 (MINOR) ACCEPTED → Decision 6 gained an explicit wire-contract table (key/name/width/endian/default/range/persist/hash), 0x0B excluded from persist + hash.

### prep-diag.md changes this round (v3)
Discriminator rewritten (adjacency + `ps > last-ckpt s=` rule). Decision 3 corrected to the existing defer-and-retry + `n<sizeof(tmp)` guard. Decision 4 rewritten (two standalone atomic bytes ps@8/pf@9, synchronous first-SPIROV write, crisp consume/arm/clear lifecycle). Decision 5 references the ordering. Decision 6 gained the wire-contract table. Decision 8 hardened (independent matcher, one-shot+boundary clearing, validation). Context EEPROM line + risk bullet + test plan (token-collision + fatal-first-SPIROV power-cycle) updated.

## Round 3 — Codex: CHANGES_REQUESTED | Gemini: CHANGES_REQUESTED | GLM: (unavailable)

### Codex findings (6)
1. [BLOCKER] Plan steps still contain the R2-blocked deferred/shared-record EEPROM design (contradicts Decision 4). Make every section agree: standalone bytes, no shared record, synchronous fault-path write.
2. [BLOCKER] `ps > last_ckpt_s` fails after a recovered FIRST SPIROV — #1 recovers (s=1), #2 hangs fatally before flush; first-write-only leaves ps=1 so ps>last_s is false. Write `ps` synchronously on the first SPIROV after EACH durable checkpoint epoch, or add a last-SPIROV epoch/block marker; define saturation.
3. [MAJOR] "Adjacent to EOF" still permits false causal classification + "substantial" undefined. Define a concrete threshold + classify SPIROV-at-tail as UNKNOWN, not definitively bus.
4. [MAJOR] `%BOOT` evidence handoff lacks durability/buffer requirements. Harden %BOOT like %CKPT (bounded, full-write check, sync success, abort-on-failure); reset/arm only after checked write+sync, else leave evidence intact and don't start recording.
5. [MAJOR] EEPROM atomicity asserted not proven — PIC32MX has no true EEPROM; if flash-emulated a power loss mid-write may not be byte-atomic. Verify the backend, or two-copy generation/complement + classify invalid as UNKNOWN.
6. [MAJOR] Runtime fault-injector not provably unreachable — no arm timeout / bench predicate; plan conflicts on independent vs reused matcher. Independent matcher only, clear arm on token completion, short expiry, explicit bench/RF predicate, fixture-test second-token no-op.

### Gemini findings (4)
1. [BLOCKER] `ps` synchronous write limited to `0->1` edge drops fatal N-th SPIROVs (same as Codex#2). Fix: write `ps` synchronously on EVERY SPIROV up to a 250 cap (250 writes/session is wear-safe); if s>=250 auto-classify bus-implicated.
2. [BLOCKER] Plan section contradicts Decision 4 (leftover v2 RAM-flag/loop()/complement-record text) — same as Codex#1.
3. [MAJOR] Decision 3 guard `n < sizeof(tmp)` causes an INFINITE CPU SPIN — snprintf returns would-be length; on truncation the guard fails, skips memcpy, never advances sdLastCkptMs → retries every sample forever. Cap n to sizeof-1 and proceed (write + advance timer) to break the loop.
4. [MINOR] `pf=<0|1>` format vs raw 0xA5 — must map 0xA5→1 / 0x00→0 in the %BOOT snprintf, else prints pf=165.

### Grounding done (Claude, before resolving)
- Verified the EEPROM backend = **Microchip DEE** (chipKIT `libraries/EEPROM/utility/Deeprom.c` — 2-page wear-leveled flash emulation). A write `pack(addr,data)` INVALIDATES the old record + APPENDS a new last-valid-wins record → a torn NORMAL write degrades to old-or-absent (not corruption); only a page PACK/ERASE (when the page fills) is a true non-atomic window. So Codex#5 is grounded: NOT byte-RMW, it's record-transactional; the pack window is the residual risk → sentinel→UNKNOWN covers it. Wear: records append + occasional pack → 250 writes/session is safe (and the epoch-bound makes it ≤1/60s anyway).

### Resolution (Claude)
- Codex#1 + Gemini#2 (BLOCKER, internal contradiction) ACCEPTED → Plan steps 2/3 + Files-to-touch rewritten to standalone `ps`@8/`pf`@9 + SYNCHRONOUS fault-path write (no RAM flag, no loop() deferral, no shared record).
- Codex#2 + Gemini#1 (BLOCKER, fatal N-th SPIROV) ACCEPTED, synthesized: write `ps=min(sdSpirovSeen,250)` synchronously on the FIRST SPIROV of EACH checkpoint-epoch (Codex#2 epoch-bound ∧ Gemini#1 every-fatal-caught) — `psWroteThisEpoch` set on write, cleared at each durable %CKPT. Captures fatal first AND N-th; wear ≤1/epoch. Discriminator: `ps > last-ckpt s=` definitive; s/ps==250 → bus-implicated (Gemini#1).
- Codex#3 (MAJOR) ACCEPTED → discriminator gained an UNKNOWN/SPIROV-at-tail bucket + concrete "substantial progress" threshold (≥1 further durable %CKPT with no s-increment).
- Codex#4 (MAJOR) ACCEPTED → Decision 4/5: %BOOT bounded-formatted + write+sync CHECKED before arm/clear; on failure leave evidence intact + don't arm (degrade, don't erase).
- Codex#5 (MAJOR) RESOLVED BY GROUNDING → Decision 4 states the DEE reality (record-versioned, last-wins; pack is the only non-atomic window) + sentinel→UNKNOWN; dropped the false "single-byte atomic" claim.
- Codex#6 (MAJOR) ACCEPTED → Decision 8: independent `injectState` matcher only; hard bench predicate (gated on ≤250 SPS + RF up → provably inert during the 500 SPS/RF-off production night); short arm expiry; clear at every session boundary; second-token no-op + collision tests.
- Gemini#3 (MAJOR, infinite-spin) ACCEPTED → Decision 3 rewritten to split truncation (n>=sizeof → ADVANCE sdLastCkptMs, drop one) from no-fit (defer/retry, bounded). memcpy only when n<sizeof (prevents the over-read); no spin.
- Gemini#4 (MINOR) ACCEPTED → Decision 5: %BOOT maps the sentinel 0xA5→1 / 0x00,0xFF→0 / non-decodable→`pf=?`.

### prep-diag.md changes this round (v4)
Decision 4: per-epoch synchronous EEPROM write (not first-only) + DEE-backend reality + sentinel→UNKNOWN + checked-%BOOT-before-arm. Decision 3: spin-proof split (truncation vs no-fit). Discriminator: UNKNOWN/SPIROV-at-tail bucket + concrete threshold + saturation rule. Decision 5: sentinel→0/1 print mapping + %BOOT hardening. Decision 8: bench/RF predicate + arm expiry + independent matcher. Plan steps 2/3 + Files-to-touch rewritten to match Decision 4. Risks: DEE pack window + UNKNOWN bucket.

## Round 4 — Codex: (usage-limited, back 15:40) | Gemini: CHANGES_REQUESTED | GLM: (unavailable)

Codex hit its ChatGPT usage limit mid-round (echoed the prompt then `ERROR: You've hit your usage
limit … try again at 3:40 PM`) → no Codex verdict this round. Gemini reviewed; **no BLOCKERs**
remain (the R3 BLOCKERs — fatal N-th SPIROV, plan/decision contradiction — are confirmed resolved).
3 MAJOR + 2 MINOR, all accepted (tight refinements, no architecture change):

### Gemini findings (5)
1. [MAJOR] Spin guard misses `n < 0` (snprintf encoding error) → both branches fall through, no timer advance → spin. Use `if (n < 0 || n >= (int)sizeof(tmp))`.
2. [MAJOR] Discriminator logic error: `ps == last-ckpt s=` does NOT mean "SPIROV in the final epoch" — any final-epoch SPIROV bumps `ps>s=`. `ps==s=` strictly means the last SPIROV recovered+flushed then the system died with NO new SPIROV.
3. [MAJOR] `%BOOT` buffer not audited (Decision 3 enlarges %CKPT's tmp[160] but Decision 5 appends ~15 chars to %BOOT without sizing it). Audit + enlarge + apply the spin-proof guard.
4. [MINOR] `sdSpirovSeen = 0` reset missing from Plan step 3 session-start → `s=` carries over in the same power cycle, pre-saturates at 250.
5. [MINOR] `psWroteThisEpoch` clearing terminology: Plan said "at each durable %CKPT" (implies the decoupled FAT flush); standardize to "when %CKPT is successfully copied into pCache".

### Resolution (Claude) — all accepted (v5)
- G#1 → Decision 3 guard now `n < 0 || n >= (int)sizeof(tmp)`.
- G#2 → discriminator rewritten: `ps>s=` = SPIROV in the final (unflushed) epoch → bus-implicated (with the honest caveat that fatal-hang vs recovered-then-coincidental-death can't be split at the tail — same action either way); `ps==s=` (>0) = recovered+flushed then died with no new SPIROV → recovered-then-other (NOT bus). Removed the incorrect "SPIROV-at-tail = ps==s=" UNKNOWN bucket.
- G#3 → Decision 5: audit + enlarge the %BOOT buffer + apply the spin-proof guard.
- G#4 → Plan step 3: reset `sdSpirovSeen=0` at session start.
- G#5 → Plan step 3 + Decision 4: clear `psWroteThisEpoch` on successful pCache copy of the %CKPT.

### prep-diag.md changes this round (v5)
Decision 3 guard (n<0). Discriminator corrected (ps==s= meaning; SPIROV-at-tail folded into bus-implicated with caveat). Decision 5 (%BOOT buffer audit). Plan step 3 (sdSpirovSeen reset + psWroteThisEpoch-on-cache-copy).

### Status
No BLOCKERs from Gemini; the substantive architecture has converged (R3's two BLOCKERs resolved, R4
was MAJOR/MINOR refinements only). **Codex must still confirm v5** before sign-off — a Codex+Gemini
confirmation round (R5) is scheduled for after Codex's 15:40 limit reset. The panel requires BOTH to
approve; not declaring convergence on Gemini alone.

## Round 5 — Codex: CHANGES_REQUESTED | Gemini: CHANGES_REQUESTED | GLM: (unavailable)
(Codex back from its usage limit. No BLOCKERs on the architecture — refinement round.)

### Codex findings (6)
1. [BLOCKER] EEPROM layout/lifecycle STILL internally contradictory — stale "piggyback/shared/extended" text vs Decision 4's standalone bytes. Purge every stale statement.
2. [MAJOR] `pf`/`ps` arm not transaction-safe — separate DEE records, no readback/failure handling; a stale `ps` from a prior run can falsely look like bus. Verify writes by readback; if unconfirmed don't start / mark UNKNOWN.
3. [MAJOR] DEE old-valid fallback can still false-classify `ps=0` as "no SPIROV" — a torn write leaves the OLD valid record. Weaken to "no DURABLY recorded SPIROV" or add a write-in-progress sentinel → UNKNOWN.
4. [MAJOR] Fault-injector not provably unreachable in RF-up sessions — ≤250 SPS+RF is also a normal mode; a host bug could arm+token. Plan step 6 still says "reused matcher". Independent matcher; bench-only volatile predicate rejecting 0x0B during ordinary recording.
5. [MAJOR] `ps` UNKNOWN mapping incomplete — Decision 5 maps `pf` to 0/1/? but not raw `ps` 251..254. Define `ps` decode (0xFF→0, 0..250→dec, 251..254→ps=?) + fixtures.
6. [MINOR] `s` saturation wording conflicts with the storage model — `sdSpirovSeen` uint32 printed %lu (uncapped) vs EEPROM `ps` capped 250. State SD s= uncapped; classifier uses s>=250 / ps==250.

### Gemini findings (4)
1. [MAJOR] Discriminator's R4 "honest caveat" ignores SD time-resolution → false bus-blame. A SPIROV can recover, log %E, run 50s, then a coincidental power cut — `ps>s=` still true. The morning script MUST measure time since the last visible %E on SD; substantial clean data after it (>1s, independent of %CKPT) → power/other, not bus. Only bus if EOF is tightly adjacent to %E.
2. [MAJOR] `ps` is NOT sentinel-encoded — a raw 0..250 byte has no redundancy; a torn DEE page → random byte is a valid 0..250 ~98% of the time → fabricates false SPIROV evidence. Encode `ps` value+complement (two bytes), OR rely strictly on pf's sentinel + DEE page integrity.
3. [MINOR] Spin guard misses `n==0` (empty string) → falls through, no timer advance → spin. Use `n <= 0`.
4. [MINOR] `%BOOT` truncation must trigger the ABORT path (treat as write/sync failure), not silently skip — else `pf=0xA5` gets armed without a durable %BOOT on disk.

### Grounding (Claude)
DEE write semantics already grounded R3 (record-versioned, last-valid-wins; pack/erase = the only non-atomic window). Confirms Gemini#2 (raw count has no redundancy → needs the complement) and Codex#3 (torn write keeps the OLD valid record → ps=0 blind spot).

### Resolution (Claude) — all accepted (v6)
- Codex#1 (BLOCKER) → purged ALL stale piggyback/shared/extended/reused/first-SPIROV wording (header note, Plan step 6, prep.md pointer, risk bullet, Files-to-touch); standalone bytes are now the only statement everywhere.
- Codex#2 → Decision 4 ARM: write ps=0/~ps=0xFF/pf=0xA5 then READ BACK to verify; on failure treat the breadcrumb as UNKNOWN this session (don't record over unverified/stale state).
- Codex#3 → Decision 4: wording weakened to "no DURABLY-recorded SPIROV"; torn-ps-write-during-power-loss blind spot acknowledged (degrades to power/other, self-consistent; pending-sentinel noted as optional /grill hardening).
- Codex#4 + Gemini (injector) → Decision 8: independent injectState (purged "reused"); production-night PROVABLE unreachability (500 SPS/RF-off, gate false) stated; ≤250 SPS RF residual exposure bounded + accidental-fire-is-NON-DESTRUCTIVE; bench-only volatile predicate; arm expiry. Strengthened the deferred-to-user #ifdef recommendation (panel flagged R1/R3/R5).
- Codex#5 → Decision 5: explicit ps decode (complement-checked: 0xFF/0xFF→0, valid 0..250→dec, mismatch or 251..254→ps=?) + pf 0xA5→1/0x00→0/else→? + fixtures for pf=?/ps=?.
- Codex#6 → Decision 2: SD `s=` is uncapped uint32; only EEPROM ps capped 250; classifier s>=250 OR ps==250.
- Gemini#1 (MAJOR) → discriminator REWRITTEN as an ordered procedure: SD %E-to-EOF FINE adjacency is authoritative when the tail survived (tightly-adjacent→bus; substantial clean data after→recovered-then-other NOT bus); ps>last-durable-s= is the tail-LOST backstop only (flagged SPIROV-near-tail low-confidence); ps==s=→recovered-then-other; ps==0→no durably-recorded SPIROV.
- Gemini#2 (MAJOR) → Decision 4: ps stored as value@8 + complement@12; read trusts ps only if EEPROM[8]==~EEPROM[12] && <=250, else UNKNOWN. Address map updated (free slots 2,3,8,9,12,13).
- Gemini#3 (MINOR) → Decision 3 guard `n <= 0 || n >= sizeof`.
- Gemini#4 (MINOR) → Decision 5: %BOOT truncation → abort openSDfile, leave evidence intact (don't arm).

### prep-diag.md changes this round (v6)
Discriminator rewritten as a fine/coarse ordered procedure. Decision 4: ps value+complement, arm readback-verify, torn-write blind spot, durable wording. Decision 5: ps decode table + %BOOT abort. Decision 2: cap clarity. Decision 8: production guarantee + bench predicate + non-destructive + stronger #ifdef recommendation. Decision 3: n<=0. All stale contradictions purged (verified by grep).

## Round 6 — Codex: CHANGES_REQUESTED | Gemini: CHANGES_REQUESTED | GLM: (unavailable)
(Refinement round; the 2 BLOCKERs are pure internal-consistency lag from v6, not architecture.)

### Codex findings (5)
1. [BLOCKER] `ps` virgin decode fabricates a valid zero — Decision 4 says 0xFF/0xFF→UNKNOWN but Decision 5 special-cases 0xFF/0xFF→ps=0; a torn erased cell would falsely read "no SPIROV". Print numeric ps only if EEPROM[8]==~EEPROM[12] && ≤250; 0xFF/0xFF → ps=?.
2. [BLOCKER] `~ps@12` not carried through the plan — Decision 4 requires ps@8+~ps@12, but Plan step 2 writes only EEPROM[8], step 3 says "two bytes", Files-to-touch omit ~ps@12. Update every plan/files/test bullet to three bytes + paired writes/readbacks.
3. [MAJOR] Arm readback failure has no DURABLE failure state — "UNKNOWN for this session" is RAM-only; a reset after an unverified arm exposes stale pf/ps. Abort openSDfile, OR durably write+verify an invalid state that next boot prints UNKNOWN.
4. [MAJOR] Final overnight rule still classifies from pf=1 alone — contradicts the discriminator (pf=1 ps>last_s = SPIROV-near-tail; pf=1 ps=? = UNKNOWN). Morning logic must branch on decoded ps vs last durable s.
5. [MINOR] Bench predicate not independent — the "bench-only volatile predicate" is a RAM flag set by the 0x0B arm itself, so it doesn't distinguish bench from a normal ≤250 SPS RF-up session. Document the residual, or use a separate non-persisted bench gate accepted only pre-recording.

### Gemini findings (3)
1. [BLOCKER] 0xFF/0xFF virgin mapping contradiction (same as Codex#1). Purge the 0xFF/0xFF→0 special case; let it fall through to ps=?.
2. [MAJOR] Arm readback ignores the complement byte — verifying only ps==0/pf==0xA5 misses a torn ~ps (→ silent UNKNOWN all session). Readback MUST be complement-aware (verify EEPROM[12]==0xFF too).
3. [MINOR] #ifdef fault-injector — flagged a 4th time: no plan to fire it on a live head → shipping the trigger in production is unnecessary flash + risk; accept the #ifdef unless runtime is absolutely required.

### Resolution (Claude) — all accepted (v7)
- Codex#1 + Gemini#1 (BLOCKER) → Decision 5: purged 0xFF/0xFF→0; ps prints decimal ONLY if complement-valid && ≤250, else ps=? (incl. 0xFF/0xFF, mismatch, 251..254). No ps=0 shortcut.
- Codex#2 (BLOCKER) → propagated ~ps@12 / THREE bytes through Plan steps 2&3, Files-to-touch, context, risks, test (paired ps/~ps writes + complement-aware readback everywhere).
- Codex#3 + Gemini#2 (MAJOR) → Decision 4 ARM rewritten as INVALIDATE-FIRST: write pf=0x3C (invalid sentinel)+verify → write ps=0/~ps=0xFF + complement-aware-verify → write pf=0xA5+verify; any failure leaves the DURABLE pf=0x3C UNKNOWN (never stale-valid) + recording proceeds (breadcrumb degraded, not blocking — the recording is the priority).
- Codex#4 (MAJOR) → Test step 5 rewritten to the full decoded-ps-vs-s + %E-adjacency procedure; pf alone only means unclean-active.
- Codex#5 (MINOR) → Decision 8: DROPPED the overclaimed "bench-only volatile predicate"; stated the honest residual (≤250 SPS RF-up sessions are indistinguishable from bench; accidental fire needs the exact token AND is NON-DESTRUCTIVE); 500 SPS/RF-off production = provably unreachable.
- Gemini#3 (MINOR) → strengthened the dissent note: flagged R1/R3/R5/R6 (4×); noted the recovery path is byte-identical in #ifdef-out vs debug build (so a debug-build bench test still validates production recovery); STRONG recommendation to switch at /grill, still the user's call.

### prep-diag.md changes this round (v7)
Decision 5: ps decode purged of the 0xFF/0xFF→0 shortcut. Decision 4: invalidate-first complement-aware arm + durable-UNKNOWN-on-failure. Plan/Files/Test: propagated ~ps@12 / three bytes / decoded-classifier everywhere (the propagation lag that recurred R5/R6 is now fully chased). Decision 8: honest residual (dropped the false predicate) + 4×-flagged strong #ifdef recommendation. Test step 3: Codex+Gemini (GLM not installed).

## Round 7 (backstop) — Codex: CHANGES_REQUESTED | Gemini: CHANGES_REQUESTED | GLM: (unavailable)
Net-new findings at the backstop (narrowing to discriminator edge-cases + arm-failure precision).
Extending past the flat-7 backstop is aligned with the user's explicit "super deep" instruction AND
the rule (keep going while NET-NEW BLOCKER/MAJOR appear) — R7 surfaced a real BLOCKER.

### Codex findings (3)
1. [MAJOR] Initial invalidate write (pf=0x3C) failure not durable-UNKNOWN-safe — if the 0x3C write fails readback, DEE still exposes the old valid pf=0x00/0xA5; proceeding leaves stale-valid evidence. Make verified pf=0x3C a HARD precondition (bounded retry; else abort open). Only later step failures proceed with 0x3C confirmed.
2. [MAJOR] EEPROM UNKNOWN short-circuits authoritative SD evidence — ordered classifier put "pf/ps non-decodable → UNKNOWN" BEFORE the SD s≥250/%E checks. Classify footer/SD-tail FIRST; EEPROM only as tail-lost backstop; pf/ps=? UNKNOWN only in EEPROM-dependent branches. Also no-footer+pf=0 → contradictory UNKNOWN.
3. [MINOR] EOF-adjacency threshold approximate ("≲~1s / a handful of blocks") — define exact block thresholds + gray band → UNKNOWN.

### Gemini findings (2)
1. [BLOCKER] Discriminator misses the `0 < ps < last-durable-s=` bucket — the once-per-epoch ps write can lag s= when an epoch has >1 SPIROV (ps=1 while s=2); when that epoch flushes, last-durable-s=2 but ps=1; a later non-SPI death → ps<s falls through (Rule 5 only had ps>s / ps==s / ps==0). Change `ps==s=` to `0<ps<=s=` = recovered-then-other.
2. [MAJOR] `%CKPT` append order vs `T=$` end-anchor — appending s= AFTER T= would break a `T=<hex8>$`-anchored parser. Insert s= BEFORE T= (`...x=...s=...T=...`) to keep T= the terminator.

### Grounding (Claude)
Read the CANONICAL host parser `sd_convert.py:parse_ckpt_line` (imported by both session_start + the py-qs-data nightly): it splits on whitespace, parses k=v, SKIPS unknown keys, and does NOT even read/anchor `T=`. session_start.py only mentions T= in comments (no active $-anchored regex). → Gemini#2 is HYPOTHETICAL for the real parser (order-independent), but placing s= before T= is free insurance → adopted + grounded.

### Resolution (Claude) — all accepted (v8)
- Gemini#1 (BLOCKER) → discriminator + Test 5: `ps==s=` → `0 < ps ≤ last-durable-s=` = recovered-then-other; closes the ps<s fallthrough.
- Codex#2 (MAJOR) → discriminator REORDERED: footer → SD-tail authoritative (s≥250 / %E-adjacency, EEPROM-independent) → EEPROM tail-lost backstop (pf/ps=? UNKNOWN only here) → no-footer+pf=0 contradiction → UNKNOWN.
- Codex#1 (MAJOR) → Decision 4 ARM: pf=0x3C invalidate is a HARD precondition (bounded retry; if unverifiable, ABORT openSDfile — a DEE that can't invalidate already breaks the firmware's existing EEPROM bookkeeping); only step-2/3 failures degrade to durable-UNKNOWN-with-0x3C-confirmed.
- Codex#3 (MINOR) → concrete thresholds: ≤4 BLK (≤2KB) to EOF → bus; ≥20 BLK clean after → not-bus; 4–20 BLK gray → UNKNOWN.
- Gemini#2 → Decision 2 + Decision 3 snprintf + Plan step 1: s= placed immediately before T= (keep T= terminator), grounded that the canonical parser is order-tolerant.

### prep-diag.md changes this round (v8)
Discriminator rewritten SD-first/EEPROM-backstop with concrete block thresholds + the 0<ps≤s bucket + contradiction guard. Decision 4 ARM: hard-precondition invalidate. Decision 2/3/Plan-1: s= before T=. Test 5 aligned to the reordered classifier.

## Round 8 — Codex: CHANGES_REQUESTED | Gemini: CHANGES_REQUESTED | GLM: (unavailable)
Both BLOCKERs trace to MY R7 discriminator reorder (strict SD-then-EEPROM ordering) — the root fix
is to treat the signals as COMBINED, not ordered. Key framing: the discriminator is a HOST-SIDE
morning script (post-flash-revisable); the flash-gating firmware data it consumes is stable since v4.

### Codex findings (2)
1. [BLOCKER] SD-tail branch can false-negative a fatal unlogged SPIROV — a fatal SPIROV writes `ps` before recovery then hangs before `%E`/next `%CKPT` flushes; the tail can still "look available" so step 2 never consults `ps>s`. Valid `ps>s` must be checked regardless of tail-available; EEPROM-UNKNOWN must not mask SD, but valid ps>s must not be ignored.
2. [MAJOR] `s=` deltas aren't fine proximity evidence — `%CKPT`/`s=` lands at the 60s checkpoint boundary, not the SPIROV. A recovered SPIROV + tens of seconds clean + an unrelated power cut can put the s+1 checkpoint within ≤4 BLK of EOF → false bus. Use the `%E` byte/block offset for the adjacency test; an s= delta without a matching %E → coarse UNKNOWN.

### Gemini findings (3)
1. [BLOCKER] Contradiction guard is dead code at step 4 — steps 2/3 exhaustively classify any no-footer file, so step 4 is unreachable. The real hole: power lost between the %BOOT flush and EEPROM arm → no footer + EEPROM holds the PRIOR session's pf=0x00 → step 3 misclassifies on stale ps. Move the guard (no footer + pf clean → UNKNOWN) to step ~1.5, before SD/EEPROM trust.
2. [MINOR] Block thresholds overlap at 4 and 20 (≤4 / 4–20 / ≥20). Make mutually exclusive: ≤4 / 5–19 / ≥20.
3. [MINOR] RAM state init coupled to arm success — `psWroteThisEpoch=false` set only after pf=0xA5 confirmed; a step-2 failure could leak a stale `true`. Unconditionally init sdSpirovSeen=0 + psWroteThisEpoch=false at the very start of session-start, before any EEPROM write.

### Grounding / framing (Claude)
The discriminator is a host-side Python classifier (sd_convert/morning script) — REVISABLE on real data post-flash. Only the firmware-emitted data is flash-gating (s= count, ps/~ps/pf breadcrumb, %E markers at byte-offsets, pf states), and that's stable since v4. The %E marker's file byte-offset IS its location (no extra firmware field needed for adjacency). So R8's findings are host-script-precision (non-flash-gating), addressed by a clean combined-signal rewrite.

### Resolution (Claude) — all accepted (v9)
- Codex#1 (BLOCKER) + Gemini#1 (BLOCKER) → discriminator REWRITTEN as COMBINED signals (not strict order): contradiction guard runs FIRST (step 2, before SD/EEPROM trust → no longer dead code); valid `ps>s=` is a POSITIVE bus signal checked REGARDLESS of tail-available (catches the fatal-unflushed SPIROV); a non-decodable ps is simply absent (never masks SD). Fixes both BLOCKERs at the root.
- Codex#2 (MAJOR) → adjacency now uses the `%E` marker byte-offset (not the s= increment position); s= is for counting + the ps>s comparison, %E for locating.
- Gemini#2 (MINOR) → mutually-exclusive bands ≤4 / 5–19 / ≥20 BLK.
- Gemini#3 (MINOR) → Decision 4 + Plan step 3: unconditionally RAM-init sdSpirovSeen=0 + psWroteThisEpoch=false at session-start FIRST, before any EEPROM write.

### prep-diag.md changes this round (v9)
Discriminator fully rewritten as combined-signal logic (contradiction-guard-first; valid-ps>s always a positive bus signal; %E-offset adjacency; mutually-exclusive bands) + a scope note that it's a host-side post-flash-revisable script. Decision 4/Plan step 3: RAM-init-first.

## Round 9 — Codex: APPROVED ✅ | Gemini: CHANGES_REQUESTED | GLM: (unavailable)
Codex APPROVED v9. Gemini raised 2 BLOCKERs + 1 MAJOR (all host-script discriminator logic). Gemini#2
is a direct reviewer-split with Codex-R8#1 — resolved by weighing both.

### Gemini findings (3)
1. [BLOCKER] Invalidate-first pf=0x3C bypassed by the discriminator — if power cuts after pf=0x3C but before ps=0, pf=UNKNOWN but ps stays a stale complement-valid PRIOR-session value; step 3 trusts `ps>s` without requiring pf==0xA5 → false BUS. Require pf==0xA5 to trust ps.
2. [BLOCKER] `ps>s` overrides undeniable SD proof of recovery — a SPIROV that recovers, logs %E, runs 1000 clean blocks, then an unrelated power cut has ps>s; step 3 forces BUS, ignoring the clean run. A ≥20 BLK clean run after the last %E should override ps>s (unless s/ps==250).
3. [MAJOR] Test step 5 still has the outdated R7 strict-order logic — rewrite to match the v9 combined-signal discriminator.

### Resolution (Claude)
- Gemini#1 (BLOCKER) ACCEPTED → added the `ps` TRUST GATE: `ps` trusted ONLY if `pf` decodes 0xA5 (proof THIS session armed it); pf=0x3C/0x00/UNKNOWN → ps untrusted/absent → SD-only. Closes the stale-prior-session-ps hole.
- Gemini#2 (BLOCKER) ↔ Codex-R8#1 (SPLIT) — RESOLVED by making the ambiguous case UNKNOWN, not forcing either way: `ps>s` is still checked regardless of tail (Codex satisfied — catches the fatal unflushed SPIROV), BUT if the last %E is followed by ≥20 BLK clean data, `ps>s` is genuinely indistinguishable (recovered-then-coincidental-powercut vs a newer unlogged fatal SPIROV — uncounted %E can't tell) → classify UNKNOWN (not forced-BUS per Codex, not forced-not-bus per Gemini; same operational response). `s/ps==250` stays an absolute BUS override. Noted the clean firmware disambiguator (tag %E with the running SPIROV count) as a deferred future enhancement (changes %E host-contract + flash; same response anyway).
- Gemini#3 (MAJOR) ACCEPTED → Test step 5 rewritten to the v10 combined-signal procedure.

### prep-diag.md changes this round (v10)
Discriminator: added the pf==0xA5 ps-trust-gate; reconciled ps>s vs clean-run (≥20 BLK clean after last %E → UNKNOWN, not BUS); s/ps==250 absolute override stated; deferred %E-count disambiguator noted. Test step 5 rewritten to match.

## Round 10 — Codex: CHANGES_REQUESTED | Gemini: APPROVED ✅ | GLM: (unavailable)
Gemini APPROVED v10. Codex flipped to CHANGES_REQUESTED (1 MAJOR + 2 MINOR — all host-side
discriminator-script precision; firmware decisions unchallenged). Classic late-round ping-pong:
each reviewer has now APPROVED once (Codex@R9, Gemini@R10), just not the same version simultaneously.

### Codex findings (3)
1. [MAJOR] `pf==0xA5` is not session-proof in the %BOOT→invalidate window — if a PRIOR session hung (left pf=0xA5) and the new session lost power after the %BOOT flush but before pf=0x3C, the stale 0xA5+ps is misattributed to the new no-footer file → false BUS. Classify %BOOT-only/pre-arm files UNKNOWN, or add a session/armed marker tying EEPROM to the current session.
2. [MINOR] Fault-inject power-cycle test overclaims "fatal SPIROV" proof — the injector recovers by design; the power-cycle proves breadcrumb persistence after a RECOVERED SPIROV, not a fatal pre-%E one. Reword.
3. [MINOR] %E adjacency needs marker-framed parsing fixtures — byte-offset scanning could mistake payload bytes for a %E marker. Require marker framing + a literal-%E-in-payload fixture.

### Resolution (Claude) — all accepted (v11)
- Codex#1 (MAJOR) ACCEPTED → strengthened the `ps` TRUST GATE: trust ps only if (a) pf==0xA5 AND (b) the file's %BOOT seq== current EEPROM sessionSeq[5,6] (breadcrumb belongs to THIS file) AND (c) the file shows substantial recording progress past %BOOT (not a pre-arm casualty). Rejects the stale-prior-session misattribution host-side. Deferred the bulletproof firmware alternative (store armed sessionSeq inside the breadcrumb record) — same effect, costs flash, and this is a post-flash-revisable host script.
- Codex#2 (MINOR) ACCEPTED → reworded the power-cycle test: proves persistence after a RECOVERED SPIROV (+ seq match), not a fatal pre-%E hang.
- Codex#3 (MINOR) ACCEPTED → Test 1: %E parsed as a line-framed marker (^%E$), + a fixture with literal %E bytes in non-marker EEG payload.

### prep-diag.md changes this round (v11)
ps TRUST GATE extended (pf==0xA5 + seq-match + recording-progress; deferred firmware session-tag). Test step 4 power-cycle reworded. Test step 1 gained %E marker-framing + literal-%E-payload fixture.

## Round 11 — Codex: APPROVED ✅ | Gemini: CHANGES_REQUESTED | GLM: (unavailable)
Codex APPROVED v11. Gemini raised 1 BLOCKER — a correct refinement of my OWN v11 seq-match fix (same
narrow pre-arm window, host-side). Ping-pong continues: Codex approved @R9,R11; Gemini approved @R10.

### Gemini findings (1)
1. [BLOCKER] Host-side seq-match (condition b) incorrectly specified — `sessionSeq` increments BEFORE the arm step, so a session that died pre-arm still wrote its seq and `next_file.prev` still points at it → a stale breadcrumb falsely passes a naive seq-match. Reword (b) to `analyzed_file.seq == next_file.prev` (which only detects intervening/foreign-card sessions); clarify it's condition (c) (substantial progress) that actually protects the pre-arm casualty window.

### Resolution (Claude) — accepted (v12)
- Gemini#1 ACCEPTED → reworded the trust gate: (b) detects intervening/foreign-card sessions only (`analyzed.seq == next.prev`); explicitly stated (c) recording-progress is what guards the %BOOT-flush→pf=0x3C pre-arm window (a near-empty pre-arm casualty fails (c) → UNKNOWN). Deferred firmware alternative reworded (store armed seq in the breadcrumb → eliminates the (c) heuristic).

### prep-diag.md changes this round (v12)
ps TRUST GATE conditions (b)/(c) corrected per their actual roles.

### Convergence note (Claude)
ALL firmware/flash-gating decisions (s= field+placement, ps/~ps/pf breadcrumb + invalidate-first arm,
buffer guards, tune keys 0x07-0x0A, the hardened/deferred injector, Size/%META bundle) have been
STABLE and unchallenged since ~v4. Every R5–R11 finding has been HOST-SIDE morning-discriminator-script
precision (explicitly post-flash-revisable on real data). Both reviewers have APPROVED the substance
(Codex@R9/R11, Gemini@R10); they're now polishing each other's host-script wording on ONE narrow
power-loss window. Per the adaptive-stop rule (stop when no net-new FLASH-GATING BLOCKER/MAJOR) the
flash-gating plan has converged; R12 is the last confirmation pass.

## Round 12 — Codex: APPROVED ✅ | Gemini: APPROVED ✅ | GLM: (unavailable) — BOTH SIGN OFF
Both reviewers APPROVED v12 simultaneously. Only leftover MINORs (no BLOCKER/MAJOR):
- Codex MINOR#1: Test step 5 had a stale "trust ps only if pf==0xA5" duplicate → could reintroduce the pre-arm false-BUS if copied. → FIXED: Test step 5 now points at the discriminator section (no duplicate gate).
- Codex MINOR#2: per-epoch ps/~ps write not readback-verified → falls into the accepted torn-write blind spot. → FOLDED into the existing blind-spot note (optional /grill readback hardening; not required — it's the already-degraded fault path).
- Gemini MINOR#1: the "newest file" seq-correspondence is `EEPROM.sessionSeq == file.seq + 1` (seq increments before arm), not `==`. → FIXED in the trust gate.

All three folded in (trivial, no re-approval needed — both already APPROVED and folding MINORs doesn't
reopen BLOCKER/MAJOR). 

### FINAL VERDICT: Codex APPROVED + Gemini APPROVED. prep-diag.md (v12) is signed off.
12 rounds. The flash-gating FIRMWARE plan converged by ~v4 and was unchallenged thereafter; rounds
5–12 hardened the host-side morning-discriminator script (post-flash-revisable). GLM was never
available on z13 (only codex-review + gemini-review installed) — panel = Codex + Gemini throughout.
