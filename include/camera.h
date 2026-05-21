#pragma once
// =============================================================
// camera.h — Image capture stub for SenseCAP Watcher
//
// Official firmware routes vision through the SSCMA (Himax) module
// over SPI — there is no esp_camera / OV2640 bus wired directly
// to the ESP32-S3 in the BSP. This class keeps the ritual API
// (capture → preview → save). The stub returns a tiny embedded JPEG so
// the UI can decode and show a real preview until SSCMA is wired up.
//
// TODO: [Bridge to SSCMA JPEG output when an Arduino wrapper exists,
//        mirroring factory_firmware camera paths.]
// =============================================================

#include <Arduino.h>
#include "config.h"

// Minimal buffer descriptor (replaces esp32-camera's camera_fb_t).
//
// Two output formats are supported:
//   1. JPEG (future SSCMA-camera path): data != nullptr, len > 0,
//      isStub == false. GUI decodes via JPEGDEC.
//   2. Pre-decoded RGB565 bitmap (stub path): bmp565 != nullptr with
//      bmpW / bmpH set. GUI blits directly — no JPEG decode, no crashes,
//      and the image is a real photo-like scene shipped in flash.
//
// `isStub` is a hint so the GUI can badge the preview as placeholder
// content until the real camera is online.
struct HabitCamBuffer {
  uint8_t*        data   = nullptr;   // JPEG bytes (future camera path)
  size_t          len    = 0;
  const uint16_t* bmp565 = nullptr;   // pre-decoded RGB565 pixels (stub)
  int             bmpW   = 0;
  int             bmpH   = 0;
  bool            isStub = false;
};

class Camera {
public:
  bool begin();
  void end();
  HabitCamBuffer* capture();
  void returnFrame(HabitCamBuffer* fb);
  void applyRGB565Tint(uint16_t* pixels, size_t pixelCount,
                       uint16_t tintColor, uint8_t strength = 60);
  bool isReady() const;

private:
  bool _ready = false;
};
