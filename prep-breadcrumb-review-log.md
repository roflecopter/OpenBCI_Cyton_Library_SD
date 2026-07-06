# Prep review log — forensic exception breadcrumb

Plan: `prep-breadcrumb.md`. Panel: Codex (gpt-5.5, xhigh) + Gemini (3.1 Pro). Claude orchestrates.

## Round 1 — Codex: CHANGES_REQUESTED | Gemini: CHANGES_REQUESTED
### Codex findings
1. [BLOCKER] EEPROM/DEE write in the exception handler unacceptable — page-pack erase/program hazard even for 2 bytes. Write .noinit only in the handler; copy to EEPROM from normal boot context.
2. [MAJOR] EEPROM record torn-write unsafe — magic written first → a fault mid-record yields valid-magic/garbage payload. Write payload first, magic/checksum LAST.
3. [MAJOR] One-shot consume wrong — if both .noinit+EEPROM valid, clearing only RAM leaves EEPROM magic → stale re-emit. Clear persistent state ONLY after %BOOT emit succeeds.
4. [MAJOR] .noinit cannot stay unverified — prove it's outside _bss_begin.._bss_end + NOLOAD, or use the core's persistent section.
5. [MAJOR] Handler linkage — extern "C" in an .ino isn't enough; put it in a .c/.cpp, strong unmangled _general_exception_handler, nomips16 + noreturn, no Arduino prototype interference.
6. [MAJOR] Store raw Cause, not just ExcCode — BD bit matters (if set, EPC = the branch, not the delay-slot faulting instr).
7. [MAJOR] %BOOT parser safety — prove by replaying session_start.py against a sample "%BOOT … exc=… epc=…" line, not just literal grep.
8. [MINOR] SYSKEY→RSWRST — add barriers/sync, IE off, KSEG1 SFR access, dummy-read RSWRST, never return.
9. [MINOR] RCON/SFR alternatives don't localize (no fault PC); raw pre-erased flash slot only safer if linker-reserved+magic-last+tested; else .noinit + normal-context EEPROM copy is safest.

### Gemini findings
1. [BLOCKER] EEPROM DEE write in the handler = severe brick risk — a page-pack (flash erase ~20ms/page, IE-disabled) from a faulted context risks a WDT reset MID-ERASE → permanently corrupts the DEE space (destroys ALL persistent config). Drop the EEPROM backup from the handler.
2. [MAJOR] .noinit IS safe + verifiable — COMMON.ld puts _bss_end at the end of the explicit .bss; the linker orphans .noinit AFTER _bss_end; cpp-startup.S zeroes only _bss_begin.._bss_end → .noinit survives a warm reset, guaranteed. Use it (exclusively) → no flash-pack risk, saves flash.
3. [MINOR] The entry stub loops (while(1)), it does NOT ERET — the freeze is that explicit loop, not a refault. Your handler's soft-reset is still correct (else it drops into the stub's while(1) → WDT resets).
4. [MINOR] SYSKEY from KSEG0 can hit cache-miss pipeline delays that break the back-to-back unlock → keep the while(1) fallback; the armed WDT resets cleanly and preserves .noinit.

### Resolution (Claude)
BLOCKER (both) ACCEPTED — REMOVE all flash/EEPROM writes from the handler. New design: handler writes
`.noinit` ONLY, then soft-resets; `setup()` (safe normal context) copies `.noinit`→EEPROM for
power-cycle survival (honors the user's "both" choice without the brick risk; Codex 1/9). Gemini 2
ACCEPTED + RESOLVES Codex 4 — `.noinit` is proven-safe (orphaned after _bss_end). Accepted: magic-LAST
(Codex 2); clear only after %BOOT emit, consume both stores consistently (Codex 3); handler in a
separate `.c` file, strong unmangled symbol, nomips16/noreturn (Codex 5); store RAW Cause incl. BD, not
just ExcCode (Codex 6); replay-test the real %BOOT parser (Codex 7); reset barriers/IE-off/KSEG1/
while(1)-WDT-backstop (Codex 8, Gemini 4). Gemini 3 — corrected the plan's "refault" wording to the
stub's while(1). Split: Gemini leaned .noinit-ONLY (simplest); Codex leaned .noinit-in-handler +
EEPROM-copy-in-setup (the SAFE "both"). Chose Codex's safe-both — matches the user's Act-1 "both"
(power-cycle survival) while the handler still touches no flash; .noinit-only is the documented fallback
if flash is too tight.

## Round 2 — Codex: CHANGES_REQUESTED | Gemini: CHANGES_REQUESTED (both: precise impl-detail fixes)
### Codex: 1[MAJOR] EEPROM 12..22 = 11 B < 12 B payload (undersized). 2[MAJOR] EEPROM copy torn/stale — needs invalidate-before-write + payload-first + magic-last. 3[MAJOR] EEPROM address reservation not proven — audit the map.
### Gemini: 1[BLOCKER] EEPROM range math (11 B < 13-16 B). 2[MAJOR] setup() EEPROM copy also needs payload-first/magic-last. 3[MINOR] wrap the g_crash extern in `extern "C" {}`.
### Resolution (Claude): ALL accepted. EEPROM MAP AUDITED (grep): firmware uses 0-7,10,11; bytes 8,9 &
12+ FREE. New layout [12..15]cause [16..19]epc [20..23]badv [24]magic = 13 B (12..24). Both the .noinit
handler write AND the setup() EEPROM copy are payload-first/magic-last + invalidate-before-write.
g_crash extern wrapped in `extern "C" {}` in the .ino. Clear-after-emit targets EEPROM[24].

## Round 3 — Codex: APPROVED | Gemini: APPROVED ✅ (both signed off, 3 rounds)
Confirmation after the R2 impl-detail fixes. Both: no net-new blocker/major; R2 fixes internally
consistent (EEPROM 12..24 = 12 payload + magic-last; invalidate/payload/magic-last setup copy; audited
free range; extern "C"). Plan APPROVED. Real holes the panel caught across 3 rounds: (R1 BLOCKER, both)
EEPROM-write-in-exception-handler = DEE-page-pack brick risk → handler writes .noinit ONLY, EEPROM copy
moved to safe setup() context; (R1 Gemini) .noinit proven-safe via linker analysis; (R1 Codex) store
raw Cause+BD not just ExcCode, magic-last torn-write safety, clear-after-emit, separate-.c-file strong
symbol; (R2 both) EEPROM range math (11B<13B) + torn-write on the setup copy + map audit. Next: /grill.
