#define BLOCK_5MIN    16890 
#define BLOCK_15MIN  (BLOCK_5MIN*3)    
#define BLOCK_30MIN  (BLOCK_15MIN*2)   
#define BLOCK_1HR    (BLOCK_30MIN*2)  
#define BLOCK_2HR    (BLOCK_1HR*2)    
#define BLOCK_4HR    (BLOCK_1HR*4)    
#define BLOCK_12HR   (BLOCK_1HR*12)  
#define BLOCK_24HR   (BLOCK_1HR*24) 

#define OVER_DIM      20 // make room for up to 20 write-time overruns
#define ERROR_LED     false
#define OK_LED        true

char    fileSize = '0';  // SD file size indicator
int blockCounter =  0 ;


uint32_t BLOCK_COUNT;
SdFile openfile;  // want to put this before setup...
Sd2Card card(&board.spi,SD_SS);// SPI needs to be init'd before here
SdVolume volume;
SdFile root;
uint8_t* pCache;      // array that points to the block buffer on SD card
uint32_t MICROS_PER_BLOCK = 2000; // block write longer than this will get flaged
uint32_t bgnBlock, endBlock; // file extent bookends
int byteCounter = 0;    // used to hold position in cache
//int blockCounter;       // count up to BLOCK_COUNT with this
boolean openvol;
boolean cardInit = false;
boolean fileIsOpen = false;
uint8_t BLOCK_DIV = 1;          // DEFAULT VALUE
boolean sdBlockDivManual = false; // true if host sent explicit 'c'/'C' override

struct {
  uint32_t block;   // holds block number that over-ran
  uint32_t micro;  // holds the length of this of over-run
} over[OVER_DIM];

uint32_t overruns;      // count the number of overruns
uint32_t sdErrs = 0;    // failed card.writeData() calls (SD multi-block hiccups)
uint32_t sdRetries = 0; // writeStop+writeStart recovery attempts issued
uint8_t  sdMetaState = 0;   // 0=idle, 1=need-len-lo, 2=need-len-hi, 3=copying payload, 4=draining bad len
uint16_t sdMetaCount = 0;   // bytes remaining (state 3) or accumulated len (state 2)
uint16_t sdMetaLen   = 0;   // parsed length saved at state 2→3 (echoed back on completion)
uint16_t sdMetaSum   = 0;   // running 16-bit sum of payload bytes (echoed back on completion)
uint32_t sdMetaStart = 0;   // millis() when 'M' arrived (1 s timeout safety)
uint8_t  sdPendingErrs = 0; // %E markers to emit AFTER META write completes (keeps the META line atomic)
boolean  sdMetaCorrupted = false; // set when an UNRECOVERABLE writeCache fails during META state-3 only;
                                  // pad-flush failures are caught separately via sdErrs delta at ACK time
// --- Recovery + checkpoint state (added 2026-05-08) ---
// Closes a recovery hole: the original patch's skip-forward writeStart was fire-and-forget;
// if it failed too, the multi-block context died silently and the firmware kept "writing"
// to nowhere. Now we retry the skip-forward up to 5x; if those all fail, attempt one
// full card.init() + writeStart as a last-resort recovery; if that also fails, declare
// the card dead and tear down the open-file flags so the main loop stops calling
// writeCache (rather than zombie-recording). Checkpoints are periodic markers in the SD
// stream so a recording that ends early still has visible counters + heartbeat timeline.
//
// TRADE-OFF: recovery runs inline with SD_SS held low. The SD library has long internal
// timeouts: writeData up to SD_WRITE_TIMEOUT (~600 ms), writeStop ~2x300 ms, each
// writeStart calls waitNotBusy(300 ms). Realistic worst case per recovery event is on
// the order of 1–2 seconds — during which the ADS1299 has no FIFO so we drop ~500–1000
// samples (~1–2 s gap at 500 Hz). We accept this because the alternative — failing fast
// at the first persistent error — costs the *rest of the night* of recording, which is
// far worse than a visible gap. The post-processor will see the gap via missing
// timestamps between %CKPT lines and treat it as "no data" for that interval.
//
// One card.init() per recovery event (not per session). If the card hiccups multiple
// times across the night, sdReinits accumulates one per event. Each event ends in
// either resumed=true (recording continues) or sdCardDead=true (clean stop, no further
// init attempts because the early-return at the top of writeCache fires).
//
// SPI clock note: Sd2Card::init() ignores its sckRateID parameter when constructed with
// a DSPI pointer (always ends at 20 MHz), and csLow(SD_SS) hard-sets 20 MHz on every
// transaction. We cannot drop to half-speed without library-level changes.
#define CKPT_INTERVAL_MS 60000UL    // emit a %CKPT line about once per minute
boolean  sdCardDead    = false;     // skip-forward + card.init both failed; recording is over
uint32_t sdReinits     = 0;         // card.init() recovery cycles run this session (one per recovery event)
uint32_t sdLastCkptMs  = 0;         // last %CKPT emit time (millis())
uint32_t maxWriteTime;  // keep track of longest write time
uint32_t minWriteTime;  // and shortest write time
uint32_t t;        // used to measure total file write time
uint8_t ERROR_BLINKS = 3;
uint8_t OK_BLINKS    = 3;


byte fileTens, fileOnes;  // enumerate succesive files on card and store number in EEPROM
char currentFileName[] = "OBCI_00.TXT"; // file name will enumerate in hex 00 - FF
prog_char samplingFreq[] PROGMEM = {"\n%SamplingFreq:\n"};  // 16
prog_char elapsedTime[] PROGMEM = {"%Total time mS:\n"};  // 17
prog_char minTime[] PROGMEM = {  "%min Write time uS:\n"};  // 20
prog_char maxTime[] PROGMEM = {  "%max Write time uS:\n"};  // 20
prog_char overNum[] PROGMEM = {  "%Over:\n"};               //  7
prog_char errStamp[] PROGMEM = { "%Errors:\n"};             //  9
prog_char retryStamp[] PROGMEM = { "%Retries:\n"};          // 10
prog_char reinitStamp[] PROGMEM = { "%Reinits:\n"};         // 10
prog_char blockTime[] PROGMEM = {  "%block, uS\n"};         // 11    74 chars + 2 32(16) + 2 16(8) = 98 + (n 32x2) up to 24 overruns...
prog_char stopStamp[] PROGMEM = {  "%STOP AT\n"};      // used to stamp SD record when stopped by PC
prog_char startStamp[] PROGMEM = {  "%START AT\n"};    // used to stamp SD record when started by PC



bool LED_SD_Status_Indication(uint8_t blinks_num, uint8_t blink_period_num, bool ok_indication){
  
  for(uint8_t i=0; i<blinks_num; i++){
     digitalWrite(OPENBCI_PIN_LED, LOW);
     delay(blink_period_num);
     digitalWrite(OPENBCI_PIN_LED, HIGH);
     delay(blink_period_num);
  }
  
  if(ok_indication){
    digitalWrite(OPENBCI_PIN_LED,HIGH);
    return true;
  }else {
    digitalWrite(OPENBCI_PIN_LED,LOW);
    return false;
  }
  
}



char sdProcessChar(char character) {
  
    switch (character) {
        case 'A': // 5min
        case 'S': // 15min
        case 'F': // 30min
        case 'G': // 1hr
        case 'H': // 2hr
        case 'J': // 4hr
        case 'K': // 12hr
        case 'L': // 24hr
        case 'a': // 512 blocks
             
            fileSize = character;
            SDfileOpen = setupSDcard(character);
            break;
            
        case 'j': // close the file, if it's open
            if(SDfileOpen){

                SDfileOpen = closeSDfile();
            }
            if(board.streaming)board.streamStop(); // Stop streamming 
            break;
            
        case 's':
            if(SDfileOpen) {
              
                stampSD(ACTIVATE);
            }
            break;
            
        case 'b':
            if(SDfileOpen) {
                stampSD(DEACTIVATE);
            } 
            break;

        case 'c':
        	   // Consider 8-Channels - Single Cyton Board
            BLOCK_DIV = 2;
            sdBlockDivManual = true;
            break;

         case 'C':
            // Consider 16-Channels - Daisy attached on Cyton Board
            BLOCK_DIV = 1;
            sdBlockDivManual = true;
            break;
            
        default:
            break;
        
    }

    return character;

}


boolean setupSDcard(char limit){
    
  if(!cardInit){
      if(!card.init(SPI_FULL_SPEED, SD_SS)) {
        if(!board.streaming) {
          Serial0.println("initialization failed. Things to check:");
          Serial0.println("* is a card is inserted?");
        }
      //    card.init(SPI_FULL_SPEED, SD_SS);
      } else {
        if(!board.streaming) {
          Serial0.println("Wiring and sdcard is correct.");
        }
        cardInit = true;
      }
      if (!volume.init(card)) { // Now we will try to open the 'volume'/'partition' - it should be FAT16 or FAT32
        if(!board.streaming) {
          Serial0.println("Could not find FAT16/FAT32 partition. Make sure you've formatted the card");
          board.sendEOT();
        }
        return fileIsOpen;
      }
   }


       
  // Auto-pick BLOCK_DIV based on actual daisy presence so the slot is sized
  // for the line length we actually emit (8-ch ≈ 60 B, 16-ch ≈ 115 B).
  // Skipped if the host already sent an explicit 'c' / 'C' override.
  if (!sdBlockDivManual) BLOCK_DIV = board.daisyPresent ? 1 : 2;

  // use limit to determine file size
  switch(limit){
    case 'h':
      BLOCK_COUNT = 50/BLOCK_DIV; break;
    case 'a':
      BLOCK_COUNT = 512/BLOCK_DIV; break;
    case 'A':
      BLOCK_COUNT = BLOCK_5MIN/BLOCK_DIV; break;
    case 'S':
      BLOCK_COUNT = BLOCK_15MIN/BLOCK_DIV; break;
    case 'F':
      BLOCK_COUNT = BLOCK_30MIN/BLOCK_DIV; break;
    case 'G':
      BLOCK_COUNT = BLOCK_1HR/BLOCK_DIV; break;
    case 'H':
      BLOCK_COUNT = BLOCK_2HR/BLOCK_DIV; break;
    case 'J':
      BLOCK_COUNT = BLOCK_4HR/BLOCK_DIV; break;
    case 'K':
      BLOCK_COUNT = BLOCK_12HR/BLOCK_DIV; break;
    case 'L':
      BLOCK_COUNT = BLOCK_24HR/BLOCK_DIV; break;
    default:
      if(!board.streaming) {
        Serial0.println("invalid BLOCK count");
        board.sendEOT(); // Write end of transmission because we exit here
      }
      return fileIsOpen;
  }

  String boardFreq = board.getSampleRate();
  BLOCK_COUNT = ceil(BLOCK_COUNT / 250 * boardFreq.toInt());

  incrementFileCounter();
  openvol = root.openRoot(volume);
  openfile.remove(root, currentFileName); // if the file is over-writing, let it!

  if (!openfile.createContiguous(root, currentFileName, BLOCK_COUNT*512UL)) {
    if(!board.streaming) {
      Serial0.print("createfdContiguous fail");
      LED_SD_Status_Indication(ERROR_BLINKS, 500, ERROR_LED);
      
    }
    cardInit = false;
  }//else{Serial0.print("got contiguous file...");delay(1);}
  // get the location of the file's blocks
  if (!openfile.contiguousRange(&bgnBlock, &endBlock)) {
    if(!board.streaming) {
      Serial0.print("get contiguousRange fail");
      LED_SD_Status_Indication(ERROR_BLINKS, 500, ERROR_LED);
   
    }
    cardInit = false;
  }//else{Serial0.print("got file range...");delay(1);}
  
  // grab the Cache
  pCache = (uint8_t*)volume.cacheClear();
  
  // tell card to setup for multiple block write with pre-erase
  if (!card.erase(bgnBlock, endBlock)){
    if(!board.streaming) {
      Serial0.println("erase block fail");
      LED_SD_Status_Indication(ERROR_BLINKS, 500, ERROR_LED);
    }
    cardInit = false;
  }//else{Serial0.print("erased...");delay(1);}
 
  if (!card.writeStart(bgnBlock, BLOCK_COUNT)){
    if(!board.streaming) {
      Serial0.println("writeStart fail");
      LED_SD_Status_Indication(ERROR_BLINKS, 500, ERROR_LED);
    }
    cardInit = false;
  } else{
    fileIsOpen = true;
    delay(1);
  }
  board.csHigh(SD_SS);  // release the spi
  
  // initialize write-time overrun error counter and min/max wirte time benchmarks
  overruns = 0;
  sdErrs = 0; sdRetries = 0; board.ledSDError = false;
  // Intentional: a host-issued file-size command (h/A/S/.../K/L) starts a fresh
  // recording and thereby acts as the manual recovery for a sdCardDead state from
  // a prior session. The card.init() at the top of this function (line ~155) is
  // the only init the firmware does at session start; in-session inline recovery
  // does its own card.init() if needed (see writeCache).
  sdCardDead = false; sdReinits = 0; sdLastCkptMs = millis();
  sdMetaCorrupted = false; sdPendingErrs = 0;  // clear META flags from any prior dead-card event
  maxWriteTime = 0;
  minWriteTime = 65000;
  byteCounter = 0;  // counter from 0 - 512
  blockCounter = 0; // counter from 0 - BLOCK_COUNT;

  // Persist session config + advance session counters BEFORE %BOOT emit so the
  // line carries the right session= value. Order matters: a true resume keeps
  // the prior sessionSeq (chain shares one id); only a new/clean session bumps
  // it. autoResume is set by setup() in DefaultBoard.ino before this fires.
  if (fileIsOpen) {
    EEPROM.write(10, (uint8_t)limit);                            // slotChar
    EEPROM.write(11, (uint8_t)board.curSampleRate);              // ADS rate enum
    if (!autoResume) {
      sessionSeq++;
      EEPROM.write(5, sessionSeq & 0xFF);
      EEPROM.write(6, (sessionSeq >> 8) & 0xFF);
      EEPROM.write(7, 0);                                        // reset retry cap
      resumeCount = 0;
      prevFileTens = 'N'; prevFileOnes = 'O';                    // → "prev=NONE"
    }
    EEPROM.write(4, 1);                                          // sessionActive — set LAST
  }

  // Boot diagnostic stamp: visible as the very first bytes of every recording.
  // Written before %META / %START AT so it can never be lost to a mid-session
  // failure. Format (fixed field count for parser simplicity):
  //   %BOOT seq=NNNN rcon=0xNN session=NNNN prev=OBCI_XX.TXT resume=N
  // - seq      : MCU boot counter (every reset bumps it)
  // - rcon     : PIC32 RCON low byte from this boot. Bits per Microchip ref:
  //              POR=0x01, BOR=0x02, IDLE=0x04, SLEEP=0x08, WDTO=0x10,
  //              (bit5 unused), SWR=0x40, EXTR=0x80, VREGS=0x100, CMR=0x200.
  // - session  : shared across a resume continuation chain; new session bumps it
  // - prev     : previous file in the chain, or NONE when not a resume
  // - resume   : 0 on the original session file, 1..3 on auto-resumed continuations
  // Hand-rolled hex emit — printf would pull ~20 KB of libc on PIC32.
  if (fileIsOpen) {
    static const char prefixA[] = "%BOOT seq=";       // 10 chars
    static const char prefixB[] = " rcon=0x";         //  8 chars
    static const char prefixC[] = " session=";        //  9 chars
    static const char prefixD[] = " prev=OBCI_";      // 11 chars (rendered always; "NO" sentinel
                                                      //          → "prev=OBCI_NO.TXT" parses as NONE
                                                      //          but we want literal "NONE"; see below)
    static const char prefixE[] = ".TXT resume=";     // 12 chars

    // helper: write 4 nibbles of a u16 in big-endian
    #define EMIT_HEX16(val) do { \
      for (int8_t _n = 3; _n >= 0; _n--) { \
        uint8_t _nib = ((val) >> (_n * 4)) & 0x0F; \
        pCache[byteCounter++] = _nib < 10 ? '0' + _nib : 'A' + (_nib - 10); \
        if (byteCounter == 512) writeCache(); \
      } } while (0)
    #define EMIT_HEX8(val) do { \
      for (int8_t _n = 1; _n >= 0; _n--) { \
        uint8_t _nib = ((uint8_t)(val) >> (_n * 4)) & 0x0F; \
        pCache[byteCounter++] = _nib < 10 ? '0' + _nib : 'A' + (_nib - 10); \
        if (byteCounter == 512) writeCache(); \
      } } while (0)
    #define EMIT_LIT(lit, n) do { \
      for (uint8_t _i = 0; _i < (n); _i++) { \
        pCache[byteCounter++] = (uint8_t)(lit)[_i]; \
        if (byteCounter == 512) writeCache(); \
      } } while (0)
    #define EMIT_BYTE(b) do { \
      pCache[byteCounter++] = (uint8_t)(b); \
      if (byteCounter == 512) writeCache(); \
    } while (0)

    EMIT_LIT(prefixA, 10);
    EMIT_HEX16(bootSeq);
    EMIT_LIT(prefixB, 8);
    EMIT_HEX8(bootResetCause);
    EMIT_LIT(prefixC, 9);
    EMIT_HEX16(sessionSeq);
    // prev= field: real OBCI_XX.TXT for resumes, literal "NONE" for new sessions.
    // Also fall back to NONE if the captured prev bytes are not valid hex
    // digits (corrupt/virgin EEPROM), so the parser never sees a malformed
    // OBCI_<garbage>.TXT filename.
    boolean prevValid = autoResume
        && ( (prevFileTens >= '0' && prevFileTens <= '9') || (prevFileTens >= 'A' && prevFileTens <= 'F') )
        && ( (prevFileOnes >= '0' && prevFileOnes <= '9') || (prevFileOnes >= 'A' && prevFileOnes <= 'F') );
    if (prevValid) {
      EMIT_LIT(prefixD, 11);                    // " prev=OBCI_"
      EMIT_BYTE(prevFileTens);
      EMIT_BYTE(prevFileOnes);
      EMIT_LIT(prefixE, 12);                    // ".TXT resume="
    } else {
      static const char prefixD_none[] = " prev=NONE resume=";   // 18 chars
      EMIT_LIT(prefixD_none, 18);
    }
    // resume count is 0..3 — emit as single ASCII digit
    EMIT_BYTE('0' + (resumeCount > 9 ? 9 : resumeCount));
    EMIT_BYTE('\n');

    #undef EMIT_HEX16
    #undef EMIT_HEX8
    #undef EMIT_LIT
    #undef EMIT_BYTE
  }

  if(fileIsOpen == true){  // send corresponding file name to controlling program
    if(!board.streaming) {
      Serial0.print("Size ");
      Serial0.print(BLOCK_COUNT);
      Serial0.print(" SD file ");
      Serial0.println(currentFileName);
      LED_SD_Status_Indication(OK_BLINKS, 250, OK_LED);
    }
  }
  if(!board.streaming) {
    board.sendEOT();
  }
  return fileIsOpen;
}






boolean closeSDfile(){

  if(fileIsOpen){
    // Clear the auto-resume "session active" flag — a clean close means the
    // next boot should NOT auto-resume into this session. We write this first
    // (BEFORE writeStop / openfile.close) so even a power loss interrupting
    // the close midway leaves the flag cleared. This is deliberate: an MCU
    // reset *during* close is treated as not-resumable, since we may be in
    // an indeterminate state (footer partially written, multi-block partly
    // closed). Better to lose the resume than to chain into a corrupt file.
    EEPROM.write(4, 0);
    board.csLow(SD_SS);  // take spi
    card.writeStop();
    openfile.close();
    board.csHigh(SD_SS);  // release the spi
    fileIsOpen = false;
    if(!board.streaming){ // verbosity. this also gets insterted as footer in openFile
      Serial0.print("SamplingRate: ");Serial0.print(board.getSampleRate());Serial0.println("Hz"); //delay(10);
      Serial0.print("Total Elapsed Time: ");Serial0.print(t);Serial0.println(" mS");              //delay(10);
      Serial0.print("Max write time: "); Serial0.print(maxWriteTime); Serial0.println(" uS");     //delay(10);
      Serial0.print("Min write time: ");Serial0.print(minWriteTime); Serial0.println(" uS");      //delay(10);
      Serial0.print("Overruns: "); Serial0.print(overruns); Serial0.println(); //delay(10);
      if (overruns) {
        uint8_t n = overruns > OVER_DIM ? OVER_DIM : overruns;
        Serial0.println("fileBlock,micros");
        for (uint8_t i = 0; i < n; i++) {
          Serial0.print(over[i].block); Serial0.print(','); Serial0.println(over[i].micro);
        }
        
      }
      board.sendEOT();
    }


  }else{
    if(!board.streaming) {
      Serial0.println("No open file to close");
      board.sendEOT();
    }
    
  }
  
  // delay(100); // cool down
  return fileIsOpen;
}



// Meta-line raw-write protocol:
//   host sends: 'M' <lenLo> <lenHi> <N bytes payload>
// Returns true if the byte was consumed by the protocol (caller should skip
// normal command dispatch). Length-bounded (cap 1024), time-bounded (1 s),
// and rejected while streaming so it can never interleave with sample data.
// On invalid length, transitions to drain state (4) that absorbs up to 1024
// bytes of would-be payload so the host can't accidentally feed those bytes
// into the normal command dispatcher.
boolean sdMetaProcess(char c) {
  if (sdMetaState != 0 && (millis() - sdMetaStart) > 1000) sdMetaState = 0;
  if (sdMetaState == 0) {
    if (c == 'M' && SDfileOpen && !board.streaming) {
      sdMetaState = 1;
      sdMetaCount = 0;
      sdMetaSum   = 0;
      sdMetaLen   = 0;
      sdPendingErrs = 0;        // drop stale markers from a prior aborted META
      sdMetaCorrupted = false;
      sdMetaStart = millis();
      return true;
    }
    return false;
  }
  if (sdMetaState == 1) {
    sdMetaCount = (uint8_t)c;
    sdMetaState = 2;
  } else if (sdMetaState == 2) {
    sdMetaCount |= ((uint16_t)(uint8_t)c) << 8;
    if (sdMetaCount == 0) {
      // empty payload — host explicitly said zero bytes; ACK and idle
      sdMetaState = 0;
      if (!board.streaming) { Serial0.println("META OK 0 0"); board.sendEOT(); }
    } else if (sdMetaCount > 1024) {
      // bad length: drain up to 1024 bytes so a host that already shipped
      // the payload can't have those bytes leak into command dispatch
      sdMetaCount = 1024;
      sdMetaState = 4;
      if (!board.streaming) { Serial0.println("META ERR"); board.sendEOT(); }
    } else { sdMetaLen = sdMetaCount; sdMetaState = 3; }
  } else if (sdMetaState == 3) { // write payload byte directly to SD cache
    pCache[byteCounter++] = c;
    sdMetaSum += (uint8_t)c;
    if (byteCounter == 512) writeCache();
    if (--sdMetaCount == 0) {
      sdMetaState = 0;
      // Flush any %E markers that were deferred while META was being written
      // (writeCache may emit multiple of these if the SD failed across blocks).
      // Pre-flush if fewer than 3 bytes remain in pCache to avoid overrun.
      while (sdPendingErrs > 0) {
        if (byteCounter > 509) writeCache();
        pCache[byteCounter++] = '%';
        pCache[byteCounter++] = 'E';
        pCache[byteCounter++] = '\n';
        sdPendingErrs--;
      }
      // Force-flush pCache to a block boundary so the META line(s) land on
      // their own SD block(s) before we ACK. This makes META durably on-disk
      // and isolates it from any later sample-write retry. Pad with newlines
      // — they're harmless to text parsers (split to len-1 empty strings, no
      // match against ^[0-9A-F]{2},).
      uint32_t sdErrsBefore = sdErrs;
      if (byteCounter > 0) {
        while (byteCounter < 512) pCache[byteCounter++] = '\n';
        writeCache();
      }
      if (!board.streaming) {
        // Two failure modes feed META FAIL:
        //   sdMetaCorrupted: an unrecoverable write happened *during* META payload
        //                    (set in writeCache when state==3 and retry failed)
        //   sdErrs > sdErrsBefore: the final pad-flush itself failed
        // Either way, the host's retry logic re-sends META.
        if (sdMetaCorrupted || sdErrs > sdErrsBefore) {
          Serial0.println("META FAIL");
        } else {
          Serial0.print("META OK ");
          Serial0.print(sdMetaLen);
          Serial0.print(" ");
          Serial0.println(sdMetaSum);
        }
        board.sendEOT();
      }
    }
  } else { // sdMetaState == 4 — drain abandoned payload bytes
    if (--sdMetaCount == 0) sdMetaState = 0;
  }
  return true;
}



void writeDataToSDcard(byte sampleNumber){
  // Emit deferred markers at sample boundary BEFORE we start writing the new
  // sample's CSV. byteCounter at function entry is positioned just after the
  // previous sample's '\n' (or at 0 after a writeCache flush), so writes
  // here cannot split a sample line. We only emit if the marker fits within
  // the current 512-byte block — if it doesn't fit, defer to the next sample
  // boundary (next block) since all markers are size-bounded.

  // Flush any deferred %E markers (one per failed-write event the previous
  // writeCache() recorded). Three bytes each; cheap to fit.
  while (sdPendingErrs > 0 && byteCounter + 3 <= 512) {
    pCache[byteCounter++] = '%';
    pCache[byteCounter++] = 'E';
    pCache[byteCounter++] = '\n';
    sdPendingErrs--;
  }

  // Emit %CKPT heartbeat if the interval has elapsed and the line fits.
  if (sdMetaState == 0 &&
      ((uint32_t)(millis() - sdLastCkptMs) >= CKPT_INTERVAL_MS)) {
    char tmp[80];
    int n = snprintf(tmp, sizeof(tmp),
                     "%%CKPT t=%lu b=%d e=%lu r=%lu n=%lu o=%lu\n",
                     (unsigned long)millis(),
                     blockCounter,
                     (unsigned long)sdErrs,
                     (unsigned long)sdRetries,
                     (unsigned long)sdReinits,
                     (unsigned long)overruns);
    if (n > 0 && byteCounter + n <= 512) {
      memcpy(pCache + byteCounter, tmp, n);
      byteCounter += n;
      sdLastCkptMs = millis();
    }
    // If it didn't fit in this block, retry on the next sample boundary.
  }

  boolean addComma = true;
  // convert 8 bit sampleCounter into HEX
  convertToHex(sampleNumber, 1, addComma);
  // convert 24 bit channelData into HEX
  for (int currentChannel = 0; currentChannel < 8; currentChannel++){
    convertToHex(board.boardChannelDataInt[currentChannel], 5, addComma);
    
    // If Daisy Is NOT Attached -> stop putting comma delimiter at 7th sample 
    if(board.daisyPresent == false){
      if(currentChannel == 6){
        addComma = false;
        if(addAuxToSD || addAccelToSD) { addComma = true; }  // format CSV
      }
    }
    
   } 

   // If Daisy Is Attached -> stop putting comma delimiter at 7th sample
  if(board.daisyPresent){
    for (int currentChannel = 0; currentChannel < 8; currentChannel++){
      convertToHex(board.daisyChannelDataInt[currentChannel], 5, addComma);
      if(currentChannel == 6){
        addComma = false;
        if(addAuxToSD || addAccelToSD) {addComma = true;}  // format CSV
      }
    }
    
  }

  

  if(addAuxToSD == true){
    // convert auxData into HEX
    for(int currentChannel = 0; currentChannel <  3; currentChannel++){
      convertToHex(board.auxData[currentChannel], 3, addComma);
      if(currentChannel == 1) addComma = false;
    }
    addAuxToSD = false;
  }// end of aux data log

  
  else if(addAccelToSD == true){  // if we have accelerometer data to log
    // convert 16 bit accelerometer data into HEX
    for (int currentChannel = 0; currentChannel < 3; currentChannel++){
      convertToHex(board.axisData[currentChannel], 3, addComma);
      if(currentChannel == 1) addComma = false;
    }
    addAccelToSD = false;  // reset addAccel
  }// end of accelerometer data log

   // add aux data logging...
}



void writeCache(){

    // sdCardDead: skip-forward retry + half-speed re-init both failed earlier.
    // The card is hard-stuck. Drop further sample data on the floor and tell
    // the rest of the firmware the file is no longer open so callers stop
    // pumping bytes through writeCache (which would otherwise hang the SPI bus).
    if (sdCardDead) {
      byteCounter = 0;
      fileIsOpen = false;
      SDfileOpen = false;
      board.sdFileOpen = false;
      return;
    }

    if(blockCounter > BLOCK_COUNT) {
      blockCounter=0;
      return;
    }

    uint32_t tw = micros();  // start block write timer
    boolean errOccurred = false;
    board.csLow(SD_SS);  // take spi
    boolean ok = card.writeData(pCache);
    if (!ok) {
      errOccurred = true;
      sdErrs++;
      board.ledSDError = true;
      // single-shot recovery: stop & restart multi-block from the current block.
      // sdRetries counts attempts issued (successful or not) so the footer
      // counter matches the number of recovery cycles the firmware ran.
      sdRetries++;
      card.writeStop();
      if (card.writeStart(bgnBlock + blockCounter, BLOCK_COUNT - blockCounter)) {
        ok = card.writeData(pCache);  // capture retry result
      }
      if (!ok) {
        // Persistent failure on the original block. Skip forward to the next
        // block so subsequent writes target the right position. Retry the
        // skip-forward writeStart up to 5x — the original code did it once
        // fire-and-forget, which left the multi-block context dead silently
        // when even the skip failed (root cause of the late-night silent halt).
        //
        // If 5x writeStart still fails, attempt one full card.init() to reset
        // card-internal state and resume from the next block. This is the slow
        // path (potentially ~1–2 seconds in the absolute worst case due to the
        // SD library's internal timeouts on writeStop/writeStart/writeData) and
        // will drop a corresponding burst of ADC samples — but the alternative
        // is losing the rest of the recording, which is far worse. A visible
        // gap in sample timestamps is preferable and detectable.
        //
        // Per-event cap: at most one card.init() per failure event in this
        // function. If a second event happens later in the session sdReinits
        // increments again. Real-card failures that won't recover after one
        // init don't recover after ten either, so further inline retries
        // would only worsen the gap.
        card.writeStop();
        boolean resumed = false;
        if (blockCounter + 1 < BLOCK_COUNT) {
          for (uint8_t a = 0; a < 5 && !resumed; a++) {
            sdRetries++;  // counts attempts issued (success or fail) for footer accounting
            if (card.writeStart(bgnBlock + blockCounter + 1, BLOCK_COUNT - blockCounter - 1)) {
              resumed = true;
            } else {
              delay(1);
            }
          }
          // Last-resort recovery: full card re-init. One attempt per failure event.
          if (!resumed) {
            sdReinits++;
            if (card.init(SPI_FULL_SPEED, SD_SS) &&
                card.writeStart(bgnBlock + blockCounter + 1, BLOCK_COUNT - blockCounter - 1)) {
              resumed = true;
            }
          }
        }
        if (!resumed) {
          // No path forward — tear down all open flags AND return immediately,
          // so the rest of writeCache (checkpoint logic, footer-trigger check,
          // close-trigger check) doesn't run on a dead card and the main loop
          // in DefaultBoard.ino:45-47 stops calling writeDataToSDcard on the
          // very next iteration. Recording ends here with whatever %CKPT
          // markers landed on disk before this moment.
          sdCardDead = true;
          fileIsOpen = false;
          SDfileOpen = false;
          board.sdFileOpen = false;
          // Clear sessionActive so the next MCU reset won't auto-resume into a
          // card we already declared dead — a fresh session start (host-driven
          // recovery) is required to retry the card.
          EEPROM.write(4, 0);
          board.csHigh(SD_SS);
          // If this happened mid-META payload, flag corruption so the host
          // gets META FAIL and can retry on the next session.
          if (sdMetaState == 3) sdMetaCorrupted = true;
          return;
        }
        // If this happened mid-META payload, flag corruption so the host gets
        // META FAIL and can retry.
        if (sdMetaState == 3) sdMetaCorrupted = true;
      }
    }
    board.csHigh(SD_SS);  // release spi
    tw = micros() - tw;      // stop block write timer
    if (tw > maxWriteTime) maxWriteTime = tw;  // check for max write time
    if (tw < minWriteTime) minWriteTime = tw;  // check for min write time
    if (tw > MICROS_PER_BLOCK) {      // check for overrun
    if (overruns < OVER_DIM) {
        over[overruns].block = blockCounter;
        over[overruns].micro = tw;
      }
      overruns++;
    }

    byteCounter = 0; // reset 512 byte counter for next block
    blockCounter++;    // increment BLOCK counter

    if (errOccurred) {
      // Defer the %E marker — emitting it here can split a sample line
      // because writeCache() can be invoked mid-byte from convertToHex().
      // sdPendingErrs is flushed at the next sample boundary by
      // writeDataToSDcard(). Keep the same counter both for normal
      // operation and the META-payload edge case (the META post-processing
      // block also flushes sdPendingErrs).
      if (sdPendingErrs < 255) sdPendingErrs++;
    }
    // Note: %CKPT emission also moved out of writeCache() to writeDataToSDcard()
    // for the same reason — heartbeat insertion mid-sample-line was breaking
    // the CSV parser. See writeDataToSDcard() for the new placement.

    if(blockCounter == BLOCK_COUNT-1){
      t = millis() - t;

      // Time to Close the file but do not stop Streaming 
      writeFooter();
    }
    
    if(blockCounter == BLOCK_COUNT){
       SDfileOpen  = closeSDfile(); // Update open-file flag     
    }  // we did it!
    
}


void incrementFileCounter(){
  
  fileTens = EEPROM.read(0);
  fileOnes = EEPROM.read(1);
 
  // if it's the first time writing to EEPROM, seed the file number to '00'
  if(fileTens == 0xFF | fileOnes == 0xFF){
    fileTens = fileOnes = '0';
  }
  fileOnes++;   // increment the file name
  if (fileOnes == ':'){fileOnes = 'A';}
  if (fileOnes > 'F'){
    fileOnes = '0';         // hexify
    fileTens++;
    if(fileTens == ':'){fileTens = 'A';}
    if(fileTens > 'F'){fileTens = '0';fileOnes = '1';}
  }
  EEPROM.write(0,fileTens);     // store current file number in eeprom
  EEPROM.write(1,fileOnes);
  currentFileName[5] = fileTens;
  currentFileName[6] = fileOnes;
   //  // send corresponding file name to controlling program
   //  Serial0.print("Corresponding SD file ");Serial0.println(currentFileName);
}






void stampSD(boolean state){

  unsigned long time = millis();
  if(state){
    for(int i=0; i<10; i++){
      pCache[byteCounter] = pgm_read_byte_near(startStamp+i);
      byteCounter++;
      if(byteCounter == 512){
        writeCache();
      }
    }
  }
  else{
    for(int i=0; i<9; i++){
      pCache[byteCounter] = pgm_read_byte_near(stopStamp+i);
      byteCounter++;
      if(byteCounter == 512){
        writeCache();
      }
    }
  }
  convertToHex(time, 7, false);
}




void writeFooter(){
 
  for(int i=0; i<16; i++){
    pCache[byteCounter] = pgm_read_byte_near(samplingFreq+i);
    byteCounter++;
  }
  String daqFreq = board.getSampleRate();
  convertToHex(daqFreq.toInt(), 4, false);
  
  for(int i=0; i<17; i++){
    pCache[byteCounter] = pgm_read_byte_near(elapsedTime+i);
    byteCounter++;
  }
  convertToHex(t, 7, false);

  for(int i=0; i<20; i++){
    pCache[byteCounter] = pgm_read_byte_near(minTime+i);
    byteCounter++;
  }
  convertToHex(minWriteTime, 7, false);

  for(int i=0; i<20; i++){
    pCache[byteCounter] = pgm_read_byte_near(maxTime+i);
    byteCounter++;
  }
  convertToHex(maxWriteTime, 7, false);

  for(int i=0; i<7; i++){
    pCache[byteCounter] = pgm_read_byte_near(overNum+i);
    byteCounter++;
  }
  convertToHex(overruns, 7, false);

  for(int i=0; i<9; i++){
    pCache[byteCounter] = pgm_read_byte_near(errStamp+i);
    byteCounter++;
  }
  convertToHex(sdErrs, 7, false);

  for(int i=0; i<10; i++){
    pCache[byteCounter] = pgm_read_byte_near(retryStamp+i);
    byteCounter++;
  }
  convertToHex(sdRetries, 7, false);

  for(int i=0; i<10; i++){
    pCache[byteCounter] = pgm_read_byte_near(reinitStamp+i);
    byteCounter++;
  }
  convertToHex(sdReinits, 7, false);

  for(int i=0; i<11; i++){
    pCache[byteCounter] = pgm_read_byte_near(blockTime+i);
    byteCounter++;
  }

  if (overruns) {
    uint8_t n = overruns > OVER_DIM ? OVER_DIM : overruns;
    for (uint8_t i = 0; i < n; i++) {
      convertToHex(over[i].block, 7, true);
      convertToHex(over[i].micro, 7, false);
    }
  }

  for(int i=byteCounter; i<512; i++){
    pCache[i] = NULL;
  }

  writeCache();
}




//    CONVERT RAW BYTE DATA TO HEX FOR SD STORAGE
void convertToHex(long rawData, int numNibbles, boolean useComma){

  for (int currentNibble = numNibbles; currentNibble >= 0; currentNibble--){
    byte nibble = (rawData >> currentNibble*4) & 0x0F;
    if (nibble > 9){
      nibble += 55;  // convert to ASCII A-F
    }
    else{
      nibble += 48;  // convert to ASCII 0-9
    }
    pCache[byteCounter] = nibble;
    byteCounter++;
    if(byteCounter == 512){
      writeCache();
    }
  }
  if(useComma == true){
    pCache[byteCounter] = ',';
  }else{
    pCache[byteCounter] = '\n';
  }
  byteCounter++;
  if(byteCounter == 512){
    writeCache();
  }
}// end of byteToHex converter
