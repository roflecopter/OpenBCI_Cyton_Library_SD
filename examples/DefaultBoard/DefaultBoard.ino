#include <DSPI.h>
#include <OBCI32_SD.h>
#include <EEPROM.h>
#include <OpenBCI_Wifi_Master_Definitions.h>
#include <OpenBCI_Wifi_Master.h>
#include <OpenBCI_32bit_Library.h>
#include <OpenBCI_32bit_Library_Definitions.h>


// Booleans Required for SD_Card_Stuff.ino
boolean addAccelToSD = false; // On writeDataToSDcard() call adds Accel data to SD card write
boolean addAuxToSD = false; // On writeDataToSDCard() call adds Aux data to SD card write
boolean SDfileOpen = false; // Set true by SD_Card_Stuff.ino on successful file open

// Boot diagnostic state (used by setupSDcard's %BOOT line emit).
// EEPROM layout (post-redesign 2026-05-11, cap raised 2026-05-13):
//   0-1 : fileTens/fileOnes (existing — SD file rotation counter)
//   2-3 : bootSeq (uint16, increments per MCU boot)
//   7   : resumeCount (uint8, 0..MAX_RESUMES — bounds infinite-thrash on a
//         dying cell; reset to 0 on first %CKPT of a successful replay, on
//         host 'j' clean close, and on a fresh P command. MAX_RESUMES is
//         defined in SD_Card_Stuff.ino — currently 25 to cover ~16 GB / 1.2 GB
//         ≈ 13 nightly slots × safety margin.)
//
// Legacy bytes 4 (sessionActive), 5/6 (sessionSeq), 10/11 (savedSlot/savedRate)
// are no longer read by the firmware. They may still be written by setupSDcard
// for migration safety, but the SESSION.TXT file is the source of truth for
// whether the cyton should auto-resume and with what config.
uint32_t bootResetCause = 0;  // RCON snapshot at MCU setup() entry (pre-clear)
uint16_t bootSeq        = 0;  // increments on every MCU boot
uint16_t sessionSeq     = 0;  // legacy field — kept so setupSDcard's %BOOT emit still works
uint8_t  resumeCount    = 0;  // 0..MAX_RESUMES cap, see EEPROM[7] notes above
boolean  autoResume     = false;  // true if replaySessionFile() succeeded
char     prevFileTens   = 'N';    // captured at resume time so %BOOT can emit prev=OBCI_XX
char     prevFileOnes   = 'O';    // sentinel "NO" → renders as prev=NONE when no prev exists

// Soft-WDT (added 2026-05-15) — forward-declared here because loop() references
// these before the Arduino preprocessor reaches SD_Card_Stuff.ino. Threshold
// floor = 120 s (2× the 60 s default CKPT cadence); scales up dynamically as
// max(floor, 2× tuneCkptIntervalMs) so the host can tune CKPT cadence across
// its full 1 s..1 h range without false-firing the WDT.
extern uint32_t sdLastCkptMs;
extern uint32_t tuneCkptIntervalMs;
#define SOFT_WDT_FLOOR_MS  120000UL

// Stray-RX hardening (2026-06-24) — defined in SD_Card_Stuff.ino, referenced by loop().
extern volatile boolean abortRequested; // set by the hardened-escape matcher
void feedEscape(uint8_t c);             // escape detector — fed every raw inbound byte
void dispatchCommandByte(char c);          // recording-state-keyed command policy
void performAbort();                    // safe top-level abort-close (escape stop)

// SPIROV-overrun hang fix (prep.md, 2026-06-29): the sticky SPI-bus fault latch + the
// atomic SPI-module flush, both defined in the OBCI32_SD fork (Sd2Card.cpp).
extern volatile uint8_t sdSpiFault;     // set by a bounded SPI primitive on SPIROV/deadline bail
extern void sdSpiModuleFlush(void);     // atomic SPI1 flush (FIFO + SPIROV); CLEARS sdSpiFault

// Hardware WDT auto-recovery (Stage B, prep.md) — the primitive lives in the OBCI32_SD fork
// (so the SD block-write busy-wait can pet it during a >WDTPS wait). The sketch drives it:
// arms it on stream-start (gated on the runtime FWDTEN==0 check below), pets at top-of-loop,
// and bumps recording-progress on each sample. A WDT reset -> setup() -> the EXISTING
// replaySessionFile() auto-resume salvages the night into the next slot.
extern void petWDT(void);               // pet (top-of-loop + inside waitNotBusy); CP0-progress-gated
extern void wdtProgress(void);          // bump the "last recording progress" CP0 stamp (each sample/block)
extern void wdtArm(void);               // arm the WDT on stream-start (WDTCON.ON=1 + stamp); sketch gates it
extern void wdtDisarm(void);            // disarm on stream-stop (WDTCON.ON=0) -> WDT OFF at idle (no false reset)
boolean wdtEnableGate = false;          // set in setup(): true iff DEVCFG1 FWDTEN==0 (safe to arm)
// NOTE: the persistent freeze-breadcrumb (` hp=<phase>` in %BOOT, to pinpoint the hang phase) was
// designed for Stage B but does NOT fit this 96%-full fragmented flash — deferred to a focused
// follow-up flash. In the meantime the EXISTING resumed-%BOOT `rcv=` field is a free coarse
// discriminator: rcv=1 (sdBusRecover ran + card was wedged) => the hang left the card mid-CMD25 =>
// SD-write path; rcv=0 => the card wasn't wedged => a non-SD hang. Plus `resume=N` counts the hangs.

void setup() {
  // Capture MCU reset cause BEFORE anything that might touch RCON. Then clear
  // the sticky bits so the next reset's cause is unambiguous. NOTE: on the
  // chipKIT bootloader RCON is cleared before user code, so this typically
  // reads 0x00 — captured anyway for %BOOT diagnostics in case a future
  // bootloader fix lands.
  bootResetCause = RCON;
  RCONCLR = 0xFFFF;

  // WDT OFF as the very first act (Stage B, defensive): if a prior WDT-timeout reset somehow left
  // WDTCON.ON set (PIC32MX docs say ON reverts to FWDTEN=0 on any reset, but don't depend on it),
  // clear it NOW so the whole setup()/replaySessionFile() resume path runs with the WDT disabled —
  // it is re-armed only when steady streaming (re)starts. FWDTEN=0 means software owns ON, so this
  // clear is always effective. Removes any infinite-reset risk during the long recovery path.
  WDTCONCLR = _WDTCON_ON_MASK;

  // Boot counter — survives reset via EEPROM. Two consecutive recordings with
  // bootSeq differing by N>1 means the MCU reset N-1 times between sessions.
  uint8_t lo = EEPROM.read(2);
  uint8_t hi = EEPROM.read(3);
  bootSeq = ((uint16_t)hi << 8) | lo;
  if (bootSeq == 0xFFFF) bootSeq = 0;   // virgin/erased EEPROM
  bootSeq++;
  EEPROM.write(2, bootSeq & 0xFF);
  EEPROM.write(3, (bootSeq >> 8) & 0xFF);

  // Capture prevFile{Tens,Ones} from the file rotation counter so that if
  // replaySessionFile() succeeds (it'll bump these via incrementFileCounter),
  // the %BOOT line in the new continuation file can render "prev=OBCI_XX.TXT"
  // pointing at the file we resumed from.
  prevFileTens = (char)EEPROM.read(0);
  prevFileOnes = (char)EEPROM.read(1);

  resumeCount = EEPROM.read(7);
  if (resumeCount == 0xFF) resumeCount = 0;   // virgin EEPROM

  // WDT runtime gate (Stage B): arm the hardware WDT ONLY if BOTH hold on the actual silicon —
  //   (1) FWDTEN==0 (DEVCFG1 bit 23): WDT off at reset, software-controllable -> no infinite-reset brick.
  //   (2) WDTPS >= 0x0A (DEVCFG1 bits 20:16): the HW timeout is >= ~1.02s, the value this design was
  //       VALIDATED against. A board/bootloader-batch with a SMALLER postscale (e.g. 1-32ms) would
  //       false-reset a healthy recording (a 512B block SPI transfer isn't petted mid-burst), so we
  //       FAIL CLOSED (never arm) rather than risk it (Codex+Gemini review #1 BLOCKER). This board read
  //       wd=0xFF6A0D5B -> WDTPS=0x0A -> passes; anything shorter ships as no-WDT (Stage A behaviour).
  // Read-only KSEG1 boot-flash word.
  uint32_t devcfg1 = *(volatile uint32_t *)0xBFC00BF8;
  wdtEnableGate = ((devcfg1 & 0x00800000u) == 0u)             // FWDTEN==0
               && (((devcfg1 >> 16) & 0x1Fu) >= 0x0Au);       // WDTPS >= 0x0A (~1s), the validated range

  // Park the SD chip-select HIGH (deselected) as raw GPIO BEFORE board.begin(). board.begin()
  // brings up the shared DSPI and clocks the ADS1299 over the SAME SCK/MOSI/MISO lines; if a
  // prior external glitch reset left the SD card selected + stuck mid-CMD25, those ADS init
  // clocks would reach the card and corrupt its state further before sdBusRecover() can run.
  // The MCU boots with high-Z GPIOs, so drive SD_SS high now; sdBusRecover() selects it later
  // deliberately. (board.begin() already parks the ADS + accel chip-selects.) Harmless on a
  // cold boot — the card is just deselected, exactly as it should be at rest.
  digitalWrite(SD_SS, HIGH);   // set the latch HIGH first, THEN enable output, so flipping the pin to
  pinMode(SD_SS, OUTPUT);      // OUTPUT can't briefly drive a power-up-low latch (select the card)

  // Bring up the OpenBCI Board (resets ADS chips, enables SPI). MUST run
  // before replaySessionFile() so the xNGSIBPnX commands the replay feeds
  // through processChar can talk to the ADS over SPI.
  board.begin();

  // Bring up wifi (no-op if no wifi shield present, otherwise initialises
  // the shield's serial link).
  wifi.begin(true, true);

  // SESSION.TXT replay. If a valid session-config file is on the SD root
  // AND the resume-cap counter (EEPROM[7]) hasn't been blown by MAX_RESUMES
  // prior failed attempts, this:
  //   - reads SESSION.TXT into RAM (avoids SPI bus contention with the
  //     multi-block writes that follow)
  //   - validates %PBEGIN header + %PEND footer (rejects partial files)
  //   - pre-scans for ~<K<M<b ordering (rejects bad files)
  //   - bumps EEPROM[7]++ as a "this resume in flight" witness
  //   - feeds each byte through sdPersistProcess→sdMetaProcess→sdProcessChar
  //     →board.processChar (same dispatch loop() uses for host serial bytes)
  //   - The file's K command opens a fresh OBCI_<N+1>.TXT and emits %BOOT
  //   - The file's M command writes %META into that file (now self-describing)
  //   - The file's b command starts the ADS streaming firehose
  // EEPROM[7] resets to 0 on first %CKPT of the resumed session (success
  // witness, in writeDataToSDcard) — so dying-cell scenarios with repeated
  // silent halts get a fresh MAX_RESUMES-strike budget per successful chunk
  // rather than exhausting the cap across the night.
  // replaySessionFile sets the global `autoResume = true` internally before
  // it starts feeding bytes (so the K command's setupSDcard sees autoResume
  // and renders %BOOT prev=OBCI_<NN> resume=N correctly). If replay fails
  // somewhere along the way, board.streaming stays false and we'll go idle.
  // We don't reassign the global from the return value — the global already
  // reflects "we attempted resume" which is the useful semantic for %BOOT.
  replaySessionFile();
}

void loop() {
  // Service a hardened-escape abort at a safe top-level point (the matcher only sets the
  // flag; closing here, between SD block writes, avoids any mid-CMD25 re-entrancy).
  if (abortRequested) performAbort();

  // ADS-path guard (prep.md Layer 1c / Decision 10): if a bounded SPI primitive latched a
  // bus fault on a terminal/non-writeCache path (cardCommand, FAT op, footer, close), flush
  // the SPI module here — at a safe block boundary — so the next ADS read never runs on a
  // poisoned bus. Mode-safe (the masked-CON flush preserves the active SPI mode), and a no-op
  // on a clean bus (the common case — writeCache's SPIROV branch already cleared the latch).
  if (sdSpiFault) sdSpiModuleFlush();

  // Stage B WDT: pet every loop iteration (no-op until armed; CP0-recording-progress-gated while
  // streaming, unconditional otherwise). Arm on the transition INTO streaming (gated on the runtime
  // FWDTEN==0 check) and stamp progress there so the ~4s no-progress deadline can't instantly trip
  // on the first sample. A WDT reset -> setup() -> replaySessionFile() auto-resume salvages the night.
  petWDT();
  {
    static boolean wasStreaming = false;
    if (board.streaming != wasStreaming) {
      if (board.streaming) {           // stream just started (fresh 'b' OR a resumed session)
        if (wdtEnableGate) wdtArm();   // arm + stamp progress (gated on FWDTEN==0 && WDTPS>=0x0A)
      } else {
        wdtDisarm();                   // stopped -> WDT OFF (a long idle command can't false-reset)
      }
      wasStreaming = board.streaming;
    }
  }

  if (board.streaming) {
    if (board.channelDataAvailable) {
      // Read from the ADS(s), store data, set channelDataAvailable flag to false
      board.updateChannelData();
      wdtProgress();                   // Stage B: recording progress — a fresh sample was acquired+cached

      // Check to see if accel has new data
      if (board.curAccelMode == board.ACCEL_MODE_ON) {
        if(board.accelHasNewData()) {
          // Get new accel data
          board.accelUpdateAxisData();

          // Tell the SD_Card_Stuff.ino to add accel data in the next write to SD
          addAccelToSD = true; // Set false after writeDataToSDcard()
        }
      } else {
        addAuxToSD = true;
      }

      // Verify the SD file is open
      if(SDfileOpen) {
        // Write to the SD card, writes aux data
        writeDataToSDcard(board.sampleCounter);
      }
      // this is required for the board to be aware of active SD card writes
      board.sdFileOpen = SDfileOpen;

      board.sendChannelData();
    }
  }

  // Check serial 0 for new data
  if (board.hasDataSerial0()) {
    // Read one char from the serial 0 port
    char newChar = board.getCharSerial0();

    // Hardened-escape matcher sees EVERY raw byte first (before any parser consumes it).
    feedEscape((uint8_t)newChar);

    // Dispatch chain for inbound host bytes. Order matters — each handler
    // returns true if it consumed the byte and the chain stops there.
    //   P → tune → M → SD single-char → main command processor
    //
    // 'P' first: SESSION.TXT-write transactions can contain any byte in
    //   their payload (incl. 'T', 'M'), so the P state machine must own
    //   those bytes mid-transaction.
    // 'T' second: tune is rare and self-contained; sdTuneProcess gates on
    //   sessState==0 && sdMetaState==0 && !streaming so it only fires when
    //   no other protocol is active. (Added 2026-05-15.)
    // 'M' third: META payloads can contain 'T' bytes (JSON note strings),
    //   but those land only AFTER sdMetaState != 0 — so T's gate keeps
    //   them safe.
    if (!sdPersistProcess(newChar)) {
      if (!sdTuneProcess(newChar)) {
        if (!sdMetaProcess(newChar)) {
          // Recording-state-keyed command policy (replaces the bare sdProcessChar +
          // board.processChar): a stray byte can no longer mutate an open/active recording.
          dispatchCommandByte(newChar);
        }
      }
    }
  }

  if (board.hasDataSerial1()) {
    // Read one char from the serial 1 port
    char newChar = board.getCharSerial1();
    feedEscape((uint8_t)newChar);
    dispatchCommandByte(newChar);
  }

  // Call the loop function on the board
  board.loop();

  // Soft-WDT: catches loop()-progress hangs the SD-error cascade can't
  // see (DRDY ISR dead, writeCache wedged behind a peripheral hang) by
  // watching %CKPT heartbeat freshness. Gated on streaming AND
  // SDfileOpen — the latter goes false at natural end-of-slot
  // (closeSDfile in writeCache:~1819) while streaming stays true ("do
  // not stop Streaming"); without this gate the WDT would false-fire
  // ~120 s after every clean nightly recording. Threshold is dynamic:
  // max(120 s floor, 2× current tuneCkptIntervalMs) so any host-tuned
  // value in the 1 s..1 h range stays safe. Subtraction is uint32
  // wrap-safe over the 49.7-day millis() rollover. Reset path: same
  // SESSION.TXT auto-resume chain as writeCache's recovery-exhausted
  // reset (~line 1764). No csHigh(SD_SS) before reset — by the time
  // loop() reaches the WDT, CS has been raised on the last successful
  // writeCache:1780 or by closeSDfile:~1353; the writeCache reset site
  // at ~1764 needs the raise because CS is held low across its
  // multi-block context, loop() doesn't.
  // ROLLBACK 2026-06-23: soft-WDT self-reset DISABLED. The mid-night
  // executeSoftReset() here was the regression source — an MCU reset mid-SD-write
  // corrupts the recording's FAT/directory entry, so the whole night reads back
  // all-NUL (lost). A stalled %CKPT now simply continues (the SD-error cascade keeps
  // retrying); a genuinely wedged card falls to sdCardDead in writeCache (clean stop,
  // partial night preserved) rather than a self-reset. Re-enable only with the
  // hardware-WDT + bounded-traversal design in prep.md, never this bare soft reset.
  // if (board.streaming && SDfileOpen) {
  //   uint32_t thresh = (uint32_t)tuneCkptIntervalMs * 2;
  //   if (thresh < SOFT_WDT_FLOOR_MS) thresh = SOFT_WDT_FLOOR_MS;
  //   if ((uint32_t)(millis() - sdLastCkptMs) > thresh) executeSoftReset(0);
  // }
  (void)sdLastCkptMs; (void)tuneCkptIntervalMs;  // still updated elsewhere; silence unused warnings

  // Call to wifi loop
  // wifi.loop() removed (flash reclaim): WiFi shield never released; no wifi RX serviced

  // wifi RX block removed (flash reclaim): shield never present, no wifi bytes to dispatch

  // wifi.sentGains block removed 2026-05-15 to free 8 bytes of flash for
  // the soft-WDT SDfileOpen gate. The OpenBCI WiFi Shield was never
  // released (confirmed with the project owner), so wifi.present is
  // always false on every real Cyton — this block was a no-op anyway.
  // Restore from git history if a WiFi shield ever appears in the wild.
}
