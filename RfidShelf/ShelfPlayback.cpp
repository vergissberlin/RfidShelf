#include "ShelfPlayback.h"

void ShelfPlayback::begin() {

  if (AMP_POWER > 0) {
    // Disable amp
    pinMode(AMP_POWER, OUTPUT);
    digitalWrite(AMP_POWER, LOW);
    Sprintln("Amp powered down");
  }

  // initialise the music player
  if (!_musicPlayer.begin()) { // initialise the music player
    Sprintln("Couldn't find VS1053, do you have the right pins defined?");
    while (1) delay(500);
  }
  Sprintln("VS1053 found");

  /* Fix for the design fuckup of the cheap LC Technology MP3 shield
    see http://www.bajdi.com/lcsoft-vs1053-mp3-module/#comment-33773
    Doesn't hurt for other shields
  */
  _musicPlayer.sciWrite(VS1053_REG_WRAMADDR, VS1053_GPIO_DDR);
  _musicPlayer.sciWrite(VS1053_REG_WRAM, 0x0003);
  _musicPlayer.GPIO_digitalWrite(0x0000);
  _musicPlayer.softReset();

  Sprintln("VS1053 soft reset done");

  if (_patchVS1053()) {
#ifdef USE_DIFFERENTIAL_OUTPUT
    // Enable differential output
    uint16_t mode = VS1053_MODE_SM_DIFF | VS1053_MODE_SM_SDINEW;
    _musicPlayer.sciWrite(VS1053_REG_MODE, mode);
#else
    // Enable Mono Output
    _musicPlayer.sciWrite(VS1053_REG_WRAMADDR, 0x1e09);
    _musicPlayer.sciWrite(VS1053_REG_WRAM, 0x0001);
#endif
    Sprintln("VS1053 patch installed");
  } else {
    Sprintln("Could not load patch");
  }

  volume(ShelfConfig::config.defaultVolumne);

  setBassAndTreble(TREBLE_AMPLITUDE, TREBLE_FREQLIMIT, BASS_AMPLITUDE, BASS_FREQLIMIT);

  Sprintln("VS1053 initialized");

  if(_shuffleHistory.begin(BOOLARRAY_MAXSIZE) != BOOLARRAY_OK){
    Sprintln("Error initializing shuffle history");
  }
}

const bool ShelfPlayback::switchFolder(const char *folder) {
  Sprint("Switching folder to "); Sprintln(folder);

  if (!_SD.exists(folder)) {
    Sprintln("Folder does not exist");
    return false;
  }
  stopPlayback();
  _currentFolder.close();
  _currentFolder.open(folder);
  _currentFolder.rewind();
  _currentFile[0] = '\0';
  _currentFolderFileCount = 0;

  sdfat::SdFile file;
  char filenameChar[100];

  while (file.openNext(&_currentFolder, sdfat::O_READ))
  {
    file.getName(filenameChar, sizeof(filenameChar));

    if (!file.isDir() && _musicPlayer.isMP3File(filenameChar)) {
      _currentFolderFileCount++;
    }
    file.close();
  }

  return true;
}

void ShelfPlayback::currentFolder(char *foldername, size_t size) {
  _currentFolder.getName(foldername, size);
}

void ShelfPlayback::currentFile(char *filename, size_t size) {
  strncpy(filename, _currentFile, size);
}

void ShelfPlayback::resumePlayback() {
  if(_playing != PLAYBACK_PAUSED) {
    Sprintf("Wrong playback state for resume: %d\n", _playing);
    return;
  }
  if(!_musicPlayer.currentTrack) {
    // Workaround until https://github.com/adafruit/Adafruit_VS1053_Library/pull/58 gets merged
    Sprintln("No playback file open");
    return;
  }
  if (AMP_POWER > 0) {
    digitalWrite(AMP_POWER, HIGH);
  }
  _musicPlayer.pausePlaying(false);
  _playing = PLAYBACK_FILE;
}

void ShelfPlayback::pausePlayback() {
  if(_playing != PLAYBACK_FILE) {
    return;
  }
  if (AMP_POWER > 0) {
    digitalWrite(AMP_POWER, LOW);
  }
  _musicPlayer.pausePlaying(true);
  _playing = PLAYBACK_PAUSED;
  if(isNight()) {
    _lastNightActivity = millis();
  }
}

void ShelfPlayback::togglePause() {
  if(_playing == PLAYBACK_PAUSED) {
    playingByCard = false;
    resumePlayback();
  }else if(_playing != PLAYBACK_NO) {
    pausePlayback();
  }
}

void ShelfPlayback::stopPlayback() {
  Sprintln("Stopping playback");
  if(_playing == PLAYBACK_NO) {
    return;
  }

  if (AMP_POWER > 0) {
    digitalWrite(AMP_POWER, LOW);
  }
  _musicPlayer.stopPlaying();
  _playing = PLAYBACK_NO;
  _shuffleHistory.clear();
  _shufflePlaybackCount = 0;
  _repeatMode = ShelfConfig::config.defaultRepeat;
  _shuffleMode = ShelfConfig::config.defaultShuffle;
  if(isNight()) {
    _lastNightActivity = millis();
  }
}

void ShelfPlayback::startPlayback() {
  // IO takes time, reset watchdog timer so it does not kill us
  ESP.wdtFeed();
  sdfat::SdFile file;
  _currentFolder.rewind();

  char nextFile[100] = "";
  char filenameChar[100];

  // Find next file in shuffle playback
  if(_shuffleMode && (_currentFolderFileCount-_shufflePlaybackCount > 0)) {

    uint16_t shuffleNumber = random(_currentFolderFileCount-_shufflePlaybackCount);

    uint16_t hits = 0;
    uint16_t nextFileIndex = 0;
    while (file.openNext(&_currentFolder, sdfat::O_READ)) {
      file.getName(filenameChar, sizeof(filenameChar));

      if (file.isDir() || !_musicPlayer.isMP3File(filenameChar)) {
        Sprint("Ignoring "); Sprintln(filenameChar);
        file.close();
        continue;
      }
      file.close();

      if(!_shuffleHistory.get(nextFileIndex)) {
        if(hits == shuffleNumber) {
          strncpy(nextFile, filenameChar, sizeof(nextFile));
          break;
        }
        hits++;
      }
      nextFileIndex++;
    }

    _shuffleHistory.set(nextFileIndex, true);
    _shufflePlaybackCount++;
  }

  // Find next file for alphabetical playback
  if(!_shuffleMode) {
    while (file.openNext(&_currentFolder, sdfat::O_READ))
    {
      file.getName(filenameChar, sizeof(filenameChar));

      if (file.isDir() || !_musicPlayer.isMP3File(filenameChar)) {
        Sprint("Ignoring "); Sprintln(filenameChar);
        file.close();
        continue;
      }
      file.close();

      if (strcmp(_currentFile, filenameChar) < 0 && (strcmp(filenameChar, nextFile) < 0 || strlen(nextFile) == 0)) {
        strncpy(nextFile, filenameChar, sizeof(nextFile));
      }
    }
  }

  // Start folder from the beginning
  if (strlen(nextFile) == 0) {
    if (!_repeatMode) {
      stopPlayback();
      return;
    }
    if (strlen(_currentFile) == 0) {
      // No _currentFile && no nextFile => Nothing to play!
      Sprintln("No mp3 files found");
      stopPlayback();
      return;
    } else {
      if(_shuffleMode) {
        _shuffleHistory.clear();
        _shufflePlaybackCount = 0;
      }
      _currentFile[0] = '\0';
      startPlayback();
      return;
    }
  }

  char folder[100];
  _currentFolder.getName(folder, sizeof(folder));

  startFilePlayback(folder, nextFile);
}

void ShelfPlayback::startFilePlayback(const char *folder, const char *file) {
  char fullPath[201];
  snprintf(fullPath, sizeof(fullPath), "/%s/%s", folder, file);

  Sprint("Playing "); Sprintln(fullPath);

  _playing = PLAYBACK_FILE;
  strncpy(_currentFile, file, sizeof(_currentFile));

  if (AMP_POWER > 0) {
    digitalWrite(AMP_POWER, HIGH);
  }

  if(!_musicPlayer.startPlayingFile(fullPath)) {
    Sprintln("Could not start playback");
    stopPlayback();
  }
}

void ShelfPlayback::skipFile() {
  _musicPlayer.stopPlaying();
  startPlayback();
}

void ShelfPlayback::volume(uint8_t volume) {
  if(volume > 50) {
    _volume = 50;
  } else {
    _volume = volume;
  }

  uint8_t calcVolume = volume;

  if(isNight()) {
    calcVolume = 50 - (NIGHT_FACTOR * (50 - _volume));
  }
  _musicPlayer.setVolume(calcVolume, calcVolume);
}

void ShelfPlayback::volumeUp() {
  if(_volume < 5) {
    volume(0);
  } else {
    volume(_volume - 5);
  }
}

void ShelfPlayback::volumeDown() {
  volume(_volume + 5);
}

void ShelfPlayback::setBassAndTreble(uint8_t trebleAmplitude, uint8_t trebleFreqLimit, uint8_t bassAmplitude, uint8_t bassFreqLimit) {
  uint16_t bassReg = 0;
  bassReg |= trebleAmplitude;
  bassReg <<= 4;
  bassReg |= trebleFreqLimit;
  bassReg <<= 4;
  bassReg |= bassAmplitude;
  bassReg <<= 4;
  bassReg |= bassFreqLimit;
  _musicPlayer.sciWrite(VS1053_REG_BASS, bassReg);
}

const bool ShelfPlayback::_patchVS1053() {
  Sprintln("Installing patch to VS1053");

  sdfat::SdFile file;
  if (!file.open("patches.053", sdfat::O_READ)) return false;

  uint16_t addr, n, val, i = 0;

  while (file.read(&addr, 2) && file.read(&n, 2)) {
    i += 2;
    if (n & 0x8000U) {
      n &= 0x7FFF;
      if (!file.read(&val, 2)) {
        file.close();
        return false;
      }
      while (n--) {
        _musicPlayer.sciWrite(addr, val);
      }
    } else {
      while (n--) {
        if (!file.read(&val, 2)) {
          file.close();
          return false;
        }
        i++;
        _musicPlayer.sciWrite(addr, val);
      }
    }
  }
  file.close();

  return true;
}

void ShelfPlayback::startNight() {
  _lastNightActivity = millis();
  _nightMode = true;
  volume(_volume);
}

const bool ShelfPlayback::isNight() {
  return _nightMode;
}

void ShelfPlayback::stopNight() {
  _nightMode = false;
  volume(_volume);
}

void ShelfPlayback::startShuffle() {
  _shuffleMode = true;
  _shuffleHistory.clear();
  _shufflePlaybackCount = 0;
}

const bool ShelfPlayback::isShuffle() {
  return _shuffleMode;
}

void ShelfPlayback::stopRepeat() {
  _repeatMode = false;
}

void ShelfPlayback::startRepeat() {
  _repeatMode = true;
}

const bool ShelfPlayback::isRepeat() {
  return _repeatMode;
}

void ShelfPlayback::stopShuffle() {
  _shuffleMode = false;
}

void ShelfPlayback::work() {
  if (_playing == PLAYBACK_FILE) {
    if (_musicPlayer.playingMusic) {
      _musicPlayer.feedBuffer();
      return;
    }

    // If playingMusic is false there might still be data buffered in the VS1053 so we need to query its registers
    if((_musicPlayer.sciRead(VS1053_REG_HDAT0) == 0) && (_musicPlayer.sciRead(VS1053_REG_HDAT1) == 0)) {
      startPlayback();
      return;
    }

    Sprintln("Flushing VS1053 buffer");

    // Follow the datasheet:
    // Read extra parameter value endFillByte
    _musicPlayer.sciWrite(VS1053_REG_WRAMADDR, VS1053_GPIO_DDR);
    uint8_t endFillByte = (uint8_t)(_musicPlayer.sciRead(VS1053_REG_WRAM) & 0xFF);
    uint8_t endFillBytes[32];
    memset(endFillBytes, endFillByte, sizeof(endFillBytes));
    // Send at least 2052 bytes of endFillByte[7:0] (we'll do a few more)
    for(uint8_t i = 0; i <= 64; i++) {
      while(!_musicPlayer.readyForData()) ESP.wdtFeed();
      _musicPlayer.playData(endFillBytes, sizeof(endFillBytes));
    }
    // Set SCI MODE bit SM CANCEL
    _musicPlayer.sciWrite(VS1053_REG_MODE, VS1053_MODE_SM_LINE1 | VS1053_MODE_SM_SDINEW | VS1053_MODE_SM_CANCEL);
    // Send at least 32 bytes of endFillByte[7:0]
    // Read SCI MODE. If SM CANCEL is still set, send again. 
    for(uint8_t i = 0; i <= 64; i++) {
      while(!_musicPlayer.readyForData()) ESP.wdtFeed();
      _musicPlayer.playData(endFillBytes, sizeof(endFillBytes));
      if((_musicPlayer.sciRead(VS1053_REG_MODE) & VS1053_MODE_SM_CANCEL) == 0) {
        uint16_t hdat0 = _musicPlayer.sciRead(VS1053_REG_HDAT0);
        uint16_t hdat1 = _musicPlayer.sciRead(VS1053_REG_HDAT1);
        if(hdat0 != 0 || hdat1 != 0) {
          Sprint("HDAT not 0: "); Sprint(hdat0); Sprint(" "); Sprint(hdat1); Sprintln();
        }
        return;
      }
    }

    // If SM CANCEL hasn’t cleared after sending 2048 bytes, do a 
    // software reset (this should be extremely rare)
    Sprintln("Cancel after playback failed.");
    // TODO check if this deletes the patch
    _musicPlayer.softReset();
    return;

  // if not playing and timeout => disable night mode
  } else if (isNight() && (millis() - _lastNightActivity > NIGHT_TIMEOUT)) {
    stopNight();
  }
}
