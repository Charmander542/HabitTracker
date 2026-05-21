#pragma once
// =============================================================
// haptic.h — Non-blocking "haptic" feedback on SenseCAP Watcher
//
// The Watcher has no vibration motor in the official BSP. The RGB
// WS2812 on GPIO40 is used as a short coloured pulse to substitute
// for tactile feedback (see knob_rgb / factory UI patterns).
//
// The LED is coloured to reflect *meaning*:
//   - Short press buzz      -> cool blue flash
//   - Countdown             -> warm amber
//   - Celebration           -> rainbow cycle
//   - SOS                   -> red
//   - Heartbeat             -> dim state-coloured pulse
//
// Patterns can be launched either with the convenience enum (which
// picks a sensible default colour) or with an explicit RGB so the
// caller can tie the LED to the pet's current state.
// =============================================================

#include <Arduino.h>
#include "config.h"

enum HapticPattern {
  HAPTIC_NONE,
  HAPTIC_HEARTBEAT,
  HAPTIC_CELEBRATION,
  HAPTIC_COUNTDOWN,
  HAPTIC_SOS,
  HAPTIC_SOFT_BUZZ,
  HAPTIC_SHUTTER,
  HAPTIC_REWARD
};

class Haptic {
public:
  void begin();

  // Fire a pattern with its default colour for that pattern.
  void play(HapticPattern pattern);

  // Fire a pattern using an explicit RGB colour. Useful for the idle
  // heartbeat where we want the LED to match the pet's emotional state.
  // r/g/b are 0..255; the pattern's intensity curve modulates these.
  void play(HapticPattern pattern, uint8_t r, uint8_t g, uint8_t b);

  // Low-level: solid colour on the LED (no pattern). Overwritten on next play().
  void setSolid(uint8_t r, uint8_t g, uint8_t b);

  // Turn the LED off immediately.
  void stop();

  // Must be called every loop iteration. Returns true while a pattern is active.
  bool update();

  // Diagnostic: whether a pattern is currently running.
  bool isActive() const { return _active; }

private:
  struct PatternStep {
    uint8_t  intensity;   // 0..255 — multiplier against the chosen RGB
    uint16_t durationMs;
  };

  static const int MAX_STEPS = 8;
  PatternStep   _steps[MAX_STEPS];
  int           _stepCount   = 0;
  int           _currentStep = 0;
  unsigned long _stepStart   = 0;
  bool          _active      = false;

  uint8_t _r = 0;
  uint8_t _g = 0;
  uint8_t _b = 0;

  void _loadPattern(HapticPattern pattern);
  void _applyIntensity(uint8_t intensity);

  static void _defaultColorFor(HapticPattern pattern,
                               uint8_t& r, uint8_t& g, uint8_t& b);
};
