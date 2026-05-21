// =============================================================
// duck_sprite.cpp — ABGR32 -> RGB565 conversion for Duck.c frames
//
// Duck.c lives at the repo root (not under src/), so it is pulled in
// via #include here. PlatformIO only compiles files inside src/ by
// default, which means the `static` array in Duck.c is only visible
// in this translation unit — no duplicate symbol risk.
//
// Byte layout of each 32-bit pixel in Duck.c (Piskel export):
//   bits 31..24 = A   (alpha, 0 = transparent, 0xFF = opaque)
//   bits 23..16 = B
//   bits 15..8  = G
//   bits  7..0  = R
//
// Alpha 0 pixels are stored as transparent (alpha=0) so the GUI can
// composite the duck over a coloured background ring.
// =============================================================

#include "duck_sprite.h"
#include <Arduino.h>

// Duck.c is in the project root, outside src/. Including it here makes
// its `static const uint32_t new_piskel_data[9][625]` visible only in
// this translation unit.
#include "../Duck.c"

static uint16_t s_rgb565[DUCK_FRAME_COUNT][DUCK_W * DUCK_H];
static uint8_t  s_alpha [DUCK_FRAME_COUNT][DUCK_W * DUCK_H];
static bool     s_initialised = false;

static inline uint16_t rgb_to_rgb565(uint8_t r, uint8_t g, uint8_t b) {
  return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

void duck_init() {
  if (s_initialised) return;
  uint32_t opaquePixels = 0;
  for (int f = 0; f < DUCK_FRAME_COUNT; f++) {
    for (int i = 0; i < DUCK_W * DUCK_H; i++) {
      uint32_t v = new_piskel_data[f][i];
      uint8_t a = (uint8_t)((v >> 24) & 0xFF);
      uint8_t b = (uint8_t)((v >> 16) & 0xFF);
      uint8_t g = (uint8_t)((v >> 8)  & 0xFF);
      uint8_t r = (uint8_t)( v        & 0xFF);
      if (a == 0) {
        s_rgb565[f][i] = 0x0000;
        s_alpha [f][i] = 0x00;
      } else {
        s_rgb565[f][i] = rgb_to_rgb565(r, g, b);
        s_alpha [f][i] = a;
        opaquePixels++;
      }
    }
  }
  s_initialised = true;
  Serial.printf("[Duck] Converted %d frames (%dx%d), %lu opaque px total\n",
                DUCK_FRAME_COUNT, DUCK_W, DUCK_H, (unsigned long)opaquePixels);
}

static inline int clampFrame(int idx) {
  if (idx < 0) return 0;
  if (idx >= DUCK_FRAME_COUNT) return DUCK_FRAME_COUNT - 1;
  return idx;
}

const uint16_t* duck_getFrameRGB565(int frameIdx) {
  if (!s_initialised) duck_init();
  return s_rgb565[clampFrame(frameIdx)];
}

const uint8_t* duck_getFrameAlpha(int frameIdx) {
  if (!s_initialised) duck_init();
  return s_alpha[clampFrame(frameIdx)];
}

int duck_pickFrameForPet(PetState state, int animFrame) {
  // 64-frame anim cycle (~3.2s at 50ms tick). Blink = last 4 frames.
  bool blink = ((animFrame % 64) >= 60);

  switch (state) {
    case PET_THRIVING:
    case PET_HAPPY:
      return blink ? DUCK_FRAME_CLOSED : DUCK_FRAME_OPEN;

    case PET_TIRED:
      // Mostly eyes-open with occasional closed beats
      return blink ? DUCK_FRAME_CLOSED : DUCK_FRAME_SAD;

    case PET_STRUGGLING:
      return blink ? DUCK_FRAME_LOW_CLOSED : DUCK_FRAME_LOW_OPEN;

    case PET_CRITICAL:
      // Flicker dead / low_sad to feel ghostly
      return ((animFrame % 16) < 8) ? DUCK_FRAME_DEAD : DUCK_FRAME_LOW_SAD;
  }
  return DUCK_FRAME_OPEN;
}

const char* duck_frameName(int frameIdx) {
  switch (clampFrame(frameIdx)) {
    case DUCK_FRAME_OPEN:        return "OPEN";
    case DUCK_FRAME_CLOSED:      return "CLOSED";
    case DUCK_FRAME_SAD:         return "SAD";
    case DUCK_FRAME_LOW_OPEN:    return "LOW_OPEN";
    case DUCK_FRAME_LOW_CLOSED:  return "LOW_CLOSED";
    case DUCK_FRAME_LOW_SAD:     return "LOW_SAD";
    case DUCK_FRAME_DEAD:        return "DEAD";
    case DUCK_FRAME_SLEEP_LAY:   return "SLEEP_LAYER";
    case DUCK_FRAME_HAPPY_LAY:   return "HAPPY_LAYER";
  }
  return "?";
}
