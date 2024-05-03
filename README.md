# OpenBCI 32bit Library with SD improvements

This is a modified library for the OpenBCI 32bit Board firmware which adds additional SD card functionality. 

Main purpose is to improve experience while using OpenBCI Cyton as a part of home based PSG sleep research. You can read more about my quantified self project and especially OpenBCI experience [here](https://blog.kto.to/hypnodyne-zmax-vs-openbci-eeg-psg) .

* short (50ms) flashing LED during writing, every 3 seconds. [Source](https://github.com/OpenBCI/OpenBCI_Cyton_Library/pull/80)
* increased sampling rate. [Source](https://github.com/OpenBCI/OpenBCI_Cyton_Library/pull/96)
* bugfixes. [Source](https://github.com/OpenBCI/OpenBCI_Cyton_Library/pull/93)
* it works for both Cyton and Cyton+Daisy
* i use it on a daily basis with 3 different Cyton/Cyton+Daisy and it work stable, rarely i'm getting empty sd files for unknown reason (once 50-60 recordings). [Read more](https://openbci.com/forum/index.php?p=/discussion/3775/sd-card-file-is-empty/)

TODO:
* add external trigger to stard sd writing with increased sampling rate and hardcoded config, without using a dongle at all

Instructions:
* Follow OpenBCI Cyton official firmware upgrade [instructions](https://docs.openbci.com/Cyton/CytonProgram/)
* The only diffrence is that you clone [modded OpenBCI 32bit Library](https://github.com/roflecopter/OpenBCI_Cyton_Library_SD/) from this repo instead of default [OpenBCI_32bit_Library](https://github.com/OpenBCI/OpenBCI_32bit_Library)

Importan notice: 
* You do flashing on your own risk
* If something goes wrong you can potentially flash stock firmware back if hardware is not faulty.
* I dont take any responsibility for potential damage, firmware provided as is.
