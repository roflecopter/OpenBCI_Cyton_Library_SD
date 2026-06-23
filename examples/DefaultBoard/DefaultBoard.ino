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

void setup() {
  // Capture MCU reset cause BEFORE anything that might touch RCON. Then clear
  // the sticky bits so the next reset's cause is unambiguous. NOTE: on the
  // chipKIT bootloader RCON is cleared before user code, so this typically
  // reads 0x00 — captured anyway for %BOOT diagnostics in case a future
  // bootloader fix lands.
  bootResetCause = RCON;
  RCONCLR = 0xFFFF;

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
  if (board.streaming) {
    if (board.channelDataAvailable) {
      // Read from the ADS(s), store data, set channelDataAvailable flag to false
      board.updateChannelData();

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
          // Send to the sd library for processing
          sdProcessChar(newChar);

          // Send to the board library
          board.processChar(newChar);
        }
      }
    }
  }

  if (board.hasDataSerial1()) {
    // Read one char from the serial 1 port
    char newChar = board.getCharSerial1();

    // Send to the sd library for processing
    sdProcessChar(newChar);

    // Read one char and process it
    board.processChar(newChar);
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
  wifi.loop();

  if (wifi.hasData()) {
    // Read one char from the wifi shield
    char newChar = wifi.getChar();

    // Send to the sd library for processing
    sdProcessChar(newChar);

    // Send to the board library
    board.processCharWifi(newChar);
  }

  // wifi.sentGains block removed 2026-05-15 to free 8 bytes of flash for
  // the soft-WDT SDfileOpen gate. The OpenBCI WiFi Shield was never
  // released (confirmed with the project owner), so wifi.present is
  // always false on every real Cyton — this block was a no-op anyway.
  // Restore from git history if a WiFi shield ever appears in the wild.
}
