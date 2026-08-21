# work-board3: board #3 minimal firmware (clean baseline + %META + 250 Hz SD-only gate)

## Task
A THIRD OpenBCI firmware, for a fresh board #3, that is a **true minimal control**: the
pre-meta/pre-SD-fixes baseline, plus ONLY (1) %META saving ported in, and (2) a fix so SD-only
recording starts at **250 Hz** as well as 500 Hz. Then flash it. Board #3 is partly a control for
the Cyton-B freeze question (does a stock stack freeze?).

Lane: **involved** (3 separately-versioned components, %META port surgery, stock-driver
reconstruction, no-ICSP brick risk, mandatory Codex+Gemini panel per repo CLAUDE.md).

## User decisions (Act-1 forks, answered)
- **SD driver = reconstruct STOCK** (true minimal control), not "keep current inert".
- **Stop = power-cycle** (do NOT port the hardened `--stop` escape token / RX-hardening).

## The three components and their clean state
1. **Sketch** `examples/DefaultBoard/{DefaultBoard.ino, SD_Card_Stuff.ino}` — revert to **`e8f8f63`**
   (last commit before `63d4191` introduced the `M`/%META protocol + SD-error retry together).
   Verified: `e8f8f63` already prints the `Size N SD file …` host-contract line and already tracks
   `SDfileOpen`/`board.sdFileOpen`.
2. **Library** `~/Arduino/libraries/OpenBCI_32bit_Library` (own git, HEAD `ce6df66`, uncommitted
   Stage-A bounded-SPI) — discard the uncommitted Stage-A edits (`git checkout` → `ce6df66`), then
   add the one-line 250 Hz gate. Residual soft-wdt/tunables code in `ce6df66` is INERT with the
   clean sketch (never invoked); reverting the library further adds build-break risk for no
   active-behavior change.
3. **SD driver** `~/Arduino/libraries/OpenBCI_32bit_SD/utility/{Sd2Card.cpp, Sd2Card.h}` — restore
   **pristine** from this repo's OWN history `0b37db3:libraries/OBCI32_SD/utility/…`. VERIFIED
   authoritative: the three untouched sibling files (`FatStructs.h`, `SdFat.h`, `Sd2PinMap.h`) are
   md5-identical between the install and `0b37db3`, proving `0b37db3`'s `Sd2Card.*` are the exact
   pristine originals for this install. Pristine `Sd2Card.cpp` has **0** freeze-machinery refs (vs
   58 now). Pristine `Sd2Card.h` restores `const SD_WRITE_TIMEOUT = 600` (stock).

## Plan
- **P1 — Isolate the build.** The driver + library are SHARED with Cyton B's build. Build board #3
  in a dedicated sketchbook with an isolated `--libraries` dir holding: pristine `OpenBCI_32bit_SD`,
  clean `OpenBCI_32bit_Library`+gate, and the other required libs copied stock from the current
  install. **Never mutate the in-place shared libs.**
- **P2 — Sketch baseline.** New branch `board3-minimal-meta250` in this repo; `git checkout e8f8f63 --`
  the two DefaultBoard files.
- **P3 — Port %META.** Source = **`00d4cf6`** (the RAM-staged %META merge, the last state BEFORE
  `3b91a89` RX-hardening). Graft ONLY the meta machinery onto the `e8f8f63` base: `sdMetaProcess()`
  + the `M <lenLo><lenHi><payload>` raw-write protocol + RAM-staging + the `META OK <len> <sum>` /
  `META FAIL` acks. Hook `sdMetaProcess(newChar)` into `e8f8f63`'s byte dispatch. Do NOT bring
  auto-resume/%CKPT/`ledSDError`/RX-hardening.
- **P4 — 250 Hz SD-only gate.** In `OpenBCI_32bit_Library.cpp` `sendChannelData()` (~1465): gate the
  per-sample serial send at 250 Hz with `&& !sdFileOpen` so an open SD file suppresses the radio
  send — 250 Hz then behaves like 500 Hz (SD-only). `sdFileOpen` is an existing member (line 532).
  Leave the BLE path alone.
- **P5 — Build** with `arduino-cli --fqbn chipKIT:pic32:openbci --libraries <isolated>`; confirm it
  links, note size (stock baseline has ample headroom).
- **P6 — Adversarial review** (Codex + Gemini) of the full diff across all three components.
- **P7 — Flash** only after explicit go-ahead on the built image; normal `pic32prog` (NEVER a
  kill-timeout).

## Test plan
- Compile clean in the isolated env.
- Bench start via `session_start.py`: `?` idle (stock ✓), `Size` slot-open (✓), `M…`→`META OK`
  (ported), `b` start (stock ✓); verify a short SD file records + carries %META.
- **250 Hz** SD recording with the dongle absent records cleanly (the gate's job); 500 Hz still works.
- Final verification is the user's real recording; board #3 also serves as a freeze control.

## Risks
- **%META port is surgery** across a base (`e8f8f63`) older than the meta code's origin — integration
  friction at the byte-dispatch hook + shared meta state; compile + bench-verify.
- Stock `SD_WRITE_TIMEOUT=600 ms` retries more on GC-bursty cards than the 1500 ms fork — stock
  behavior, acceptable for a control; noted.
- No-ICSP: a bad flash bricks board #3. Panel + clean compile + confirm-before-flash gate it.
- `session_start.py --stop` won't work (by design). Stop = power-cycle.

## Prep-panel resolutions (Codex + Gemini, 1 round → both CHANGES_REQUESTED, all folded)
- **Codex was reading blind** (its sandbox mounted /home/lst 0700 nobody) → weight Gemini on code
  facts, Codex on methodology. Noted per finding.
- **[BLOCKER both] `--libraries` ≠ isolation** → build with `--directories.user <isolated>` (full
  sandbox); verify every "Using library" path is isolated. ACCEPTED.
- **[Codex #3] pristine-driver proof** → STRENGTHENED: `Sd2Card.cpp`+`.h` @0b37db3 are byte-identical
  to Initial Commit `4bcbc3b` (0 diff) = untouched upstream import. Proven.
- **[Codex #4] library not era-matched** → keep `ce6df66`+gate but PROVE the roflecopter additions
  (soft-wdt, `T` tunables) are unreachable from the e8f8f63 sketch (call-path check) — Codex's
  sanctioned alternative. The active recording path in ce6df66 is stock (`:532`/`:1465` are stock
  OpenBCI, not drift). Discard the uncommitted Stage-A bounded-SPI.
- **[Codex #5] hook all ingress** → CONFIRMED: e8f8f63 loop has `sdProcessChar`+`board.processChar`
  at 3 sites (Serial0, Serial1, WiFi); wrap each `if(!sdMetaProcess(c)){…}`.
- **[Gemini #2/#3] meta graft deps + durability** → the 00d4cf6 `sdMetaProcess` depends on
  `sessBuf` RAM-staging + `sdPendingErrs`/`%E` + `sdMetaCorrupted`-set-in-writeCache. e8f8f63 lacks
  all of that. ADAPT: add `metaBuf[1024]` (dedicated RAM-stage), STRIP `%E`/`sdPendingErrs`, and add
  `sdMetaCorrupted=true` to e8f8f63 writeCache's `if(!card.writeData(pCache))` branch so `META OK`
  never lies. ACCEPTED.
- **[Codex #7] 250 gate sufficiency** → Gemini verified: `sdFileOpen` set each sample right before
  `sendChannelData()` (no ordering hazard), `sampleCounter++` unconditional (cadence safe), BLE in
  its own branch (untouched), line ~343's `!=SAMPLE_RATE_250` only warns then `streamStart()`s
  anyway. Will still audit all 250 send sites at build.
- **[Codex #8] flash provenance** → capture verbose lib-resolution + size/hex ranges + core version;
  pic32prog verifies on write; confirm no bootloader/config region touched.

## Build (isolated env) — CLEAN
- **Library decision RESOLVED better than planned:** the `OpenBCI_32bit_Library` dir shares the main
  repo's git history, so the era-matched stock library is simply the `.cpp/.h/Definitions.h` at
  **`e8f8f63`** — ZERO roflecopter drift (no `ledReplayFail`/soft-wdt/tunables). Fully satisfies
  Codex #4 by era-matching (not the weaker "prove unreachable"). ce6df66 failed to link
  (`undefined reference to ledReplayFail` — a sketch symbol from the a724b9a auto-resume era, AFTER
  e8f8f63) — proving the drift was real.
- **Driver toolchain-compat:** pristine `0b37db3` `Sd2Card.cpp` `#include <plib.h>` fails on the
  chipKIT 2.1.0 core (Microchip dropped plib). Removed that ONE line — vestigial (build has zero
  undefined refs → no plib symbol used); the current fork made the identical removal. Driver stays
  behaviorally stock (0 freeze refs).
- **Isolation VERIFIED:** verbose build shows all 3 OpenBCI libs resolving to
  `/home/lst/board3-build/libraries/…`, never `~/Arduino/libraries` (Cyton B untouched).
- **`arduino-cli compile` exit 0 — Sketch 91300 B (74%), globals 11648 B (35%).** Ample flash
  headroom (freeze machinery gone). metaBuf[1024] is the bulk of the +RAM.

## Review panel — BOTH APPROVED (Codex now non-blind after the /home/lst fix)
- **Round 1:** Codex APPROVED (1 cosmetic MINOR); Gemini 3 BLOCKERs — all real, all fixed:
  1. `M`-payload command injection when SD closed → accept the frame unconditionally (state machine
     swallows payload), gate the write on `SDfileOpen` at completion (never touches pCache).
  2. `writeCache` "block write fail$$$" print broke the host ACK mid-meta → suppressed during state 3;
     `sdMetaCorrupted` scoped to state 3 (also fixes Codex's MINOR).
  3. Pre-existing STOCK integer-division truncation `BLOCK_COUNT/250*freq` (short/0-length files) →
     float math. Bonus fix (matters now 250 Hz is in play).
- **Round 2:** BOTH APPROVED. Gemini verified the float cast is exact under IEEE-754 for all ≤24 h
  block counts (~4.8M < 2^24); injection fully closed; host-ACK preserved. Build 92980 B / 75%.

## The full board #3 firmware = 3 components (build recipe)
- **Sketch:** branch `board3-minimal-meta250`, `examples/DefaultBoard/*` = `e8f8f63` + %META port + BLOCK float fix.
- **Library:** `OpenBCI_32bit_Library.{cpp,h,_Definitions.h}` @ `e8f8f63` + the 250 Hz `!sdFileOpen` gate.
- **SD driver:** `OpenBCI_32bit_SD/utility/Sd2Card.{cpp,h}` @ pristine `0b37db3` (== upstream `4bcbc3b`) − `#include <plib.h>`.
- Built ISOLATED via `ARDUINO_DIRECTORIES_USER=/home/lst/board3-build` (Cyton B's shared libs untouched).

## HARDWARE VERIFICATION (on board #3, after flash)
- **Flash #1 write completed** (`Program flash … done`); verify readback was cut by the user pulling
  the dongle — harmless (verify is read-only). Board booted the new firmware.
- **Boot + alive:** `?` → full ADS + LIS3DH register dump + `$$$` (stock pristine SPI driver reads
  both chips cleanly).
- **session_start drove it** through `?`/mode/500 Hz/channel-set (all acked) — then **aborts at
  `TUNE FAIL`**: the minimal firmware has no runtime-tune protocol (by design). ⇒ **session_start.py
  is NOT compatible with board #3 as-is** (also expects PERSIST + the escape-token stop). FOLLOW-UP:
  needs a "minimal-firmware" mode (skip TUNE/PERSIST, power-cycle stop) to drive board #3.
- **%META CONFIRMED** via direct paced driving: SD open `Size 33780 SD file OBCI_BF.TXT` (float fix
  correct at 500 Hz) → `M` frame → **`META OK 47 3316`** (correct len + checksum) → clean close
  (`Max write 326 uS, Overruns: 0`). The ported %META feature works end-to-end.
- 250 Hz `!sdFileOpen` gate: reviewed one-liner, not separately bench-tested here (verified at 500 Hz);
  confirm in real 250 Hz use.
- ⚠ Test junk on the card (incl. a big 24 h slot `OBCI_BB` from an early mis-paced test) — reformat
  the card before real recording if desired.
