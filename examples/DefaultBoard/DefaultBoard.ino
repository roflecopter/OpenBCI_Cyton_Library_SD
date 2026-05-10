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

// Boot + auto-resume diagnostic state.
// EEPROM layout:
//   0-1 : fileTens/fileOnes (existing — SD file rotation)
//   2-3 : bootSeq (uint16, increments per MCU boot)
//   4   : sessionActive flag (1 while recording in progress; cleared on clean close,
//         on sdCardDead, or on a refused resume — anything that drops to idle)
//   5-6 : sessionSeq (uint16, increments only on NEW sessions — shared across resume chain)
//   7   : resumeCount (uint8, 0..3, capped — incremented on every auto-resume in same session)
//   10  : slotChar saved for resume ('h','A','S','F','G','H','J','K','L','a')
//   11  : saved sample-rate enum (SAMPLE_RATE_*: 0=16000…6=250)
uint32_t bootResetCause = 0;  // RCON snapshot at MCU setup() entry (pre-clear)
uint16_t bootSeq        = 0;  // increments on every MCU boot
uint16_t sessionSeq     = 0;  // shared across resume chain, only incremented on new session
uint8_t  resumeCount    = 0;  // 0 on a new/clean session; 1..3 across auto-resume continuations
boolean  autoResume     = false;  // true when this boot is auto-resuming a prior session
char     prevFileTens   = 'N';    // captured at resume time so %BOOT can emit prev=OBCI_XX
char     prevFileOnes   = 'O';    // sentinel "NO" → renders as prev=NONE when no prev exists

void setup() {
  // Capture MCU reset cause BEFORE anything that might touch RCON. Then clear
  // the sticky bits so the next reset's cause is unambiguous (RCON accumulates
  // bits across resets if not cleared).
  bootResetCause = RCON;
  RCONCLR = 0xFFFF;

  // Boot counter — survives reset via EEPROM. Two consecutive recordings with
  // bootSeq differing by N>1 means the MCU reset N-1 times between sessions
  // (i.e. silent reboot mid-night, exactly the failure mode we're hunting).
  uint8_t lo = EEPROM.read(2);
  uint8_t hi = EEPROM.read(3);
  bootSeq = ((uint16_t)hi << 8) | lo;
  if (bootSeq == 0xFFFF) bootSeq = 0;   // virgin/erased EEPROM
  bootSeq++;
  EEPROM.write(2, bootSeq & 0xFF);
  EEPROM.write(3, (bootSeq >> 8) & 0xFF);

  // Read sessionSeq always (used by setupSDcard's %BOOT emit even on clean boot).
  uint8_t slo = EEPROM.read(5);
  uint8_t shi = EEPROM.read(6);
  sessionSeq = ((uint16_t)shi << 8) | slo;
  if (sessionSeq == 0xFFFF) sessionSeq = 0;

  resumeCount = EEPROM.read(7);
  if (resumeCount == 0xFF) resumeCount = 0;   // virgin EEPROM

  // Auto-resume gate. POR vetoes everything (full power loss = user intent or
  // cell death — retrying is unsafe). Resume only on the silent-fault causes:
  //   BOR  = 0x02 (brownout that didn't cross VPOR)
  //   WDTO = 0x10 (watchdog timeout — firmware hang)
  //   SWR  = 0x40 (software-triggered reset)
  //   EXTR = 0x80 (MCLR pin glitch)
  // PIC32MX RCON bit positions per Microchip ref manual section 7 — note SWR
  // is bit6 (0x40) and EXTR is bit7 (0x80); bit5 is unimplemented.
  uint8_t cause = (uint8_t)(bootResetCause & 0xFFu);
  boolean prevSessionActive = (EEPROM.read(4) == 1);
  uint8_t savedSlot         = EEPROM.read(10);
  uint8_t savedRate         = EEPROM.read(11);
  // Sanity-check saved config — virgin EEPROM (0xFF) or out-of-range values
  // disable resume (the new file would have a wrong BLOCK_COUNT or ADS rate).
  static const char validSlots[] = "hASFGHJKLa";
  boolean validSlot = false;
  for (uint8_t i = 0; i < sizeof(validSlots) - 1; i++) {
    if ((char)savedSlot == validSlots[i]) { validSlot = true; break; }
  }
  boolean validRate = (savedRate <= 6);   // SAMPLE_RATE_250 enum is 6

  autoResume = prevSessionActive
            && !(cause & 0x01)                            // POR vetoes
            && (cause & (0x02 | 0x10 | 0x40 | 0x80))      // BOR/WDTO/SWR/EXTR
            && (resumeCount < 3)                          // retry cap
            && validSlot && validRate;

  if (autoResume) {
    // Capture previous filename BEFORE setupSDcard's incrementFileCounter
    // bumps fileTens/fileOnes. Used by %BOOT emit as prev=OBCI_XX.TXT.
    prevFileTens = (char)EEPROM.read(0);
    prevFileOnes = (char)EEPROM.read(1);
    // Increment resume count immediately so a crash during the resume itself
    // still costs a retry (prevents infinite-thrash loops).
    resumeCount++;
    EEPROM.write(7, resumeCount);
  } else if (prevSessionActive) {
    // Resume was refused but the prior session's flag is still set. Clear it
    // now so a partial EEPROM burst during the next setupSDcard (slot/rate/
    // sessionSeq writes happen before the final byte-4=1) can't leave us in
    // a stale half-configured "active" state. Causes a refusal to land here:
    // POR (user power-cycle / cell death), retry-cap exhausted, or saved
    // slot/rate corrupted/virgin. In every one of those, the right behavior
    // for the next boot is "no resume" — so committing the clear is safe.
    EEPROM.write(4, 0);
  }

  // Bring up the OpenBCI Board
  board.begin();

  // Bring up wifi
  wifi.begin(true, true);

  if (autoResume) {
    // Restore the previous session's sample rate BEFORE setupSDcard — that
    // function reads getSampleRate() to compute BLOCK_COUNT, so the saved
    // rate must be in effect by the time the file is sized.
    board.setSampleRate(savedRate);

    // Reopen recording. setupSDcard increments fileTens/fileOnes (so we land
    // on OBCI_<prev+1>.TXT), opens contiguous file, marks sessionActive=1,
    // and emits the %BOOT header line carrying session+prev+resume linkage.
    SDfileOpen = setupSDcard((char)savedSlot);

    if (SDfileOpen) {
      // No %META — host metadata was lost when RAM cleared. Post-processor
      // links via %BOOT prev= and inherits metadata from the prev file.
      // stampSD(DEACTIVATE) writes the %STOP AT marker that the host's 'b'
      // command would normally emit; streamStart() turns on the ADS firehose
      // (configures channels, timestamps, SD write pointer — must use this
      // not just streaming=true, per ADS init contract).
      stampSD(DEACTIVATE);
      board.streamStart();
    }
    // If setupSDcard failed, fall through to idle — same as a normal failed
    // start. resumeCount is already incremented so we won't loop forever.

    // Consume the flag — only this first setupSDcard call after a resume boot
    // is the resume itself. Any subsequent setupSDcard (e.g. host 'j' close
    // followed by manual 'K' new session) must be treated as a fresh session
    // and bump sessionSeq, otherwise the chain id leaks into the next session.
    autoResume = false;
  }
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

    // 'M' meta-line protocol intercepts the byte stream; skip normal dispatch when consumed
    if (!sdMetaProcess(newChar)) {
      // Send to the sd library for processing
      sdProcessChar(newChar);

      // Send to the board library
      board.processChar(newChar);
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

  if (!wifi.sentGains) {
    if(wifi.present && wifi.tx) {
      wifi.sendGains(board.numChannels, board.getGains());
    }
  }
}
