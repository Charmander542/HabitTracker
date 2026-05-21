// =============================================================
// encoder.cpp — SenseCAP Watcher rotary knob (GPIO41/42) + expander button
//
// Knob quadrature matches Seeed knob_rgb example (BSP_KNOB_A/B).
// Centre push is NOT a GPIO: it is PCA9535 pin 3 (see watcher_bsp).
// =============================================================

#include "encoder.h"
#include "watcher_bsp.h"

static Encoder* _encoderInstance = nullptr;

static const int8_t _grayTable[16] = {
  0, -1, +1,  0,
  +1,  0,  0, -1,
  -1,  0,  0, +1,
  0, +1, -1,  0
};

void IRAM_ATTR encoderISR() {
  if (!_encoderInstance) return;
  uint8_t a = digitalRead(PIN_KNOB_A);
  uint8_t b = digitalRead(PIN_KNOB_B);
  uint8_t encoded = (a << 1) | b;
  uint8_t idx = (_encoderInstance->_lastEncoded << 2) | encoded;
  _encoderInstance->_tickAccum += _grayTable[idx & 0x0F];
  _encoderInstance->_lastEncoded = encoded;
}

void Encoder::begin() {
  _encoderInstance = this;
  pinMode(PIN_KNOB_A, INPUT_PULLUP);
  pinMode(PIN_KNOB_B, INPUT_PULLUP);

  // Let pull-ups stabilise before sampling -- SPI3 init for the SPD2010
  // LCD runs just before this in setup() and can briefly couple noise onto
  // neighbouring GPIOs.
  delay(50);

  uint8_t a = digitalRead(PIN_KNOB_A);
  uint8_t b = digitalRead(PIN_KNOB_B);
  _lastEncoded = (a << 1) | b;

  attachInterrupt(digitalPinToInterrupt(PIN_KNOB_A), encoderISR, CHANGE);
  attachInterrupt(digitalPinToInterrupt(PIN_KNOB_B), encoderISR, CHANGE);

  // Discard any edge counts captured during pinMode/attachInterrupt setup.
  noInterrupts();
  _tickAccum = 0;
  interrupts();

  // Input grace period: ignore presses / rotation / holds for the first 2s.
  _inputEnabledAt = millis() + 2000;
}

void Encoder::update() {
  unsigned long now = millis();

  // During the boot grace period: clear any accumulated state and skip
  // debounce work entirely so we don't latch a phantom press.
  if ((long)(now - _inputEnabledAt) < 0) {
    noInterrupts();
    _tickAccum = 0;
    interrupts();
    _btnDown       = false;
    _pressConsumed = false;
    _holdFired     = false;
    return;
  }

  bool rawDown = watcher_knob_button_is_pressed();

  if (rawDown && !_btnDown) {
    _btnDown        = true;
    _btnDownAt      = now;
    _pressConsumed  = false;
    _holdFired      = false;
    _sleepHoldFired = false;
  } else if (!rawDown && _btnDown) {
    unsigned long held = now - _btnDownAt;
    // If the user held long enough to trigger sleep, swallow the matching
    // release so we don't ALSO emit a short-press event after toggling sleep.
    if (held >= ENCODER_DEBOUNCE_MS && held < BUTTON_HOLD_MS && !_pressConsumed) {
      _pressConsumed = true;
    }
    _btnDown        = false;
    _holdFired      = false;
    // Note: _sleepHoldFired stays sticky until isSleepHeld() consumes it.
  } else if (_btnDown) {
    unsigned long held = now - _btnDownAt;
    if (!_holdFired && held >= BUTTON_HOLD_MS) {
      _holdFired = true;
    }
    // Sleep-hold edge: fires once when we cross the longer threshold.
  }
}

int Encoder::getDelta() {
  if ((long)(millis() - _inputEnabledAt) < 0) return 0;

  // _stepsPerDetent quadrature edges = 1 logical step. Remainder is kept
  // across calls so slow turns accumulate ticks across many loop iterations
  // instead of being silently discarded.
  const int div = (_stepsPerDetent > 0) ? _stepsPerDetent : 1;

  noInterrupts();
  int raw       = _tickAccum;
  int steps     = raw / div;
  int remainder = raw - steps * div;
  _tickAccum    = remainder;
  interrupts();
  return steps;
}

void Encoder::setStepsPerDetent(int n) {
  if (n < 1) n = 1;
  if (n > 4) n = 4;
  noInterrupts();
  _stepsPerDetent = n;
  _tickAccum      = 0;   // fresh start so old partial counts don't bleed in
  interrupts();
}

int Encoder::peekRawAccum() const {
  noInterrupts();
  int v = _tickAccum;
  interrupts();
  return v;
}

bool Encoder::isPressed() {
  if ((long)(millis() - _inputEnabledAt) < 0) return false;
  if (_pressConsumed && !_btnDown) {
    _pressConsumed = false;
    return true;
  }
  return false;
}

bool Encoder::isHeld() {
  if ((long)(millis() - _inputEnabledAt) < 0) return false;
  return _holdFired;
}

// Fires exactly once per long-hold (>= BUTTON_SLEEP_HOLD_MS).
// Also marks the press as consumed so the upcoming release doesn't fire
// isPressed() — otherwise the sleep gesture would also be interpreted as
// a "short press" and immediately wake the device back up.
bool Encoder::isSleepHeld() {
  if ((long)(millis() - _inputEnabledAt) < 0) return false;
  if (_btnDown && !_sleepHoldFired) {
    if ((millis() - _btnDownAt) >= BUTTON_SLEEP_HOLD_MS) {
      _sleepHoldFired = true;
      _pressConsumed  = true;   // swallow the release
      return true;
    }
  }
  return false;
}
