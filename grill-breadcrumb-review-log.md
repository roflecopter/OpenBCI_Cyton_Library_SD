# /grill — forensic exception breadcrumb — review log (2026-07-06)

Plan: `prep-breadcrumb.md` (approved 3 rounds, Codex+Gemini). Branch: `work/hang-prevent-and-diagnose`.

## Outcome: breadcrumb INFEASIBLE (hard, measured flash blocker); shipped the flashable RECOVER-fix baseline instead.

### The wall (all numbers MEASURED with `arduino-cli compile --fqbn chipKIT:pic32:openbci`, real ceiling = 118784 B)
The /prep plan assumed the breadcrumb would fit after "trimming one non-contract diagnostic string"
(it estimated the baseline at 118780 B → 4 B free). **No reviewer had a real build** — that premise was
false. Building it:

| Tree state | Size | vs 118784 ceiling |
|---|---|---|
| Committed HEAD `e3e46c9` (MAX_RESUMES cap + SPI settle) | ~118792 B | **8 B OVER — does not link** |
| Clean baseline after string trims + g= drop (no breadcrumb) | 118292 B | 492 B free |
| + full exception breadcrumb (crash_handler.c + .noinit + exc=/epc= emit + setup read) | ~119052 B | **268 B OVER** |

- **The breadcrumb costs ~760 B**, not the ~small the plan assumed. Root cause: installing a **strong
  `_general_exception_handler`** override defeats `gc-sections`' stripping of the chipKIT core's
  **exception-vector machinery** (`cores/pic32/exceptions.c` entry stub + vector path), pulling in
  hundreds of bytes that **no string-trimming can touch**. 492 B free − 760 B needed = the −268 B measured.
- **Zero non-contract strings remain** to reclaim — every remaining `Serial0.print` in SD_Card_Stuff.ino
  is a host-contract token (PERSIST OK / TUNE OK / Size / META). The staged fallback ladder from the plan
  (drop EEPROM copy → drop `bv=`) was fully exhausted and still 268 B short.
- The only ways to fit it require a decision the firmware author should NOT make unilaterally on a
  **no-ICSP (brick-if-bad-flash)** board: (a) drop the accelerometer (user said keep it), or (b)
  reclaim the reserved DEE/splitflash pages via linker-script surgery (brick risk). → escalated to user.

### What shipped instead (commit `fb8b9cf`)
The committed HEAD `e3e46c9` was itself **8 B over the ceiling → unflashable** (the `delayMicroseconds(5)`
SPI-settle add tipped it past; that commit was never link-verified). Fixed by trimming **5 host-UNPARSED
diagnostic strings** → **118520 B, 264 B free, links cleanly**. Keeps every functional behaviour, the
`g=0x` resume diagnostic, all `board.sendEOT()`/control flow, and the whole Stage-B WDT + MAX_RESUMES=3 +
SPI-settle RECOVER payload. So the user's real goal — the freeze RECOVERS (WDT → auto-resume salvage) —
is delivered and flashable; only the *breadcrumb* (LOCATE) is deferred.

Host-contract audit (the 2026-06-28 `Size`-regression check): the trimmed `PERSIST ERR EMPTY/TOOBIG`
and `PERSIST FAIL` step-number appear only in `p_cmd_helper.py`'s **docstring** (never parsed);
`session_start.py` regexes only `PERSIST OK (\d+) (\d+)`; `"invalid BLOCK count"` and
`"No open file to close"` have **zero** host matches. All 5 still emit `$$$` (EOT).

## Review — flashable fix (diff-only, host-contract focus)

### Round 1 — Codex: CHANGES_REQUESTED | Gemini: (timeout)
Codex ignored the inline diff and reviewed the surrounding **pre-existing committed** code (xhigh-wander):
all 4 findings (Sd2Card.cpp:484 readData, :527 readRegister, SD_Card_Stuff.ino:1972 SPIROV branch, :2245
%Total time footer) are OUT OF SCOPE — none on a changed line; all from prior-reviewed commits (Stage A /
3e93cde). **Resolution: rejected as out-of-scope.** Gemini timed out (Bash 120 s cap < wrapper time).
> The 4 out-of-scope items are logged as *observations on prior-committed code* for the user, not blockers
> here (esp. Codex #4 — worth a look during the next real grill on the footer, though CLAUDE.md records the
> host ingest breaks AT `%Total time` and the footer emits `\n%Total time mS:\n`).

### Round 2 — Codex: APPROVED | Gemini: APPROVED
Re-run with a hard scope constraint ("review ONLY changed lines; all other code is out of scope") + a
540 s timeout. Codex: *"No findings in the changed lines. VERDICT: APPROVED."* Gemini: *"All 5 trimmed
strings verified against the host driver's parsed token list and none are required; the board.sendEOT()
calls and early-return control flows strictly preserved. VERDICT: APPROVED."* → **both sign off.**

### Tests
`arduino-cli compile --fqbn chipKIT:pic32:openbci examples/DefaultBoard` → **118520 B (96%), 264 B free,
RAM 11572 B (35%)**, hex artifact produced. NOT flashed (user-gated — the multi-hour freeze only
reproduces on a real overnight soak).

## Deferred to the user (breadcrumb — needs a tradeoff decision)
1. **Accept RECOVER-only** (ship fb8b9cf; WDT salvages the night; no in-firmware LOCATE). Default.
2. **Drop the accelerometer** (~frees enough for the breadcrumb) — but user asked to keep accel.
3. **Reclaim the reserved DEE/splitflash flash pages** via linker-script surgery — brick-risk on a
   no-ICSP board; needs its own careful investigation.
Breadcrumb work preserved: `crash_handler.c.breadcrumb-wip` + `breadcrumb-defaultboard.diff`.
