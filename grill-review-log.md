# grill-review-log — Cyton freeSD stray-RX hardening (implementation of prep.md)

Builder: Claude (orchestrator, no review vote). Reviewers: Codex (gpt-5.5) + Gemini (3.1 Pro).
Branch: grill/cyton-rx-hardening. Build: 118,324/122,880 B (96%, 4,556 free), RAM 36%, clean.

## Implementation consults
(none yet — flash-fit reclaim decided by the builder: dropped dead wifi.loop()+wifi RX block,
gc-sections reclaimed the unreferenced WiFi methods; kept wifi.begin(). Logged as a deviation
from prep's "trim B=/strings" order — the additions were ~430 B, more than prep estimated, and
dead-WiFi reclaim preserves all user-chosen features instead of cutting B= or mangling diagnostics.)

## Review round 1 — Codex: CHANGES_REQUESTED | Gemini: CHANGES_REQUESTED
### Codex findings
1. [MAJOR] SD_Card_Stuff.ino recovery drain — only feeds Serial0, and doesn't return when abortRequested
   becomes true mid-stall (escape can wait out the recovery window; Serial1 stop bytes invisible).
2. [MAJOR] session_start.py --stop — writes the token once, sleeps, prints success, exits 0 WITHOUT
   confirming firmware close; a dropped byte leaves the lockout active.
### Gemini findings
1. [MAJOR] recovery drain — missing Serial1 drain (prep mandated "every active ingress transport").
2. [MINOR] metaArmed gate breaks legacy OpenBCI GUI SD recordings (slot→'b' with no %META) — accept as
   intentional deprecation or document.
### Resolution (orchestrator)
- Codex#1/Gemini#1 ACCEPTED — recovery drain now feeds BOTH Serial0 and Serial1, and bails immediately on
  abortRequested (`byteCounter=0; board.csHigh(SD_SS); return;`) so the escape is responsive mid-stall and
  loop() services performAbort promptly.
- Codex#2 ACCEPTED — rewrote --stop as send_stop_and_confirm(): moved AFTER the helper defs (it used
  wait_for_response before definition); now waits for the close/EOT '$$$' confirmation with a 10s timeout,
  retries the token up to 3x, and sys.exit(nonzero) if never confirmed.
- Gemini#2 REJECTED (documented) — the %META-before-'b' requirement is the user-confirmed Decision 4b
  (premature-'b' defense); added an explicit firmware comment noting the stock-GUI incompatibility.
### Tests after this round
- Firmware: compiles, 118,492 B (96%, 4,388 free). Host: py_compile OK. Token still matches.

## Review round 2 — Codex: APPROVED | Gemini: CHANGES_REQUESTED (1 net-new MAJOR)
### Gemini finding
1. [MAJOR] session_start.py --stop Two-Generals: if the escape STOPS the board but the '$$$' close
   confirmation is dropped over RF, the retry hits an already-IDLE board where performAbort emits no output
   (SDfileOpen already false) → host times out, falsely reporting failure. Fix: unconditional firmware ACK
   in performAbort, or host probes IDLE via 'v'.
### Resolution (orchestrator)
- ACCEPTED (Gemini option b) — performAbort() now ALWAYS calls board.sendEOT() at the end, so an idempotent
  retry on an IDLE board still emits '$$$'. The host's send_stop_and_confirm (waits for '$$$', 3 retries)
  now confirms in both the first-abort and dropped-ACK-retry cases. Chose the firmware ACK over a host 'v'
  probe (simpler, no new host round-trip, ~no flash cost — reuses the existing EOT).
- Codex APPROVED round 2 (round-1 fixes confirmed real).
### Tests after this round
- Firmware compiles, size below.

## Review round 3 — Codex: APPROVED | Gemini: APPROVED ✅
Both signed off. Codex: "No findings." Gemini: escape/abort rock solid (byteCounter=0 neutralizes the
pCache appender; top-level block-boundary abort avoids FAT re-entrancy), auto-resume fully handled
(metaArmed bypass + escape breaks lockout), Two-Generals resolved, root-cause truncation mitigated at both
the dispatch and caller-guard levels. NO open BLOCKER/MAJOR after 3 rounds.

## Deploy
- Firmware repo is LOCAL (z13) — committed on branch `grill/cyton-rx-hardening`. Built artifact:
  `build-grill/DefaultBoard.ino.hex` (118,504 B / 122,880, 96%, 4,376 free; RAM 36%).
- Host repo `openbci-session` — session_start.py committed.
- The actual DEPLOY for firmware = flashing the PIC32 via pic32prog. This is a PHYSICAL, user-gated step
  (board currently powered off, SD card out; P3 verification needs the user to inject stray bytes +
  power-cycle). Flash + Act-5 verification handed to the user with exact steps; NOT done unattended.

## Deploy (FLASHED) — 2026-06-24
- pic32prog v2.1.46, STK500v2 bootloader, /dev/ttyUSB0 @ 115200, no kill-timeout.
- build-grill/DefaultBoard.ino.hex (md5 9155fbd4667c9bc01300035184449e66, 119,808 B data).
- Program flash: done. Verify flash: done. exit 0. ~797 B/s. NO brick.

## Post-deploy verification
- Liveness ('v'): "OpenBCI V3 8-16 channel ... Firmware: v3.1.2-freeSD $$$" — new firmware boots,
  ADS1299 (ID 0x3E) + LIS3DH (0x33) up.
- IDLE-state dispatch ('?'): full ADS register dump returned → new dispatchCommandByte processes
  commands correctly in IDLE (SD card out → board idle, no replay).
- PENDING (user-driven, needs SD card in the board): the P3 deterministic fault-injection
  (start -a 20, inject stray F/j/s/1 mid-stream, prove B=2432000 holds + recording survives) and
  the --stop escape round-trip. The board liveness + flash integrity are confirmed; the behavioral
  fault-injection requires the physical SD card + a live recording.

## Post-deploy verification — HARDWARE FAULT-INJECTION (PASSED) 2026-06-24
Activity 22 added: 1000Hz / 8ch / 12H / cyton (cyton_cap_channels + emg_empty) — exhaustive stress.
- session_start -a 22 --tune ckpt_interval_ms=5000: handshake clean, "Size 4864000 SD file OBCI_50.TXT"
  (B=4,864,000 = correct 12H@1000Hz), "meta verified: 286 bytes", "Session started". metaArmed gate
  accepted the legit %META-before-b. 1000Hz×8ch sustained: write 326-332us, Overruns 0.
- Injected stray 'F' (30-min slot), 'j' (close), '1' (channel off) MID-STREAM via the dongle.
- Sent the 8-byte escape token → ABORT CONFIRMED ($$$$$$ = closeSDfile EOT + performAbort unconditional
  EOT), board returned to IDLE & responsive ('v' banner) — NOT wedged. closeSDfile's footer-echo on the
  escape proves the FILE WAS STILL OPEN (the stray 'j' did NOT close it).
- Read OBCI_50.TXT back: %META PRESENT (8ch/1000Hz JSON); 21 %CKPT lines, EVERY one B=4864000 (distinct
  set = {4864000}); blockCounter monotonic 348->11963 with NO reset; no early footer (clean escape abort).
  ⇒ the stray 'F' was IGNORED — BLOCK_COUNT held at 4,864,000, never the 202,000 it would have forced.

VERDICT: the 06-23 stray-slot-char truncation is FIXED and verified on hardware. Flash + behavioral proof complete.

---

# /grill: post-reset SD auto-resume recovery (2026-06-28, branch grill/cyton-sd-recover)

Implements prep.md "robust post-reset SD auto-resume". Builds on the rx-hardening work above.

## Implementation consults

### Consult 1 (Act 1) — ARCHITECTURE FORK: hardware-DSPI vs the prep's bit-bang  [Codex + Gemini]
**Discovery during implementation:** the prep (7 rounds) assumed the SD card is driven by the STOCK
chipKIT SD library's SOFTWARE BIT-BANG (PORT-register toggling of prtSCK/prtSDO/prtSDI), and built an
elaborate sdBusRecover around disabling DSPI + clearing the MOSI/SDO1 PPS map (RPxnR=0) + LAT-before-
TRIS + ISR-masking. Reading the ACTUAL source contradicts that premise:
- The firmware uses a local fork **OBCI32_SD** (~/Arduino/libraries/OpenBCI_32bit_SD), constructed
  `Sd2Card card(&board.spi, SD_SS)`. In that fork `spiRec()/spiSend()` route through `_spi->transfer()`
  (HARDWARE DSPI0) whenever `_spi` is non-null — which it is. The IOPORT_G bit-bang branch is DEAD code.
- `board.begin()` (DefaultBoard.ino:84) runs BEFORE `replaySessionFile()` (:113) and calls
  `spi.begin(); spi.setMode(DSPI_MODE0)` — the DSPI0 is fully initialized before recovery runs. It also
  parks BOARD_ADS + LIS3DH_SS chip-selects HIGH. ADS+SD share DSPI0 (separate CS GPIOs 8 vs 2).
- `board.csLow(SD_SS)/csHigh(SD_SS)` (public) set MODE0/20MHz + CS; `board.spi.transfer()` is public;
  `waitNotBusy()` already implements the bounded MISO-high busy-wait. The ADS_DRDY ISR only reads PORTA
  and sets a flag (no SPI), so ISR-masking is moot.

**Proposed correction:** do the CMD25/CMD18 abort over the already-initialized HARDWARE DSPI
(board.csLow(SD_SS) → drain >=514x 0xFF → bounded busy-wait → 0xFD → busy-wait → best-effort CMD12 →
csHigh + trailing clocks), entirely in the .ino, NO bit-bang/PPS/LAT-TRIS/ISR-mask.

**Both models: CONFIRMED — ship the hardware-DSPI sdBusRecover, drop ALL bit-bang/PPS machinery.**
- Codex: "the bit-bang/PPS machinery is unnecessary and inapplicable... use the hardware-DSPI recovery.
  The original bit-bang/PPS plan was based on the wrong SD transport for this source tree." Confirmed the
  busy-wait `!=0xFF` test, the 520-byte drain (512+2+margin), CMD12 framing + R1 stuff-byte skip, the 2s
  bound (> SD_WRITE_TIMEOUT 1500ms). Nit: order the while as `(millis()-t0)<BOUND && transfer(0xFF)!=0xFF`
  to avoid one extra transfer after timeout (harmless, adopted).
- Gemini: "the previous plan's core premise was fundamentally flawed for this fork... textbook SD protocol
  recovery state machine... ship your proposed sdBusRecover() in the .ino. Rip out all the PPS/LAT/TRIS
  bit-bang complexity." Validated busy-wait, bounded loops (rollover-safe), CMD12 framing, R1 16-byte scan,
  trailing clocks.
- **KEEP (both):** SD_SS park HIGH before board.begin() (ADS+SD share DSPI0); force cardInit=false + re-run
  card.init/volume.init; resume-only stabilization delay; non-destructive REPLAYFL; setupSDcard fail-fast +
  remove-guard. **DROP (both):** DSPI disable/reacquire, PPS unmap, GPIO bit-bang, LAT/TRIS ordering, DRDY
  ISR masking.

Resolution: implement the hardware-DSPI sdBusRecover (Codex's term-ordering nit adopted; 520 = 512+2+6
margin). All other prep decisions (5,6,7,8 + the pre-begin park) carry over unchanged.

## Review round 1 — Codex: CHANGES_REQUESTED | Gemini: CHANGES_REQUESTED
### Codex findings
1. [MAJOR] sdBusRecover clocks a pre-init card at 20MHz (board.csLow sets 20MHz) — violates SD
   identification timing / changes normal boot. Fix: init-safe rate, or init-first then recover-on-fail.
2. [MAJOR] setupSDcard still falls through after card.init() failure into volume.init on a dead transport.
   Fix: on card.init fail, sendEOT + return (fail-fast).
3. [MAJOR] An existing ZERO-BYTE REPLAYFL.TXT suppresses all future failure markers (one-shot guard
   treats any readable file as valid). Fix: only skip a validated non-zero marker; replace zero-byte.
### Gemini findings
1. [BLOCKER] probe.open(root,...) API mismatch — claims compilation failure (expects pointer).
### Resolution (orchestrator)
- Codex#1 → ACCEPTED, via a BETTER restructure: init-first, and ONLY if card.init fails run sdBusRecover
  + retry init (in both replaySessionFile and writeReplayFail). This makes a normal/cold boot
  byte-identical (recovery never runs unless the card is actually wedged) and never clocks a healthy card
  at 20MHz before card.init's own low-speed identification. Strictly better than the unconditional recover.
- Codex#2 → ACCEPTED: setupSDcard init block now `else { sendEOT; return fileIsOpen; }` on card.init fail.
- Codex#3 → ACCEPTED: writeReplayFail now reads the existing REPLAYFL's fileSize — skip only if >0
  (valid prior marker); an absent OR zero-byte/partial marker is (re)written so a botched stub can't
  silence later failures.
- Gemini#1 → REJECTED as stated (NOT a compilation failure — the build links at 118752 bytes, and
  SdFat.h has the `open(SdFile& dirFile, const char*, uint8_t)` reference overload at line 329). BUT
  adopted the `&root` form anyway for consistency with writeReplayFail (rf.open(&root,...)). Harmless.
- Flash: round-1 fixes re-overflowed by ~120B; trimmed more dead !board.streaming diagnostics in
  setupSDcard (erase/writeStart fail strings, the "Size N SD file NAME" success print). Now 118752/122880.
### Tests after this round
arduino-cli compile chipKIT:pic32:openbci → links, 118752 bytes (96%), RAM 11848 (36%) unchanged. .hex produced.
### Decision-9 verification (collect_bci tolerance)
Confirmed by the existing footerless workflow: the user powers off (no --stop), so EVERY night's prior
slot already ends in an undefined tail after the last good sample and collect_bci processes it fine.
The drain just makes the interrupted block deterministic (partial+0xFF) vs stale — same junk-tail class,
no new data-integrity risk.

## Review round 2 — Codex: CHANGES_REQUESTED | Gemini: APPROVED (1 MINOR)
### Codex findings
1. [MAJOR] REPLAYFL self-heal trusts ANY nonzero stub (prevSz>0) — a write truncated by a 2nd reset
   leaves a 1-byte/partial file that suppresses all later markers. Fix: validate marker shape (prefix
   + newline + min length) or rewrite unless it parses.
2. [MAJOR] setupSDcard still falls through after erase()/writeStart() failure — leaks the open
   contiguous openfile handle + continues after a failed erase. Fix: fail-fast cleanup on both.
### Gemini findings
APPROVED. Validated boundedness, init-first/recover/retry, fail-fast, remove-guard, REPLAYFL, SD_SS park.
1. [MINOR] snprintf returns INTENDED length — if the format ever exceeded b[40], rf.write(b,n) reads
   past the buffer. Caps at 26 chars today so can't trigger, but fragile. Fix: clamp n to sizeof(b).
### Resolution (orchestrator)
- Codex#1 → ACCEPTED: REPLAYFL now reads the first 5 bytes and trusts the existing marker only if they
  == "code=" (every real marker, complete or partial-but-readable, starts with it; written L-to-R). A
  stub/garbage fails the prefix → SdFile::remove + (re)write. Robust against truncation-by-2nd-reset.
- Codex#2 → ACCEPTED: factored a `setupSDfail()` helper (openfile.close + sendEOT + csHigh + return
  fileIsOpen) and routed ALL FOUR allocation failures (createContiguous/contiguousRange/erase/
  writeStart) through it — no fall-through, no handle leak. DRY (also saved flash vs inline copies).
- Gemini MINOR → ACCEPTED: `if (n > (int)sizeof(b)) n = sizeof(b);` before rf.write.
- FLASH: the round-2 fixes re-overflowed by ~56B and the remaining Serial0 strings are host-protocol
  ACKs (META OK/FAIL, TUNE/PERSIST FAIL) that must NOT be trimmed. Instead DROPPED the CMD12 step from
  sdBusRecover: this firmware only issues multi-block WRITES (CMD25) during recording, so the wedge is
  always a stuck CMD25, for which 0xFD is the correct+sufficient stop (both models confirmed CMD12 is
  not required for CMD25). CMD12 only aborts a multi-block READ (CMD18), which this firmware never
  issues — so it guarded an impossible state. Documented inline with a restore note. Now 118676/122880.
### Tests after this round
arduino-cli compile → links, 118676 bytes (96%), RAM 11848 (36%) unchanged. .hex produced.

## Review round 3 — Codex: CHANGES_REQUESTED | Gemini: APPROVED (2 MINOR)
### Codex findings
1. [MAJOR] setupSDfail leaves an orphaned FULL-SIZE preallocated file after a post-createContiguous
   failure (contiguousRange/erase/writeStart) — only closes, doesn't remove. The remove-guard later
   treats it as real data; collection could ingest garbage. Fix: remove currentFileName in cleanup.
2. [MAJOR] REPLAYFL prefix-only accepts a bare "code=" stub → future markers suppressed. Fix: validate
   a complete marker (prefix + ... + newline).
3. [MAJOR] remove-guard 32-cap can fail when free names exist past 32 occupied slots. Fix: scan full
   256-name namespace / until counter wraps to start.
4. [MINOR] SD_SS pinMode(OUTPUT) before digitalWrite(HIGH) can briefly drive a power-up-low latch
   (select card). Fix: latch high first, then OUTPUT.
### Gemini findings
APPROVED. Validated setupSDfail idempotency, REPLAYFL purge, sdBusRecover bus-abort + boundedness,
init-first/recover/retry, boot safety, SD_SS park.
1. [MINOR] snprintf clamp should be sizeof(b)-1 (else a truncated write would emit the '\0'). 
2. [MINOR] add O_TRUNC to the REPLAYFL recreate (clean slate if remove silently failed).
### Resolution (orchestrator) — ALL accepted
- Codex#1 → setupSDfail now `SdFile::remove(&root, currentFileName)` after close — removes the orphaned
  full-size slot on any post-create failure; harmless no-op on the pre-create path (name already free).
- Codex#2 → REPLAYFL now reads up to 40 bytes and trusts the marker ONLY if it starts with "code=" AND
  ends with '\n' (a complete "code=...\n"); a stub/torn-garbage is removed + rewritten.
- Codex#3 → remove-guard rewritten: scan until a free/0-byte name is found OR the counter wraps back to
  the starting candidate (all 255 legal names hold real data → fail-fast). No arbitrary cap. Typical
  case is 1 iteration (the just-incremented next name is free).
- Codex#4 → DefaultBoard.ino: digitalWrite(SD_SS,HIGH) BEFORE pinMode(SD_SS,OUTPUT).
- Gemini#1 → snprintf clamp to sizeof(b)-1. Gemini#2 → O_TRUNC added (verified defined at SdFat.h:70).
### Tests after this round
arduino-cli compile → links, 118744 bytes (96%), RAM 11848 (36%) unchanged. .hex produced.

## Review round 4 (backstop) — Codex: APPROVED (2 MINOR) | Gemini: CHANGES_REQUESTED (1 BLOCKER)
### Codex findings
APPROVED. 1.[MINOR] REPLAYFL validator looser than format (would keep "code=\n"/"code=garbage\n") — self-
rated non-blocking (the formatter can't naturally produce that). 2.[MINOR] remove-guard treats any
probe.open()==false as "free", collapsing media/FAT read errors into not-found — self-rated non-blocking
(a real SD fault fails the subsequent create too).
### Gemini findings
1. [BLOCKER] remove-guard infinite loop on a start of file "00": incrementFileCounter wraps FF→01
   (skipping 00), so a wrap-detection keyed on the start "00" never re-matches → infinite loop → hang →
   brick (WDT off). Fix: decouple termination from the filename wrap; use a numeric attempts<256 bound.
### Resolution (orchestrator)
- Gemini BLOCKER → the SPECIFIC scenario CANNOT occur (verified): the guard's starting candidate is set
  by incrementFileCounter() at line ~1297 BEFORE the guard, and incrementFileCounter NEVER yields "00"
  (virgin EEPROM seeds '0','0' then ones++ → "01"; the cycle is 01..0F,10..FF). So the start is always
  in-cycle and the wrap DOES return to it. BUT Gemini's underlying point is correct and important:
  string-wrap termination is fragile, and with the soft-WDT disabled an unbounded scan would brick the
  night. ACCEPTED the simpler numeric `attempts >= 256` bound — guaranteed-terminating regardless of the
  filename cycle, and still covers the full 255-name namespace (satisfies R3 Codex#3). Defensive
  correctness in WDT-off boot code beats "can't happen today".
- Codex#1 MINOR → ACCEPTED (lightweight): validator now also requires a digit at prev[5]
  (code=<digit>...) — rejects "code=\n"/"code=<nondigit>". ~free.
- Codex#2 MINOR → ACCEPTED as a documented best-effort limitation (no code): under a transient SD I/O
  fault probe.open()==false is read as "free", but the subsequent createContiguous fails on the same
  fault → setupSDfail → REPLAYFL/idle, so no data is overwritten. Distinguishing media-error from
  not-found isn't worth the flash here.
### Tests after this round
arduino-cli compile → links, 118716 bytes (96%), RAM 11848 (36%) unchanged. .hex produced.

## Review round 5 (confirm round-4 BLOCKER fix) — Codex: APPROVED | Gemini: APPROVED
### Codex findings
No findings. Confirmed: remove-guard terminates on the `attempts < 256` numeric bound, closes
`probe` on every successful open, finds any free/0-byte slot before fail-fast, fails only when all
255 legal names are occupied by non-zero files; REPLAYFL now rejects `code=\n`/torn prefixes; no
net-new boot-safety/data bug; normal no-reset SD path byte-identical.
### Gemini findings
No findings. Confirmed: (1) remove-guard bounded by `++attempts >= 256`, guaranteed-terminating,
no off-by-one (covers 255-name namespace), no handle leak (`probe.close()` unconditional before the
fail-fast test); (2) REPLAYFL `got>=6 && "code=" && digit@prev[5] && prev[got-1]=='\n'` correctly
rejects `code=\n`; (3) `sdBusRecover()` gated behind initial `card.init()` failure and the
stabilize delay gated inside the resume success block → a glitch-free boot bypasses both, byte-for-
byte identical SD protocol + FAT mutation. No new regressions.
### Resolution (orchestrator)
Both APPROVED, no net-new findings → review CONVERGED. The round-4 numeric-bound + digit-check fix
is confirmed correct by both reviewers. No code change this round.
### Tests after this round
No code change since round 4 → build unchanged: 118716 bytes (96%) of program storage, RAM 11848
(36%). .hex present in build-grill/.

## Final state
Codex+Gemini both APPROVED at round 5 (rounds 1-4 CHANGES_REQUESTED→fixed, round 5 clean). Total 5
review rounds + 1 implementation/architecture consult. Build green at 118716/122880 bytes (4164 B
headroom), RAM 11848/32768. Artifact: build-grill/DefaultBoard.ino.hex. NOT flashed — final
verification is the user's flash + overnight sleep test (a mid-night external reset cannot be
reproduced on the bench).

## Resume-diagnostics breadcrumb + flash trim (2026-06-28, single-flash bundle)
Added AFTER the 5-round SD-recovery sign-off, at user request ("add the breadcrumb + a gap estimate;
keep flashes minimal" → one flash with both). Change: (1) `uint8_t sdRecoverOutcome` stamped into the
resumed slot's %BOOT as `rcv=<0-2>` (0 recovery not needed / 1 sdBusRecover ran + retry init OK / 2 ran
but failed — practically unobservable since a failed recover returns before %BOOT); (2) `g=0x<HEX16>` =
low-16-bits of millis() at slot creation = boot→resume latency = a LOWER BOUND of the inter-slot gap
(no RTC → unpowered time unmeasurable; for a brief glitch off-time~0 so this is the estimate); both
GATED behind `if (autoResume)` so a normal night's %BOOT is byte-for-byte unchanged; (3) flash trim —
removed the closeSDfile `!board.streaming` console write-time/overrun stats (kept board.sendEOT $$$),
net build 118520 B (below the pre-breadcrumb 118716).

### Review round 1 — Codex: APPROVED | Gemini: CHANGES_REQUESTED
- [Gemini MAJOR] EMIT_HEX16 is a macro evaluating its arg once per nibble (4x) → `EMIT_HEX16(millis()&0xFFFF)`
  re-reads the live timer mid-expansion and can TEAR the hex across timestamps. (Latent: existing
  EMIT_HEX16 callers pass stable globals.) → FIXED: snapshot `uint16_t gapMs = millis()&0xFFFF; EMIT_HEX16(gapMs);`
- [Gemini MINOR] sdRecoverOutcome stale carry-over if replaySessionFile re-runs w/o BSS clear → FIXED:
  `sdRecoverOutcome = 0;` at top of the recover block every invocation.
- [Codex+Gemini MINOR] comment said "8-nibble hex" but emits 4 nibbles → FIXED comment.
### Review round 2 — Codex: APPROVED | Gemini: APPROVED
Both confirmed: gapMs is a single side-effect-free snapshot (no tearing/UB); sdRecoverOutcome reset
each invocation; normal-night %BOOT byte-identical (autoResume gate); prefixR/prefixG are exactly 5
chars matching EMIT_LIT; ternary bounds rcv to a printable digit; trim preserves sendEOT. No net-new
findings → CONVERGED (2 rounds).
### Tests
arduino-cli compile → 118520 bytes (96%), RAM 11852 (36%). .hex in build-grill/.

## Host-contract regression fix — restore the "Size " slot-open print (2026-06-28, hardware-caught)
Found at RUNTIME on the first fresh start after flashing: session_start.py reported "SD init failed"
even though the board opened the slot fine. Root cause: the recovery-fix flash trim removed
setupSDcard's `Serial0.print("Size "); print(BLOCK_COUNT); print(" SD file "); println(currentFileName);`
success line — which the HOST parses (`re.findall('Size ', res)` + `r'I\_.*\.T'` for the filename) to
confirm the slot opened and to send %META + 'b'. No "Size " → host bails before 'b' → nothing records.
The firmware panel had reviewed the firmware in ISOLATION and couldn't see the host contract.
(Tonight's recording survived only because a user power-cycle made the firmware AUTO-RESUME from the
SESSION.TXT the failed run had already written — auto-resume needs no host.)

### Fix
Restored the exact original print at the setupSDcard success point (fileIsOpen && !board.streaming) +
a loud `⚠ HOST CONTRACT — DO NOT trim` comment. Build 118600 B (96%), RAM 11852 unchanged.

### Audit (Gemini+Codex both demanded it — "if they trimmed Size, they trimmed others")
Grepped the firmware for EVERY string session_start.py parses: `Size `(restored), `META OK/FAIL/ERR`
(2/2/1 — intact), `PERSIST OK/FAIL/ERR`(2/2/2 — intact), `TUNE OK/FAIL`(2/4 — intact). `Sample rate
is …Hz`, the `?` register dump, and `$$$` are library-level (untouched). PERSIST/TUNE/$$$/register
dump were ALSO confirmed working live tonight. CONCLUSION: `Size ` was the ONLY host string trimmed.

### Review — Codex: APPROVED | Gemini: APPROVED
Both confirmed the restored line emits literal `Size ` + the OBCI_NN.TXT name (host regex matches),
is correctly gated (fileIsOpen && !streaming, no mid-stream emit), is print-only (no control-flow
change to sdBusRecover/breadcrumb/fail-fast paths), no net-new bug. MINORs: (1) Gemini `F()` macro —
REJECTED (AVR Harvard-arch only; PIC32 maps const char[] to flash already, and the whole file uses
plain literals — F() would be inconsistent + pointless). (2) both flagged "audit other host strings"
— DONE above, all intact. Documented the contract in CLAUDE.md "⛔ HOST-CONTRACT STRINGS — NEVER TRIM".
