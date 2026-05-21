#pragma once
// =============================================================
// Shim: Arduino_GFX expects IDF 5+ private header layout.
// Arduino-ESP32 2.x (PlatformIO espressif32@6) exposes the legacy
// driver/periph_ctrl.h API instead.
// =============================================================

#include "driver/periph_ctrl.h"
