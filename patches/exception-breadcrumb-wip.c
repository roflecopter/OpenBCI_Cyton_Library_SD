/* Forensic exception breadcrumb (2026-07-05, /work) — see prep-breadcrumb.md.
 *
 * Overrides the chipKIT WEAK `_general_exception_handler` (cores/pic32/exceptions.c). The core's
 * `_general_exception_handler_entry` stub already read CP0 Cause/EPC and calls us with the shifted
 * ExcCode + EPC; we RE-READ the raw Cause (to keep the BD branch-delay bit) + BadVAddr, stash all
 * three + a magic into a `.noinit` RAM record, then software-reset. On a warm reset `.noinit` survives
 * (the GNU linker orphans it AFTER `_bss_end`, and cpp-startup.S zeroes only `_bss_begin.._bss_end` —
 * verified in the .ld/.map), so `setup()` reads it and copies it to EEPROM (in NORMAL context), and
 * `setupSDcard()` emits `exc=/epc=/bv=` in the resumed slot's %BOOT.
 *
 * This handler does NO flash / EEPROM / Serial / SD / SPI — a DEE page-pack (flash erase, ~20 ms,
 * interrupts off) from a faulted context could take a WDT reset mid-erase and PERMANENTLY corrupt the
 * EEPROM config space (Codex+Gemini prep BLOCKER). Only CP0 reads, RAM writes, and the reset.
 *
 * It is a SEPARATE .c file (not the .ino) so the symbol is a strong, unmangled `_general_exception_
 * handler` that overrides the core's weak default (Arduino auto-prototyping / C++ mangling in an .ino
 * would not reliably produce that).
 */
#include <p32xxxx.h>
#include <stdint.h>

/* Persistent crash record. Payload first, `magic` LAST (torn-write safety). Shared with the .ino via
 * an `extern "C"` block. `.noinit` = not zeroed by startup (survives a warm/soft reset). */
struct CrashRec { uint32_t cause; uint32_t epc; uint32_t badv; uint32_t magic; };
struct CrashRec __attribute__((section(".noinit"))) g_crash;
#define CRASH_MAGIC 0x0BC1FA17u

void __attribute__((nomips16)) _general_exception_handler(uint32_t code, uint32_t address)
{
    (void)code; (void)address;                        /* stub-supplied shifted ExcCode/EPC — we re-read raw */
    uint32_t cause, epc, badv;
    __asm__ volatile ("mfc0 %0,$13" : "=r"(cause));   /* Cause: ExcCode(6:2), BD(31) */
    __asm__ volatile ("mfc0 %0,$14" : "=r"(epc));     /* EPC (faulting instr, or the branch if BD=1) */
    __asm__ volatile ("mfc0 %0,$8"  : "=r"(badv));    /* BadVAddr */

    /* payload FIRST, magic LAST, with a barrier so the compiler/CPU can't publish magic early. */
    g_crash.cause = cause;
    g_crash.epc   = epc;
    g_crash.badv  = badv;
    __asm__ volatile ("sync" ::: "memory");
    g_crash.magic = CRASH_MAGIC;
    __asm__ volatile ("sync" ::: "memory");

    /* Software reset: interrupts off, SYSKEY unlock (must be consecutive), RSWRST.SWRST(bit0)=1,
     * dummy-read RSWRST to trigger, then spin. The while(1) is a real backstop: if a KSEG0 cache-miss
     * pipeline delay voids the SYSKEY sequence, the ARMED hardware WDT resets the board within ~1 s and
     * `.noinit` still survives. */
    __builtin_disable_interrupts();
    SYSKEY = 0x00000000;
    SYSKEY = 0xAA996655;
    SYSKEY = 0x556699AA;
    RSWRSTSET = 1;                                     /* SWRST = bit 0 */
    (void)RSWRST;                                      /* read arms/triggers the reset */
    while (1) { }
}
