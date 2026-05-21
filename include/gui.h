#pragma once
// =============================================================
// gui.h — SPD2010 412×412 display (SenseCAP Watcher)
//
// Panel: Solomon SPD2010 over ESP32 QSPI (SPI3_HOST), matching Seeed BSP.
// Double-buffered via Arduino_Canvas → Arduino_SPD2010 (see Arduino_GFX
// WAVESHARE_ESP32_S3_LCD_1_46 pattern in Arduino_GFX_dev_device.h).
// Ref: SenseCAP-Watcher-Firmware sensecap-watcher.c bsp_lcd_pannel_init
// =============================================================

#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include "Arduino_SPD2010.h"
#include "config.h"
#include "pet.h"
#include "habits.h"
#include "camera.h"

class GUI {
public:
  void begin();

  void drawIdle(const Pet& pet, HabitManager& habits);
  void drawHabitSelect(HabitManager& habits, int selectedIndex);
  void drawHabitDetail(const Habit& habit);
  void drawCaptureRitual(int countdown, HabitCamBuffer* fb);
  void drawCelebration(int vitalityGain, unsigned long elapsed);

  // Sleep / wake transition screens for the long-press deep-sleep gesture.
  // drawSleepScreen() shows a moon + "Sleeping" hint right before the
  // backlight is faded to zero. drawWakeScreen() is the inverse — used
  // immediately after a wake to give a 250 ms "Hello!" flash.
  void drawSleepScreen();
  void drawWakeScreen();

  void drawPetFace(int cx, int cy, PetState state, int animFrame);
  void drawDialogueBubble(const String& message);
  void setBrightness(uint8_t brightness0to255);

  // ---- Test-harness helpers (driven by the serial command parser) ----
  // Fill the whole display with a solid colour and flush. Used by `disp`.
  void fillTestColor(uint16_t color);
  // Paint 4 horizontal bars (R/G/B/W) then flush. Used by `disp bars`.
  void fillTestBars();
  // Draw one Duck frame scaled up, centred, over a black screen.
  void drawDuckFrameCentred(int frameIdx, int scale = 8);
  // Direct access to the canvas so tests can report dimensions, etc.
  bool isCanvasReady() const { return _canvas != nullptr; }
  void flushNow();

  // Draw a duck bitmap at arbitrary position/scale onto the canvas.
  // Does NOT fillScreen first — caller can composite over any background.
  // scale=1 renders at native 25x25; scale=8 ≈ 200x200 (fits 412x412).
  void drawDuck(int cx, int cy, int frameIdx, int scale = 8);

private:
  Arduino_ESP32QSPI* _qspi  = nullptr;
  Arduino_SPD2010*   _panel = nullptr;
  Arduino_Canvas*    _canvas = nullptr;

  void _drawVitalityRing(int vitality);
  void _drawProgressRing(int cx, int cy, int radius, float pct, uint16_t color);
  void _drawProgressBar(int x, int y, int w, int h,
                        float pct, uint16_t fillColor, uint16_t bgColor);
  void _drawHabitCard(int cx, int cy, int radius, const Habit& h, bool selected);
  void _drawArcPixels(int cx, int cy, int r, int thickness,
                      float startDeg, float endDeg, uint16_t color);
  void _drawSmile(int cx, int cy, int radius, uint16_t color, int thickness);
  void _drawFrown(int cx, int cy, int radius, uint16_t color, int thickness);
  void _drawCheckmark(int cx, int cy, int size, uint16_t color);
  // Procedural polaroid-style preview shown as a last-ditch fallback.
  // Only hit now if a real camera frame comes in and fails to decode.
  void _drawPolaroidPlaceholder();
  // Blit a raw RGB565 bitmap (from flash or RAM) centred on the canvas
  // inside a polaroid-style white frame. `stub` adds a subtle "placeholder"
  // badge in the corner so we know at a glance this wasn't a real capture.
  void _drawBitmap565Centred(const uint16_t* px, int w, int h, bool stub);
  void _drawFloatingVitalityText(int vitalityGain, unsigned long elapsed);
  uint16_t _vitalityToColor(int vitality);
  void _flush();
  void _drawCentredText(const String& text, int y, uint16_t color, uint8_t size);
  void _drawCard(int x, int y, int w, int h, int r, uint16_t color);
};
