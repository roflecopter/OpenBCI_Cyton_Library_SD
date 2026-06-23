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
// timeouts: writeData up to SD_WRITE_TIMEOUT (raised to 1500 ms on 2026-05-13;
// see Sd2Card.h for rationale), writeStop ~2x300 ms, each
// writeStart calls waitNotBusy(300 ms). Realistic worst case per recovery event is on
// the order of 1–2 seconds — during which the ADS1299 has no FIFO so we drop ~500–1000
// samples (~1–2 s gap at 500 Hz). We accept this because the alternative — failing fast
// at the first persistent error — costs the *rest of the night* of recording, which is
// far worse than a visible gap. The post-processor will see the gap via missing
// timestamps between %CKPT lines and treat it as "no data" for that interval.
//
// One card.init() per recovery event (not per session). If the card hiccups multiple
// times across the night, sdReinits accumulates one per event. Each event ends in
// either resumed=true (recording continues in same file) or — when ALL in-place
// recovery is exhausted — executeSoftReset(0): hands control to the auto-resume
// chain in setup(). RCON.SWR (0x40) is just the reset cause that ends up in
// bootResetCause — replaySessionFile() itself gates only on SESSION.TXT presence
// and resumeCount<MAX_RESUMES (EEPROM[7]) (cause-based POR veto is a separate planned change
// not yet wired in). On the SWR boot, replaySessionFile() re-runs the K/L/etc
// command from SESSION.TXT; setupSDcard() allocates the NEXT OBCI_<N+1>.TXT slot
// via incrementFileCounter; recording resumes in that fresh slot. resumeCount cap
// (EEPROM[7]) bounds the chain at MAX_RESUMES attempts so a fully-dying card
// eventually idles solid (ledReplayFail double-flash). This is "skip the bad block range, try the
// next slot" — far better than declaring the whole card dead on a localized flash
// failure (2026-05-12 design fix; before this, sdCardDead actively poisoned the
// resume gates by deleting SESSION.TXT and clearing EEPROM[4], so a bad-block night
// left the recording silently dead despite the chain mechanism being in place).
//
// SPI clock note: Sd2Card::init() ignores its sckRateID parameter when constructed with
// a DSPI pointer (always ends at 20 MHz), and csLow(SD_SS) hard-sets 20 MHz on every
// transaction. SPI_HALF_SPEED is therefore a no-op via the DSPI path — we always run
// at full 20 MHz. All call sites in this file pass SPI_FULL_SPEED for explicitness;
// SPI_HALF_SPEED is intentionally never used here. (Kept the constant in the SD lib
// to preserve the upstream API; it just has no effect on this board.)
//
// SD_WRITE_TIMEOUT was raised 600 → 1500 ms in the local SD-library fork on
// 2026-05-13 to absorb pSLC GC bursts on Max Endurance / Industrial cards
// without falsely tripping the multi-block recovery path. See Sd2Card.h.
// ---------------------------------------------------------------------------
// Runtime-tunable recovery / SD constants (T-protocol + %TUNE; added 2026-05-15).
//
// Each of these was a compile-time #define before; now they're RAM-backed
// uint8/16/32s so a host can change them mid-session via the binary 'T'
// command, or session_start.py can persist them in SESSION.TXT as a
// `%TUNE k=v` line for auto-resume. Lets us A/B values per card class
// without reflashing (cyton bootloader dance is ~30 s/cycle and easy to miss
// the upload window).
//
// Defaults match the pre-2026-05-15 #define values verbatim. Code paths read
// the variable directly so a host write is immediately effective. Wire format
// + valid ranges + key IDs: see applyTune() below.
#define DEFAULT_CKPT_INTERVAL_MS         60000UL  // emit a %CKPT line about once per minute
// Resume cap (EEPROM[7]) ceiling. Bumped 3 → 25 on 2026-05-13 after a night
// where one slot was created but recorded zero samples (controller wedged on
// the very first block-write of the new slot), burning a full budget unit on
// nothing useful. With per-good-CKPT reset, a healthy night gets effectively
// unlimited retries; with cap=25 a true card-death scenario still idles instead
// of thrashing the SD until physical exhaustion. EEPROM[7] is uint8 with 0xFF
// reserved as virgin sentinel — anything <= 254 is safe.
#define DEFAULT_MAX_RESUMES              25
// Extended in-place recovery window before falling back to slot recreation
// (executeSoftReset). Added 2026-05-13. Inline writeStart retry (5x) + one
// card.init+writeStart already covers brief stalls; this extends with a
// delay+retry loop that drains host serial between attempts. Targets two
// real-world failure modes: (a) SD-sniffer micro-movement causing brief
// contact loss, (b) Max Endurance / pSLC controllers stalling SPI for
// hundreds of ms during background GC bursts. Sample stream pauses during
// the wait; gap is recorded in sdRetries (so next %CKPT shows the cost).
#define DEFAULT_EXT_RECOVERY_WINDOW_MS   8000UL
#define DEFAULT_EXT_RECOVERY_CHUNK_MS    500UL

uint8_t  tuneMaxResumes          = DEFAULT_MAX_RESUMES;
uint16_t tuneExtRecoveryWindowMs = DEFAULT_EXT_RECOVERY_WINDOW_MS;
uint16_t tuneExtRecoveryChunkMs  = DEFAULT_EXT_RECOVERY_CHUNK_MS;
uint32_t tuneCkptIntervalMs      = DEFAULT_CKPT_INTERVAL_MS;
// The 5th tunable (SD_WRITE_TIMEOUT) lives in the SD library fork
// (Sd2Card.h) because that's where the SPI waitNotBusy() call sites live.
// Converted from `uint16_t const` to plain `uint16_t` on 2026-05-15 so
// applyTune() below can write to it like the others. See
// patches/sd-fork-write-timeout.patch for the library change.
extern uint16_t SD_WRITE_TIMEOUT;

// Tune-protocol key IDs. uint8 so the wire byte is unambiguous; gaps left for
// future additions. Stable on the wire — never renumber.
#define TUNE_KEY_MAX_RESUMES             0x01
#define TUNE_KEY_EXT_RECOVERY_WINDOW_MS  0x02
#define TUNE_KEY_EXT_RECOVERY_CHUNK_MS   0x03
#define TUNE_KEY_CKPT_INTERVAL_MS        0x04
#define TUNE_KEY_SD_WRITE_TIMEOUT        0x05
boolean  sdCardDead    = false;     // skip-forward + card.init both failed; recording is over
uint32_t sdReinits     = 0;         // card.init() FAST-path recovery cycles run this session (1x per failure event)
uint32_t sdExtRetries  = 0;         // EXTENDED-window card.init+writeStart attempts (added 2026-05-13);
                                    // accumulates across the per-event 8 s window — ~1..16 per event
uint32_t sdLastCkptMs  = 0;         // last %CKPT emit time (millis()); soft-WDT input
                                    // — refresh of this timestamp proves writeDataToSDcard
                                    // is being called with samples (ISR alive + writeCache
                                    // returning). Checked from loop() in DefaultBoard.ino
                                    // against max(SOFT_WDT_FLOOR_MS, 2× tuneCkptIntervalMs).
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
prog_char extRetryStamp[] PROGMEM = { "%ExtRetries:\n"};    // 13 (added 2026-05-13)
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


// ---------------------------------------------------------------------------
// SESSION.TXT persisted-session-config protocol (commands 'P' + boot replay)
//
// Design intent: a single file at SD root is the source of truth for "should
// the cyton auto-resume on this boot, and with what config". File contents
// are a literal byte stream of the OpenBCI host commands that session_start.py
// would send to start a session (channel config xNGSIBPnX, sample rate ~N,
// board mode /N, slot K, META M..., streaming b). At boot, if the file is
// present and valid, the firmware feeds those bytes through the same three-
// stage dispatch loop() uses for host serial bytes (sdMetaProcess →
// sdProcessChar → board.processChar), reproducing the session start without
// any host involvement.
//
// File framing:
//   first 8 bytes  : "%PBEGIN\n"  magic header (detects partial / never-written
//                    file vs torn mid-write — replay refuses if missing)
//   payload        : raw command stream (any bytes, up to 1024)
//   last 6 bytes   : "%PEND\n"    end sentinel
//
// Wire protocol (host → cyton) mirrors the existing M META protocol:
//   'P' <lenLo> <lenHi> <up to 1024 bytes payload>
//   ack: "PERSIST OK <len> <sum>\n$$$"  or  "PERSIST FAIL\n$$$"
//
// Why a separate opcode from M: M writes %META INLINE into the currently-open
// OBCI_*.TXT recording file. P writes a SEPARATE file (SESSION.TXT). Different
// destinations, different state machines.
//
// Why 'P' (not 'W'): the upstream OpenBCI protocol uses 'W' as CHANNEL_ON_10
// (OpenBCI_32bit_Library_Definitions.h:233). 'P' is unused. Picked over 'B'
// because P=Persist is mnemonic.
// ---------------------------------------------------------------------------

// State machine for the P command (mirrors sdMetaProcess shape exactly)
uint8_t  sessState = 0;   // 0=idle, 1=need lenLo, 2=need lenHi, 3=copy payload, 4=drain bad len
uint16_t sessLen   = 0;   // payload length parsed from lenLo|lenHi (also bytes-remaining in state 3)
uint16_t sessTotal = 0;   // total payload bytes the host promised (echoed in ack)
uint16_t sessSum   = 0;   // running 16-bit sum of payload bytes (echoed in ack)
uint32_t sessStart = 0;   // millis() when 'P' arrived (1 s timeout safety)
uint8_t  sessBuf[1024];   // RAM buffer for full payload (avoids SPI contention with any open
                          // multi-block recording — though P is only valid when !streaming)

// Replay mode flag — set true while replaySessionFile() is feeding bytes from
// SESSION.TXT through processChar. Lets producers of host-facing chatter
// (sdMetaProcess META OK lines, sdProcessChar size prints, processChar's
// channel-set ACKs, board.sendEOT $$$) suppress output that would otherwise
// confuse a host that's not even connected yet. Read by the chatter sites
// directly — keep it global for cheap access.
boolean replayingSession = false;

// Tracks whether the post-replay success witness has fired this session.
// Reset to false on every fresh setupSDcard. Set true (with EEPROM[7]=0
// commit) on the first %CKPT emit after a successful replay — that's our
// proof that the resume actually started writing samples. Subsequent CKPTs
// in the same session are no-ops on EEPROM (flash wear).
boolean firstCkptResetDone = false;

// Tracks sdErrs at the most recent %CKPT emit. If the next %CKPT shows the
// same value (no new errors in the last ~60s), we know the recovery has
// settled and clear ledSDError so the LED returns to the normal recording
// pattern. Without this, a single transient SD hiccup at 03:00 leaves the
// LED strobing the rest of the night even though recording is fine — the
// morning user sees the strobe and can't tell whether recording is still
// alive. With the auto-clear, the LED self-heals within ~60s of recovery,
// so the morning state is a real-time "is it working RIGHT NOW" indicator
// rather than a stale "did anything go wrong sometime tonight" alert. The
// error counters in the file's %CKPT lines preserve the forensic history.
uint32_t lastCkptSdErrs = 0;

// Set true when replaySessionFile() returned without starting streaming —
// signals driveLed to emit a "replay attempted but failed" double-flash so
// the user can distinguish "no SESSION.TXT present, idle on purpose" from
// "tried to resume but something went wrong" without pulling the SD card.
// driveLed needs to import this — declared in OpenBCI_32bit_Library too.
boolean ledReplayFail = false;


boolean sdPersistProcess(char c){
  // 1 s safety timeout: if mid-state stalls, abort and let next byte start fresh
  if (sessState != 0 && (millis() - sessStart) > 1000) sessState = 0;
  if (sessState == 0) {
    // Dispatch order in loop() puts sdPersistProcess BEFORE sdMetaProcess so
    // that 'P' is intercepted at top level. But 'P' (ASCII 80) is also a
    // perfectly valid byte inside an in-flight 'M' META payload (e.g. a JSON
    // note containing "%CKPT" — the literal P would otherwise be hijacked
    // here, the rest of the META payload would be mis-parsed as P-protocol
    // length+payload bytes, and sdMetaProcess would never see the body it
    // expects). Gate state-0 entry on sdMetaState == 0 so we never start a
    // P transaction during an active META payload.
    if (c == 'P' && !board.streaming && !replayingSession && sdMetaState == 0) {
      // We require not-streaming to avoid contending the SD cache with active
      // multi-block writes. replayingSession guard prevents recursive write
      // from the replay path itself feeding our own bytes back through here.
      sessState = 1;
      sessLen   = 0;
      sessTotal = 0;
      sessSum   = 0;
      sessStart = millis();
      return true;
    }
    return false;
  }
  if (sessState == 1) { sessLen = (uint8_t)c; sessState = 2; return true; }
  if (sessState == 2) {
    sessLen |= ((uint16_t)(uint8_t)c) << 8;
    if (sessLen == 0) {
      sessState = 0;
      if (!replayingSession) { Serial0.println("PERSIST ERR EMPTY"); board.sendEOT(); }
      return true;
    }
    if (sessLen > 1024) {
      sessLen   = 1024;        // drain at most a sane max so spurious bytes can't leak
      sessState = 4;
      if (!replayingSession) { Serial0.println("PERSIST ERR TOOBIG"); board.sendEOT(); }
      return true;
    }
    sessTotal = sessLen;
    sessState = 3;
    return true;
  }
  if (sessState == 3) {
    // Buffer in RAM. We won't open SESSION.TXT until the full payload is in
    // hand — keeps the SD write atomic with respect to host's payload stream.
    sessBuf[sessTotal - sessLen] = (uint8_t)c;
    sessSum += (uint8_t)c;
    sessLen--;
    if (sessLen == 0) {
      sessState = 0;
      // Now write the file. Failure here doesn't corrupt any active recording
      // — SESSION.TXT is independent of the OBCI_*.TXT files.
      boolean ok = false;
      uint8_t pfail = 0;   // diagnostic — last failed step, emitted in PERSIST FAIL response
      if (!cardInit) {
        if (card.init(SPI_FULL_SPEED, SD_SS)) cardInit = volume.init(card);
      }
      if (!cardInit) pfail = 1;
      else {
        // SdFile::openRoot returns false if `root` is already open. In this
        // firmware, replaySessionFile (boot time) and setupSDcard both call
        // root.openRoot(volume) and rely on the side-effect of pointing root
        // at the volume's root cluster; setupSDcard explicitly doesn't check
        // the return (line 249 just does `openvol = root.openRoot(volume);`
        // and never reads openvol). Match that pattern — call it for the
        // side-effect, ignore the return. (Was previously pfail=2 here when
        // root was already open from boot, even though root state was valid.)
        root.openRoot(volume);
        // Remove any prior SESSION.TXT — start fresh. SdFile::remove returns
        // 0 if the file doesn't exist; that's fine, we ignore the result.
        SdFile::remove(&root, "SESSION.TXT");
        SdFile sess;
        // O_TRUNC dropped — the SdFat fork's open(..O_TRUNC) returned 0 in
        // practice even though the file didn't exist (since we just removed
        // it). O_CREAT | O_WRITE alone is sufficient on this fork.
        if (!sess.open(&root, "SESSION.TXT", O_CREAT | O_WRITE)) pfail = 3;
        else {
          // Magic header first (eight bytes including the trailing newline)
          const char hdr[] = "%PBEGIN\n";
          const char ftr[] = "%PEND\n";
          int wrote_hdr = sess.write((const uint8_t*)hdr, sizeof(hdr) - 1);
          int wrote_pay = sess.write(sessBuf, sessTotal);
          // Always terminate the payload with a newline before %PEND so the
          // last command isn't glued to the sentinel (some host commands like
          // 'X'-latch are single chars and need their own line/separator).
          uint8_t nl = '\n';
          sess.write(&nl, 1);
          int wrote_ftr = sess.write((const uint8_t*)ftr, sizeof(ftr) - 1);
          sess.sync();   // flush FAT cache so a power loss after ACK doesn't lose the file
          sess.close();
          ok = (wrote_hdr == (int)(sizeof(hdr) - 1)) &&
               (wrote_pay == (int)sessTotal) &&
               (wrote_ftr == (int)(sizeof(ftr) - 1));
          if (!ok) {
            // pfail codes 4..6 narrow down which write returned wrong byte count.
            if (wrote_hdr != (int)(sizeof(hdr) - 1)) pfail = 4;
            else if (wrote_pay != (int)sessTotal)    pfail = 5;
            else                                     pfail = 6;
          }
        }
      }
      if (ok) {
        // Successful new session config → reset resumeCount cap so the new
        // session gets a fresh MAX_RESUMES-strike budget for silent halts.
        EEPROM.write(7, 0);
      }
      if (!replayingSession) {
        if (ok) {
          Serial0.print("PERSIST OK ");
          Serial0.print(sessTotal);
          Serial0.print(" ");
          Serial0.println(sessSum);
        } else {
          // Emit pfail step code so the host can see which SD step failed:
          // 1=card/volume init, 2=root open, 3=file open, 4=hdr write,
          // 5=payload write, 6=footer write.
          Serial0.print("PERSIST FAIL ");
          Serial0.println(pfail);
        }
        board.sendEOT();
      }
    }
    return true;
  }
  // sessState == 4: drain garbage payload bytes from a too-big header
  if (sessState == 4) {
    if (--sessLen == 0) sessState = 0;
    return true;
  }
  return false;
}


// ---------------------------------------------------------------------------
// Tune protocol (binary 'T' command + %TUNE text form for SESSION.TXT)
//
// Wire protocol (host → cyton):
//   'T' <key_id:1B> <value bytes, LSB-first, count implied by key>
//   ack: "TUNE OK <key_id>\n$$$"   on success
//        "TUNE FAIL <code>\n$$$"   on failure (code: 1=unknown key,
//                                              2=value out of range,
//                                              3=1s mid-transaction timeout —
//                                                partial-tune bytes safely
//                                                swallowed, host should resend)
//
// Value-byte counts by key (set in tuneKeyValueLength()):
//   0x01 MAX_RESUMES             : 1 byte (uint8)
//   0x02 EXT_RECOVERY_WINDOW_MS  : 2 bytes (uint16)
//   0x03 EXT_RECOVERY_CHUNK_MS   : 2 bytes (uint16)
//   0x04 CKPT_INTERVAL_MS        : 4 bytes (uint32)
//   0x05 SD_WRITE_TIMEOUT        : 2 bytes (uint16)
//
// Gating: T is intercepted at top-level only when no streaming, no replay,
// no in-flight M META payload, no in-flight P SESSION.TXT payload. Every
// byte beyond the first two (T + key) is consumed by this state machine —
// no fall-through to board.processChar — so transient state cannot leak
// stale value bytes back into the command processor.
//
// Persistence: a host that wants the tune to survive a silent halt sends
// `%TUNE k=v k=v ...\n` as part of the SESSION.TXT payload via P. At boot,
// replaySessionFile() parses %TUNE lines BEFORE feeding the rest of the
// body through dispatch (so the slot/K command runs with the right cap).
// ---------------------------------------------------------------------------

uint8_t  tuneState        = 0;   // 0=idle, 1=need key, 2=consuming value bytes
uint8_t  tuneKey          = 0;
uint32_t tuneVal          = 0;
uint8_t  tuneValPos       = 0;
uint8_t  tuneValBytesLeft = 0;
uint32_t tuneStart        = 0;

// How many value bytes follow a given key on the wire.
uint8_t tuneKeyValueLength(uint8_t key) {
  switch (key) {
    case TUNE_KEY_MAX_RESUMES:             return 1;
    case TUNE_KEY_EXT_RECOVERY_WINDOW_MS:  return 2;
    case TUNE_KEY_EXT_RECOVERY_CHUNK_MS:   return 2;
    case TUNE_KEY_CKPT_INTERVAL_MS:        return 4;
    case TUNE_KEY_SD_WRITE_TIMEOUT:        return 2;
    default: return 0;   // unknown — caller fails fast
  }
}

// Returns 0 on success, 1 unknown key, 2 value out of range.
//
// Range bounds are conservative — they catch fat-finger errors but not
// "this combination of values is operationally sane". Validate the
// combination host-side in session_start.py before sending.
uint8_t applyTune(uint8_t key, uint32_t val) {
  switch (key) {
    case TUNE_KEY_MAX_RESUMES:
      // 0 means "never auto-resume" which kills the safety net; 254 is the
      // max EEPROM[7] can hold (0xFF reserved as virgin sentinel).
      if (val < 1 || val > 254) return 2;
      tuneMaxResumes = (uint8_t)val;
      // If EEPROM[7] (resumeCount) is already ≥ new cap, reset to 0 to
      // preserve "raise the cap" intent. The ROADMAP flagged this case:
      // without the reset, lowering MAX_RESUMES at runtime would
      // immediately lock out auto-resume. Guarded read-then-write so a
      // host re-tuning to the same value repeatedly doesn't burn EEPROM
      // write cycles on a no-op.
      {
        uint8_t rc = EEPROM.read(7);
        if (rc != 0xFF && rc != 0 && rc >= tuneMaxResumes) EEPROM.write(7, 0);
      }
      return 0;
    case TUNE_KEY_EXT_RECOVERY_WINDOW_MS:
      // 0 would disable the extended window entirely (degrading recovery
      // back to the pre-2026-05-13 5x-retry path). Cap at 60 s so a typo
      // can't park the firmware in recovery for the rest of the night.
      // Cross-check: window must be ≥ current chunk or the window would
      // degenerate to a single attempt (caller would then need to set
      // chunk first). Symmetric to the chunk-setter's check below.
      if (val == 0 || val > 60000UL || val < tuneExtRecoveryChunkMs) return 2;
      tuneExtRecoveryWindowMs = (uint16_t)val;
      return 0;
    case TUNE_KEY_EXT_RECOVERY_CHUNK_MS:
      // Must be ≤ window or the window degenerates to a single attempt.
      // Lower bound 10 ms — below that the host-serial drain loop starves.
      if (val < 10 || val > 5000UL || val > tuneExtRecoveryWindowMs) return 2;
      tuneExtRecoveryChunkMs = (uint16_t)val;
      return 0;
    case TUNE_KEY_CKPT_INTERVAL_MS:
      // 1 s lower bound — sub-second CKPT would flood the file with meta.
      // 1 h upper bound — anything longer makes the morning user wait that
      // long for the ledSDError auto-clear path. The soft-WDT threshold in
      // DefaultBoard.ino scales with this value as max(SOFT_WDT_FLOOR_MS,
      // 2× tuneCkptIntervalMs), so any value in this range is safe.
      if (val < 1000UL || val > 3600000UL) return 2;
      tuneCkptIntervalMs = val;
      return 0;
    case TUNE_KEY_SD_WRITE_TIMEOUT:
      // 100 ms floor: below the typical pSLC GC burst, would re-introduce
      // the very failure mode patch B addresses. 5 s ceiling: above this
      // the firmware's recovery cascade can't fire — a truly dead card
      // would hang the sample loop instead of declaring failure.
      if (val < 100UL || val > 5000UL) return 2;
      SD_WRITE_TIMEOUT = (uint16_t)val;
      return 0;
    default:
      return 1;
  }
}

// FNV-1a 32-bit over the 5 current tunables. Emitted in %CKPT as `T=<hex8>`
// so a single line lets the post-processor identify which tuning set
// produced the night's data (cross-checked against %META's `tune` block
// for confidence).
//
// Leading "domain version" byte makes the hash schema-stable: when a 6th
// tunable is added later, bump TUNE_HASH_DOMAIN_VERSION so old recordings
// hash-identify under v1 and new ones under v2 — saves a year-from-now
// archaeology session ("which firmware version produced this hash?").
// Hash space is wide enough that the extra byte costs nothing.
#define TUNE_HASH_DOMAIN_VERSION 0x01
uint32_t tuneSummaryHash() {
  uint32_t h = 0x811C9DC5UL;  // FNV-1a offset basis
  const uint32_t prime = 0x01000193UL;
  h = (h ^ (uint32_t)TUNE_HASH_DOMAIN_VERSION) * prime;
  h = (h ^ (uint32_t)tuneMaxResumes) * prime;
  h = (h ^ (uint32_t)tuneExtRecoveryWindowMs) * prime;
  h = (h ^ (uint32_t)tuneExtRecoveryChunkMs) * prime;
  h = (h ^ tuneCkptIntervalMs) * prime;
  h = (h ^ (uint32_t)SD_WRITE_TIMEOUT) * prime;
  return h;
}

// Reset all tune state-machine variables. Called on every state→0 transition
// (success, failure, timeout) so a future maintainer adding a code path that
// reads them mid-transition gets zeros, not stale leftovers.
static inline void resetTuneState() {
  tuneState        = 0;
  tuneKey          = 0;
  tuneVal          = 0;
  tuneValPos       = 0;
  tuneValBytesLeft = 0;
}

boolean sdTuneProcess(char c) {
  // 1 s safety timeout — mirrors sdPersistProcess + sdMetaProcess shape.
  //
  // CRITICAL: when the timeout fires we MUST also consume the byte that
  // triggered the timeout check (return true) and emit a TUNE FAIL so the
  // host learns its in-flight transaction was abandoned. Without this, a
  // stalled value byte falls through to processChar and a value byte
  // equal to 'b' / 'j' / a channel toggle would silently fire the wrong
  // command. Emit FAIL code 3 = "timeout".
  if (tuneState != 0 && (millis() - tuneStart) > 1000) {
    resetTuneState();
    Serial0.print("TUNE FAIL ");
    Serial0.println(3);
    board.sendEOT();
    return true;
  }
  if (tuneState == 0) {
    // Gate state-0 entry on every other state machine being idle — same
    // reasoning as P's `sdMetaState == 0` guard: a 'T' byte can appear
    // mid-P-payload (SESSION.TXT line bytes) or mid-M-payload (META JSON,
    // e.g. a {"note": "T-test"} string). Letting state-0 fire in those
    // contexts would hijack the wrong byte and break both protocols.
    if (c == 'T' && !board.streaming && !replayingSession
        && sdMetaState == 0 && sessState == 0) {
      resetTuneState();
      tuneState = 1;
      tuneStart = millis();
      return true;
    }
    return false;
  }
  if (tuneState == 1) {
    tuneKey = (uint8_t)c;
    tuneValBytesLeft = tuneKeyValueLength(tuneKey);
    if (tuneValBytesLeft == 0) {
      resetTuneState();
      Serial0.print("TUNE FAIL ");
      Serial0.println(1);   // unknown key code
      board.sendEOT();
      return true;
    }
    tuneValPos = 0;
    tuneState = 2;
    return true;
  }
  if (tuneState == 2) {
    // LSB-first accumulation — matches struct.pack('<...') host-side.
    tuneVal |= ((uint32_t)(uint8_t)c) << (tuneValPos * 8);
    tuneValPos++;
    tuneValBytesLeft--;
    if (tuneValBytesLeft == 0) {
      uint8_t key = tuneKey;
      uint32_t val = tuneVal;
      uint8_t code = applyTune(key, val);
      resetTuneState();
      if (code == 0) {
        Serial0.print("TUNE OK ");
        Serial0.println((int)key);
      } else {
        Serial0.print("TUNE FAIL ");
        Serial0.println((int)code);
      }
      board.sendEOT();
    }
    return true;
  }
  return false;
}

// Parse `%TUNE k=v k=v ...` text payload (after the leading "%TUNE " has
// been stripped, before any trailing newline). Apply each recognised pair
// via applyTune(). Silently skips unknown keys and unparseable values —
// the firmware never aborts a session because of a malformed tune line;
// at worst the value stays at its prior (defaults-or-previous-T) state.
//
// Used by replaySessionFile() to apply %TUNE lines pulled from SESSION.TXT.
// NOT used by sdTuneProcess (that's the binary form). The two share
// applyTune() so the validation + side-effects stay in one place.
//
// Key names MUST match the host's `tune_helpers.py` TUNE_KEYS dict AND the
// C variable names they map to. Renamed 2026-05-15 to remove an asymmetry
// caught in review:
//
//   text                       C variable                     wire key
//   ----                       ----------                     --------
//   max_resumes                tuneMaxResumes                  0x01
//   ext_recovery_window_ms     tuneExtRecoveryWindowMs         0x02
//   ext_recovery_chunk_ms      tuneExtRecoveryChunkMs          0x03
//   ckpt_interval_ms           tuneCkptIntervalMs              0x04
//   sd_write_timeout           SD_WRITE_TIMEOUT (SD lib fork)  0x05
void applyTuneTextLine(const uint8_t* buf, uint16_t len) {
  const uint8_t* p   = buf;
  const uint8_t* end = buf + len;
  while (p < end) {
    while (p < end && (*p == ' ' || *p == '\t')) p++;
    if (p >= end) break;
    const uint8_t* keyStart = p;
    while (p < end && *p != '=' && *p != ' ' && *p != '\t') p++;
    if (p >= end || *p != '=') {
      // Malformed pair — skip to next whitespace and continue.
      while (p < end && *p != ' ' && *p != '\t') p++;
      continue;
    }
    uint16_t keyLen = (uint16_t)(p - keyStart);
    p++;   // skip '='
    const uint8_t* valStart = p;
    while (p < end && *p != ' ' && *p != '\t') p++;
    uint16_t valLen = (uint16_t)(p - valStart);
    if (valLen == 0) continue;
    // Parse value as decimal uint32. Reject on any non-digit (incl. '-')
    // AND on overflow (anything that would wrap uint32). Earlier version
    // relied on applyTune's range check to catch wrapped values — but a
    // catastrophic typo like "ckpt_interval_ms=99999999999" would wrap
    // into the valid range (1000..3600000) by luck, then apply silently.
    uint32_t v = 0;
    boolean bad = false;
    for (uint16_t i = 0; i < valLen; i++) {
      if (valStart[i] < '0' || valStart[i] > '9') { bad = true; break; }
      uint32_t digit = (uint32_t)(valStart[i] - '0');
      // Pre-multiplication overflow check: if v > (UINT32_MAX - digit) / 10
      // then v*10 + digit would wrap. UINT32_MAX = 4294967295.
      if (v > (4294967295UL - digit) / 10UL) { bad = true; break; }
      v = v * 10UL + digit;
    }
    if (bad) continue;
    uint8_t kid = 0;
    if      (keyLen == 11 && memcmp(keyStart, "max_resumes",            11) == 0) kid = TUNE_KEY_MAX_RESUMES;
    else if (keyLen == 22 && memcmp(keyStart, "ext_recovery_window_ms", 22) == 0) kid = TUNE_KEY_EXT_RECOVERY_WINDOW_MS;
    else if (keyLen == 21 && memcmp(keyStart, "ext_recovery_chunk_ms",  21) == 0) kid = TUNE_KEY_EXT_RECOVERY_CHUNK_MS;
    else if (keyLen == 16 && memcmp(keyStart, "ckpt_interval_ms",       16) == 0) kid = TUNE_KEY_CKPT_INTERVAL_MS;
    else if (keyLen == 16 && memcmp(keyStart, "sd_write_timeout",       16) == 0) kid = TUNE_KEY_SD_WRITE_TIMEOUT;
    if (kid != 0) applyTune(kid, v);
    // Unknown keys silently skipped — forward-compat with future tunables.
  }
}


// Boot-time replay of SESSION.TXT. Returns true if a replay was attempted (in
// which case the cyton is now either streaming or has irrecoverably failed
// the resume and is idle), false if no replay was warranted (no file, bad
// framing, cap exhausted).
//
// Called from setup() after board.begin()+wifi.begin() so the ADS chips are
// initialised before xNGSIBPnX lines reconfigure them. Reads the file
// entirely into RAM and closes it BEFORE feeding bytes through processChar,
// because the replay's `K` command will open a new multi-block recording
// file on the same SdVolume — keeping SESSION.TXT's handle open during that
// would contend the SD library's single-cached-block model.
// Best-effort SD writer for a tiny REPLAYFL.TXT forensic file. Called from
// every failure return in replaySessionFile() with a code matching the
// failure point — gives the morning user a way to know WHICH replay step
// failed when ledReplayFail double-flash fires. Silent on SD-write errors:
// if the card is unwritable (failure cause was cardInit itself) we just
// skip and the user has only the LED + missing file as the indicator.
//
// File layout: ASCII line "code=N t=NNNms\n" — short enough to fit in one
// directory entry's worth of clusters. Overwritten on each failed replay
// so the LATEST failure is what the user sees; old causes don't accumulate.
//
// Codes:
//   1 = resume cap exhausted (EEPROM[7] >= MAX_RESUMES)
//   2 = SD init / volume init failed (won't actually write the file in
//       this case — the cardInit guard at function start refuses)
//   3 = root.openRoot failed (refused for same reason as code 2)
//   4 = SESSION.TXT size out of bounds (<16 or >1056)
//   5 = SESSION.TXT short read
//   6 = bad %PBEGIN header
//   7 = bad %PEND footer
//   8 = pre-scan: ~ after K
//   9 = pre-scan: K after M
//  10 = pre-scan: K after b
//  11 = pre-scan: M after b
//  12 = feed completed but board.streaming did not start
static void writeReplayFail(uint8_t code){
  // Force a fresh card.init() before this short forensic write. By the
  // time we land here the card is in a known-bad state (we're called
  // precisely BECAUSE recovery has exhausted, OR because a higher-level
  // condition like cap-exhaust fired with the card possibly degraded).
  // A fresh card.init() resets controller-side state and gives our
  // ~50-byte file its best shot at landing. Without this, a card that
  // failed multi-block writes also failed the REPLAYFL write — leaving
  // the morning user with only the LED double-flash, no forensic file
  // (exactly what we observed on the 2026-05-13 night chain).
  //
  // If init/volume.init() truly fail (card physically dead), we silently
  // return — the LED is still the user's primary indicator. Same fail-mode
  // as before, just less likely.
  if (!card.init(SPI_FULL_SPEED, SD_SS)) { cardInit = false; return; }
  if (!volume.init(card)) { cardInit = false; return; }
  cardInit = true;  // refresh state flag — caller may have stale value

  // root.openRoot returns false when already open (same gotcha as the
  // sdPersistProcess fix on 2026-05-11) — replaySessionFile may have
  // already opened root by the time we get here. Call for side-effect
  // (point root at the volume's root cluster) and ignore the return —
  // root is valid either way. Matches setupSDcard's pattern at line 249.
  root.openRoot(volume);
  SdFile::remove(&root, "REPLAYFL.TXT");
  SdFile rf;
  if (!rf.open(&root, "REPLAYFL.TXT", O_CREAT | O_WRITE)) return;
  char b[40];
  int n = snprintf(b, sizeof(b), "code=%u t=%lums\n",
                   (unsigned)code, (unsigned long)millis());
  if (n > 0) rf.write((const uint8_t*)b, n);
  rf.sync();
  rf.close();
}


boolean replaySessionFile(){
  // No Serial0.println diagnostics at boot — the dongle's RFduino link isn't
  // established yet at this point, so any chatter floods the cyton-side TX
  // buffer (the cyton-RFduino can't drain to radio fast enough), and worse,
  // can wedge the link entirely. Use ledReplayFail double-flash + the
  // REPLAYFL.TXT forensic file (writeReplayFail) as the sole failure
  // indicators. Codes 2/3 can't write the file (no SD); for those the user
  // sees double-flash + absence of REPLAYFL.TXT = "SD itself is the issue".

  // Resume cap — bounds infinite-thrash on a dying cell. Reset to 0 on a
  // successful replay (first %CKPT emit) and on a P command (new session).
  // tuneMaxResumes governs the ceiling — runtime-tunable since 2026-05-15
  // (host T-protocol / %TUNE), defaults to DEFAULT_MAX_RESUMES near top.
  uint8_t rc = EEPROM.read(7);
  if (rc == 0xFF) rc = 0;
  if (rc >= tuneMaxResumes) {
    // Hard cap — refuse and idle. Don't even open the SD; if the cell is
    // truly dying we don't want to fail mid-resume yet again. cardInit
    // hasn't run here so writeReplayFail will be a no-op — but a previous
    // successful boot may have left cardInit=true from earlier in this
    // power-up. Worth trying.
    writeReplayFail(1);
    ledReplayFail = true;
    return false;
  }

  if (!cardInit) {
    if (card.init(SPI_FULL_SPEED, SD_SS)) cardInit = volume.init(card);
  }
  if (!cardInit) {
    // Can't write REPLAYFL.TXT — SD itself is broken. LED double-flash +
    // no file is the diagnostic ("LED says fail, but no REPLAYFL.TXT
    // appeared after the morning processing" → it was an SD-init issue).
    ledReplayFail = true;
    return false;
  }
  // root.openRoot returns false when already open — same gotcha as writeReplayFail
  // and sdPersistProcess (fixed 2026-05-11). Call for side-effect, ignore return.
  root.openRoot(volume);

  SdFile sess;
  if (!sess.open(&root, "SESSION.TXT", O_READ)) {
    // Normal case when there's no auto-resume to do — don't light the
    // failure LED, just idle quietly so a clean morning power-on doesn't
    // look like a failure.
    return false;
  }

  uint32_t sz = sess.fileSize();
  // Minimum viable file is the two markers with at least one command byte:
  // "%PBEGIN\n" (8) + 1 byte + "\n" (1) + "%PEND\n" (6) = 16
  if (sz < 16 || sz > (1024 + 32)) {
    sess.close();
    writeReplayFail(4);
    ledReplayFail = true;
    return false;
  }

  // Read whole file into RAM. We reuse sessBuf — same 1024-byte buffer the
  // P command uses; nobody else needs it at boot time.
  uint8_t buf[1056];
  int n = sess.read(buf, sz);
  sess.close();
  if (n != (int)sz) {
    writeReplayFail(5);
    ledReplayFail = true;
    return false;
  }

  // Validate header
  static const char hdr[] = "%PBEGIN\n";
  if (memcmp(buf, hdr, sizeof(hdr) - 1) != 0) {
    writeReplayFail(6);
    ledReplayFail = true;
    return false;
  }
  // Validate footer (last 6 bytes)
  static const char ftr[] = "%PEND\n";
  if (memcmp(buf + sz - (sizeof(ftr) - 1), ftr, sizeof(ftr) - 1) != 0) {
    writeReplayFail(7);
    ledReplayFail = true;
    return false;
  }

  // Pre-scan: enforce command ordering on top-level command lines. The
  // xNGSIBPnX multi-char commands contain ASCII params that could falsely
  // match our anchors, so scan line-by-line and only look at the first
  // non-space char of each line.
  //
  // Required order: ~ (rate) before K (slot), K before M (META) if M
  // present, K before b (stream start). We don't require all four — only
  // the ones present must be in order.
  uint8_t* p = buf + sizeof(hdr) - 1;
  uint8_t* end = buf + sz - (sizeof(ftr) - 1);
  int pos_tilde = -1, pos_K = -1, pos_M = -1, pos_b = -1, idx = 0;
  uint8_t* lineStart = p;
  while (lineStart < end) {
    // Find first non-whitespace in this line
    uint8_t* lp = lineStart;
    while (lp < end && (*lp == ' ' || *lp == '\t')) lp++;
    if (lp < end) {
      char first = (char)*lp;
      if (first == '~') pos_tilde = idx;
      else if (first == 'K') pos_K = idx;
      else if (first == 'M') pos_M = idx;
      else if (first == 'b') pos_b = idx;
    }
    // Advance to next line
    while (lineStart < end && *lineStart != '\n') lineStart++;
    if (lineStart < end) lineStart++;   // skip the \n
    idx++;
  }
  // Order checks — any pair that's both present must be in increasing order
  if (pos_tilde >= 0 && pos_K >= 0 && pos_tilde > pos_K) {
    writeReplayFail(8);
    ledReplayFail = true;
    return false;
  }
  if (pos_K >= 0 && pos_M >= 0 && pos_K > pos_M) {
    writeReplayFail(9);
    ledReplayFail = true;
    return false;
  }
  if (pos_K >= 0 && pos_b >= 0 && pos_K > pos_b) {
    writeReplayFail(10);
    ledReplayFail = true;
    return false;
  }
  if (pos_M >= 0 && pos_b >= 0 && pos_M > pos_b) {
    writeReplayFail(11);
    ledReplayFail = true;
    return false;
  }
  // For sleep-EEG the typical config has all four, but a minimal recording
  // (no META) is allowed.

  // Bump resume cap BEFORE replay starts so a crash mid-replay still costs
  // a retry — guards infinite-thrash. Update the in-memory `resumeCount`
  // too so %BOOT's "resume=N" field reads the post-bump value (otherwise
  // the line emits resume=0 even though EEPROM was just bumped to 1).
  EEPROM.write(7, rc + 1);
  resumeCount = rc + 1;

  // Now actually replay. Feed each byte through the same three-stage dispatch
  // that loop() uses for host serial bytes. Skip the %PBEGIN line entirely
  // (don't want firmware to dispatch '%' as anything) and stop at %PEND.
  //
  // Set the global `autoResume` flag BEFORE the feed loop so that when the
  // file's `K`/`A`/etc. slot command lands in setupSDcard, the %BOOT line it
  // emits renders as "prev=OBCI_<NN>.TXT resume=N". Without this, the global
  // is still its boot-default (false) at %BOOT-emit time and the line comes
  // out as "prev=NONE resume=0" — losing the chain linkage the post-processor
  // uses to thread a multi-file resumed session together.
  autoResume         = true;
  replayingSession   = true;
  firstCkptResetDone = false;
  uint8_t* feed = buf + sizeof(hdr) - 1;
  // Feed loop is LINE-AWARE so `%TUNE k=v ...` lines can be parsed BEFORE
  // the rest of the body reaches dispatch. Dispatching the raw bytes of
  // "%TUNE max_resumes=25" through board.processChar would fire spurious
  // CHANNEL_ON commands (the literal 'T','U','E' chars are top-level
  // CHANNEL_ON_13/15/11 in the OpenBCI command set).
  //
  // Two paths through the loop body:
  //   - %TUNE line: applyTuneTextLine() parses the body, the ENTIRE LINE
  //     including the trailing newline is consumed silently — NO bytes
  //     reach dispatch. (See `feed = lineEnd + 1` below.)
  //   - Non-%TUNE line: every byte INCLUDING the trailing newline is fed
  //     through dispatch — same shape as the original loop body, just
  //     re-anchored at line boundaries. The inner `feed <= lineEnd` (not
  //     `<`) is what carries the `\n` through. Verified by review.
  while (feed < end) {
    uint8_t* lineEnd = feed;
    while (lineEnd < end && *lineEnd != '\n') lineEnd++;
    uint16_t lineLen = (uint16_t)(lineEnd - feed);
    if (lineLen >= 6 && memcmp(feed, "%TUNE ", 6) == 0) {
      applyTuneTextLine(feed + 6, (uint16_t)(lineLen - 6));
      // Consume entire line including the newline (if present); do NOT
      // feed any of its bytes through dispatch.
      feed = lineEnd < end ? lineEnd + 1 : lineEnd;
      continue;
    }
    // Default path — feed every byte of the line (and the trailing newline)
    // through the same three-stage dispatch loop() uses for host serial
    // bytes. sdPersistProcess gate stays first (replayingSession=true keeps
    // it inert, but feed it anyway for symmetry with loop()). sdTuneProcess
    // is NOT in the replay path — replay uses the text %TUNE form only.
    while (feed <= lineEnd && feed < end) {
      char c = (char)*feed++;
      if (!sdPersistProcess(c)) {
        if (!sdMetaProcess(c)) {
          sdProcessChar(c);
          board.processChar(c);
        }
      }
    }
  }
  replayingSession = false;

  // Successful replay if streaming actually started. If it didn't, the
  // resume failed somewhere during the byte feed (most likely setupSDcard
  // got called via 'K' but createContiguous failed, OR streamStart didn't
  // fire properly). Leave resumeCount bumped (so next boot may trip the
  // cap), light the failure LED, write the forensic file, and idle.
  //
  // CRITICAL: at this point setupSDcard MAY have opened an OBCI file in
  // multi-block-write mode (the file's K command ran and reached writeStart
  // before something else broke). writeReplayFail opens REPLAYFL.TXT on
  // the same SdVolume — opening a second SdFile while a multi-block-write
  // context is active will either silently no-op the write (same class of
  // bug we hit in closeSDfile's SdFile::remove ordering) or worse corrupt
  // the OBCI multi-block reservation. End the multi-block context first.
  //
  // NOTE: we deliberately do NOT call closeSDfile() here — that path deletes
  // SESSION.TXT which we want to preserve so the next boot can RETRY the
  // resume (resumeCount cap will eventually stop infinite-thrash). Manual
  // writeStop + openfile.close keeps SESSION.TXT intact.
  if (!board.streaming) {
    if (fileIsOpen) {
      board.csLow(SD_SS);
      card.writeStop();
      openfile.close();
      board.csHigh(SD_SS);
      fileIsOpen = false;
      SDfileOpen = false;
      board.sdFileOpen = false;
    }
    writeReplayFail(12);
    ledReplayFail = true;
  }
  return board.streaming;
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

  // Every fresh setupSDcard call starts a new session — re-arm the
  // "successful streaming witness" so the first %CKPT of THIS session
  // resets EEPROM[7]=0 (resume cap). Without this, only the FIRST session
  // since boot triggers the reset; subsequent host-`K` sessions on the
  // same MCU uptime leave stale resumeCount values that could trip the
  // cap on a later night.
  firstCkptResetDone = false;

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
  sdErrs = 0; sdRetries = 0; board.ledSDError = false; lastCkptSdErrs = 0;
  // Intentional: a host-issued file-size command (h/A/S/.../K/L) starts a fresh
  // recording and thereby acts as the manual recovery for a sdCardDead state from
  // a prior session. The card.init() at the top of this function (line ~155) is
  // the only init the firmware does at session start; in-session inline recovery
  // does its own card.init() if needed (see writeCache).
  sdCardDead = false; sdReinits = 0; sdExtRetries = 0; sdLastCkptMs = millis();
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
    // resume count: 0..MAX_RESUMES. Emit as 1 or 2 ASCII digits — single
    // digit when <10 (back-compat with pre-2026-05-13 parsers that only
    // saw 0..3), two digits when >=10. Post-processor reads "resume=" up
    // to whitespace/EOL so either width parses fine.
    {
      uint8_t rc_emit = resumeCount;
      if (rc_emit > 99) rc_emit = 99;          // hard ceiling — display only
      if (rc_emit >= 10) {
        EMIT_BYTE('0' + (rc_emit / 10));
        EMIT_BYTE('0' + (rc_emit % 10));
      } else {
        EMIT_BYTE('0' + rc_emit);
      }
    }
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
    // Clean close = "user said stop, do not auto-resume on next boot".
    //
    // Order matters for the power-loss safety: the SESSION.TXT-presence flow
    // makes the FILE the source of truth for whether replay should fire on
    // next boot. EEPROM[7] is the resume-cap counter — EEPROM[7]=0 is the
    // FRESH-CAP state (MAX_RESUMES retries available), NOT a "no resume" signal. So:
    //
    //   (1) writeStop + openfile.close — end the SD multi-block-write
    //       context (set up by setupSDcard's card.writeStart). Must come
    //       first so the SD will accept FAT-modifying ops.
    //   (2) SdFile::remove(SESSION.TXT) — THE authoritative "do not resume"
    //       signal. Once this commits, no future boot will auto-resume.
    //   (3) EEPROM[7]=0 (cap refresh) and EEPROM[4]=0 (legacy clear) —
    //       hygiene only. NOT load-bearing on the resume decision. Doing
    //       them AFTER step 2 ensures any power-loss interleaving doesn't
    //       leave us in EEPROM[7]=0 + SESSION.TXT-still-present, which
    //       would auto-resume on next boot despite the user's clean-close
    //       intent. (Codex review 2026-05-11 flagged this ordering bug.)
    //
    // Power-loss windows after this reorder:
    //   * before writeStop: SD multi-block context preserved on disk; next
    //     boot's mount sees a partially-closed file (filefrag-style) but
    //     the FAT entry is consistent. SESSION.TXT still present → boot
    //     auto-resumes into a new file (same as a real silent halt).
    //   * after writeStop, before remove: SESSION.TXT still on disk → boot
    //     auto-resumes. EEPROM[7] still holds prior value. resumeCount
    //     cap still bounds infinite-thrash. Same as silent halt.
    //   * after remove, before EEPROM clear: SESSION.TXT gone → boot idles
    //     cleanly. Cap counter may be stale but next P command resets it.
    //   * after EEPROM clear: fully clean state.
    board.csLow(SD_SS);  // take spi
    card.writeStop();
    openfile.close();
    SdFile::remove(&root, "SESSION.TXT");
    EEPROM.write(7, 0);
    EEPROM.write(4, 0);  // legacy migration clear
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
  // D3a — INACTIVITY timeout (was absolute-from-'M'), refreshed on every byte below,
  // so it only fires after the link has been quiet > 1 s. What we do on expiry depends
  // on which state stalled:
  //   - a RECEIVE state (1/2/3): the rest of the stalled payload may STILL arrive late
  //     over the RF link, and those bytes must NOT fall through to the command
  //     dispatcher (a delayed 'b'/digit would execute = silent corruption). So emit
  //     META FAIL and enter the drain-until-quiet state (4), swallowing the triggering
  //     byte and the dead tail.
  //   - the DRAIN state (4): a >1 s gap means the dead tail has stopped and the link is
  //     quiet, so THIS byte is a fresh command (the host's resync / 'b') — release it
  //     (fall through) and process it normally.
  if (sdMetaState != 0 && (millis() - sdMetaStart) > 1000) {
    uint8_t prevState = sdMetaState;
    sdMetaState = 0;
    if (prevState != 4 && !board.streaming) { Serial0.println("META FAIL"); board.sendEOT(); }
    if (prevState != 4) {            // 1/2/3 → drain the (possibly still-arriving) tail
      sdMetaState = 4;
      sdMetaStart = millis();        // arm the drain inactivity timer
      return true;                   // swallow the timeout-triggering byte
    }
    // prevState == 4: fall through with sdMetaState now 0 → this post-quiet byte is real
  }
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
  sdMetaStart = millis();   // D3a — refresh the inactivity deadline on every frame byte
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
      // bad length: drain-until-quiet (no fixed count) so a host that ships MORE than
      // it declared can't have the overflow leak into command dispatch — the top
      // inactivity timeout releases us once the burst stops. (A fixed-count drain
      // under-drains an over-length burst and leaks the remainder.)
      sdMetaState = 4;
      if (!board.streaming) { Serial0.println("META ERR"); board.sendEOT(); }
    } else { sdMetaLen = sdMetaCount; sdMetaState = 3; }
  } else if (sdMetaState == 3) {
    // D2 — RAM-STAGE the whole payload, then write once. NO SD write happens
    // while bytes are arriving, so a blocking writeCache() can never starve the
    // UART RX FIFO and drop a payload byte mid-frame (the act-20 bug: byteCounter
    // ~57 from the un-flushed %BOOT line + a >455 B payload used to cross the 512
    // boundary mid-frame). Mirrors the proven 'P' path (sdPersistProcess). sessBuf
    // reuse is safe: a 'P' byte inside this payload cannot start a P transaction
    // because sdPersistProcess gates state-0 entry on sdMetaState == 0, so P never
    // touches sessBuf during an active META.
    sessBuf[sdMetaLen - sdMetaCount] = (uint8_t)c;
    sdMetaSum += (uint8_t)c;
    if (--sdMetaCount == 0) {
      // Full payload in RAM. COMMIT: replay into pCache via the EXISTING
      // writeCache() 512-boundary path (NOT SdFile::write — M writes inside the
      // active contiguous OBCI recording, so pCache/byteCounter/blockCounter
      // alignment must be preserved for the following %START AT + sample CSV).
      // These blocking writes now happen AFTER RX is complete, while the host is
      // waiting for the ACK and sending nothing — so no byte can be dropped.
      // Keep sdMetaState == 3 through the replay AND the pad-flush so a failed
      // writeCache() still sets sdMetaCorrupted (the only correct fail signal).
      for (uint16_t i = 0; i < sdMetaLen; i++) {
        pCache[byteCounter++] = sessBuf[i];
        if (byteCounter == 512) writeCache();
      }
      // Flush any %E markers deferred during the replay (writeCache may emit
      // several if the SD failed across blocks). Pre-flush if < 3 bytes remain.
      while (sdPendingErrs > 0) {
        if (byteCounter > 509) writeCache();
        pCache[byteCounter++] = '%';
        pCache[byteCounter++] = 'E';
        pCache[byteCounter++] = '\n';
        sdPendingErrs--;
      }
      // Force-flush pCache to a block boundary so the META line(s) land on their
      // own SD block(s) before we ACK — durable + isolated from later sample
      // writes. Pad with newlines (harmless to the text parser: split to len-1
      // empty strings, no match against ^[0-9A-F]{2},).
      if (byteCounter > 0) {
        while (byteCounter < 512) pCache[byteCounter++] = '\n';
        writeCache();
      }
      sdMetaState = 0;   // leave state 3 only AFTER every writeCache() above
      if (!board.streaming) {
        // D3b — verdict on sdMetaCorrupted ALONE. The old `sdErrs > before` check
        // false-FAILed when a writeData hiccup was recovered by the same-block
        // retry (no data lost). sdMetaCorrupted is set only on a genuine
        // META-block-losing skip-forward — including during the pad-flush above,
        // because we held state == 3 through it.
        if (sdMetaCorrupted) {
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
  } else { // sdMetaState == 4 — DRAIN-UNTIL-QUIET: swallow every byte (over-length
    // payload, or the dead tail of an aborted frame) without dispatching it. The
    // top-of-function inactivity timeout (prevState==4 branch) releases us to state 0
    // on the first byte that arrives after a >1 s quiet gap, which is then processed
    // as a real command. No fixed count → late RF bytes can never leak into dispatch.
    // (sdMetaStart was refreshed above, keeping the drain armed while bytes flow.)
    //
    // KNOWN LIMITATION (unreachable in this system): the drain release happens here in
    // sdMetaProcess, which loop() dispatches AFTER sdPersistProcess/sdTuneProcess —
    // both of which gate their state-0 entry on sdMetaState==0. So if the FIRST
    // post-quiet byte were 'P' or 'T', those gates would see the not-yet-cleared
    // sdMetaState==4, reject it, and the byte would fall through to board.processChar
    // (which ignores P/T) and be dropped. This never happens in practice: the host
    // sends 'P'/'T' only during setup BEFORE the 'M' frame, never after; the only
    // post-drain bytes are the resync ('\n','?') and 'b' — none of which are P/T; and
    // auto-resume (replaySessionFile) never issues 'M' so it never enters this drain.
    // A full fix would hoist the inactivity reap into loop() ahead of the P/T/M
    // dispatch — deferred (it touches the hot path and can't be bench-tested here).
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
      ((uint32_t)(millis() - sdLastCkptMs) >= tuneCkptIntervalMs)) {
    char tmp[128];   // bumped 80 -> 128 on 2026-05-15 for the new T=<hex8>
                     // field. Worst-case line is ~100 chars; 128 gives slack
                     // for any future single-field addition before we have
                     // to revisit. snprintf truncates if undersized, so an
                     // over-tight buffer would silently shorten the line.
    // %CKPT format extended 2026-05-13 with x= (extended-window attempts).
    // Extended again 2026-05-15 with T=<hex8> — FNV-1a hash over the 5
    // runtime tunables, so each morning file self-documents which tuning
    // was active. Older parsers tolerant of unknown k=v fields will just
    // skip the new field; readers that want it can pull `T=([0-9a-f]+)`.
    int n = snprintf(tmp, sizeof(tmp),
                     "%%CKPT t=%lu b=%d e=%lu r=%lu n=%lu o=%lu x=%lu T=%08lx\n",
                     (unsigned long)millis(),
                     blockCounter,
                     (unsigned long)sdErrs,
                     (unsigned long)sdRetries,
                     (unsigned long)sdReinits,
                     (unsigned long)overruns,
                     (unsigned long)sdExtRetries,
                     (unsigned long)tuneSummaryHash());
    if (n > 0 && byteCounter + n <= 512) {
      memcpy(pCache + byteCounter, tmp, n);
      byteCounter += n;
      sdLastCkptMs = millis();
      // Success witness for the resume-cap reset: first %CKPT in this
      // session proves the resume reached steady-state streaming. Reset
      // EEPROM[7]=0 so a dying-cell pattern of repeated silent halts gets
      // a fresh MAX_RESUMES-strike budget after each successful chunk,
      // rather than exhausting the cap after MAX_RESUMES cumulative resets across the whole
      // night. Guarded so we only write EEPROM once per session.
      if (!firstCkptResetDone) {
        EEPROM.write(7, 0);
        firstCkptResetDone = true;
      }
      // Auto-clear ledSDError when the current %CKPT shows no new errors
      // since the previous %CKPT — i.e. the past ~60s of writes were clean,
      // so any earlier transient SD hiccup has fully recovered. Without
      // this, ledSDError sticks on for the rest of the session and the
      // morning user can't distinguish "recording fine, had a glitch 6h ago"
      // from "recording is currently broken". The forensic history is
      // preserved in the %CKPT counters in the file (e=, r=, n=).
      if (board.ledSDError && sdErrs == lastCkptSdErrs) {
        board.ledSDError = false;
      }
      lastCkptSdErrs = sdErrs;
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
          // Extended recovery wait window (added 2026-05-13). The fast paths
          // above (5x writeStart + 1x card.init+writeStart) cover ~hundreds
          // of ms of brief stalls. This second-tier loop covers the longer
          // failure modes that previously fell straight through to
          // executeSoftReset (and burnt a full resume-cap unit on a slot
          // that often recorded NOTHING after recreation):
          //   (a) SD-sniffer micro-movement causing a brief contact loss
          //       — usually clears after 100s of ms once contact restores
          //   (b) Max Endurance / pSLC controllers stalling SPI for
          //       hundreds of ms during background GC bursts — exceeds
          //       SD_WRITE_TIMEOUT but resolves on the next card.init()
          //   (c) Voltage-rail brown-outs on AA-battery-powered cytons
          //       during card.init() inrush — settles after ~seconds
          //
          // Strategy: wait EXT_RECOVERY_WINDOW_MS total, retrying every
          // EXT_RECOVERY_CHUNK_MS with a fresh card.init+writeStart. Drain
          // host serial during the wait so the RFduino link stays alive
          // (incoming bytes accumulating in the cyton-side FIFO would
          // otherwise back up and eventually wedge the radio link).
          // Discard those bytes — we can't safely process commands while
          // mid-recovery (the SD layer is in flux).
          //
          // Sample stream pauses for the duration of the wait: ADS DRDY
          // interrupts may fire but loop()'s updateChannelData isn't
          // called while we're inside this delay loop, so samples are
          // dropped at the source. The cost (up to EXT_RECOVERY_WINDOW_MS
          // of dropped samples) is recorded in sdRetries; next %CKPT
          // shows the delta. Visible gap >> losing rest of the night.
          if (!resumed && blockCounter + 1 < BLOCK_COUNT) {
            uint32_t windowStart = millis();
            while (((uint32_t)(millis() - windowStart) < tuneExtRecoveryWindowMs) && !resumed) {
              // Wait one chunk while draining host serial (1ms granularity
              // so we don't spin CPU and can react quickly when the chunk
              // boundary lands). EXT_RECOVERY_CHUNK_MS / 1ms = N iterations.
              uint32_t chunkEnd = millis() + tuneExtRecoveryChunkMs;
              while ((int32_t)(chunkEnd - millis()) > 0) {
                delay(1);
                while (board.hasDataSerial0()) (void)board.getCharSerial0();
              }
              // Use the dedicated extended-window counter (sdExtRetries)
              // rather than sdReinits so the morning user can distinguish
              //   n=N : real fast-path card.init events (1 per failure event)
              //   x=N : extended-window attempts (up to ~16 per event)
              // Conflating them was a confusing semantics change; keeping
              // them separate preserves per-version comparability of the
              // existing n= field.
              sdExtRetries++;
              if (card.init(SPI_FULL_SPEED, SD_SS) &&
                  card.writeStart(bgnBlock + blockCounter + 1, BLOCK_COUNT - blockCounter - 1)) {
                resumed = true;
              }
            }
          }
        }
        if (!resumed) {
          // In-place recovery exhausted (5x skip-forward writeStart + 1x
          // card.init+writeStart + EXT_RECOVERY_WINDOW_MS extended retry
          // all failed). Two failure modes look the same here: a
          // localized bad-block range in the current slot's extent (next
          // slot will be fine) vs whole-card flash death (next slot will
          // fail the same way). We can't distinguish at this layer, so
          // we trigger the auto-resume chain to try the next slot:
          // executeSoftReset(0) issues a software reset → next boot
          // enters setup() with RCON.SWR=0x40, calls replaySessionFile()
          // which reads SESSION.TXT (preserved on disk, not deleted here)
          // and re-runs the original K/L/etc command → setupSDcard() bumps
          // the OBCI_<N>.TXT counter and allocates the NEXT slot → recording
          // resumes in that fresh extent. resumeCount cap (EEPROM[7]) bounds
          // this chain to MAX_RESUMES attempts: bad-block scenario succeeds
          // on the first few attempts; whole-card-dead scenario eventually
          // exhausts to ledReplayFail.
          //
          // CRITICAL: preserve the auto-resume gates. Do NOT remove
          // SESSION.TXT, do NOT clear EEPROM[4]. Both are what the chain
          // needs to fire on the next boot. (Pre-2026-05-12 code did both
          // here and silently defeated its own chain mechanism.)
          //
          // Release SPI before reset (defensive; reset will tear down all
          // peripherals anyway, but a low CS line during the reset window
          // could leave the SD card mid-transaction — releasing first is
          // cleaner for the post-reset card.init() that runs in setup()).
          board.csHigh(SD_SS);

          // If this happened mid-META payload, flag corruption so the host
          // gets META FAIL when it queries — though in practice the host
          // won't see this response because we're about to reset.
          if (sdMetaState == 3) sdMetaCorrupted = true;

          // ROLLBACK 2026-06-23: self-reset-on-recovery-exhaustion DISABLED. This
          // executeSoftReset()→auto-resume chain was corrupting nights: the reset fires
          // mid-write, orphaning the slot's FAT/directory entry so the whole recording
          // reads back all-NUL (lost). Reverted to the pre-2026-05-12 behavior — fall
          // straight through to the clean sdCardDead stop below, which preserves the
          // partial recording up to the failure point (recoverable, like a normal slot)
          // instead of resetting and losing everything.
          // executeSoftReset(0);

          // Now the ACTUAL path (was the unreachable belt-and-braces fallback): clean
          // teardown, stop writing, leave the partial file intact. SESSION.TXT is left
          // on disk (harmless; the host's session_start overwrites it next session).
          sdCardDead = true;
          fileIsOpen = false;
          SDfileOpen = false;
          board.sdFileOpen = false;
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

  for(int i=0; i<13; i++){
    pCache[byteCounter] = pgm_read_byte_near(extRetryStamp+i);
    byteCounter++;
  }
  convertToHex(sdExtRetries, 7, false);

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
