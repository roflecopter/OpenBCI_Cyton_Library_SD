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
