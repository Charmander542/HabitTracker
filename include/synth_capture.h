#pragma once
// =============================================================
// synth_capture.h — Procedural "captured photo" generator
//
// The Himax SSCMA AI chip on this device does not respond to SSCMA
// commands (boot diagnostics over `cambus` / `camcold` confirm: chip is
// powered, RST line works, but no SSCMA firmware is running on it).
// Until that's resolved, we generate a fresh 240x240 RGB565 "photo" in
// PSRAM at every capture. Each frame:
//
//   * uses a different seed (driven by millis() at capture time) so
//     subsequent captures look genuinely distinct,
//   * paints a believable "scene" (sky band, sun, horizon, terrain, lens
//     vignette + film grain) so on-screen it reads as a snapshot,
//   * stamps the corner with the capture index + an UPPER-CASE "SIM"
//     badge so the user can always tell on-device whether the frame was
//     real or synthesised.
//
// The buffer is owned by the module — capture() hands back a pointer
// that stays valid until the next capture(). One-shot allocation so we
// don't thrash PSRAM.
// =============================================================

#include <Arduino.h>

// 240x240 is the canonical "photo" size used by the rest of the GUI
// (matches HabitCamBuffer::bmpW/H and the polaroid card layout).
#define SYNTH_CAPTURE_W   240
#define SYNTH_CAPTURE_H   240

// Generate a fresh "photo" into the module's PSRAM buffer. Returns a
// pointer to a 240x240 array of RGB565 pixels (big-endian in flash, but
// the GUI's _drawBitmap565Centred handles that). Returns nullptr only
// if PSRAM allocation has failed catastrophically.
//
// `frameSeed` is mixed into the procedural noise so the same seed always
// produces the same scene; pass 0 to mean "use current millis()".
const uint16_t* synth_capture_generate(uint32_t frameSeed = 0);

// Last-generated frame's serial number (1-based). Useful for printing
// "[capture frame N]" banners.
uint32_t synth_capture_index();
