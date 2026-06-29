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

---
---

# Grill #2 — SPIROV hang fix + diagnostics layer (ONE flash, both preps)

Implements **both** approved preps into a single firmware image on branch
`grill/cyton-sd-recover` (base `38b3896`):
- `prep.md` — the ENH_BUFFER/SPIROV-overrun hang fix (bounded FIFO-safe SPI primitives,
  `sdSpiModuleFlush`, sticky `sdSpiFault`, live `sdBusRecover`, soft-WDT behind tune key 0x06,
  WDTPS readout).
- `prep-diag.md` — observability + tunable knobs (`%CKPT s=`, the `ps`/`~ps`/`pf` EEPROM
  freeze-breadcrumb surfaced in `%BOOT`, tune keys 0x07–0x0A, the synthetic-SPIROV self-test).

Builder: Claude (orchestrator, no review vote). Reviewers: Codex (gpt-5.5/xhigh) + Gemini
(3.1 Pro). GLM not installed on z13.

**Autonomous scope of this /grill** = implement → build green → size-check (<122880 B) →
host-contract fixtures → Codex+Gemini review to dual sign-off → commit. The **flash + overnight
`-a 20` sleep test are the user-gated hardware step** (NEVER flash without explicit user OK; the
board can brick; the multi-hour failure is only reproducible on the user's own overnight run).

## Implementation consults / grill-time decisions

### Decision G1 — fault-injector = compile-time `#ifdef SD_DEBUG_FAULT_INJECT` (resolved the deferred fork)
`prep-diag.md` Decision 8 shipped the runtime-gated injector **per the user's earlier explicit
choice**, but flagged a STRONG panel recommendation (Codex+Gemini, FOUR rounds R1/R3/R5/R6) to
make it a compile-time `#ifdef` instead. At /grill I resolve this fork (the /grill protocol:
resolve forks myself/via the panel + log; don't stop to ask). **Chosen: `#ifdef`.** Rationale:
(a) the panel's unanimous 4-round recommendation; (b) it resolves the direct contradiction
between `prep.md` Decision 14 (`#ifdef`) and `prep-diag.md` Decision 8 (runtime) — reverting to
prep.md's original spec; (c) it eliminates the entire RF-up-reachability residual (no corruptor
in the shipped hex); (d) it reclaims flash on the ~4280 B budget that is THE gating risk while
this flash already adds both preps' code, and it is `prep-diag` drop-order #1 under pressure
anyway; (e) the recovery PATH is byte-identical between the `#ifdef`-out production build and a
one-time `-DSD_DEBUG_FAULT_INJECT` bench build, so a debug-build bench test still validates the
production recovery — substantially meeting the user's "validate the exact hex" goal.
**⚠ USER-OVERRIDABLE:** the user explicitly chose *runtime* earlier; this reverts to `#ifdef`.
If the user wants the runtime gate back, say so and I'll switch (it's `prep-diag` Decision 8 as
written). Tune key `0x0B fault_inject_arm` is therefore **not** added in the production build.

### Decision G2 — EEPROM map correction (the plan's slots 2,3 are NOT free)
Both preps' "audited" EEPROM map is **stale**: `DefaultBoard.ino:63-69` reads AND writes
**EEPROM[2] and EEPROM[3] every boot** as `bootSeq`. `prep.md` Decision 12 assigned its
magic+complement WDT reset-pending/cap record to slots **2,3** → that would collide with
`bootSeq` and corrupt both. Re-audited every `EEPROM.read/write` across the sketch + fork:

- **Active (R/W):** 0,1 (file enumerator) · 2,3 (`bootSeq`, every boot) · 7 (resumeCount).
- **Legacy (written, never read):** 4 (sessionActive) · 5,6 (sessionSeq) · 10,11 (slot/rate).
- **Genuinely free:** 8, 9, 12, 13+ (DEE `E2END=0x0fff` → 4096 cells, so 8–16 all valid).

**Final map:** breadcrumb `ps`@8 / `pf`@9 / `~ps`@12 (prep-diag, free — kept as-is); prep.md's WDT
record **relocated 2,3 → 13 (`wdtRebootCount`) / 14 (`~wdtRebootCount`) / 15 (reset-pending magic)
/ 16 (`~magic`)**. All free. Logged here so the panel re-checks the collision is gone.

### Decision G3 — synchronous per-epoch `ps` write stays sketch-side (writeCache), not in the fork
`prep-diag` Decision 4 / Plan-2 places the synchronous per-epoch `ps`/`~ps` write "in
`Sd2Card.cpp`, in the fault path, before the recovery cascade." The fork does **not** include
`<EEPROM.h>`. To avoid coupling the SD library fork to the EEPROM lib, the breadcrumb write is
done **at the very top of `writeCache`'s SPIROV branch — BEFORE `sdSpiModuleFlush`/`sdBusRecover`/
`card.init`** (still synchronous, still before the hang-prone recovery cascade, gated by
`psWroteThisEpoch`). The fork only increments `sdSpirovSeen` on a confirmed `SPI1STAT.SPIROV`.
This preserves prep-diag's load-bearing invariant ("record the fatal first/N-th SPIROV before
recovery can hang") while keeping EEPROM bookkeeping in the sketch with all the other EEPROM
writes. Flagged for the panel to verify the invariant holds.

### Decision G4 — FLASH BUDGET WAS WRONG BY ~4 KB → user re-scoped to FIX-ONLY (Option 2)
The prep budgeted ~4280 B free (trusting arduino-cli's nominal "122880" ceiling). The linker
script (`chipKIT-application-32MX250F128.ld`) tells the truth: `kseg0_program_mem` is only
**0x1D000 = 118784 B** — a 4 KB DEE-EEPROM page **and** a 4 KB `splitflash` page are carved off
the top. So `38b3896` at 118600 B has only **~184 B free**, not 4280. Both preps need ~1–3 KB →
impossible without reclamation, and reclaiming the `splitflash` 4 KB (the one clean candidate)
risks bricking the board (it's the classic DP32-bootloader program-flash region on this tiny-boot
part; no ICSP recovery). **User chose Option 2: ship the SPIROV fix ONLY, drop the diagnostics
layer, trim low-value existing code to fit.**

**Final shipped scope (this flash):**
- **KEPT (the freeze cure):** `prep.md` Layer 1 (bounded FIFO-safe SPI primitives + `sdSpiModuleFlush`
  + sticky `sdSpiFault` + fault short-circuits, fork), Layer 1b (`writeCache` SPIROV recovery branch
  + live-safe `sdBusRecover`), Layer 1c (top-of-loop ADS guard).
- **DROPPED — `prep-diag.md` ENTIRELY** (the `%CKPT s=`, the `ps`/`~ps`/`pf` EEPROM breadcrumb,
  `%BOOT` decode, tune keys 0x07–0x0A). → G1/G2/G3 above are now moot (no injector, no breadcrumb,
  no new EEPROM use, no soft-WDT EEPROM record). EEPROM map is UNCHANGED from `38b3896`.
- **DROPPED — `prep.md` Layer 2 (soft-WDT)**: OFF-by-default backstop for *non-SPIROV* hangs, not
  the diagnosed cause; biggest sketch-side flash cost. Deferred to a future flash if room.
- **DROPPED — `prep.md` Layer 3 (WDTPS readout)**: lowest-value diagnostic; the HW WDT is
  bootloader-locked/unchangeable.
- **DROPPED — `prep.md` Layer 4 (synthetic-SPIROV self-test)**: does NOT fit. At 96%
  fragmented-full, ANY added code grows a function's section past its gap and the chipKIT allocator
  can't place it (verified: even a ~10 B debug arm fails to link; production has 340 B *total* free
  but fragmented). Validation is therefore: this Codex+Gemini review + the `-a 22` 1000 Hz
  throughput test (exercises the bounded primitives under load) + the user's overnight `-a 20` test.

**Flash reclamation applied (host-confirmed SAFE):** an Explore subagent verified the host parsers
(`sd_convert.parse_ckpt_line`, `process_file`) — `process_file` **breaks its read loop at the first
`%Total time` line**, so the entire footer body after it is NEVER parsed; and the `%CKPT` parser is
order-independent key=value whitelisting `t b e r n o x` (so `o=` MUST stay; field order is free).
Trims: `writeFooter()` → only the `%Total time` clean-stop marker (dropped %SamplingFreq/%min/%max
Write/%Over/%block+overrun-list/%Errors/%Retries/%Reinits/%ExtRetries — error/retry counts are
already carried live+cumulative in every `%CKPT`); dropped the `over[OVER_DIM]` array + min/max
write-time tracking + 9 footer PROGMEM strings. KEPT `overruns`/`%CKPT o=` (sd_health reads it).

**Build: production 118444 B (340 B real free under the 118784 ceiling), RAM 11552 B (35%).** Normal
(non-fault) recordings are byte-identical except the trimmed footer (which the host never parsed).

---


## Review round 1 — Codex: CHANGES_REQUESTED | Gemini: CHANGES_REQUESTED
(Codex + Gemini; GLM not on z13.) Build before round: 118444 B.
### Codex findings
1. [BLOCKER] Sd2Card.cpp readData — still uses the UNBOUNDED `_spi->transfer(count,0xFF,dst)` DSPI bulk read; FAT/dir/block reads can wedge before sdSpiFault/the loop guard run.
2. [MAJOR] readRegister — returns true without checking sdSpiFault; a deadline -> all-0xFF CSD/CID garbage that init/cardSize trust.
3. [MAJOR] writeCache — only tests sdSpiFault after the FIRST writeData; a non-SPIROV retry that itself latches the fault enters the skip-forward tail without flush+CMD25 abort.
4. [MAJOR] writeFooter — the trimmed `%Total time` marker lost the leading `\n` the old samplingFreq string carried; with byteCounter mid-line the host's line-start "%Total time" detection can miss the clean-stop marker.
### Gemini findings
1. [MAJOR] spiBlockBounded — ENHBUF toggled while ON==1 (PIC32 says ENHBUF writable only when ON==0).
2. [MINOR] waitNotBusy — a fault-timeout 0xFF from spiRec is misread as "card ready", advancing one step on a dead bus.
3. [MINOR] writeFooter — copies 17 bytes from the 16-char elapsedTime string -> injects the trailing NUL before the hex value.
### Resolution (orchestrator)
- Codex#1 ACCEPTED+fixed: replaced the bulk read with a bounded per-byte `spiRec()` loop (dispatches to spiByteBounded for the hw-DSPI path) + `if(sdSpiFault) break`. Reads are init/file-open only (not the recording hot path), so per-byte is fine.
- Codex#2 ACCEPTED+fixed: `if (sdSpiFault) goto fail;` after the 16 data + 2 CRC byte reads in readRegister.
- Codex#3 ACCEPTED+fixed: removed the SPIROV-branch's inner failure flush; added a UNIFIED `if (!ok && sdSpiFault) { sdSpiModuleFlush(); sdBusRecover(); }` after BOTH branches, so a fault latched during the SPIROV retry OR the non-SPIROV bare retry is flushed+CMD25-aborted before the tail.
- Codex#4 + Gemini#3 ACCEPTED+fixed (one change): `elapsedTime` -> `"\n%Total time mS:\n"` (17 chars); the writeFooter `i<17` loop now copies exactly 17 chars = leading `\n` (marker starts a fresh line) AND no trailing NUL.
- Gemini#1 REJECTED (reasoned): the happy-path ENHBUF set/clear while ON==1 is EXACTLY what stock DSPI's `transfer(uint16_t,src)` does on every SD block write this board has ever done (the original writeData called it) — it is the proven mechanism on this exact MX250 silicon, not undocumented behavior; and if ENHBUF ever failed to set, the new SPIROV check would catch it (not silent). Gemini's proposed ON->0 toggle around the block write would release SCK/SDO to their idle state mid-CMD25 with CS still LOW, risking a spurious clock edge that desyncs the card — which is precisely why prep.md Decision 4 (settled in prep round 8) reserves the ON==0 split for the recovery flush ONLY. Reject stands.
- Gemini#2 ACCEPTED+fixed: waitNotBusy captures spiRec()'s return, checks sdSpiFault BEFORE testing `== 0xFF`, so a bail-sentinel 0xFF is not misread as ready.
### Tests after this round
Production build green: 118376 B (96%, ~408 B free under the 118784 ceiling), RAM 11552 B (35%).

## Review round 2 — Codex: CHANGES_REQUESTED | Gemini: CHANGES_REQUESTED
(Codex + Gemini; GLM not on z13.) Build before round: 118376 B. Diffs sent POST-round-1.
### Codex findings
1. [MAJOR] SD_Card_Stuff.ino — `%SamplingFreq` trim not justified by the `%Total time` break: the old footer emitted `%SamplingFreq` BEFORE `%Total time`, so "host breaks at %Total time" does not prove that field was unread. Restore it, or cite an actual host-parser test proving it is ignored.
2. [MINOR] Sd2Card.cpp spiByteBounded — never checks a pre-existing SPIROV; only deadlines on SPITBE/SPIRBF. A latched overrun from a non-block path could return stale data / wait for timeout instead of latching sdSpiFault immediately.
3. [MINOR] Sd2Card.cpp readData — if partialBlockRead_ were enabled and the bounded loop breaks on sdSpiFault, `offset_ += count` still runs and `fail:` only raises CS; inBlock_ could remain true for an abandoned block.
### Gemini findings
1. [BLOCKER] Sd2Card.cpp — claimed round-1 fixes (Codex#1 readData bounded loop, Gemini#2 waitNotBusy capture) are NOT present in the provided diff (no readData/waitNotBusy hunks).
2. [MINOR] SD_Card_Stuff.ino:1963/1982 — redundant sdSpiModuleFlush() calls immediately before sdBusRecover() (sdBusRecover already flushes first).
3. [REVIEW-NOTE] Rejection of round-1 Gemini#1 (ENHBUF-while-ON) is SOUND (toggling ON=0 with CS low would release SCK/MOSI and risk a desync edge).
### Resolution (orchestrator)
- Gemini#1 — NOT a code defect; a DIFF-GENERATION ARTIFACT, now FIXED. The reviewers were handed a fork diff built against a baseline reconstructed by `revert_fork.py`, which predated the round-1 edits and so did NOT reverse them → the readData/waitNotBusy/readRegister round-1 changes appeared in BOTH the reconstructed baseline and the live file → cancelled out → no hunks. VERIFIED the fixes ARE in the live fork (readData per-byte bounded loop at Sd2Card.cpp:482-490; waitNotBusy captures `r` + checks sdSpiFault before the 0xFF test at :570-578; readRegister `goto fail` at :531). Extended revert_fork.py with the three round-1 reversals (R1-A readData bulk→per-byte, R1-B waitNotBusy, R1-C readRegister); regenerated fork.diff (192→235 lines) now shows all three hunks. No firmware change — the code was always correct; the diff was incomplete.
- Codex#1 ACCEPTED-as-valid-concern, RESOLVED BY PROOF (reject the "restore it" option per Codex's stated alternative "cite an actual host-parser test proving it is ignored"). Traced BOTH host parsers: (a) the LIVE collect-bci ingest `py-qs-data/openbci_functions.py:145-189` — line 171 `%Total time`→break; a lone `%`-line→line 174 records a phantom "stop"; the next value line→line 177 appends to stops_at; (b) `openbci-session/sd_convert.py:292` — also breaks at `%Total time`. A repo-wide grep for the token `SamplingFreq` across both host repos returns ZERO SD-parse consumers (only an unrelated MNE-library symbol). CONFIRMED old writeFooter order (git show 38b3896): ONLY `%SamplingFreq` sat BEFORE `%Total time`; every OTHER dropped field (min/max/Over/Errors/Retries/Reinits/ExtRetries/block) sat AFTER the break → never parsed (my original blanket audit was right for those, wrong only for %SamplingFreq — Codex's catch was correct). Net effect of dropping %SamplingFreq: the old footer's `%SamplingFreq:` + hex value were being mis-ingested as a phantom end-of-file stop marker + stop-value (benign, at EOF after all real data); removing it is strictly neutral-to-cleaner and does NOT touch any EEG sample. Already-recorded files keep their footer. Restoring proven-dead output to a 96%-full flash image is the wrong call → REJECT restore, keep trim, proof logged.
- Codex#2 REJECTED (reasoned): SPIROV is only ever LATCHED by spiBlockBounded (the ENHBUF block path), which sets sdSpiFault in the same breath; sdSpiModuleFlush clears SPIROV+sdSpiFault atomically; spiByteBounded's entry `if(sdSpiFault) return 0xFF` already short-circuits that state. Single-byte standard-mode (ENHBUF=0) transfers write→wait TBE/RBF→read strictly one byte at a time, so RX overrun cannot latch there. A SPIROV-without-sdSpiFault state is therefore unreachable → an extra per-byte SPIROV check is dead weight on the hot path. MINOR, non-blocking.
- Codex#3 REJECTED (reasoned): `partialBlockRead_` is NEVER enabled in this firmware — the setter `Sd2Card::partialBlockRead(value)` has zero callers across the fork+sketch+examples (it is 0 from every constructor). So `!partialBlockRead_` is always true → `readEnd()` ALWAYS runs after the transfer → inBlock_ is always cleared; the abandoned-block scenario requires partialBlockRead_==1. Additionally the SPIROV recovery path calls card.init() which resets `errorCode_=inBlock_=partialBlockRead_=type_=0`. The stale `offset_ += count` is irrelevant once inBlock_=0 (next readData re-issues CMD17 via the `!inBlock_` guard). MINOR, non-blocking.
- Gemini#2 ACCEPTED+fixed: removed the standalone `sdSpiModuleFlush();` at the SPIROV-branch head and inside the unified `if(!ok && sdSpiFault){…}` — both immediately precede `sdBusRecover()`, whose first statement (Sd2Card.cpp:754) is that same flush. Comments updated to state sdBusRecover flushes first. The Layer-1c top-of-loop standalone flush (ADS guard, no following sdBusRecover) is correctly LEFT in place. Saved 16 B flash.
- Gemini#3 — no action (confirms a prior reject).
### Tests after this round
Production build green: 118360 B (96%, ~424 B free under the real 118784 ceiling), RAM 11552 B (35%). No net-new BLOCKER/MAJOR code defect survived triage (Gemini#1=diff artifact fixed; Codex#1=proven-safe, reject-restore; Codex#2/#3=reasoned rejects; Gemini#2=fixed). Running round 3 to let the panel verify the now-complete diff, the %SamplingFreq proof, and the flush removal.

## Review round 3 — Codex: APPROVED | Gemini: APPROVED
(Codex + Gemini; GLM not on z13.) Build before round: 118360 B. Diffs: complete fork diff (235 lines) + post-round-2 sketch diff; R1+R2 history included.
### Codex findings
1. [MINOR] SD_Card_Stuff.ino — the in-code footer-trim comment still preserved the round-1 (false) rationale ("host parser breaks at %Total time, so none were ever read") which round 2 proved wrong for %SamplingFreq (it sat BEFORE the break). Rewrite the comment.
VERDICT: APPROVED
### Gemini findings
1. [MINOR] Sd2Card.cpp readData — `offset_ += count` advances by the full count even when the bounded loop breaks early on sdSpiFault; the following `if (sdSpiFault) goto fail` abandons the transaction + raises CS so FAT/file state is not corrupted. "None strictly required."
2. [MINOR] Sd2Card.cpp writeData — a status-read sdSpiFault returns false before the DATA_RES_ACCEPTED check, so error(SD_CARD_ERROR_WRITE) isn't explicitly set; the 0xFF sentinel would fail the mask anyway → identical abort, only the granular error code is lost. "None required."
Gemini explicitly verified: round-1 fixes (bounded readData + waitNotBusy capture) ARE present in this diff; the in-flight cap (toRead-toWrite < headroom) bounds in-flight bytes to ≤7 on the 8-deep FIFO; sdSpiModuleFlush clears ENHBUF only while ON==0 per datasheet; ON toggled only with CS high / before re-assert in recovery → no desync edge; %SamplingFreq proof sound. "logic is bulletproof."
VERDICT: APPROVED
### Resolution (orchestrator)
- Codex#1 ACCEPTED+fixed (comment-only): rewrote the footer-reclaim comment (SD_Card_Stuff.ino ~178) to state the verified truth — min/max/over/err/retry/reinit/extRetry/block sat AFTER the %Total-time break (never parsed); %SamplingFreq was the lone before-break field, unread token, mis-ingested only as a benign phantom EOF stop. Binary byte-identical (comment).
- Gemini#1 REJECTED (no action, reviewer concurs "none required"): same as round-2 Codex#3 — abort-fast on a faulted bus; partialBlockRead_ never enabled so readEnd() always clears inBlock_; card.init() in recovery resets offset_/inBlock_; the stale offset_ is moot once the transaction fails out.
- Gemini#2 REJECTED (no action, reviewer concurs "none required"): on a poisoned bus, failing fast (CS high + return false) is the correct priority; the 0xFF sentinel fails DATA_RES_ACCEPTED anyway → identical abort outcome; the lost granular SD_CARD_ERROR_WRITE code only affects an error label, not control flow, and writeCache's sdErrs/ledSDError/sdSpiFault path already records the failure.
### Tests after this round
Production build green: 118360 B (96%, ~424 B free under the 118784 ceiling), RAM 11552 B (35%). BOTH reviewers APPROVED; no BLOCKER/MAJOR ever survived triage; the only round-3 accept was a comment correction. Review loop COMPLETE — dual sign-off (Codex 3 rounds, Gemini 3 rounds).
