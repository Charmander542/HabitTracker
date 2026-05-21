#pragma once
// =============================================================
// encoder.h — SenseCAP Watcher knob (GPIO41/42) + expander button
//
// Quadrature on A/B with interrupts. Centre push is read via
// watcher_bsp (PCA9535), not a direct GPIO — see Seeed BSP.
// =============================================================

#include <Arduino.h>
#include "config.h"

class Encoder {
public:
  // Attach interrupts and configure GPIO pull-ups; call in setup()
  void begin();

  // Must be called every loop iteration.
  // Reads accumulated ISR state, applies debounce, updates button.
  void update();

  // Returns the net rotation since the last getDelta() call.
  // Positive = clockwise, negative = counter-clockwise, 0 = no change.
  int getDelta();

  // Returns true once per short button press (< BUTTON_HOLD_MS).
  // Consumed on read — subsequent calls return false until next press.
  bool isPressed();

  // Returns true while the button has been held for >= BUTTON_HOLD_MS.
  // Stays true until the button is released.
  bool isHeld();

  // Fires exactly once per long-hold (>= BUTTON_SLEEP_HOLD_MS, e.g. 2 s).
  // Used by the sleep-mode gesture in main.cpp. After it fires once, we
  // arm the press-consumption flag so the matching button release does
  // NOT additionally trigger isPressed() — otherwise waking and sleeping
  // would chain on the same gesture.
  bool isSleepHeld();

  // Number of quadrature ISR edges that count as one logical "detent".
  // Valid values: 1, 2, or 4. Default 2 (matches most EC11-class knobs
  // as shipped by Seeed on the Watcher). Exposed so the serial harness
  // can tune this on-device without reflashing.
  void setStepsPerDetent(int n);
  int  getStepsPerDetent() const { return _stepsPerDetent; }

  // Peek-only read of the raw ISR accumulator (does not consume it).
  // Used by the `encraw` serial command to calibrate step count.
  int peekRawAccum() const;

private:
  // Accumulated tick count written by ISR; read and cleared by getDelta()
  volatile int _tickAccum = 0;

  // ISR-updated raw A/B state for direction detection
  volatile uint8_t _lastEncoded = 0;

  // Divisor used by getDelta() to convert raw ISR edges into logical steps.
  // Start at 2 — field data on the Seeed Watcher knob shows ~2 edges per
  // physical detent, so /4 was swallowing half of all detents.
  int _stepsPerDetent = 2;

  // Button state tracking
  bool          _btnDown        = false;
  unsigned long _btnDownAt      = 0;
  bool          _pressConsumed  = false;  // short press has been handed off
  bool          _holdFired      = false;  // hold event has been fired this press
  bool          _sleepHoldFired = false;  // sleep-hold event already consumed

  // Snapshot of _tickAccum taken atomically in getDelta()
  int _pendingDelta = 0;

  // Ignore all input until millis() reaches this mark. Prevents phantom
  // presses during the first ~2s of boot, while the PCA9535 I2C bus and
  // knob pin pull-ups settle after watcher_bsp power sequencing and SPI3
  // display init.
  unsigned long _inputEnabledAt = 0;

  // Friends with ISR so it can update private state
  friend void IRAM_ATTR encoderISR();
};

// Global ISR handler; defined in encoder.cpp
void IRAM_ATTR encoderISR();
