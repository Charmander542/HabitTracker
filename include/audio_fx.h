#pragma once

#include <Arduino.h>

enum SoundFx {
  SFX_NONE = 0,
  SFX_TICK,
  SFX_SHUTTER,
  SFX_REWARD,
  SFX_TONE_LONG
};

class AudioFx {
public:
  bool begin();
  bool isReady() const { return _ready; }
  bool isBusy() const;
  bool play(SoundFx fx);
  void stop();
  bool waitIdle(uint32_t timeoutMs);
  bool setMode(uint8_t mode);
  uint8_t getMode() const;
  uint8_t getModeCount() const;
  const char* getModeName() const;

private:
  bool _ready = false;
};

