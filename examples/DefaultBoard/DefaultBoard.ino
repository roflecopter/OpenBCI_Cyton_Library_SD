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

// Boot diagnostic state (consumed by SD_Card_Stuff.ino → %BOOT line in every recording).
// EEPROM bytes 0/1 are used for the SD file rotation counter; we use bytes 2/3 for bootSeq.
uint32_t bootResetCause = 0;  // RCON snapshot at MCU setup() entry (pre-clear)
uint16_t bootSeq        = 0;  // EEPROM-backed: increments on every MCU boot (POR/WDT/MCLR/BOR/SWR/...)

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

  // Bring up the OpenBCI Board
  board.begin();

  // Bring up wifi
  wifi.begin(true, true);


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
