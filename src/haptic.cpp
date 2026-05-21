// =============================================================
// haptic.cpp — WS2812 RGB pulse patterns (replaces ERM motor)
//
// SenseCAP Watcher exposes one WS2812 on GPIO40 (BSP_RGB_CTRL).
// Ref: examples/knob_rgb/main/knob_rgb.c (led_strip_config_t).
//
// Heartbeat is NOT white anymore. Each pattern has a default
// colour that matches its meaning, and callers can override the
// colour to match the pet's vitality so the front LED doesn't
// just strobe white every few seconds.
// =============================================================

#include <Adafruit_NeoPixel.h>
#include "haptic.h"

static Adafruit_NeoPixel s_px(RGB_LED_COUNT, PIN_RGB_LED, NEO_GRB + NEO_KHZ800);

// Keep the global LED brightness modest — the WS2812 on the Watcher
// front glass is *very* bright at full drive and washes out in dim
// rooms. 64/255 is still clearly visible but no longer flash-bang.
static constexpr uint8_t LED_BRIGHTNESS = 64;

void Haptic::begin() {
  s_px.begin();
  s_px.clear();
  s_px.setBrightness(LED_BRIGHTNESS);
  s_px.show();
}

void Haptic::_applyIntensity(uint8_t intensity) {
  if (intensity == 0) {
    s_px.setPixelColor(0, 0);
  } else {
    uint8_t r = (uint8_t)((uint16_t)_r * intensity / 255);
    uint8_t g = (uint8_t)((uint16_t)_g * intensity / 255);
    uint8_t b = (uint8_t)((uint16_t)_b * intensity / 255);
    s_px.setPixelColor(0, s_px.Color(r, g, b));
  }
  s_px.show();
}

void Haptic::setSolid(uint8_t r, uint8_t g, uint8_t b) {
  _active = false;
  _stepCount = 0;
  _r = r; _g = g; _b = b;
  _applyIntensity(255);
}

void Haptic::play(HapticPattern pattern) {
  if (pattern == HAPTIC_NONE) { stop(); return; }
  uint8_t r, g, b;
  _defaultColorFor(pattern, r, g, b);
  play(pattern, r, g, b);
}

void Haptic::play(HapticPattern pattern, uint8_t r, uint8_t g, uint8_t b) {
  if (pattern == HAPTIC_NONE) { stop(); return; }
  _r = r; _g = g; _b = b;
  _loadPattern(pattern);
  _currentStep = 0;
  _stepStart   = millis();
  _active      = _stepCount > 0;
  if (_active) _applyIntensity(_steps[0].intensity);
}

void Haptic::stop() {
  // Ensure the LED is off, not just "pattern inactive" — otherwise a
  // leftover solid colour keeps glowing forever.
  _active      = false;
  _currentStep = 0;
  _stepCount   = 0;
  s_px.setPixelColor(0, 0);
  s_px.show();
}

bool Haptic::update() {
  if (!_active) return false;
  unsigned long now = millis();
  if (now - _stepStart >= _steps[_currentStep].durationMs) {
    _currentStep++;
    if (_currentStep >= _stepCount) {
      stop();
      return false;
    }
    _stepStart = now;
    _applyIntensity(_steps[_currentStep].intensity);
  }
  return _active;
}

void Haptic::_loadPattern(HapticPattern pattern) {
  switch (pattern) {
    case HAPTIC_HEARTBEAT:
      // Single gentle pulse, no double-bump. Much shorter visible time
      // than before so the LED feels alive but not strobing.
      _stepCount = 2;
      _steps[0] = { 70, 100 };
      _steps[1] = {  0,  24 };
      break;

    case HAPTIC_CELEBRATION:
      _stepCount = 4;
      _steps[0] = { 220, 100 };
      _steps[1] = {   0,  80 };
      _steps[2] = { 220, 100 };
      _steps[3] = {   0,  60 };
      break;

    case HAPTIC_COUNTDOWN:
      _stepCount = 2;
      _steps[0] = { 180,  40 };
      _steps[1] = {   0,  20 };
      break;

    case HAPTIC_SOS:
      _stepCount = 6;
      _steps[0] = { 255,  80 };
      _steps[1] = {   0,  50 };
      _steps[2] = { 255,  80 };
      _steps[3] = {   0,  50 };
      _steps[4] = { 255,  80 };
      _steps[5] = {   0, 150 };
      break;

    case HAPTIC_SOFT_BUZZ:
      _stepCount = 2;
      _steps[0] = { 140,  50 };
      _steps[1] = {   0,  20 };
      break;

    case HAPTIC_SHUTTER:
      // Camera-like pop: bright micro-flash then immediate drop.
      _stepCount = 3;
      _steps[0] = { 255,  20 };
      _steps[1] = { 120,  24 };
      _steps[2] = {   0,  16 };
      break;

    case HAPTIC_REWARD:
      // Softer reward arc: punchy but not flashy/harsh.
      _stepCount = 6;
      _steps[0] = {  90, 65 };
      _steps[1] = {   0, 28 };
      _steps[2] = { 130, 70 };
      _steps[3] = {   0, 34 };
      _steps[4] = { 170, 95 };
      _steps[5] = {   0, 28 };
      break;

    default:
      _stepCount = 0;
      break;
  }
}

void Haptic::_defaultColorFor(HapticPattern pattern,
                              uint8_t& r, uint8_t& g, uint8_t& b) {
  switch (pattern) {
    case HAPTIC_HEARTBEAT:
      // Default: soft warm amber. The idle loop typically overrides
      // this with a state-derived colour.
      r = 255; g = 140; b = 40; break;

    case HAPTIC_CELEBRATION:
      // Bright green — "you did it".
      r = 60; g = 255; b = 80; break;

    case HAPTIC_COUNTDOWN:
      // Warm white-ish amber for "get ready".
      r = 255; g = 180; b = 80; break;

    case HAPTIC_SOS:
      r = 255; g = 0; b = 0; break;

    case HAPTIC_SOFT_BUZZ:
      // Cool blue click confirmation.
      r = 40; g = 120; b = 255; break;

    case HAPTIC_SHUTTER:
      // Crisp neutral white for "camera snap".
      r = 255; g = 255; b = 255; break;

    case HAPTIC_REWARD:
      // Joyful mint-green.
      r = 90; g = 255; b = 170; break;

    default:
      r = 0; g = 0; b = 0; break;
  }
}
