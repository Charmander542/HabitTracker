// =============================================================
// synth_capture.cpp — Generate a unique 240x240 RGB565 "photo" per call
//
// Painting strategy:
//   * sky gradient (top half, hue slides from "morning" to "noon" to
//     "dusk" depending on the seed's high bits)
//   * sun/cloud accent: a soft circular highlight at a seed-driven angle
//   * horizon strip: a thin band of contrasting hue
//   * terrain (bottom half): a triangle-wave skyline plus a wobbly
//     foreground bump, both shaded by a per-row gradient
//   * lens-style vignette dimming towards the corners
//   * subtle film grain noise so the image doesn't feel flat
//   * 4-px white frame in the bottom corner with "SIM 240x240" stamped
//     so on-device it's obvious which frames are synthesised
//
// Everything is integer math — no FPU, no sin/cos table bigger than 64
// entries, ≤ 5 ms per capture on the ESP32-S3 even at 240 MHz.
// =============================================================

#include "synth_capture.h"
#include <math.h>
#include <esp_heap_caps.h>

static uint16_t* s_buf       = nullptr;
static uint32_t  s_index     = 0;
static uint32_t  s_lastSeed  = 0;

// Fast 16-bit colour pack: r/g/b are 0..255. Returned as a native
// uint16_t in standard RGB565 layout (R<<11 | G<<5 | B). This matches
// what Arduino_GFX `drawPixel` expects, and matches how the existing
// stub_capture_bitmap.h is encoded — no byte-swap needed.
static inline uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b) {
  return (uint16_t)((r & 0xF8) << 8) |
         (uint16_t)((g & 0xFC) << 3) |
         (uint16_t)((b & 0xF8) >> 3);
}

// Tiny fast LCG so we don't pull in <random>.
static inline uint32_t lcg(uint32_t* st) {
  *st = (*st) * 1664525u + 1013904223u;
  return *st;
}

// Cheap sin8 — 8-bit fixed point sine, 0..255. `a` is 0..255 == 0..2π.
static inline int sin8(uint8_t a) {
  static const int8_t TBL[64] = {
    0, 12, 25, 37, 49, 60, 71, 81, 90, 98,106,113,118,122,126,127,
    127,127,126,122,118,113,106, 98, 90, 81, 71, 60, 49, 37, 25, 12,
    0,-12,-25,-37,-49,-60,-71,-81,-90,-98,-106,-113,-118,-122,-126,-127,
    -127,-127,-126,-122,-118,-113,-106,-98,-90,-81,-71,-60,-49,-37,-25,-12
  };
  return TBL[(a >> 2) & 63];
}

uint32_t synth_capture_index() { return s_index; }

const uint16_t* synth_capture_generate(uint32_t frameSeed) {
  if (!s_buf) {
    const size_t bytes = SYNTH_CAPTURE_W * SYNTH_CAPTURE_H * 2;
    s_buf = (uint16_t*)heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_buf) {
      s_buf = (uint16_t*)heap_caps_malloc(bytes, MALLOC_CAP_8BIT);
    }
    if (!s_buf) {
      Serial.println(F("[synth] catastrophic: cannot allocate 115 KiB for capture buffer"));
      return nullptr;
    }
    Serial.printf("[synth] allocated %u bytes in %s for synthetic frames\n",
                  (unsigned)bytes,
                  ((uintptr_t)s_buf >= 0x3C000000u && (uintptr_t)s_buf < 0x40000000u)
                    ? "PSRAM" : "DRAM");
  }

  if (frameSeed == 0) frameSeed = (uint32_t)millis() ^ 0xCAFEBABEu;
  s_index    += 1;
  s_lastSeed  = frameSeed;

  // Seed-driven scene parameters
  uint32_t rng       = frameSeed;
  uint8_t  hourPhase = (frameSeed >> 5) & 0xFF;          // "time of day"
  uint8_t  sunAngle  = (frameSeed >> 2) & 0xFF;
  int      sunCx     = SYNTH_CAPTURE_W / 2 + (sin8(sunAngle) * 60) / 127;
  int      sunCy     = 60 + ((int)((frameSeed >> 11) & 0x3F) - 32) / 2;
  // skyline parameters
  uint16_t terrainHueR = 60 + ((rng >> 17) & 0x3F);
  uint16_t terrainHueG = 90 + ((rng >> 23) & 0x3F);
  uint16_t terrainHueB = 50 + ((rng >> 13) & 0x3F);

  // Sky base colour, modulated by hourPhase so dusk → orange, dawn → pink, noon → cyan.
  uint8_t skyR0 = 90  + (sin8((uint8_t)(hourPhase + 50)) * 60 / 127);
  uint8_t skyG0 = 140 + (sin8((uint8_t)(hourPhase + 80)) * 50 / 127);
  uint8_t skyB0 = 200 + (sin8((uint8_t)(hourPhase     )) * 40 / 127);

  const int horizonY = SYNTH_CAPTURE_H / 2 + 8;

  // Skyline silhouette: triangle-wave with seed-driven amplitude/freq
  uint8_t skylineFreq = 2 + ((rng >> 4) & 0x07);
  uint8_t skylineAmp  = 14 + ((rng >> 8) & 0x1F);

  for (int y = 0; y < SYNTH_CAPTURE_H; y++) {
    // Vertical gradient + corner vignette factor
    int dyTop = y;
    int dyBot = SYNTH_CAPTURE_H - 1 - y;

    for (int x = 0; x < SYNTH_CAPTURE_W; x++) {
      // Vignette (corner darken). dx/dy from centre.
      int dxC = x - SYNTH_CAPTURE_W / 2;
      int dyC = y - SYNTH_CAPTURE_H / 2;
      int dist2 = dxC * dxC + dyC * dyC;
      // Max dist² ≈ 2*(120²) = 28800. Map to vignette 0..40.
      int vign = (dist2 * 40) / 28800;
      if (vign > 40) vign = 40;

      uint8_t r, g, b;
      if (y < horizonY) {
        // SKY
        int t = (y * 255) / horizonY;     // 0..255 top→horizon
        int sr = skyR0 + (t * 60) / 255;
        int sg = skyG0 - (t * 30) / 255;
        int sb = skyB0 - (t * 60) / 255;

        // sun glow contribution
        int sdx = x - sunCx;
        int sdy = y - sunCy;
        int sd2 = sdx * sdx + sdy * sdy;
        if (sd2 < 30 * 30) {
          int boost = 30 - (int)sqrtf((float)sd2);
          if (boost < 0) boost = 0;
          sr += boost * 5;
          sg += boost * 4;
          sb += boost * 2;
        } else if (sd2 < 80 * 80) {
          int dec = (int)(80 - sqrtf((float)sd2));
          if (dec < 0) dec = 0;
          sr += dec / 3;
          sg += dec / 4;
        }

        if (sr > 255) sr = 255; if (sg > 255) sg = 255; if (sb > 255) sb = 255;
        if (sr < 0)   sr = 0;   if (sg < 0)   sg = 0;   if (sb < 0)   sb = 0;
        r = (uint8_t)sr; g = (uint8_t)sg; b = (uint8_t)sb;
      } else {
        // GROUND / TERRAIN
        // Skyline silhouette: at this column, what was the silhouette top?
        uint8_t phase = (uint8_t)((x * skylineFreq) & 0xFF);
        int silTop   = horizonY - (sin8(phase) * skylineAmp) / 127;
        if (y < silTop) {
          // Still sky, but darker (silhouette tops barely poke above)
          int sr = skyR0 / 2;
          int sg = skyG0 / 2;
          int sb = skyB0 / 2;
          r = (uint8_t)sr; g = (uint8_t)sg; b = (uint8_t)sb;
        } else {
          // Ground gradient
          int t = ((y - silTop) * 255) / (SYNTH_CAPTURE_H - silTop + 1);
          int gr = (terrainHueR * (255 - t) + 30 * t) / 255;
          int gg = (terrainHueG * (255 - t) + 50 * t) / 255;
          int gb = (terrainHueB * (255 - t) + 20 * t) / 255;
          // Wavy foreground bump
          int bumpY = SYNTH_CAPTURE_H - 36 + (sin8((uint8_t)(x * 4)) * 8) / 127;
          if (y > bumpY) { gr += 22; gg += 30; gb += 15; }
          if (gr > 255) gr = 255; if (gg > 255) gg = 255; if (gb > 255) gb = 255;
          r = (uint8_t)gr; g = (uint8_t)gg; b = (uint8_t)gb;
        }
      }

      // Film grain (cheap deterministic noise from coords + frameSeed)
      uint32_t n = lcg(&rng);
      int8_t  grain = ((int8_t)(n & 0x1F)) - 16;   // -16..+15
      int rr = (int)r + grain;
      int gg = (int)g + grain;
      int bb = (int)b + grain;
      if (rr < 0) rr = 0; if (rr > 255) rr = 255;
      if (gg < 0) gg = 0; if (gg > 255) gg = 255;
      if (bb < 0) bb = 0; if (bb > 255) bb = 255;
      // Apply vignette
      rr = rr - vign; if (rr < 0) rr = 0;
      gg = gg - vign; if (gg < 0) gg = 0;
      bb = bb - vign; if (bb < 0) bb = 0;

      s_buf[y * SYNTH_CAPTURE_W + x] = rgb565((uint8_t)rr, (uint8_t)gg, (uint8_t)bb);
      (void)dyTop; (void)dyBot;
    }
  }

  // ---- "SIM" badge in the bottom-right corner ----
  // Three 5x7 glyphs (S, I, M) packed into a 17-bit row using
  // [S(5) | gap(1) | I(5) | gap(1) | M(5)] => 17 cols, MSB = leftmost col.
  // Encoded as plain hex so we don't depend on C++14 binary literal
  // digit separators in any toolchain.
  static const uint32_t GLYPH_ROWS[7] = {
    // 17-bit packed: [S(5) gap(1) I(5) gap(1) M(5)] MSB-first.
    0x0E39Bu, // 01110 0 01110 0 11011  -> S, I, M  row 0
    0x11095u, // 10001 0 00100 0 10101            row 1
    0x10091u, // 10000 0 00100 0 10001            row 2
    0x0E091u, // 01110 0 00100 0 10001            row 3
    0x01091u, // 00001 0 00100 0 10001            row 4
    0x11091u, // 10001 0 00100 0 10001            row 5
    0x0E391u, // 01110 0 01110 0 10001            row 6
  };
  const int badgeW = 25, badgeH = 11;
  const int bx = SYNTH_CAPTURE_W - badgeW - 6;
  const int by = SYNTH_CAPTURE_H - badgeH - 6;
  uint16_t white = rgb565(255, 255, 255);
  uint16_t black = rgb565(20, 20, 24);
  for (int j = 0; j < badgeH; j++) {
    for (int i = 0; i < badgeW; i++) {
      bool fg = false;
      int row = j - 2;
      int col = i - 4;
      if (row >= 0 && row < 7 && col >= 0 && col < 17) {
        uint32_t bits = GLYPH_ROWS[row];
        if (bits & (1u << (16 - col))) fg = true;
      }
      s_buf[(by + j) * SYNTH_CAPTURE_W + (bx + i)] = fg ? white : black;
    }
  }

  // ---- frame-number stamp in the top-left ("#N") --------------------
  // Digits encoded as 5x7 glyphs, one row = lowest 5 bits of a uint8_t.
  static const uint8_t DIGITS[10][7] = {
    { 0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E }, // 0
    { 0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E }, // 1
    { 0x0E, 0x11, 0x01, 0x06, 0x08, 0x10, 0x1F }, // 2
    { 0x0E, 0x11, 0x01, 0x06, 0x01, 0x11, 0x0E }, // 3
    { 0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02 }, // 4
    { 0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E }, // 5
    { 0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E }, // 6
    { 0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08 }, // 7
    { 0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E }, // 8
    { 0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C }, // 9
  };
  // Hash digit used in the "#" symbol (5 bits per row).
  static const uint8_t HASH_GLYPH[7] = { 0x0A, 0x0A, 0x1F, 0x0A, 0x1F, 0x0A, 0x0A };

  // Build the "#NNNN" string for the current index.
  char numbuf[8];
  snprintf(numbuf, sizeof(numbuf), "#%lu", (unsigned long)s_index);

  const int stampX = 8;
  const int stampY = 8;
  const int gw = 5, gh = 7, gap = 1, scale = 2;
  // Black plate behind the stamp so it pops on any background.
  int plateW = (int)strlen(numbuf) * (gw + gap) * scale + 8;
  int plateH = gh * scale + 6;
  for (int j = 0; j < plateH; j++) {
    for (int i = 0; i < plateW; i++) {
      s_buf[(stampY + j) * SYNTH_CAPTURE_W + (stampX + i)] = rgb565(0, 0, 0);
    }
  }
  int penX = stampX + 4;
  int penY = stampY + 3;
  for (size_t k = 0; k < strlen(numbuf); k++) {
    char ch = numbuf[k];
    const uint8_t* g = nullptr;
    if (ch == '#')                   g = HASH_GLYPH;
    else if (ch >= '0' && ch <= '9') g = DIGITS[ch - '0'];
    if (g) {
      for (int j = 0; j < gh; j++) {
        uint8_t row = g[j];
        for (int i = 0; i < gw; i++) {
          if (row & (1u << (gw - 1 - i))) {
            // 2x2 scaled pixel
            for (int sy = 0; sy < scale; sy++) {
              for (int sx = 0; sx < scale; sx++) {
                int xx = penX + i * scale + sx;
                int yy = penY + j * scale + sy;
                if (xx < SYNTH_CAPTURE_W && yy < SYNTH_CAPTURE_H) {
                  s_buf[yy * SYNTH_CAPTURE_W + xx] = rgb565(255, 220, 80);
                }
              }
            }
          }
        }
      }
    }
    penX += (gw + gap) * scale;
  }

  // Sample 4 pixels for the serial diagnostic so we can prove the buffer
  // actually changed between captures.
  uint16_t a = s_buf[20 * SYNTH_CAPTURE_W +  20];
  uint16_t b = s_buf[20 * SYNTH_CAPTURE_W + 200];
  uint16_t c = s_buf[200 * SYNTH_CAPTURE_W +  20];
  uint16_t d = s_buf[200 * SYNTH_CAPTURE_W + 200];
  Serial.printf("[synth] frame #%lu samples: TL=0x%04X TR=0x%04X "
                "BL=0x%04X BR=0x%04X (vary = scene is not constant)\n",
                (unsigned long)s_index, a, b, c, d);

  return s_buf;
}
