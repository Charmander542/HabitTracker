// =============================================================
// gui.cpp — Display rendering for all app screens
//
// All drawing goes through the Arduino_Canvas off-screen buffer
// (backed by PSRAM) and is flushed to the SPD2010 panel as a single
// transfer to reduce tearing on the 412×412 round display.
//
// The pet face is drawn procedurally: no large sprite arrays.
// Expressions, animation offsets, and decorations are computed
// from the PetState enum and the animFrame counter.
//
// COORDINATE SYSTEM: (0,0) is top-left. Centre = DISPLAY_CENTER_* (206,206).
// =============================================================

#include <driver/spi_master.h>
#include <esp_heap_caps.h>
#include "gui.h"
#include "config.h"
#include "camera.h"
#include "duck_sprite.h"
#include <JPEGDEC.h>
#include <math.h>

namespace {

Arduino_GFX* g_jpegDrawTarget = nullptr;
bool g_jpegCoverFill = false;
int g_jpegSrcW = 0;
int g_jpegSrcH = 0;
float g_jpegScale = 1.0f;
float g_jpegOffX = 0.0f;
float g_jpegOffY = 0.0f;

// Per-pixel blit callback. Arduino_GFX's draw16bitRGBBitmap on Canvas caused
// a StoreProhibited crash on edge MCU tiles (where JPEGDEC returns
// iWidth=16 / iWidthUsed=8) because the canvas-buffer memcpy path didn't
// respect the row stride. Iterating pixel-by-pixel with the correct
// (iWidth) stride is slower but immune to that class of bug.
int jpegDrawCallback(JPEGDRAW* pDraw) {
  if (!g_jpegDrawTarget || !pDraw || !pDraw->pPixels) return 0;
  const uint16_t* src = pDraw->pPixels;
  const int sx0    = pDraw->x;
  const int sy0    = pDraw->y;
  const int w      = pDraw->iWidthUsed;   // valid pixels per row
  const int h      = pDraw->iHeight;
  const int stride = pDraw->iWidth;       // allocated pixels per row

  if (g_jpegCoverFill) {
    for (int row = 0; row < h; row++) {
      const uint16_t* rowSrc = src + row * stride;
      const int sy = sy0 + row;
      int dy0 = (int)floorf((float)sy * g_jpegScale + g_jpegOffY);
      int dy1 = (int)floorf((float)(sy + 1) * g_jpegScale + g_jpegOffY) - 1;
      if (dy1 < 0 || dy0 >= DISPLAY_HEIGHT) continue;
      if (dy0 < 0) dy0 = 0;
      if (dy1 >= DISPLAY_HEIGHT) dy1 = DISPLAY_HEIGHT - 1;
      for (int col = 0; col < w; col++) {
        const int sx = sx0 + col;
        int dx0 = (int)floorf((float)sx * g_jpegScale + g_jpegOffX);
        int dx1 = (int)floorf((float)(sx + 1) * g_jpegScale + g_jpegOffX) - 1;
        if (dx1 < 0 || dx0 >= DISPLAY_WIDTH) continue;
        if (dx0 < 0) dx0 = 0;
        if (dx1 >= DISPLAY_WIDTH) dx1 = DISPLAY_WIDTH - 1;
        uint16_t c = rowSrc[col];
        for (int yy = dy0; yy <= dy1; yy++) {
          for (int xx = dx0; xx <= dx1; xx++) {
            g_jpegDrawTarget->drawPixel(xx, yy, c);
          }
        }
      }
    }
    return 1;
  }

  for (int row = 0; row < h; row++) {
    const uint16_t* rowSrc = src + row * stride;
    for (int col = 0; col < w; col++) {
      g_jpegDrawTarget->drawPixel(sx0 + col, sy0 + row, rowSrc[col]);
    }
  }
  return 1;
}

bool looksLikeJpeg(const uint8_t* p, size_t len) {
  return len >= 4 && p[0] == 0xFF && p[1] == 0xD8 && p[2] == 0xFF;
}

// Simple pixel-art-ish icons for default habits.
void drawHabitIcon(Arduino_Canvas* c, int cx, int cy, int s,
                   const Habit& h, uint16_t fg, uint16_t bg) {
  if (!c) return;
  const String n = h.name;
  const bool isHydrate  = n.indexOf("Hydrate") >= 0 || h.emoji == "W";
  const bool isRead     = n.indexOf("Read") >= 0    || h.emoji == "R";
  const bool isMeditate = n.indexOf("Meditate") >= 0 || h.emoji == "M";
  const bool isMove     = n.indexOf("Move") >= 0    || h.emoji == "V";
  const bool isSleep    = n.indexOf("Sleep") >= 0   || h.emoji == "Z";

  c->fillCircle(cx, cy, s + 5, bg);
  c->drawCircle(cx, cy, s + 5, fg);

  if (isHydrate) {
    c->fillTriangle(cx, cy - s, cx - s + 2, cy + s - 2, cx + s - 2, cy + s - 2, fg);
    c->fillCircle(cx, cy - s / 2, s / 2, fg);
    c->fillCircle(cx - s / 3, cy - s / 3, 2, COLOR_WHITE);
  } else if (isRead) {
    c->fillRoundRect(cx - s, cy - s + 2, s * 2, s * 2 - 4, 3, fg);
    c->drawLine(cx, cy - s + 3, cx, cy + s - 3, bg);
    c->drawLine(cx - s + 4, cy - s / 2, cx - 2, cy - s / 2, bg);
    c->drawLine(cx + 2, cy - s / 2, cx + s - 4, cy - s / 2, bg);
  } else if (isMeditate) {
    c->fillCircle(cx, cy - s / 2, s / 3 + 1, fg);
    c->drawLine(cx, cy, cx - s, cy + s - 2, fg);
    c->drawLine(cx, cy, cx + s, cy + s - 2, fg);
    c->drawLine(cx - s / 2, cy + s / 3, cx + s / 2, cy + s / 3, fg);
  } else if (isMove) {
    c->fillRoundRect(cx - s, cy - s / 2, s * 2, s, 4, fg);
    c->fillRect(cx + s / 3, cy - s / 2 - 3, s / 2, 3, fg);
    c->drawLine(cx - s + 2, cy + s / 3, cx + s - 2, cy + s / 3, bg);
  } else if (isSleep) {
    c->fillCircle(cx - 2, cy, s - 1, fg);
    c->fillCircle(cx + s / 3 + 1, cy - 1, s - 2, bg);
    c->setTextColor(fg);
    c->setTextSize(1);
    c->setCursor(cx + s - 2, cy - s);
    c->print("z");
  } else {
    c->setTextColor(fg);
    c->setTextSize(2);
    c->setCursor(cx - 6, cy - 8);
    c->print(h.emoji.length() > 0 ? h.emoji[0] : '?');
  }
}

// Decode JPEG into `gfx`, centred and scaled to fit the square display.
// Verbose on purpose — if the capture preview is blank, the serial log tells
// us exactly which step failed (looksLikeJpeg / openRAM / dimensions / decode).
bool decodeJpegCentred(Arduino_GFX* gfx, const uint8_t* data, size_t len) {
  if (!gfx) {
    Serial.println(F("[JPEG] decodeJpegCentred: gfx is null"));
    return false;
  }
  if (!data || len < 4) {
    Serial.printf("[JPEG] bad buffer (data=%p len=%u)\n", (const void*)data, (unsigned)len);
    return false;
  }
  if (!looksLikeJpeg(data, len)) {
    Serial.printf("[JPEG] not a JPEG — first 4 bytes: %02X %02X %02X %02X\n",
                  data[0], data[1], data[2], data[3]);
    return false;
  }
  Serial.printf("[JPEG] decoding %u bytes — magic %02X %02X %02X %02X OK\n",
                (unsigned)len, data[0], data[1], data[2], data[3]);

  // IMPORTANT: JPEGDEC crashes (StoreProhibited on a bogus internal pointer)
  // when fed a flash-mapped (PROGMEM) pointer on ESP32-S3. Copying the data
  // into a heap RAM buffer before calling openRAM makes the decode stable.
  // ~3.5 KB alloc is cheap vs ~60 KB of internal decoder state already used.
  uint8_t* ramBuf = (uint8_t*)heap_caps_malloc(len, MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
  if (!ramBuf) {
    // Fall back to SPIRAM if internal heap is short
    ramBuf = (uint8_t*)heap_caps_malloc(len, MALLOC_CAP_8BIT);
  }
  if (!ramBuf) {
    Serial.println(F("[JPEG] unable to allocate RAM shadow for decoder"));
    return false;
  }
  memcpy(ramBuf, data, len);
  Serial.printf("[JPEG] copied to RAM @ %p (free heap=%u)\n",
                (void*)ramBuf, (unsigned)ESP.getFreeHeap());

  static JPEGDEC jpeg;
  g_jpegDrawTarget = gfx;
  jpeg.close();

  if (jpeg.openRAM(ramBuf, (int)len, jpegDrawCallback) != 1) {
    Serial.println(F("[JPEG] openRAM failed"));
    g_jpegDrawTarget = nullptr;
    free(ramBuf);
    return false;
  }

  jpeg.setPixelType(RGB565_LITTLE_ENDIAN);

  const int iw = jpeg.getWidth();
  const int ih = jpeg.getHeight();
  Serial.printf("[JPEG] image %dx%d  (display %dx%d)\n",
                iw, ih, DISPLAY_WIDTH, DISPLAY_HEIGHT);
  if (iw <= 0 || ih <= 0) {
    Serial.println(F("[JPEG] invalid dimensions"));
    jpeg.close();
    g_jpegDrawTarget = nullptr;
    free(ramBuf);
    return false;
  }

  // "Cover" mode: prefer filling the screen even if that crops edges.
  // JPEGDEC only supports 1x, 1/2, 1/4, 1/8 scales, so we pick the first
  // scale where BOTH dimensions are >= display size.
  const int opts[4]   = {0, JPEG_SCALE_HALF,
                         JPEG_SCALE_QUARTER, JPEG_SCALE_EIGHTH};
  const int divis[4]  = {1, 2, 4, 8};
  int opt = 0;
  int scaledW = iw;
  int scaledH = ih;
  bool covers = false;
  for (int i = 0; i < 4; i++) {
    int w = (iw + divis[i] - 1) / divis[i];
    int h = (ih + divis[i] - 1) / divis[i];
    if (w >= DISPLAY_WIDTH && h >= DISPLAY_HEIGHT) {
      opt = opts[i];
      scaledW = w;
      scaledH = h;
      covers = true;
      break;
    }
  }
  if (!covers) {
    // Can't fully cover (e.g. tiny source like 240x240); keep max detail.
    opt = 0;
    scaledW = iw;
    scaledH = ih;
  }

  // True cover-to-display transform, including UPSCALE when source is smaller.
  // This guarantees the full 412x412 square is filled; round mask clips edges.
  const float sx = (float)DISPLAY_WIDTH  / (float)scaledW;
  const float sy = (float)DISPLAY_HEIGHT / (float)scaledH;
  const float coverScale = (sx > sy) ? sx : sy;
  const float ox = ((float)DISPLAY_WIDTH  - (float)scaledW * coverScale) * 0.5f;
  const float oy = ((float)DISPLAY_HEIGHT - (float)scaledH * coverScale) * 0.5f;
  g_jpegCoverFill = true;
  g_jpegSrcW = scaledW;
  g_jpegSrcH = scaledH;
  g_jpegScale = coverScale;
  g_jpegOffX = ox;
  g_jpegOffY = oy;
  Serial.printf("[JPEG] cover-fill scale-opt=%d src=%dx%d scale=%.3f off=(%.1f,%.1f)\n",
                opt, scaledW, scaledH, coverScale, ox, oy);

  gfx->fillScreen(COLOR_BLACK);
  const int ok = jpeg.decode(0, 0, opt);
  Serial.printf("[JPEG] decode() returned %d\n", ok);
  jpeg.close();
  g_jpegDrawTarget = nullptr;
  g_jpegCoverFill = false;
  free(ramBuf);
  return ok == 1;
}

}  // namespace

// ---------------------------------------------------------------
// begin — SPD2010 + QSPI (SPI3), same topology as Seeed BSP / Arduino_GFX demo
// ---------------------------------------------------------------
void GUI::begin() {
  // QSPI bus: CS, SCK, D0..D3 — matches BSP_LCD_* + BSP_SPI3_HOST_DATA*
  _qspi = new Arduino_ESP32QSPI(
      PIN_LCD_QSPI_CS, PIN_LCD_QSPI_SCK,
      PIN_LCD_QSPI_D0, PIN_LCD_QSPI_D1, PIN_LCD_QSPI_D2, PIN_LCD_QSPI_D3);

  _panel = new Arduino_SPD2010(_qspi, GFX_NOT_DEFINED /* RST */);

  _canvas = new Arduino_Canvas(DISPLAY_WIDTH, DISPLAY_HEIGHT, _panel, 0, 0, 0);

  // Panel must be inited once for QSPI. Canvas also calls output->begin() by
  // default — that would run Arduino_ESP32QSPI::begin twice and hit
  // ESP_ERR_INVALID_STATE on spi_bus_initialize. Skip the second pass.
  _panel->begin();
  _canvas->begin(GFX_SKIP_OUTPUT_BEGIN);

  // Backlight PWM on GPIO8 — 10-bit duty @ 5 kHz (matches sensecap-watcher.c)
  ledcSetup(BL_LEDC_CHANNEL, BL_LEDC_FREQ_HZ, BL_LEDC_RES_BITS);
  ledcAttachPin(PIN_LCD_BL, BL_LEDC_CHANNEL);
  setBrightness((uint8_t)((uint32_t)BL_DEFAULT_PERCENT * 255u / 100u));

  _canvas->fillScreen(COLOR_BLACK);
  _flush();
  Serial.println("[GUI] SPD2010 412x412 (QSPI SPI3) ready");
}

// ---------------------------------------------------------------
// setBrightness — map 0–255 to 10-bit LEDC duty (same range as BSP helper)
// ---------------------------------------------------------------
void GUI::setBrightness(uint8_t brightness0to255) {
  uint32_t maxDuty = (1u << BL_LEDC_RES_BITS) - 1u;
  uint32_t duty    = maxDuty * (uint32_t)brightness0to255 / 255u;
  ledcWrite(BL_LEDC_CHANNEL, duty);
}

// ================================================================
//  IDLE SCREEN
//  Pet face centered. Vitality ring around edge. Time + streak.
// ================================================================
void GUI::drawIdle(const Pet& pet, HabitManager& habits) {
  _canvas->fillScreen(COLOR_BLACK);

  // --- Background gradient: subtle radial dark-to-deeper-dark ---
  // Keep it simple with a single dark fill; the ring provides the color accent.

  // --- Vitality ring (drawn first so pet renders on top) ---
  _drawVitalityRing(pet.getVitality());

  // --- Pet face at center ---
  // Apply a gentle bounce offset for THRIVING or a shake for STRUGGLING
  int bounceY = 0;
  int bounceX = 0;
  PetState state = pet.getState();
  int frame      = pet.getAnimFrame();

  if (state == PET_THRIVING) {
    bounceY = (int)(sinf(frame * 0.196f) * 5.0f);  // 0.196 = 2π/32
  } else if (state == PET_STRUGGLING) {
    bounceX = (frame % 6 < 3) ? -3 : 3;
  } else if (state == PET_CRITICAL) {
    bounceX = (frame % 4 < 2) ? -2 : 2;
    bounceY = (frame % 8 < 4) ? -1 : 1;
  }

  drawPetFace(DISPLAY_CENTER_X + bounceX, DISPLAY_CENTER_Y + bounceY,
              state, frame);

  // --- Time (top center) ---
  // We pull time from the global rtc in main.cpp via a passed string.
  // For now the caller injects it through the Pet's dialogue if needed.
  // Time display is handled separately when the RTC value is passed in.

  // --- Vitality percentage (bottom, small) ---
  char vBuf[16];
  snprintf(vBuf, sizeof(vBuf), "Vitality: %d%%", pet.getVitality());
  _drawCentredText(String(vBuf), DISPLAY_HEIGHT - 28, COLOR_MID_GREY, 1);

  // --- Best streak across all habits ---
  int bestStreak = 0;
  for (int i = 0; i < habits.getCount(); i++) {
    if (habits.getHabit(i).streak > bestStreak)
      bestStreak = habits.getHabit(i).streak;
  }
  if (bestStreak > 0) {
    char sBuf[20];
    snprintf(sBuf, sizeof(sBuf), "Best streak: %d", bestStreak);
    _drawCentredText(String(sBuf), DISPLAY_HEIGHT - 14, COLOR_LIGHT_GREY, 1);
  }

  _flush();
}

// ================================================================
//  HABIT SELECT SCREEN — carousel of circular habit cards
// ================================================================
void GUI::drawHabitSelect(HabitManager& habits, int selectedIndex) {
  _canvas->fillScreen(0x0821);   // very dark blue

  int count = habits.getCount();
  if (count == 0) {
    _drawCentredText("No habits yet!", DISPLAY_CENTER_Y, COLOR_WHITE, 2);
    _flush();
    return;
  }

  // Conservative safe circle for real round visible area.
  const int safeR = 170;
  _canvas->fillCircle(DISPLAY_CENTER_X, DISPLAY_CENTER_Y, safeR, 0x10A3);
  _canvas->drawCircle(DISPLAY_CENTER_X, DISPLAY_CENTER_Y, safeR, 0x31C7);

  _drawCentredText("Pick Habit", DISPLAY_CENTER_Y - 150, COLOR_WHITE, 2);

  const Habit& cur = habits.getHabit(selectedIndex);
  // Round-first design: circular focus card instead of square panel.
  const int orbY = DISPLAY_CENTER_Y + 4;
  _canvas->fillCircle(DISPLAY_CENTER_X, orbY, 104, 0x18E3);
  _canvas->drawCircle(DISPLAY_CENTER_X, orbY, 104, COLOR_WHITE);
  _canvas->drawCircle(DISPLAY_CENTER_X, orbY, 103, COLOR_WHITE);

  drawHabitIcon(_canvas, DISPLAY_CENTER_X, orbY - 40, 22, cur, COLOR_WHITE, cur.color);

  _canvas->setTextColor(COLOR_WHITE);
  _canvas->setTextSize(2);
  String title = cur.name;
  if (title.length() > 12) title = title.substring(0, 12);
  int titleX = DISPLAY_CENTER_X - ((int)title.length() * 12) / 2;
  _canvas->setCursor(titleX, orbY - 4);
  _canvas->print(title);

  char pBuf[28];
  snprintf(pBuf, sizeof(pBuf), "%d / %d %s", cur.completedToday, cur.goalToday, cur.unit.c_str());
  _canvas->setTextColor(0xE73C);
  _canvas->setTextSize(2);
  int progX = DISPLAY_CENTER_X - ((int)strlen(pBuf) * 12) / 2;
  _canvas->setCursor(progX, orbY + 24);
  _canvas->print(pBuf);

  float pct = (cur.goalToday > 0) ? (float)cur.completedToday / (float)cur.goalToday : 0.0f;
  pct = constrain(pct, 0.0f, 1.0f);
  _drawProgressBar(DISPLAY_CENTER_X - 88, orbY + 62, 176, 16, pct, cur.color, 0x2965);

  // Small side previews instead of full side cards (fits round screen better).
  const Habit& prev = habits.getHabit((selectedIndex - 1 + count) % count);
  const Habit& next = habits.getHabit((selectedIndex + 1) % count);
  drawHabitIcon(_canvas, DISPLAY_CENTER_X - 132, orbY, 13, prev, 0xC638, 0x2104);
  drawHabitIcon(_canvas, DISPLAY_CENTER_X + 132, orbY, 13, next, 0xC638, 0x2104);
  _canvas->setTextColor(0xE73C);
  _canvas->setTextSize(2);
  _canvas->setCursor(DISPLAY_CENTER_X - 138, orbY + 24);
  _canvas->print("<");
  _canvas->setCursor(DISPLAY_CENTER_X + 128, orbY + 24);
  _canvas->print(">");

  _drawCentredText("Turn to switch", DISPLAY_CENTER_Y + 132, 0xD6FA, 2);
  _drawCentredText("Click to open", DISPLAY_CENTER_Y + 150, 0xD6FA, 2);

  _flush();
}

// ================================================================
//  HABIT DETAIL SCREEN
// ================================================================
void GUI::drawHabitDetail(const Habit& habit) {
  _canvas->fillScreen(0x0842);
  const int safeR = 170;
  _canvas->fillCircle(DISPLAY_CENTER_X, DISPLAY_CENTER_Y, safeR, 0x10A3);
  _canvas->drawCircle(DISPLAY_CENTER_X, DISPLAY_CENTER_Y, safeR, 0x31C7);

  // Circular composition: top badge + central progress orb.
  drawHabitIcon(_canvas, DISPLAY_CENTER_X, 74, 19, habit, COLOR_WHITE, habit.color);
  _canvas->setTextColor(COLOR_WHITE);
  _canvas->setTextSize(2);
  String name = habit.name;
  if (name.length() > 12) name = name.substring(0, 12);
  int nameX = DISPLAY_CENTER_X - ((int)name.length() * 12) / 2;
  _canvas->setCursor(nameX, 102);
  _canvas->print(name);

  // Main progress ring
  float pct = (habit.goalToday > 0)
              ? (float)habit.completedToday / (float)habit.goalToday
              : 0.0f;
  pct = constrain(pct, 0.0f, 1.0f);
  _drawProgressRing(DISPLAY_CENTER_X, 208, 82, pct, habit.color);

  // Large readable progress inside ring
  char progBuf[16];
  snprintf(progBuf, sizeof(progBuf), "%d/%d", habit.completedToday, habit.goalToday);
  _drawCentredText(String(progBuf), 196, COLOR_WHITE, 3);
  _drawCentredText(habit.unit, 226, COLOR_LIGHT_GREY, 2);

  // Tomorrow projection chip
  int tomorrowGoal = habit.goalToday;   // Current goal unless today is complete
  if (habit.completedToday >= habit.goalToday) {
    tomorrowGoal = min(habit.goalToday + GOAL_STEP_AMOUNT, habit.maxGoal);
  } else if (habit.goalToday > 0 &&
             (habit.completedToday * 100 / habit.goalToday) < GOAL_STAY_THRESHOLD) {
    tomorrowGoal = max(habit.goalToday - GOAL_STEP_AMOUNT, habit.minGoal);
  }

  _drawCard(74, 274, DISPLAY_WIDTH - 148, 34, 12, 0x18E3);
  char tmrBuf[30];
  snprintf(tmrBuf, sizeof(tmrBuf), "Tomorrow: %d %s", tomorrowGoal, habit.unit.c_str());
  _drawCentredText(String(tmrBuf), 286, 0xE73C, 1);

  char streakBuf[22];
  snprintf(streakBuf, sizeof(streakBuf), "Streak: %d days", habit.streak);
  _drawCentredText(String(streakBuf), 308, 0xFFE0, 2);

  _drawCentredText("Click photo", DISPLAY_CENTER_Y + 132, 0xD6FA, 2);
  _drawCentredText("Hold to add +1", DISPLAY_CENTER_Y + 150, 0xD6FA, 1);

  _flush();
}

// ================================================================
//  CAPTURE RITUAL SCREEN
// ================================================================
void GUI::drawCaptureRitual(int countdown, HabitCamBuffer* fb) {
  _canvas->fillScreen(COLOR_BLACK);
  Serial.printf("[GUI] drawCaptureRitual countdown=%d  fb=%p  len=%u  stub=%d\n",
                countdown, (void*)fb, fb ? (unsigned)fb->len : 0u,
                (int)(fb ? fb->isStub : 0));

  const bool havePayload = fb && ((fb->bmp565 && fb->bmpW > 0 && fb->bmpH > 0)
                                  || (fb->data && fb->len > 0));
  if (havePayload && countdown == 0) {
    bool showed = false;

    // Preferred path: pre-decoded RGB565 bitmap (our stub, or any future
    // camera bridge that can give us raw pixels). Much faster than JPEG and
    // never crashes.
    if (fb->bmp565 && fb->bmpW > 0 && fb->bmpH > 0) {
      Serial.printf("[GUI] blitting RGB565 bitmap %dx%d @ %p\n",
                    fb->bmpW, fb->bmpH, (const void*)fb->bmp565);
      _drawBitmap565Centred(fb->bmp565, fb->bmpW, fb->bmpH, fb->isStub);
      showed = true;
    }
    // Real JPEG camera path — try to decode, fall back to polaroid on fail.
    else if (fb->data && fb->len > 0) {
      bool decoded = decodeJpegCentred(_canvas, fb->data, fb->len);
      Serial.printf("[GUI] capture preview: decoded=%s\n",
                    decoded ? "yes" : "no (using polaroid fallback)");
      if (decoded) {
        _canvas->fillRect(0, DISPLAY_HEIGHT - 40, DISPLAY_WIDTH, 40, COLOR_BLACK);
        _drawCentredText("Saved", DISPLAY_HEIGHT - 30, COLOR_WHITE, 2);
        _drawCheckmark(DISPLAY_WIDTH - 48, DISPLAY_HEIGHT - 32, 22, COLOR_GREEN);
        showed = true;
      }
    }

    if (!showed) {
      Serial.println(F("[GUI] falling back to polaroid placeholder"));
      _drawPolaroidPlaceholder();
    }

  } else {
    // --- Countdown / loading phase ---
    // Countdown number (3,2,1)
    if (countdown > 0) {
      char cBuf[4];
      snprintf(cBuf, sizeof(cBuf), "%d", countdown);
      // Large centered number
      _canvas->setTextColor(COLOR_WHITE);
      _canvas->setTextSize(6);
      int numX = DISPLAY_CENTER_X - (countdown >= 10 ? 28 : 14);
      _canvas->setCursor(numX, DISPLAY_CENTER_Y - 8);
      _canvas->print(countdown);
      _canvas->setTextSize(1);

      _drawCentredText("Get ready...", DISPLAY_CENTER_Y + 54, COLOR_LIGHT_GREY, 2);
    } else {
      // Loading animation while capture/decode is in flight.
      const unsigned long t = millis();
      const float spinDeg = (float)(t % 1200) * 0.3f;  // 0..360 every 1.2s
      const int cx = DISPLAY_CENTER_X;
      const int cy = DISPLAY_CENTER_Y - 8;
      const int r  = 34;
      _drawArcPixels(cx, cy, r, 7, 0.0f, 360.0f, 0x2104);
      _drawArcPixels(cx, cy, r, 7, spinDeg, spinDeg + 95.0f, 0x07FF);
      _drawCentredText("Processing photo...", DISPLAY_CENTER_Y + 54, COLOR_WHITE, 2);
      _drawCentredText("Please hold still", DISPLAY_CENTER_Y + 74, COLOR_LIGHT_GREY, 1);
    }
  }

  _flush();
}

// ================================================================
//  CELEBRATION SCREEN
// ================================================================
void GUI::drawCelebration(int vitalityGain, unsigned long elapsed) {
  _canvas->fillScreen(COLOR_BLACK);

  // --- Animated sparkle background ---
  // Sparkles fan out from center over time
  uint16_t sparkColors[] = { COLOR_YELLOW, COLOR_CYAN, COLOR_MAGENTA,
                              COLOR_GREEN,  COLOR_WHITE, COLOR_ORANGE };
  for (int s = 0; s < 12; s++) {
    float angle  = (s * 30.0f + elapsed * 0.05f) * (M_PI / 180.0f);
    float dist   = min((float)elapsed * 0.06f, 90.0f);
    int sx = DISPLAY_CENTER_X + (int)(cosf(angle) * dist);
    int sy = DISPLAY_CENTER_Y + (int)(sinf(angle) * dist);
    uint16_t col = sparkColors[s % 6];
    _canvas->fillCircle(sx, sy, 4, col);
  }

  // --- Pet face (thriving, jumping) ---
  int jumpY = (int)(-fabsf(sinf(elapsed * 0.006f)) * 20.0f);  // Jump arc
  drawPetFace(DISPLAY_CENTER_X, DISPLAY_CENTER_Y + jumpY, PET_THRIVING,
              (int)(elapsed / ANIMATION_TICK_MS) % 64);

  // --- Floating "+N Vitality!" text ---
  _drawFloatingVitalityText(vitalityGain, elapsed);

  _flush();
}

// ================================================================
//  PET FACE — Duck sprite (from Duck.c)
//
//  The character on screen is the same Duck the companion-app Python
//  test system shows. The procedural cartoon-face fallback lives at
//  the bottom of this function (commented-out) for reference.
// ================================================================
void GUI::drawPetFace(int cx, int cy, PetState state, int animFrame) {
  int frameIdx = duck_pickFrameForPet(state, animFrame);

  // Vitality-tinted glow halo behind the duck to keep emotional feedback
  // even though the duck's own colours are fixed.
  uint16_t haloColor;
  switch (state) {
    case PET_THRIVING:   haloColor = 0x07E0; break;  // Green
    case PET_HAPPY:      haloColor = 0xAFE5; break;  // Mint
    case PET_TIRED:      haloColor = 0xFFE0; break;  // Yellow
    case PET_STRUGGLING: haloColor = 0xFD20; break;  // Orange
    case PET_CRITICAL:   haloColor = 0xF800; break;  // Red
    default:             haloColor = COLOR_MID_GREY;
  }
  // Dim the halo so it's a hint, not a flood.
  uint16_t haloDim = (haloColor >> 2) & 0x39E7;
  _canvas->fillCircle(cx, cy, 106, haloDim);
  _canvas->drawCircle(cx, cy, 106, haloColor);

  drawDuck(cx, cy, frameIdx, 8);
  return;

  // ---- Legacy procedural face (kept for quick A/B reference) ----
  (void)animFrame;
  uint16_t faceColor;
  switch (state) {
    case PET_THRIVING:   faceColor = 0xFFF4; break;  // Bright warm yellow
    case PET_HAPPY:      faceColor = COLOR_SKIN;      break;  // Peach
    case PET_TIRED:      faceColor = COLOR_SKIN_DARK; break;  // Muted peach
    case PET_STRUGGLING: faceColor = 0xC694;          break;  // Grey-peach
    case PET_CRITICAL:
      // Critical flickers between dark grey and even darker during animFrame
      faceColor = (animFrame % 8 < 6) ? 0x8410 : 0x4208;
      break;
    default: faceColor = COLOR_SKIN;
  }

  // ---- Face circle ----
  _canvas->fillCircle(cx, cy, 46, faceColor);
  _canvas->drawCircle(cx, cy, 46, 0x2965);  // Soft dark outline

  // ---- Blush cheeks (only when happy or thriving) ----
  if (state == PET_THRIVING || state == PET_HAPPY) {
    _canvas->fillCircle(cx - 28, cy + 8, 9, COLOR_PINK);
    _canvas->fillCircle(cx + 28, cy + 8, 9, COLOR_PINK);
    // Slightly transparent (simulate with a dim second circle)
    _canvas->fillCircle(cx - 28, cy + 8, 7, (COLOR_PINK & 0xF7DE));
    _canvas->fillCircle(cx + 28, cy + 8, 7, (COLOR_PINK & 0xF7DE));
  }

  // ---- Eyes ----
  int eyeY       = cy - 10;
  int leftEyeX   = cx - 15;
  int rightEyeX  = cx + 15;
  int eyeRadius  = 9;

  _canvas->fillCircle(leftEyeX,  eyeY, eyeRadius, COLOR_WHITE);
  _canvas->fillCircle(rightEyeX, eyeY, eyeRadius, COLOR_WHITE);

  switch (state) {
    case PET_THRIVING: {
      // Star/sparkle eyes — filled with gold + cross highlights
      _canvas->fillCircle(leftEyeX,  eyeY, 6, COLOR_YELLOW);
      _canvas->fillCircle(rightEyeX, eyeY, 6, COLOR_YELLOW);
      _canvas->drawLine(leftEyeX  - 7, eyeY, leftEyeX  + 7, eyeY, COLOR_WHITE);
      _canvas->drawLine(leftEyeX,  eyeY - 7, leftEyeX,  eyeY + 7, COLOR_WHITE);
      _canvas->drawLine(rightEyeX - 7, eyeY, rightEyeX + 7, eyeY, COLOR_WHITE);
      _canvas->drawLine(rightEyeX, eyeY - 7, rightEyeX, eyeY + 7, COLOR_WHITE);
      break;
    }
    case PET_HAPPY: {
      // Normal round pupils, slight downward shift (relaxed)
      _canvas->fillCircle(leftEyeX,  eyeY + 1, 4, COLOR_BLACK);
      _canvas->fillCircle(rightEyeX, eyeY + 1, 4, COLOR_BLACK);
      // Light catchlight
      _canvas->fillCircle(leftEyeX  - 2, eyeY - 2, 2, COLOR_WHITE);
      _canvas->fillCircle(rightEyeX - 2, eyeY - 2, 2, COLOR_WHITE);
      break;
    }
    case PET_TIRED: {
      // Half-closed: paint the top half of each eye with face color
      _canvas->fillRect(leftEyeX  - eyeRadius, eyeY - eyeRadius,
                        eyeRadius * 2, eyeRadius, faceColor);
      _canvas->fillRect(rightEyeX - eyeRadius, eyeY - eyeRadius,
                        eyeRadius * 2, eyeRadius, faceColor);
      _canvas->fillCircle(leftEyeX,  eyeY + 3, 3, 0x2965);
      _canvas->fillCircle(rightEyeX, eyeY + 3, 3, 0x2965);
      break;
    }
    case PET_STRUGGLING: {
      // Small worried pupils, furrowed brows
      _canvas->fillCircle(leftEyeX,  eyeY, 3, COLOR_BLACK);
      _canvas->fillCircle(rightEyeX, eyeY, 3, COLOR_BLACK);
      // Angled brow lines (inner corners raised = worry)
      _canvas->drawLine(leftEyeX  - 7, eyeY - 12, leftEyeX  + 3, eyeY - 8, COLOR_BLACK);
      _canvas->drawLine(leftEyeX  - 6, eyeY - 11, leftEyeX  + 3, eyeY - 7, COLOR_BLACK);
      _canvas->drawLine(rightEyeX - 3, eyeY - 8,  rightEyeX + 7, eyeY - 12, COLOR_BLACK);
      _canvas->drawLine(rightEyeX - 3, eyeY - 7,  rightEyeX + 6, eyeY - 11, COLOR_BLACK);
      break;
    }
    case PET_CRITICAL: {
      // Every 8 frames: normal wide pupils; then X-eyes flicker
      if (animFrame % 8 < 6) {
        _canvas->fillCircle(leftEyeX,  eyeY, 6, COLOR_BLACK);  // Wide fear
        _canvas->fillCircle(rightEyeX, eyeY, 6, COLOR_BLACK);
        _canvas->fillCircle(leftEyeX  - 2, eyeY - 3, 2, COLOR_WHITE);  // catchlight
        _canvas->fillCircle(rightEyeX - 2, eyeY - 3, 2, COLOR_WHITE);
      } else {
        // X-eyes during flicker frame
        _canvas->drawLine(leftEyeX  - 6, eyeY - 6, leftEyeX  + 6, eyeY + 6, COLOR_BLACK);
        _canvas->drawLine(leftEyeX  - 6, eyeY + 6, leftEyeX  + 6, eyeY - 6, COLOR_BLACK);
        _canvas->drawLine(rightEyeX - 6, eyeY - 6, rightEyeX + 6, eyeY + 6, COLOR_BLACK);
        _canvas->drawLine(rightEyeX - 6, eyeY + 6, rightEyeX + 6, eyeY - 6, COLOR_BLACK);
      }
      break;
    }
  }

  // ---- Mouth ----
  int mouthCY = cy + 16;

  switch (state) {
    case PET_THRIVING:
      _drawSmile(cx, mouthCY, 22, COLOR_BLACK, 3);
      // Add inner mouth arc (open grin)
      _drawSmile(cx, mouthCY + 4, 18, 0xF800, 2);  // Red inside
      break;

    case PET_HAPPY:
      _drawSmile(cx, mouthCY, 16, COLOR_BLACK, 2);
      break;

    case PET_TIRED:
      // Flat to slight downward mouth
      _canvas->drawLine(cx - 12, mouthCY + 2, cx + 12, mouthCY + 2, COLOR_BLACK);
      _canvas->drawLine(cx - 12, mouthCY + 3, cx + 12, mouthCY + 3, COLOR_BLACK);
      break;

    case PET_STRUGGLING:
      _drawFrown(cx, mouthCY, 14, COLOR_BLACK, 2);
      // Teardrop under left eye
      _canvas->fillCircle(leftEyeX, eyeY + eyeRadius + 5, 4, 0x065F);
      _canvas->fillTriangle(leftEyeX - 3, eyeY + eyeRadius + 4,
                            leftEyeX + 3, eyeY + eyeRadius + 4,
                            leftEyeX,     eyeY + eyeRadius + 12, 0x065F);
      break;

    case PET_CRITICAL: {
      // Open "O" mouth — frightened expression
      _canvas->fillCircle(cx, mouthCY, 10, COLOR_BLACK);
      _canvas->fillCircle(cx, mouthCY, 7, 0x8000);  // Dark red cavity
      break;
    }
  }

  // ---- State decorations ----
  if (state == PET_THRIVING) {
    // 4 sparkles orbiting the head, rotating with animFrame
    uint16_t sc[] = { COLOR_YELLOW, COLOR_MAGENTA, COLOR_CYAN, COLOR_WHITE };
    for (int i = 0; i < 4; i++) {
      float angle = ((animFrame * 3 + i * 90) % 360) * (M_PI / 180.0f);
      int sx = cx + (int)(cosf(angle) * 58.0f);
      int sy = cy + (int)(sinf(angle) * 58.0f);
      _canvas->fillCircle(sx, sy, 4, sc[i]);
      _canvas->fillCircle(sx, sy, 2, COLOR_WHITE);  // Bright center
    }
  }

  if (state == PET_TIRED) {
    // ZZZ rising above the head
    uint16_t zColor = 0x528A;
    float zOffset = (animFrame % 16) * 2.0f;
    _canvas->setTextColor(zColor);
    _canvas->setTextSize(1);
    _canvas->setCursor(cx + 32, cy - 30 - (int)zOffset);
    _canvas->print("z");
    _canvas->setCursor(cx + 38, cy - 40 - (int)zOffset);
    _canvas->print("Z");
    _canvas->setCursor(cx + 45, cy - 52 - (int)zOffset);
    _canvas->print("Z");
  }

  if (state == PET_CRITICAL) {
    // Ghostly shimmer ring — dashed circle of pixels
    for (int a = 0; a < 360; a += 15) {
      float rad = a * (M_PI / 180.0f);
      // Flicker presence based on animFrame
      if ((a / 15 + animFrame) % 4 != 0) {
        int gx = cx + (int)(cosf(rad) * 54.0f);
        int gy = cy + (int)(sinf(rad) * 54.0f);
        _canvas->fillCircle(gx, gy, 2, COLOR_LIGHT_GREY);
      }
    }
  }
}

// ================================================================
//  DIALOGUE BUBBLE — speech bubble at bottom of round display
// ================================================================
void GUI::drawDialogueBubble(const String& message) {
  // Bubble background
  int bx = 24, by = DISPLAY_HEIGHT - 90, bw = DISPLAY_WIDTH - 48, bh = 72;
  _canvas->fillRoundRect(bx, by, bw, bh, 12, 0x1082);
  _canvas->drawRoundRect(bx, by, bw, bh, 12, COLOR_MID_GREY);

  // Word-wrapped text inside bubble (single line for round display)
  _canvas->setTextColor(COLOR_WHITE);
  _canvas->setTextSize(1);
  _canvas->setTextWrap(true);
  _canvas->setCursor(bx + 8, by + 12);

  // Truncate to 40 chars to fit the bubble
  String display = message;
  if (display.length() > 40) {
    display = display.substring(0, 37) + "...";
  }
  _canvas->print(display);
}

// ================================================================
//  PRIVATE HELPERS
// ================================================================

// ---------------------------------------------------------------
// _drawVitalityRing — arc around display edge showing pet health
// ---------------------------------------------------------------
void GUI::_drawVitalityRing(int vitality) {
  // Ring radius and thickness
  const int r         = min(DISPLAY_CENTER_X, DISPLAY_CENTER_Y) - 10;
  const int thickness = 7;

  uint16_t ringColor = _vitalityToColor(vitality);
  float    arcAngle  = 360.0f * vitality / 100.0f;

  // Draw the filled background ring first (dark grey)
  _drawArcPixels(DISPLAY_CENTER_X, DISPLAY_CENTER_Y, r, thickness,
                 0.0f, 360.0f, COLOR_DARK_GREY);

  // Draw the filled vitality arc on top
  if (arcAngle > 0.5f) {
    _drawArcPixels(DISPLAY_CENTER_X, DISPLAY_CENTER_Y, r, thickness,
                   0.0f, arcAngle, ringColor);
  }
}

// ---------------------------------------------------------------
// _drawArcPixels — draw a filled arc by pixel-stepping
// ---------------------------------------------------------------
void GUI::_drawArcPixels(int cx, int cy, int r, int thickness,
                          float startDeg, float endDeg, uint16_t color) {
  // Step 0.5° for smooth curves at radius 115
  for (float a = startDeg; a < endDeg; a += 0.5f) {
    // Convert: 0° = top (−90° in math), clockwise
    float rad = (a - 90.0f) * (float)M_PI / 180.0f;
    float cosA = cosf(rad);
    float sinA = sinf(rad);

    for (int t = 0; t < thickness; t++) {
      int px = cx + (int)(cosA * (r - t));
      int py = cy + (int)(sinA * (r - t));
      _canvas->drawPixel(px, py, color);
    }
  }
}

// ---------------------------------------------------------------
// _drawProgressRing — activity-ring style habit progress
// ---------------------------------------------------------------
void GUI::_drawProgressRing(int cx, int cy, int radius, float pct, uint16_t color) {
  const int thickness = 8;

  // Background track
  _drawArcPixels(cx, cy, radius, thickness, 0.0f, 360.0f, 0x2965);

  // Progress fill
  if (pct > 0.0f) {
    _drawArcPixels(cx, cy, radius, thickness, 0.0f, pct * 360.0f, color);
  }

  // Endpoint dot
  if (pct > 0.02f) {
    float endRad = (pct * 360.0f - 90.0f) * (float)M_PI / 180.0f;
    int ex = cx + (int)(cosf(endRad) * radius);
    int ey = cy + (int)(sinf(endRad) * radius);
    _canvas->fillCircle(ex, ey, thickness / 2 + 1, COLOR_WHITE);
  }
}

// ---------------------------------------------------------------
// _drawProgressBar — horizontal progress bar
// ---------------------------------------------------------------
void GUI::_drawProgressBar(int x, int y, int w, int h,
                            float pct, uint16_t fillColor, uint16_t bgColor) {
  _canvas->fillRoundRect(x, y, w, h, h / 2, bgColor);
  int fillW = (int)(w * constrain(pct, 0.0f, 1.0f));
  if (fillW > 0) {
    _canvas->fillRoundRect(x, y, fillW, h, h / 2, fillColor);
  }
}

// ---------------------------------------------------------------
// _drawHabitCard — circular card for the carousel
// ---------------------------------------------------------------
void GUI::_drawHabitCard(int cx, int cy, int radius, const Habit& h, bool selected) {
  // Card background
  uint16_t bgColor   = selected ? h.color : (h.color >> 1 & 0x7BEF);
  uint16_t textColor = selected ? COLOR_WHITE : COLOR_LIGHT_GREY;

  _canvas->fillCircle(cx, cy, radius, bgColor);

  if (selected) {
    _canvas->drawCircle(cx, cy, radius,     COLOR_WHITE);
    _canvas->drawCircle(cx, cy, radius - 1, COLOR_WHITE);
  }

  drawHabitIcon(_canvas, cx, cy - (selected ? 8 : 4),
                selected ? 16 : 10, h, COLOR_WHITE, h.color);

  // Habit name
  _canvas->setTextSize(1);
  _canvas->setTextColor(textColor);
  String label = h.name;
  if (label.length() > (selected ? 8 : 5)) {
    label = label.substring(0, selected ? 8 : 5);
  }
  int labelX = cx - (label.length() * 3);
  _canvas->setCursor(labelX, cy + (selected ? 8 : 4));
  _canvas->print(label);

  // Progress fraction
  if (selected) {
    char pBuf[12];
    snprintf(pBuf, sizeof(pBuf), "%d/%d", h.completedToday, h.goalToday);
    _canvas->setTextColor(0xFFFF);
    _canvas->setCursor(cx - strlen(pBuf) * 3, cy + 22);
    _canvas->print(pBuf);
  }
}

// ---------------------------------------------------------------
// _drawSmile — upward-curved arc (smile)
// ---------------------------------------------------------------
void GUI::_drawSmile(int cx, int cy, int radius, uint16_t color, int thickness) {
  // Draw a smile arc from ~210° to ~330° (bottom of circle)
  for (float a = 200.0f; a <= 340.0f; a += 1.0f) {
    float rad = a * (float)M_PI / 180.0f;
    for (int t = 0; t < thickness; t++) {
      int px = cx + (int)(cosf(rad) * (radius - t));
      int py = cy + (int)(sinf(rad) * (radius - t));
      _canvas->drawPixel(px, py, color);
    }
  }
}

// ---------------------------------------------------------------
// _drawFrown — downward-curved arc (frown)
// ---------------------------------------------------------------
void GUI::_drawFrown(int cx, int cy, int radius, uint16_t color, int thickness) {
  // Draw a frown arc from ~20° to ~160° (bottom of inverted circle)
  // We place it below center by offsetting cy upward and using top arc
  for (float a = 30.0f; a <= 150.0f; a += 1.0f) {
    float rad = a * (float)M_PI / 180.0f;
    for (int t = 0; t < thickness; t++) {
      int px = cx + (int)(cosf(rad) * (radius - t));
      int py = cy - (int)(sinf(rad) * (radius - t));  // Negative = frown curves down
      _canvas->drawPixel(px, py, color);
    }
  }
}

// ---------------------------------------------------------------
// _drawBitmap565Centred — blit a raw RGB565 bitmap into a polaroid card
//
// The bitmap is read from flash (PROGMEM) one pixel at a time and drawn
// directly to the canvas, so we don't need a streaming decoder like
// JPEGDEC. ~56 KB (240×240) at ~300 ns per pixel is ≤ 20 ms — imperceptible.
// ---------------------------------------------------------------
void GUI::_drawBitmap565Centred(const uint16_t* px, int w, int h, bool stub) {
  if (!_canvas || !px || w <= 0 || h <= 0) return;

  // Backdrop (soft charcoal) so the polaroid card pops
  _canvas->fillScreen(0x2124);

  // Polaroid card dimensions: photo + white frame + caption strip
  const int margin = 22;
  const int capH   = 44;
  const int cardW  = w + margin * 2;
  const int cardH  = h + margin * 2 + capH;
  const int cardX  = (DISPLAY_WIDTH  - cardW) / 2;
  const int cardY  = (DISPLAY_HEIGHT - cardH) / 2;

  // Soft drop shadow
  _canvas->fillRoundRect(cardX + 5, cardY + 6, cardW, cardH, 10, 0x0841);
  // White frame
  _canvas->fillRoundRect(cardX, cardY, cardW, cardH, 10, COLOR_WHITE);
  _canvas->drawRoundRect(cardX, cardY, cardW, cardH, 10, 0xC618);

  // Image area
  const int imgX = cardX + margin;
  const int imgY = cardY + margin;

  // Blit pixel-by-pixel. pgm_read_word honours PROGMEM on AVR and is a
  // plain 16-bit load on ESP32 (flash is memory-mapped) so this is fast.
  for (int j = 0; j < h; j++) {
    for (int i = 0; i < w; i++) {
      uint16_t c = pgm_read_word(&px[j * w + i]);
      _canvas->drawPixel(imgX + i, imgY + j, c);
    }
  }

  // Thin inner frame around the image
  _canvas->drawRect(imgX - 1, imgY - 1, w + 2, h + 2, 0x8C51);

  // Caption strip under the photo
  const int capY = imgY + h + 10;
  _canvas->setTextColor(0x2104);
  _canvas->setTextSize(2);
  const char* caption = stub ? "Habit Logged" : "Captured";
  int cw = (int)strlen(caption) * 6 * 2;
  _canvas->setCursor(cardX + (cardW - cw) / 2, capY + 2);
  _canvas->print(caption);

  _canvas->setTextSize(1);
  _canvas->setTextColor(0x7BEF);
  const char* sub = stub ? "(placeholder scene)" : "photo saved";
  int sw = (int)strlen(sub) * 6;
  _canvas->setCursor(cardX + (cardW - sw) / 2, capY + 24);
  _canvas->print(sub);

  // Small green check in the corner of the photo
  _drawCheckmark(imgX + w - 18, imgY + 18, 10, COLOR_GREEN);
}

// ---------------------------------------------------------------
// _drawPolaroidPlaceholder — procedural "Photo captured!" preview
//
// Drawn in place of an actual JPEG decode when the frame is a stub
// (no SSCMA camera bridge yet) or when JPEG decode fails. We paint
// a polaroid-style card: coloured viewport with a centred camera icon,
// a "Captured" banner, the elapsed timestamp, and a large checkmark.
// ---------------------------------------------------------------
void GUI::_drawPolaroidPlaceholder() {
  if (!_canvas) return;

  // Background: soft gradient-ish teal
  _canvas->fillScreen(0x0A69);

  // Polaroid outer card (white border)
  const int cardX = 40;
  const int cardY = 50;
  const int cardW = DISPLAY_WIDTH  - 80;
  const int cardH = DISPLAY_HEIGHT - 100;
  _canvas->fillRoundRect(cardX, cardY, cardW, cardH, 16, COLOR_WHITE);
  _canvas->drawRoundRect(cardX, cardY, cardW, cardH, 16, 0xA514);

  // Photo viewport (dark "image area")
  const int vpX = cardX + 18;
  const int vpY = cardY + 18;
  const int vpW = cardW - 36;
  const int vpH = cardH - 90;
  _canvas->fillRect(vpX, vpY, vpW, vpH, 0x2125);

  // Fake "scene" colour bands inside the viewport
  uint16_t bands[4] = { 0x5B3F, 0x9E7F, 0xFFE0, 0xFD20 };
  const int bandH = vpH / 4;
  for (int i = 0; i < 4; i++) {
    _canvas->fillRect(vpX, vpY + i * bandH, vpW, bandH, bands[i]);
  }

  // Large camera aperture icon in the middle of the viewport
  const int cx = vpX + vpW / 2;
  const int cy = vpY + vpH / 2;
  _canvas->fillCircle(cx, cy, 44, 0x1082);
  _canvas->fillCircle(cx, cy, 38, 0x3186);
  _canvas->fillCircle(cx, cy, 28, 0x1082);
  _canvas->fillCircle(cx - 10, cy - 8, 8, 0xC618);

  // Caption strip (bottom of polaroid)
  _drawCentredText("Captured", cardY + cardH - 58, 0x1082, 3);

  // Green checkmark bottom-right
  _drawCheckmark(cardX + cardW - 40, cardY + cardH - 28, 14, COLOR_GREEN);

  // "Photo saved" footer under the polaroid
  _drawCentredText("habit logged", DISPLAY_HEIGHT - 22, COLOR_WHITE, 1);
}

// ---------------------------------------------------------------
// _drawCheckmark — large animated ✓ mark
// ---------------------------------------------------------------
void GUI::_drawCheckmark(int cx, int cy, int size, uint16_t color) {
  int t = 3;  // thickness
  // Left stroke of the checkmark (short, going down-right)
  for (int i = 0; i < t; i++) {
    _canvas->drawLine(cx - size + i, cy,
                      cx - size / 3 + i, cy + size * 2 / 3, color);
  }
  // Right stroke (long, going up-right)
  for (int i = 0; i < t; i++) {
    _canvas->drawLine(cx - size / 3 + i, cy + size * 2 / 3,
                      cx + size + i, cy - size / 2, color);
  }
}

// ---------------------------------------------------------------
// _drawFloatingVitalityText — "+N Vitality!" rising upward
// ---------------------------------------------------------------
void GUI::_drawFloatingVitalityText(int vitalityGain, unsigned long elapsed) {
  // Text rises from center-bottom to near top over 3 seconds
  float progress = (float)elapsed / (float)CELEBRATION_DURATION_MS;  // 0.0 → 1.0
  int   textY    = (int)(DISPLAY_HEIGHT * 0.7f - progress * 80.0f);

  // Alpha fade: full opacity in first half, fading in second
  uint16_t textColor = (progress < 0.5f) ? COLOR_YELLOW : COLOR_LIGHT_GREY;

  char buf[20];
  snprintf(buf, sizeof(buf), "+%d Vitality!", vitalityGain);

  _canvas->setTextColor(textColor);
  _canvas->setTextSize(2);
  int textX = DISPLAY_CENTER_X - (strlen(buf) * 6);  // Approx center
  _canvas->setCursor(textX, textY);
  _canvas->print(buf);
  _canvas->setTextSize(1);
}

// ---------------------------------------------------------------
// _vitalityToColor — map 0–100 to a green→yellow→red gradient
// ---------------------------------------------------------------
uint16_t GUI::_vitalityToColor(int vitality) {
  vitality = constrain(vitality, 0, 100);

  if (vitality >= 80) return 0x07E0;      // Full green
  if (vitality >= 60) return 0x87E0;      // Yellow-green
  if (vitality >= 40) return 0xFFE0;      // Yellow
  if (vitality >= 20) return 0xFD20;      // Orange
  return 0xF800;                           // Red (critical)
}

// ---------------------------------------------------------------
// _drawCentredText — draw text centred at a given y coordinate
// ---------------------------------------------------------------
void GUI::_drawCentredText(const String& text, int y, uint16_t color, uint8_t size) {
  _canvas->setTextColor(color);
  _canvas->setTextSize(size);
  // Estimate character width: size * 6 pixels per character
  int textW = text.length() * size * 6;
  int x     = DISPLAY_CENTER_X - textW / 2;
  _canvas->setCursor(x, y);
  _canvas->print(text);
}

// ---------------------------------------------------------------
// _drawCard — rounded rectangle with fill
// ---------------------------------------------------------------
void GUI::_drawCard(int x, int y, int w, int h, int r, uint16_t color) {
  _canvas->fillRoundRect(x, y, w, h, r, color);
}

// ---------------------------------------------------------------
// _flush — transfer canvas to physical display
// ---------------------------------------------------------------
void GUI::_flush() {
  _canvas->flush();
}

void GUI::flushNow() {
  if (_canvas) _canvas->flush();
}

// ================================================================
//  SLEEP / WAKE TRANSITION SCREENS
// ================================================================

void GUI::drawSleepScreen() {
  if (!_canvas) return;
  _canvas->fillScreen(COLOR_BLACK);

  const int cx = DISPLAY_CENTER_X;
  const int cy = DISPLAY_CENTER_Y - 20;

  // Crescent moon: big yellow disc with a black disc offset to bite a curve.
  _canvas->fillCircle(cx, cy, 70, 0xFFE0);              // yellow disc
  _canvas->fillCircle(cx + 26, cy - 16, 70, COLOR_BLACK); // shadow

  // "Z" stack rising up-right of the moon.
  _canvas->setTextColor(0xC618);   // light grey
  _canvas->setTextSize(3);
  _canvas->setCursor(cx + 70, cy - 30);  _canvas->print('Z');
  _canvas->setTextSize(2);
  _canvas->setCursor(cx + 95, cy - 60);  _canvas->print('Z');
  _canvas->setTextSize(1);
  _canvas->setCursor(cx + 115, cy - 80); _canvas->print('z');

  _drawCentredText("Sleeping", cy + 100, COLOR_WHITE, 3);
  _drawCentredText("Tap the knob to wake", cy + 140, COLOR_MID_GREY, 1);
  _flush();
}

void GUI::drawWakeScreen() {
  if (!_canvas) return;
  _canvas->fillScreen(COLOR_BLACK);
  const int cx = DISPLAY_CENTER_X;
  const int cy = DISPLAY_CENTER_Y - 20;
  // Sun: solid yellow disc with rays.
  _canvas->fillCircle(cx, cy, 50, 0xFFE0);
  for (int i = 0; i < 12; i++) {
    float a = i * (M_PI / 6.0f);
    int x1 = cx + (int)(cosf(a) * 65.0f);
    int y1 = cy + (int)(sinf(a) * 65.0f);
    int x2 = cx + (int)(cosf(a) * 90.0f);
    int y2 = cy + (int)(sinf(a) * 90.0f);
    _canvas->drawLine(x1, y1, x2, y2, 0xFD20);
  }
  _drawCentredText("Good morning!", cy + 110, COLOR_WHITE, 3);
  _flush();
}

// ================================================================
//  TEST HARNESS HELPERS — called from main.cpp's serial commands
// ================================================================

void GUI::fillTestColor(uint16_t color) {
  if (!_canvas) return;
  _canvas->fillScreen(color);
  _flush();
}

void GUI::fillTestBars() {
  if (!_canvas) return;
  const int h = DISPLAY_HEIGHT / 4;
  _canvas->fillRect(0, 0        , DISPLAY_WIDTH, h, COLOR_RED);
  _canvas->fillRect(0, h        , DISPLAY_WIDTH, h, COLOR_GREEN);
  _canvas->fillRect(0, 2 * h    , DISPLAY_WIDTH, h, COLOR_BLUE);
  _canvas->fillRect(0, 3 * h    , DISPLAY_WIDTH, DISPLAY_HEIGHT - 3 * h, COLOR_WHITE);
  _flush();
}

void GUI::drawDuckFrameCentred(int frameIdx, int scale) {
  if (!_canvas) return;
  _canvas->fillScreen(COLOR_BLACK);
  drawDuck(DISPLAY_CENTER_X, DISPLAY_CENTER_Y, frameIdx, scale);
  // Frame label at the bottom so visual + text match on the test monitor.
  char lbl[32];
  snprintf(lbl, sizeof(lbl), "duck %d: %s", frameIdx, duck_frameName(frameIdx));
  _drawCentredText(String(lbl), DISPLAY_HEIGHT - 14, COLOR_LIGHT_GREY, 1);
  _flush();
}

// ---------------------------------------------------------------
// drawDuck — blit a Duck.c frame to the canvas, nearest-neighbour
// scaled. Pixels with alpha==0 are skipped so the duck composites
// cleanly over whatever background is already drawn.
// ---------------------------------------------------------------
void GUI::drawDuck(int cx, int cy, int frameIdx, int scale) {
  if (!_canvas) return;
  if (scale < 1) scale = 1;

  const uint16_t* px    = duck_getFrameRGB565(frameIdx);
  const uint8_t*  alpha = duck_getFrameAlpha(frameIdx);

  const int w = DUCK_W * scale;
  const int h = DUCK_H * scale;
  const int x0 = cx - w / 2;
  const int y0 = cy - h / 2;

  for (int sy = 0; sy < DUCK_H; sy++) {
    for (int sx = 0; sx < DUCK_W; sx++) {
      int idx = sy * DUCK_W + sx;
      if (alpha[idx] < 0x40) continue;     // ~25% opaque threshold
      uint16_t c = px[idx];
      int dx = x0 + sx * scale;
      int dy = y0 + sy * scale;
      if (scale == 1) {
        _canvas->drawPixel(dx, dy, c);
      } else {
        _canvas->fillRect(dx, dy, scale, scale, c);
      }
    }
  }
}
