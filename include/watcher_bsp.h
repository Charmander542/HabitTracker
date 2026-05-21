#pragma once
// =============================================================
// watcher_bsp.h — Minimal SenseCAP Watcher board bring-up (Arduino)
//
// Mirrors the IO-expander + power sequencing from Seeed’s BSP so the
// PCA9535 is configured and rails enabled before reading the knob button.
// Ref: SenseCAP-Watcher-Firmware/components/sensecap-watcher/sensecap-watcher.c
//      bsp_io_expander_init(), examples/get_started/main/get_started.c
// =============================================================

#include <Arduino.h>

bool watcher_bsp_begin();

// Raw knob button: true while pressed (active low on expander input).
bool watcher_knob_button_is_pressed();

// Last successful I2C / expander init (for diagnostics).
bool watcher_bsp_ioexp_ok();
