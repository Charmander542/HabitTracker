// =============================================================
// camera.cpp — Stub capture (SSCMA camera not exposed as esp_camera)
//
// Returns a small embedded baseline JPEG so the ritual can exercise
// decode + preview; replace with SSCMA JPEG buffer when integrated.
// =============================================================

#include "camera.h"
#include "sscma_camera.h"           // real SPI path to Himax AI chip
#include "stub_capture_jpeg.h"      // kept for the saved-to-LittleFS path
#include "stub_capture_bitmap.h"    // legacy fallback bitmap (unused now)
#include "synth_capture.h"          // procedural per-capture scene

static HabitCamBuffer s_stubFrame;
static HabitCamBuffer s_realFrame;
// Persistent buffer for the most recent real JPEG. Reused every capture
// so we don't thrash PSRAM (the Himax will typically hand back 5-20 KiB).
static uint8_t* s_realJpeg   = nullptr;
static size_t   s_realJpegCap = 0;
// 640x480 JPEGs from the Himax are often 40-120 KiB; leave headroom.
#define REAL_JPEG_CAP_BYTES  (256 * 1024)

bool Camera::begin() {
  _ready = true;
  Serial.printf("[Camera] Stub: embedded RGB565 scene %dx%d (%u bytes) "
                "+ JPEG mirror (%u bytes)\n",
                STUB_CAPTURE_BMP_W, STUB_CAPTURE_BMP_H,
                (unsigned)STUB_CAPTURE_BMP_LEN,
                (unsigned)STUB_CAPTURE_JPEG_LEN);
  return true;
}

void Camera::end() { _ready = false; }

HabitCamBuffer* Camera::capture() {
  if (!_ready) return nullptr;

  // ===================================================================
  // Capture path priority:
  //   1. Real Himax JPEG  (sscma.isAlive() — only if SSCMA firmware is
  //      actually running on the AI chip, which we verify at boot)
  //   2. Procedural scene (synth_capture_generate) — fresh frame every
  //      time, with on-screen "SIM" badge so the user knows
  //   3. Last-ditch: legacy embedded RGB565 bitmap
  // Every step prints a serial banner so the host can verify what's
  // happening end-to-end.
  // ===================================================================

  Serial.println(F("\n[Camera] ============================================="));
  Serial.printf ( "[Camera] capture #%lu requested at t=%lu ms\n",
                  (unsigned long)(synth_capture_index() + 1), (unsigned long)millis());

  // --- (1) Real camera path (preferred when the Himax is responding) -
  if (sscma.isAlive()) {
    if (!s_realJpeg) {
      s_realJpegCap = REAL_JPEG_CAP_BYTES;
      s_realJpeg    = (uint8_t*)ps_malloc(s_realJpegCap);
      if (!s_realJpeg) s_realJpeg = (uint8_t*)malloc(s_realJpegCap);
      if (!s_realJpeg) {
        Serial.println(F("[Camera] ps_malloc failed, falling back to stub"));
      }
    }
    if (s_realJpeg) {
      size_t len = 0;
      Serial.println(F("[Camera] (1) SSCMA path: requesting REAL frame..."));
      uint32_t t0 = millis();
      bool ok = sscma.captureJpeg(s_realJpeg, s_realJpegCap, &len,
                                  /*timeoutMs=*/6000, /*debug=*/false);
      uint32_t dt = millis() - t0;
      if (!ok || len == 0) {
        // Keep UI capture responsive: quick fallback path without re-init.
        Serial.println(F("[Camera] SAMPLE path missed; trying INVOKE fallback..."));
        t0 = millis();
        ok = sscma.captureJpegInvoke(s_realJpeg, s_realJpegCap, &len,
                                     /*times=*/-1, /*filter=*/false,
                                     /*show=*/true, /*timeoutMs=*/4000,
                                     /*debug=*/false);
        dt = millis() - t0;
      }
      if (!ok || len == 0) {
        Serial.printf("[Camera] SSCMA capture missed after %u ms\n", (unsigned)dt);
      }
      if (ok && len > 0) {
        Serial.printf("[Camera] >>> REAL JPEG captured: %u bytes in %u ms "
                      "[%02X %02X %02X ...]\n",
                      (unsigned)len, (unsigned)dt,
                      s_realJpeg[0], s_realJpeg[1], s_realJpeg[2]);
        Serial.println(F("[Camera] ============================================="));
        s_realFrame.data   = s_realJpeg;
        s_realFrame.len    = len;
        s_realFrame.bmp565 = nullptr;     // force JPEG path in GUI
        s_realFrame.bmpW   = 0;
        s_realFrame.bmpH   = 0;
        s_realFrame.isStub = false;
        return &s_realFrame;
      }
      Serial.printf("[Camera] SSCMA capture FAILED after %u ms — "
                    "falling through to procedural scene\n", (unsigned)dt);
    }
  } else {
    Serial.println(F("[Camera] (1) SSCMA not alive — Himax appears to be "
                     "running non-SSCMA firmware (run `camcold` to confirm)"));
  }

  // --- (2) Procedural scene: generated FRESH each capture ------------
  Serial.println(F("[Camera] (2) generating procedural scene..."));
  uint32_t gt0 = millis();
  uint32_t seed = (uint32_t)millis() ^ ((uint32_t)esp_random());
  const uint16_t* synth = synth_capture_generate(seed);
  uint32_t gdt = millis() - gt0;
  if (synth) {
    Serial.printf("[Camera] >>> SYNTH frame ready: %dx%d, %u bytes, "
                  "seed=0x%08lX, build=%u ms (frame #%lu)\n",
                  SYNTH_CAPTURE_W, SYNTH_CAPTURE_H,
                  (unsigned)(SYNTH_CAPTURE_W * SYNTH_CAPTURE_H * 2),
                  (unsigned long)seed, (unsigned)gdt,
                  (unsigned long)synth_capture_index());
    Serial.println(F("[Camera] (this scene is unique to this capture; "
                     "the on-device 'SIM' badge confirms it is procedural)"));
    Serial.println(F("[Camera] ============================================="));
    s_stubFrame.data   = (uint8_t*)STUB_CAPTURE_JPEG;
    s_stubFrame.len    = STUB_CAPTURE_JPEG_LEN;
    s_stubFrame.bmp565 = synth;
    s_stubFrame.bmpW   = SYNTH_CAPTURE_W;
    s_stubFrame.bmpH   = SYNTH_CAPTURE_H;
    s_stubFrame.isStub = true;
    return &s_stubFrame;
  }

  // --- (3) Legacy fallback: pre-baked RGB565 ------------------------
  Serial.println(F("[Camera] (3) synth allocator failed, using baked "
                   "stub_capture_bitmap.h"));
  Serial.println(F("[Camera] ============================================="));
  s_stubFrame.data   = (uint8_t*)STUB_CAPTURE_JPEG;
  s_stubFrame.len    = STUB_CAPTURE_JPEG_LEN;
  s_stubFrame.bmp565 = STUB_CAPTURE_BMP;
  s_stubFrame.bmpW   = STUB_CAPTURE_BMP_W;
  s_stubFrame.bmpH   = STUB_CAPTURE_BMP_H;
  s_stubFrame.isStub = true;
  return &s_stubFrame;
}

void Camera::returnFrame(HabitCamBuffer* /*fb*/) {}

void Camera::applyRGB565Tint(uint16_t* pixels, size_t pixelCount,
                               uint16_t tintColor, uint8_t strength) {
  uint8_t tr = ((tintColor >> 11) & 0x1F) << 3;
  uint8_t tg = ((tintColor >>  5) & 0x3F) << 2;
  uint8_t tb = ( tintColor        & 0x1F) << 3;
  uint16_t inv = 255 - strength;

  for (size_t i = 0; i < pixelCount; i++) {
    uint16_t px = pixels[i];
    uint16_t swapped = (px >> 8) | (px << 8);
    uint8_t r = ((swapped >> 11) & 0x1F) << 3;
    uint8_t g = ((swapped >>  5) & 0x3F) << 2;
    uint8_t b = ( swapped        & 0x1F) << 3;
    uint8_t nr = (uint8_t)((r * inv + tr * strength) / 255);
    uint8_t ng = (uint8_t)((g * inv + tg * strength) / 255);
    uint8_t nb = (uint8_t)((b * inv + tb * strength) / 255);
    uint16_t result = ((nr >> 3) << 11) | ((ng >> 2) << 5) | (nb >> 3);
    pixels[i] = (result >> 8) | (result << 8);
  }
}

bool Camera::isReady() const { return _ready; }
