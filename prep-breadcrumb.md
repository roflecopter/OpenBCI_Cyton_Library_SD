# Prep: forensic exception-handler breadcrumb (Cyton freeSD)

## Goal
Finally LOCALIZE the intermittent mid-recording MCU freeze (both models: most likely an unhandled CPU
exception → the chipKIT weak `_general_exception_handler` no-ops → the entry stub `_general_exception_
handler_entry` falls into an explicit `while(1)` loop → frozen). Override that weak handler to capture
the CP0 raw Cause + EPC (faulting PC) + BadVAddr, persist across a reset, and surface them in the next
`%BOOT` line (`exc=<code> epc=0x<hex> bv=0x<hex>`). The next freeze then names the exact faulting
instruction — turning "guess and hope" into a fix.

## Context & constraints
- **Board**: PIC32MX250F128B, 32 KB RAM, chipKIT DP32 bootloader, **NO ICSP** → a bad flash bricks.
  NEVER flash without explicit user OK; NEVER wrap `pic32prog` in a kill-timeout.
- **Branch**: `work/hang-prevent-and-diagnose` (base = Stage B: auto-resume + WDT **ON**, kept; power
  hardwired). Already committed here: `MAX_RESUMES` hard-capped at 3 + a 5 µs SPI settle (e3e46c9).
- **Flash ceiling = 118784 B**; current build **118780 B → 4 B free**. The breadcrumb needs flash
  reclaimed by trimming a **non-host-contract** diagnostic string (user's Act-1 choice).
- **HOST-CONTRACT strings NEVER trimmed/reordered**: `Size `, `META OK/FAIL/ERR`, `PERSIST OK/FAIL`,
  `TUNE OK/FAIL`, `%Total time`, `%CKPT` keys `t b e r n o x`, `$$$`, lib `Sample rate is …Hz` + `?`.
- **The exception handler touches NO flash / Serial / SD / SPI** — only CP0 reads, a `.noinit` RAM
  write, and the reset. (A DEE/EEPROM write from a faulted, IE-disabled context can take a WDT reset
  mid-page-pack and PERMANENTLY corrupt the EEPROM config space — R1 BLOCKER, both models.)
- **Mechanism (verified)**: chipKIT `cores/pic32/exceptions.c` — `_general_exception_handler_entry`
  reads CP0 $13 (Cause) + $14 (EPC), extracts ExcCode, calls the weak
  `void _general_exception_handler(uint32_t code, uint32_t address)`, then falls into `while(1)` (it
  does NOT ERET). Overriding the weak symbol with a STRONG unmangled `_general_exception_handler` wins.
- **`.noinit` is proven-safe** (Gemini R1 linker analysis): `chipKIT-application-COMMON.ld` places
  `_bss_end` at the end of the explicit `.bss`; the GNU linker orphans `.noinit` *after* `_bss_end`;
  `cpp-startup.S` zeroes only `_bss_begin.._bss_end` → a `.noinit` var survives a warm reset, guaranteed.
- `SYSKEY` (0xBF80F230) + `RSWRST`/`RSWRSTSET` confirmed available. Mandatory Codex+Gemini panel.
  NOT flashed without explicit user OK.

## Decisions
1. **Handler writes `.noinit` RAM ONLY, then soft-resets — NO flash/EEPROM/Serial/SD/SPI** (R1 BLOCKER,
   both). This removes the brick risk entirely and is why `.noinit` (proven-safe) is the handler's store.
2. **Handler lives in a SEPARATE `.c` file** in the sketch dir (e.g. `crash_handler.c`) — Arduino
   compiles sketch-folder `.c`/`.cpp` without auto-prototyping, giving a clean STRONG unmangled
   `_general_exception_handler` C symbol. Attributes: `__attribute__((nomips16))` + `noreturn`.
   `extern "C"` inside a `.ino` is NOT sufficient (R1 Codex 5).
3. **Capture RAW Cause ($13) + EPC ($14) + BadVAddr ($8)** — store raw Cause (not just the shifted
   ExcCode) so the **BD (branch-delay) bit** is preserved: if BD=1, EPC points at the branch, not the
   delay-slot faulting instruction, and the host/analyst must know (R1 Codex 6).
4. **Torn-write-safe record**: write payload (cause/epc/badv) FIRST, the `magic` word **LAST**, with a
   compiler barrier (and a `sync`) between — so a second fault mid-write can't leave a valid-magic /
   garbage-payload record (R1 Codex 2).
5. **Soft-reset**: IE off (`__builtin_disable_interrupts`), memory barrier, then the canonical PIC32
   unlock on KSEG1 SFRs — `SYSKEY=0; SYSKEY=0xAA996655; SYSKEY=0x556699AA; RSWRSTSET=_RSWRST_SWRST_MASK;
   (void)RSWRST;` — then `while(1);`. The `while(1)` is a real backstop: if the SYSKEY sequence is
   voided by a cache-miss pipeline delay from KSEG0, the ARMED HW WDT resets the board cleanly and
   `.noinit` still survives (R1 Codex 8 / Gemini 4).
6. **Power-cycle survival via a SAFE, NORMAL-CONTEXT EEPROM copy** (honors the user's "both" choice
   without the handler brick risk — R1 Codex 1/9): `setup()`, on a valid `.noinit` record, copies it to
   a reserved EEPROM byte range using the ordinary `EEPROM.write` in normal boot context (a DEE pack
   here is safe). **EEPROM MAP (audited — firmware uses 0-7,10,11; bytes 8,9 and 12+ are FREE):**
   `[12..15]=cause, [16..19]=epc, [20..23]=badv, [24]=magic(0xE1)` — **13 bytes, 12..24** (the R2 range
   12..22 was 11 bytes, too small for a 12-byte payload — R2 BLOCKER, both). **Torn-write-safe (R2 both):
   invalidate first (`EEPROM.write(24,0)`), write the 12 payload bytes [12..23], then write magic [24]
   LAST.** So `.noinit` covers the warm-reset path; the EEPROM copy covers resume-fails-then-power-cycle.
   > ASSUMPTION (auto-resolved, Codex↔Gemini split): Gemini favoured `.noinit`-ONLY (simplest, saves
   > flash); Codex favoured `.noinit`-in-handler + EEPROM-copy-in-`setup()` (the SAFE "both"). Chose
   > Codex's safe-both to match the user's Act-1 "both" (power-cycle survival). If the final build can't
   > fit it, DROP the EEPROM copy (fall to `.noinit`-only) before dropping `bv=`.
7. **`setup()` read (early, before RAM/EEPROM clobber)**: if `g_crash.magic==CRASH_MAGIC` → load
   cause/epc/badv, `excValid=true`, and copy to EEPROM (Decision 6, torn-write-safe). ELSE if
   `EEPROM.read(24)==0xE1` (magic is at [24], written last) → load cause/epc/badv from [12..23],
   `excValid=true`. Do NOT clear either store here. The `g_crash` struct is defined in `crash_handler.c`
   (C) and referenced in the `.ino` (C++) via an **`extern "C" { … }`** block so the linker resolves
   the unmangled symbol (R2 Gemini 3).
8. **Clear only AFTER the `%BOOT` emit succeeds** (R1 Codex 3): after `setupSDcard` emits the exc=
   fields, clear `g_crash.magic=0` AND `EEPROM.write(24,0)` (the magic byte). So a boot that never opens
   a slot (resume fails, no `%BOOT`) keeps the breadcrumb for the next attempt / the EEPROM copy.
9. **`%BOOT` emits `exc=<excCode> epc=0x<hex8> bv=0x<hex8>`** (+ a `bd=1` token only when BD set) as
   trailing parser-safe tokens, gated on `excValid`, via EMIT_HEX (snapshot each value to a local first
   — the EMIT_HEX multi-eval `millis()`-tear gotcha). Include `bv=` if flash allows; it's the first
   thing dropped if tight (Decision 6's EEPROM copy is dropped before this).
10. **Flash reclaimed by trimming a NON-contract diagnostic string** (user's Act-1 choice), each
    verified host-unparsed first. Clean-boot (no crash) path is behaviourally identical.

## Plan
1. **Verify `.noinit` orphan placement** empirically from the built `.map` (belt-and-suspenders on
   Gemini's analysis): confirm the `.noinit` symbol address is ≥ `_bss_end`.
2. **Verify the trimmable string is host-unparsed**: grep `openbci-session/session_start.py` +
   `py-qs-data/collect_*.py` + `openbci-session/sd_convert.py` for the exact literal(s); confirm no match.
3. **`crash_handler.c`** (new, sketch dir): `struct CrashRec { uint32_t cause, epc, badv, magic; };`
   `struct CrashRec __attribute__((section(".noinit"))) g_crash;` + `_general_exception_handler(code,
   address)` per Decisions 2–5 (read $13/$14/$8 via `mfc0`; payload-first, magic-last + barrier; IE off;
   SYSKEY→RSWRST; while(1)). No includes beyond `<xc.h>`/`<p32xxxx.h>`.
4. **`DefaultBoard.ino`**: `extern` the `g_crash` struct + declare `uint32_t excCause,excEpc,excBadv;
   boolean excValid;`; the `setup()` read + EEPROM copy (Decisions 6–7).
5. **`SD_Card_Stuff.ino`**: the `%BOOT` `exc=/epc=/bv=` emit (Decision 9) + the clear-after-emit
   (Decision 8) + the non-contract string trim (Decision 10).
6. **Build-gate ≤118784**; if over, drop EEPROM copy → then `bv=` (Decision 6/9).
7. **Bench capture test + panel + soak** per Test plan.

## Files to touch
- `examples/DefaultBoard/crash_handler.c` — NEW: the `.noinit` struct + the exception handler.
- `examples/DefaultBoard/DefaultBoard.ino` — extern g_crash; setup() read + EEPROM copy.
- `examples/DefaultBoard/SD_Card_Stuff.ino` — `%BOOT` exc= emit; clear-after-emit; non-contract trim.
- `prep-breadcrumb.md` / `prep-breadcrumb-review-log.md` — plan + audit.

## Test plan
- **Build gate**: `arduino-cli compile --fqbn chipKIT:pic32:openbci …/examples/DefaultBoard` (as lst,
  abs path) ≤ **118784 B** with margin. RAM within budget (.noinit struct +16 B). Inspect the `.map` to
  confirm `.noinit` ≥ `_bss_end` (Plan §1).
- **Host-contract audit**: diff built strings — no host-contract string changed; only the verified
  non-parsed line trimmed; `%BOOT` gains only trailing `exc=/epc=/bv=` tokens. **Replay** session_start.py's
  actual `%BOOT` parser against a sample `%BOOT … exc=… epc=… bv=…` line (R1 Codex 7) — not just grep.
- **Handler capture (bench, deterministic — THE key test)**: a temporary debug command provokes a known
  exception (e.g. `*(volatile uint32_t*)0x00000001` load = AdEL/address-error). Flash a DEBUG build,
  trigger it; confirm the board resets and the next `%BOOT` shows `exc=<code> epc=0x<addr> bv=0x<addr>`
  matching the injection. Verify BOTH stores: warm-reset (`.noinit`) AND power-cycle-before-read
  (EEPROM copy). Remove the debug trigger before the real build.
- **Clean-boot identical**: a normal recording (no crash) — no exc= token, no reset, EEPROM untouched
  beyond the one boot read.
- **One-shot**: a captured crash is reported in exactly one slot's `%BOOT`; the next slot's is clean.
- **Primary bar = overnight soak**: an exception-freeze now resets (salvages via resume, MAX_RESUMES=3)
  and the resumed slot's `%BOOT` carries `exc=/epc=` → the freeze is located.

## Risks & open questions
- **`.noinit` reliability** — RESOLVED (Gemini linker analysis + a `.map` check in Plan §1). The EEPROM
  copy is the power-cycle backup.
- **Handler symbol must actually override the weak default** — the `.c`-file + strong-symbol approach
  (Decision 2); verify via the `.map`/`nm` that OUR `_general_exception_handler` is linked, not the core's.
- **Only EXCEPTION-type freezes are caught** — a pure infinite-loop hang never enters the handler; the
  armed WDT still resets it but with no exc= breadcrumb. Accepted (exceptions are the leading hypothesis).
- **SYSKEY voided by pipeline delay** — mitigated by the `while(1)` + armed-WDT backstop (Decision 5).
- **Flash** — the staged fallback (drop EEPROM copy → then `bv=`) keeps it fitting under 118784.

## Out of scope
- Fixing the freeze itself (this only LOCATES it); non-exception hangs beyond the WDT reset; any
  host-side change; modifying the chipKIT core `.ld`/startup; re-litigating the committed MAX_RESUMES/
  SPI-settle changes.
