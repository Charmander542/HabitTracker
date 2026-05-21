// =============================================================
// main.cpp — Top-level state machine for HabitTracker
//
// STATE MACHINE FLOW:
//
//   IDLE ──[press]──► HABIT_SELECT ──[press]──► HABIT_DETAIL
//     ▲                    ▲                         │
//     │                    │                [press]=capture
//     │               [hold]=back           [hold]= +1 unit
//     │                                             │
//     └──────── CELEBRATION ◄── CAPTURE_RITUAL ◄───┘
//
// All global instances live here. Modules use `extern` to
// reference them without creating singletons.
// =============================================================

#include <Arduino.h>
#include <ArduinoJson.h>
#include <LittleFS.h>
#include <esp_chip_info.h>
#include <esp_sleep.h>
#include <driver/gpio.h>
#include <driver/uart.h>
#include "config.h"
#include "gui.h"
#include "pet.h"
#include "habits.h"
#include "encoder.h"
#include "haptic.h"
#include "storage.h"
#include "rtc.h"
#include "camera.h"
#include "sscma_camera.h"
#include "watcher_bsp.h"
#include "duck_sprite.h"
#include "audio_fx.h"

// ---------------------------------------------------------------
// Global instances — declared here, referenced via extern elsewhere
// ---------------------------------------------------------------
GUI         gui;
Pet         pet;
HabitManager habits;
Encoder     encoder;
Haptic      haptic;
AudioFx     audioFx;
Storage     storage;
RTC         rtc;
Camera      camera;

// ---------------------------------------------------------------
// App state machine
// ---------------------------------------------------------------
enum AppState {
  STATE_IDLE,
  STATE_HABIT_SELECT,
  STATE_HABIT_DETAIL,
  STATE_CAPTURE_RITUAL,
  STATE_CELEBRATION
};

static AppState    currentState   = STATE_IDLE;
static AppState    previousState  = STATE_IDLE;
static unsigned long stateEnteredAt = 0;

// IDLE state
static unsigned long lastHeartbeat     = 0;
static unsigned long lastDayCheck      = 0;
static unsigned long lastVitalityDecay = 0;
static String        lastKnownDate     = "";
static bool          morningGreetShown = false;

// HABIT_SELECT state
static int selectedHabitIdx = 0;

// CAPTURE_RITUAL state
static int           captureCountdown  = 3;
static unsigned long lastCountdownTick = 0;
static bool          captureReady      = false;
static bool          captureFlashFired = false;
static bool          captureFlashActive = false;
static unsigned long captureFlashStartedAt = 0;
static bool          captureLoadingPrimed = false;
static unsigned long captureLoadingStarted = 0;
static TaskHandle_t  captureTaskHandle = nullptr;
static volatile bool captureTaskStarted = false;
static volatile bool captureTaskDone = false;
static HabitCamBuffer* captureTaskFrame = nullptr;

// CELEBRATION state
static int           celebVitalityGain  = 0;
static int           celebStreakForMsg   = 0;
static uint8_t       celebCueStage       = 0;

// Loop timing
static unsigned long lastRefresh    = 0;
static unsigned long lastAnimTick   = 0;
static unsigned long lastSerialCheck= 0;

// When true, the state-machine handlers skip their gui.draw*() calls
// so that whatever pattern / duck frame the test harness pushed to the
// display stays visible. Cleared by `test off`, `state idle`, or any
// command that needs to take the screen back.
static bool displayTestPinned = false;

// ---------------------------------------------------------------
// SLEEP MODE
// ---------------------------------------------------------------
// When true, the device is in low-power "sleep" mode: backlight is off,
// LED is off, the AI chip rail is gated, and the loop() function is in a
// light-sleep cycle that wakes every 500 ms (timer) or any time the
// encoder rotates (GPIO41/42 fall low). Each wake polls the knob button
// over I2C to check whether the user has tapped to wake. Holding the
// encoder button for >= BUTTON_SLEEP_HOLD_MS (2 s) from IDLE puts us
// here; a single short press from this state takes us out.
static bool         g_sleeping        = false;
static unsigned long g_sleepEnteredAt = 0;

// ---------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------

// RGB triplet that best represents the pet's current emotional state.
// Used to colour the idle heartbeat so the LED isn't strobing white.
static void petStateColor(PetState s, uint8_t& r, uint8_t& g, uint8_t& b) {
  switch (s) {
    case PET_THRIVING:   r =   0; g = 255; b =  40; break;  // green
    case PET_HAPPY:      r =  80; g = 255; b = 120; break;  // mint
    case PET_TIRED:      r = 255; g = 200; b =  40; break;  // amber
    case PET_STRUGGLING: r = 255; g = 100; b =   0; break;  // orange
    case PET_CRITICAL:   r = 255; g =   0; b =   0; break;  // red
    default:             r =  40; g = 120; b = 255; break;  // fallback blue
  }
}

static const char* stateName(AppState s) {
  switch (s) {
    case STATE_IDLE:           return "IDLE";
    case STATE_HABIT_SELECT:   return "HABIT_SELECT";
    case STATE_HABIT_DETAIL:   return "HABIT_DETAIL";
    case STATE_CAPTURE_RITUAL: return "CAPTURE_RITUAL";
    case STATE_CELEBRATION:    return "CELEBRATION";
  }
  return "?";
}

void setState(AppState s) {
  if (s != currentState) {
    Serial.printf("[State] %s -> %s @ %lums\n",
                  stateName(currentState), stateName(s), millis());
  }
  previousState  = currentState;
  currentState   = s;
  stateEnteredAt = millis();
  if (s == STATE_CELEBRATION) celebCueStage = 0;
}

// ---------------------------------------------------------------
// checkDayRollover — called once per second from loop()
//
// Detects date change (real or from RTC epoch), applies vitality
// penalties for missed habits, adapts tomorrow's goals, logs
// a daily summary JSON to /logs/YYYY-MM-DD.json.
// ---------------------------------------------------------------
void checkDayRollover() {
  String today = rtc.getDate();
  if (today == lastKnownDate) return;   // Same day — nothing to do

  // First boot: just record today's date without penalising
  if (lastKnownDate.length() == 0) {
    lastKnownDate  = today;
    morningGreetShown = false;

    // Roll over so lastLogDate is set correctly on first boot
    habits.catchUpToDate(today);
    habits.save();
    return;
  }

  Serial.printf("[Main] New day detected: %s → %s\n",
                lastKnownDate.c_str(), today.c_str());

  const int missed = habits.catchUpToDate(today);
  if (missed > 0) {
    pet.setLastDialogueContext(DIALOGUE_MISSED_HABIT);
    Serial.printf("[Main] %d habits missed yesterday — vitality: %d\n",
                  missed, pet.getVitality());
  }

  // Write daily log to /logs/YYYY-MM-DD.json
  {
    JsonDocument log;
    log["date"]   = today;
    log["missed"] = missed;
    log["vitality_after"] = pet.getVitality();
    JsonArray habArr = log["habits"].to<JsonArray>();
    for (int i = 0; i < habits.getCount(); i++) {
      const Habit& h = habits.getHabit(i);
      JsonObject ho  = habArr.add<JsonObject>();
      ho["name"]    = h.name;
      ho["streak"]  = h.streak;
    }
    String logPath = String(PATH_LOGS_DIR) + "/" + lastKnownDate + ".json";
    storage.writeJSON(logPath, log);
  }

  pet.save();
  habits.save();

  lastKnownDate     = today;
  morningGreetShown = false;   // Show morning greeting on next idle frame
}

// ---------------------------------------------------------------
// Systematic test harness — serial command parser
//
// Commands (send via Serial monitor, 115200 baud, newline-terminated):
//
//   Basic
//     help | ?                    list all commands
//     stats                       heap / PSRAM / uptime / storage
//     reset                       ESP.restart()
//
//   Pet / habits (original)
//     T<epoch>                    set RTC (e.g. T1745000000)
//     L                           list habits
//     R                           reset pet vitality to default
//     pet <v>                     set vitality 0..100
//     state <name>                force app state: idle / select /
//                                 detail / capture / celebrate
//
//   Display
//     disp <color|bars|off>       fill entire screen (red/green/blue/
//                                 white/black/grey/bars/off)
//     bright <0-255>              backlight duty
//     duck <0-8>                  draw that duck frame centred
//     duck cycle                  cycle through all 9 frames (1s each)
//     test off                    unpin the test display, resume app
//
//   Input
//     knob                        live-dump GPIO41/42 + ISR counter
//                                 + PCA9535 button for 10 s
//     btn                         live-dump PCA9535 button for 10 s
//
//   LED / haptic
//     led <r,g,b>                 solid colour (e.g. led 255,0,0)
//     led off                     LED off
//     haptic <pattern>            fire pattern: heartbeat / celebration /
//                                 countdown / sos / buzz / none
//
//   Storage
//     ls [path]                   list files under path (default "/")
//     cat <path>                  dump file as text
//
//   Full sweep
//     test                        automated run of all subsystems;
//                                 prints [PASS]/[WARN]/[INFO] tags
// ---------------------------------------------------------------

static void cmd_help() {
  Serial.println(F("--- HabitTracker serial commands ---"));
  Serial.println(F("  help | ?            this list"));
  Serial.println(F("  stats               heap/psram/uptime/storage"));
  Serial.println(F("  reset               ESP.restart()"));
  Serial.println(F("  T<epoch>  L  R      (legacy) time/list/reset-pet"));
  Serial.println(F("  pet <0-100>         set vitality"));
  Serial.println(F("  state <name>        idle|select|detail|capture|celebrate"));
  Serial.println(F("  disp <red|green|blue|white|black|grey|bars|off>"));
  Serial.println(F("  bright <0-255>      backlight duty"));
  Serial.println(F("  duck <0-8> | cycle  draw duck frame / cycle all"));
  Serial.println(F("  preview             force-run capture preview (stub scene)"));
  Serial.println(F("  caminit             probe Himax AI chip (logs every step)"));
  Serial.println(F("  camstatus           dump SPI/SYNC/RST/AVAILABLE state"));
  Serial.println(F("  cambus              low-level bus probe (rst/cs/clock)"));
  Serial.println(F("  camsweep            try all 4 SPI modes, find the right one"));
  Serial.println(F("  camblind [AT+X]     send AT, blind-read 256B (bypass AVAIL)"));
  Serial.println(F("  campoke             send AT+ID? and hex-dump any bytes back"));
  Serial.println(F("  camsend <AT+...>    send an arbitrary AT command"));
  Serial.println(F("  camsensor <0-3>     AT+SENSOR=1,1,opt (0=240 1=416 2=480 3=640)"));
  Serial.println(F("  camimg              SAMPLE photo (640x480 if sensor accepts)"));
  Serial.println(F("  caminvoke           INVOKE + 416 (object-detection preview)"));
  Serial.println(F("  camdump             capture photo + stream base64 over serial"));
  Serial.println(F("  test off            unpin test display"));
  Serial.println(F("  knob                live-dump encoder for 10s"));
  Serial.println(F("  encraw              raw ISR edges (for calibration, 12s)"));
  Serial.println(F("  encdiv <1..4>       set edges-per-detent (default 2)"));
  Serial.println(F("  btn                 live-dump button for 10s"));
  Serial.println(F("  led <r,g,b> | off   solid LED colour"));
  Serial.println(F("  haptic <pattern>    heartbeat|celebration|countdown|sos|buzz|shutter|reward|none"));
  Serial.println(F("  speaker <test|tick|shutter|reward|tone|diag|mode N|sweep|burn|off>"));
  Serial.println(F("  ls [path]  cat <p>  LittleFS browse"));
  Serial.println(F("  sleep               enter sleep mode (same as 2s hold)"));
  Serial.println(F("  test                full automated subsystem sweep"));
}

static void cmd_stats() {
  Serial.println(F("--- STATS ---"));
  Serial.printf("  uptime        : %lu ms\n", millis());
  Serial.printf("  chip model    : %s  rev %d  cores %d\n",
                ESP.getChipModel(), ESP.getChipRevision(), ESP.getChipCores());
  Serial.printf("  cpu freq      : %u MHz\n", (unsigned)ESP.getCpuFreqMHz());
  Serial.printf("  flash size    : %lu bytes\n", (unsigned long)ESP.getFlashChipSize());
  Serial.printf("  heap total    : %lu  free %lu  min %lu\n",
                (unsigned long)ESP.getHeapSize(),
                (unsigned long)ESP.getFreeHeap(),
                (unsigned long)ESP.getMinFreeHeap());
  Serial.printf("  psram total   : %lu  free %lu\n",
                (unsigned long)ESP.getPsramSize(),
                (unsigned long)ESP.getFreePsram());
  Serial.printf("  littlefs      : used %u / %u bytes\n",
                (unsigned)LittleFS.usedBytes(),
                (unsigned)LittleFS.totalBytes());
  Serial.printf("  ioexpander    : %s\n",
                watcher_bsp_ioexp_ok() ? "OK" : "NOT READY");
  Serial.printf("  pet state     : %s  vitality %d  anim %d\n",
                (int)pet.getState() == PET_THRIVING   ? "THRIVING"   :
                (int)pet.getState() == PET_HAPPY      ? "HAPPY"      :
                (int)pet.getState() == PET_TIRED      ? "TIRED"      :
                (int)pet.getState() == PET_STRUGGLING ? "STRUGGLING" : "CRITICAL",
                pet.getVitality(), pet.getAnimFrame());
  Serial.printf("  app state     : %s\n", stateName(currentState));
  Serial.printf("  display pinned: %s\n", displayTestPinned ? "YES" : "no");
}

static uint16_t parseColor(const String& name) {
  if (name == "red")    return COLOR_RED;
  if (name == "green")  return COLOR_GREEN;
  if (name == "blue")   return COLOR_BLUE;
  if (name == "white")  return COLOR_WHITE;
  if (name == "black")  return COLOR_BLACK;
  if (name == "grey" || name == "gray") return COLOR_MID_GREY;
  if (name == "yellow") return COLOR_YELLOW;
  if (name == "cyan")   return COLOR_CYAN;
  if (name == "magenta")return COLOR_MAGENTA;
  return 0xFFFF;  // fallback white
}

static void cmd_disp(const String& arg) {
  if (arg == "off" || arg.length() == 0) {
    displayTestPinned = false;
    Serial.println(F("[disp] test pin cleared -- app resumes owning the display"));
    return;
  }
  if (arg == "bars") {
    gui.fillTestBars();
    displayTestPinned = true;
    Serial.println(F("[disp] R/G/B/W bars drawn (pinned)"));
    return;
  }
  uint16_t c = parseColor(arg);
  gui.fillTestColor(c);
  displayTestPinned = true;
  Serial.printf("[disp] filled 0x%04X (%s) — pinned\n", c, arg.c_str());
}

static void cmd_duck(const String& arg) {
  if (arg == "cycle") {
    Serial.println(F("[duck] cycling frames 0..8, 1s each"));
    for (int i = 0; i < DUCK_FRAME_COUNT; i++) {
      Serial.printf("  frame %d (%s)\n", i, duck_frameName(i));
      gui.drawDuckFrameCentred(i, 8);
      delay(1000);
    }
    displayTestPinned = true;
    Serial.println(F("[duck] cycle done (last frame pinned)"));
    return;
  }
  int idx = arg.toInt();
  gui.drawDuckFrameCentred(idx, 8);
  displayTestPinned = true;
  Serial.printf("[duck] frame %d (%s) drawn — pinned\n",
                idx, duck_frameName(idx));
}

// ================================================================
//  SSCMA / Himax camera commands — all prefixed `cam*` so they're easy
//  to find with `help`. These talk directly to the AI chip over SPI2,
//  independent of the UI state machine. Use these to diagnose whether
//  the Himax is alive and whether it can take an actual photo.
// ================================================================
static void cmd_preview();    // forward decl (cmd_camimg falls back to it)
static void enterSleepMode(); // forward decl (called from `sleep` cmd)
static void exitSleepMode();

static void cmd_caminit() {
  Serial.println(F("[caminit] re-probing SSCMA/Himax..."));
  bool ok = sscma.begin(/*verbose=*/true);
  Serial.printf("[caminit] result: %s  (ready=%d alive=%d)\n",
                ok ? "OK" : "FAILED",
                (int)sscma.isReady(), (int)sscma.isAlive());
}

static void cmd_camstatus() {
  sscma.dumpStatus();
}

static void cmd_cambus() {
  sscma.busProbe();
}

static void cmd_camsweep() {
  sscma.modeSweep();
}

static void cmd_camblind(const String& arg) {
  String a = arg; a.trim();
  if (a.length() == 0) a = "AT+ID?\r\n";
  else a += "\r\n";
  sscma.blindAt(a.c_str());
}

static void cmd_camcold(const String& arg) {
  String a = arg; a.trim();
  uint32_t ms = (uint32_t)a.toInt();
  if (ms < 200 || ms > 30000) ms = 4000;
  sscma.coldBootProbe(ms);
}

static void cmd_campoke() {
  // Most paranoid diagnostic: send `AT+ID?\r\n`, print every byte we
  // see over the next 2 seconds as hex + ASCII. Great way to tell if
  // the Himax is totally silent vs. garbage vs. valid framing.
  if (!sscma.isReady()) {
    Serial.println(F("[campoke] SSCMA not ready -- run caminit first"));
    return;
  }
  sscma.flushRx();
  if (!sscma.sendAT("AT+ID?\r\n")) {
    Serial.println(F("[campoke] sendAT failed"));
    return;
  }
  Serial.println(F("[campoke] watching for 2s — using blindAt to dump UART traffic..."));
  // The new UART-based driver handles framing internally; expose what
  // arrived via the public blindAt helper which already dumps both hex
  // and ASCII for the next chunk of UART traffic.
  sscma.blindAt("AT+ID?\r\n");
  Serial.println(F("[campoke] done. (UART path — for byte-level forensics use `camcold`)"));
}

static void cmd_camsend(const String& arg) {
  if (!sscma.isReady()) {
    Serial.println(F("[camsend] SSCMA not ready -- run caminit first"));
    return;
  }
  if (arg.length() == 0) {
    Serial.println(F("usage: camsend AT+ID?   (no CRLF needed, we add it)"));
    return;
  }
  String cmd = arg;
  if (!cmd.endsWith("\r\n")) cmd += "\r\n";
  Serial.printf("[camsend] -> %s", cmd.c_str());
  sscma.flushRx();
  sscma.sendAT(cmd.c_str());
  char resp[2048];
  size_t got = sscma.readJsonResponse(resp, sizeof(resp), 2500, /*debug=*/true);
  if (got) {
    Serial.printf("[camsend] <- %u bytes:\n  %s\n", (unsigned)got, resp);
  } else {
    Serial.println(F("[camsend] no response within 2.5s"));
  }
}

static void cmd_camimg() {
  // End-to-end real-photo flow. Prints serial proof at every step.
  Serial.println(F("[camimg] === REAL CAMERA CAPTURE ==="));
  if (!sscma.isAlive()) {
    Serial.println(F("[camimg] Himax not alive -- falling back to stub"));
    Serial.println(F("[camimg]   run `caminit` first; if it still fails the"
                     " AI chip is not responding and we cannot take a"
                     " real photo."));
    cmd_preview();
    return;
  }

  const size_t JPEG_CAP = 256 * 1024;
  static uint8_t* jpeg = nullptr;
  if (!jpeg) jpeg = (uint8_t*)ps_malloc(JPEG_CAP);
  if (!jpeg) jpeg = (uint8_t*)malloc(JPEG_CAP);
  if (!jpeg) {
    Serial.println(F("[camimg] out of memory"));
    return;
  }
  size_t jpegLen = 0;
  Serial.printf("[camimg] SAMPLE path (compile opt=%d) ...\n",
                SSCMA_CAPTURE_SENSOR_OPT);
  uint32_t t0 = millis();
  bool ok = sscma.captureJpeg(jpeg, JPEG_CAP, &jpegLen, 15000, /*debug=*/true);
  if (!ok || jpegLen == 0) {
    Serial.println(F("[camimg] SAMPLE failed; trying INVOKE fallback..."));
    ok = sscma.captureJpegInvoke(jpeg, JPEG_CAP, &jpegLen,
                                 /*times=*/-1, /*filter=*/false,
                                 /*show=*/true, 10000, /*debug=*/true);
  }
  uint32_t dt = millis() - t0;
  if (!ok) {
    Serial.printf("[camimg] FAILED after %u ms\n", (unsigned)dt);
    return;
  }
  Serial.printf("[camimg] got JPEG %u bytes in %u ms  [FF D8 FF ..]\n",
                (unsigned)jpegLen, (unsigned)dt);
  Serial.print(F("[camimg] first 32 bytes:"));
  for (size_t i = 0; i < 32 && i < jpegLen; i++) Serial.printf(" %02X", jpeg[i]);
  Serial.println();

  // Hand to the GUI using the existing JPEG display path.
  // We reuse HabitCamBuffer so gui.drawCaptureRitual() can blit it.
  static HabitCamBuffer realFb;
  realFb.data   = jpeg;
  realFb.len    = jpegLen;
  realFb.bmp565 = nullptr;   // force JPEG path
  realFb.bmpW = realFb.bmpH = 0;
  realFb.isStub = false;
  gui.drawCaptureRitual(0, &realFb);
  displayTestPinned = true;
  Serial.println(F("[camimg] displayed. `test off` to resume."));
  // jpeg buffer is static/reused; no per-command leaks.
}

static void cmd_camsensor(const String& arg) {
  if (!sscma.isReady()) {
    Serial.println(F("[camsensor] SSCMA not ready — run caminit first"));
    return;
  }
  String a = arg;
  a.trim();
  if (a.length() == 0) {
    Serial.println(F("usage: camsensor <0..3>"));
    Serial.println(F("  0=240x240  1=416x416  2=480x480  3=640x480 (Seeed tf_module_ai_camera.h)"));
    return;
  }
  int opt = a.toInt();
  if (opt < 0 || opt > 3) {
    Serial.println(F("[camsensor] opt must be 0..3"));
    return;
  }
  if (sscma.setSensor(1, true, opt)) {
    Serial.printf("[camsensor] OK: AT+SENSOR=1,1,%d\n", opt);
  } else {
    Serial.printf("[camsensor] FAILED: AT+SENSOR=1,1,%d\n", opt);
  }
}

static void cmd_caminvoke() {
  Serial.println(F("[caminvoke] === INVOKE (416 + model) path ==="));
  if (!sscma.isAlive()) {
    Serial.println(F("[caminvoke] Himax not alive — run caminit"));
    return;
  }
  const size_t JPEG_CAP = 256 * 1024;
  static uint8_t* jpeg = nullptr;
  if (!jpeg) jpeg = (uint8_t*)ps_malloc(JPEG_CAP);
  if (!jpeg) jpeg = (uint8_t*)malloc(JPEG_CAP);
  if (!jpeg) {
    Serial.println(F("[caminvoke] out of memory"));
    return;
  }
  size_t jpegLen = 0;
  uint32_t t0 = millis();
  bool ok = sscma.captureJpegInvoke(jpeg, JPEG_CAP, &jpegLen,
                                     /*times=*/-1, /*filter=*/false,
                                     /*show=*/true, 15000, /*debug=*/true);
  uint32_t dt = millis() - t0;
  if (!ok) {
    Serial.printf("[caminvoke] FAILED after %u ms\n", (unsigned)dt);
    return;
  }
  Serial.printf("[caminvoke] got JPEG %u bytes in %u ms\n",
                (unsigned)jpegLen, (unsigned)dt);
  static HabitCamBuffer realFb;
  realFb.data   = jpeg;
  realFb.len    = jpegLen;
  realFb.bmp565 = nullptr;
  realFb.bmpW = realFb.bmpH = 0;
  realFb.isStub = false;
  gui.drawCaptureRitual(0, &realFb);
  displayTestPinned = true;
  Serial.println(F("[caminvoke] displayed. `test off` to resume."));
}

static uint32_t _crc32(const uint8_t* data, size_t len) {
  uint32_t crc = 0xFFFFFFFFu;
  for (size_t i = 0; i < len; i++) {
    crc ^= data[i];
    for (int j = 0; j < 8; j++) {
      uint32_t m = (uint32_t)-(int)(crc & 1u);
      crc = (crc >> 1) ^ (0xEDB88320u & m);
    }
  }
  return ~crc;
}

static void _serialWriteB64(const uint8_t* data, size_t len) {
  static const char T[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  char out[77];                  // 76 chars + NUL
  int oi = 0;
  size_t i = 0;
  while (i < len) {
    size_t rem = len - i;
    uint32_t a = data[i++];
    uint32_t b = (rem > 1) ? data[i++] : 0;
    uint32_t c = (rem > 2) ? data[i++] : 0;
    uint32_t n = (a << 16) | (b << 8) | c;
    out[oi++] = T[(n >> 18) & 63];
    out[oi++] = T[(n >> 12) & 63];
    out[oi++] = (rem > 1) ? T[(n >> 6) & 63] : '=';
    out[oi++] = (rem > 2) ? T[n & 63] : '=';
    if (oi >= 76) {
      out[oi] = '\0';
      Serial.println(out);
      oi = 0;
      yield();
    }
  }
  if (oi > 0) {
    out[oi] = '\0';
    Serial.println(out);
  }
}

static void cmd_camdump() {
  Serial.println(F("[camdump] capture + serial JPEG dump"));
  if (!sscma.isAlive()) {
    Serial.println(F("[camdump] Himax not alive — run caminit first"));
    return;
  }

  const size_t JPEG_CAP = 256 * 1024;
  static uint8_t* jpeg = nullptr;
  if (!jpeg) jpeg = (uint8_t*)ps_malloc(JPEG_CAP);
  if (!jpeg) jpeg = (uint8_t*)malloc(JPEG_CAP);
  if (!jpeg) {
    Serial.println(F("[camdump] out of memory"));
    return;
  }

  size_t jpegLen = 0;
  uint32_t t0 = millis();
  bool ok = sscma.captureJpeg(jpeg, JPEG_CAP, &jpegLen, 7000, /*debug=*/false);
  if (!ok || jpegLen == 0) {
    Serial.println(F("[camdump] SAMPLE failed; trying INVOKE"));
    ok = sscma.captureJpegInvoke(jpeg, JPEG_CAP, &jpegLen,
                                 /*times=*/-1, /*filter=*/false,
                                 /*show=*/true, 7000, /*debug=*/false);
  }
  uint32_t dt = millis() - t0;
  if (!ok || jpegLen < 64) {
    Serial.printf("[camdump] FAILED after %u ms\n", (unsigned)dt);
    return;
  }

  uint32_t crc = _crc32(jpeg, jpegLen);
  Serial.printf("[camdump] OK len=%u crc32=%08lX dt=%u ms head=%02X%02X%02X tail=%02X%02X\n",
                (unsigned)jpegLen, (unsigned long)crc, (unsigned)dt,
                jpeg[0], jpeg[1], jpeg[2], jpeg[jpegLen - 2], jpeg[jpegLen - 1]);
  Serial.println(F("CAMJPEG_BEGIN"));
  _serialWriteB64(jpeg, jpegLen);
  Serial.println(F("CAMJPEG_END"));
}

static void cmd_preview() {
  // Manually exercise the capture-preview code path so we can see the
  // full JPEG decode + fallback logs over serial without having to navigate
  // the UI. Mirrors what handleCaptureRitual() does after its countdown.
  Serial.println(F("[preview] calling camera.capture()..."));
  HabitCamBuffer* fb = camera.isReady() ? camera.capture() : nullptr;
  if (!fb || !fb->data || fb->len == 0) {
    Serial.println(F("[preview] camera returned null frame"));
    return;
  }
  Serial.printf("[preview] got frame: %u bytes @ %p\n",
                (unsigned)fb->len, (void*)fb->data);
  Serial.println(F("[preview] drawing capture ritual preview (holds 4s)..."));
  gui.drawCaptureRitual(0, fb);
  camera.returnFrame(fb);
  displayTestPinned = true;           // keep preview on screen for inspection
  Serial.println(F("[preview] preview drawn and pinned — `test off` to resume"));
}

static void cmd_state(const String& arg) {
  if      (arg == "idle")       setState(STATE_IDLE);
  else if (arg == "select")     setState(STATE_HABIT_SELECT);
  else if (arg == "detail")     setState(STATE_HABIT_DETAIL);
  else if (arg == "capture")    setState(STATE_CAPTURE_RITUAL);
  else if (arg == "celebrate")  setState(STATE_CELEBRATION);
  else { Serial.println(F("usage: state <idle|select|detail|capture|celebrate>")); return; }
  displayTestPinned = false;
  Serial.printf("[state] forced to %s\n", stateName(currentState));
}

static void cmd_led(const String& arg) {
  if (arg == "off" || arg.length() == 0) {
    haptic.stop();
    Serial.println(F("[led] off"));
    return;
  }
  int r = 0, g = 0, b = 0;
  if (sscanf(arg.c_str(), "%d,%d,%d", &r, &g, &b) != 3) {
    Serial.println(F("usage: led <r,g,b>    (0..255 each)  |  led off"));
    return;
  }
  haptic.setSolid((uint8_t)constrain(r, 0, 255),
                  (uint8_t)constrain(g, 0, 255),
                  (uint8_t)constrain(b, 0, 255));
  Serial.printf("[led] solid %d,%d,%d\n", r, g, b);
}

static void cmd_haptic(const String& arg) {
  HapticPattern p = HAPTIC_NONE;
  if      (arg == "heartbeat")    p = HAPTIC_HEARTBEAT;
  else if (arg == "celebration")  p = HAPTIC_CELEBRATION;
  else if (arg == "countdown")    p = HAPTIC_COUNTDOWN;
  else if (arg == "sos")          p = HAPTIC_SOS;
  else if (arg == "buzz")         p = HAPTIC_SOFT_BUZZ;
  else if (arg == "shutter")      p = HAPTIC_SHUTTER;
  else if (arg == "reward")       p = HAPTIC_REWARD;
  else if (arg == "none" || arg == "off") { haptic.stop(); Serial.println(F("[haptic] stopped")); return; }
  else { Serial.println(F("usage: haptic <heartbeat|celebration|countdown|sos|buzz|shutter|reward|none>")); return; }
  haptic.play(p);
  Serial.printf("[haptic] playing %s\n", arg.c_str());
}

static void cmd_speaker(const String& arg) {
  if (!audioFx.isReady()) {
    Serial.println(F("[speaker] not ready (codec/i2s unavailable)"));
    return;
  }
  if (arg == "diag") {
    Serial.printf("[speaker] ready=%s busy=%s\n",
                  audioFx.isReady() ? "yes" : "no",
                  audioFx.isBusy() ? "yes" : "no");
    Serial.printf("[speaker] mode=%u (%s)\n",
                  (unsigned)audioFx.getMode(), audioFx.getModeName());
    bool a = audioFx.play(SFX_TICK);
    delay(120);
    bool b = audioFx.play(SFX_TICK);
    Serial.printf("[speaker] quick retrigger test: first=%s second=%s\n",
                  a ? "ok" : "busy/fail", b ? "ok" : "busy/fail");
    return;
  }
  if (arg.startsWith("mode ")) {
    int n = arg.substring(5).toInt();
    bool okm = audioFx.setMode((uint8_t)n);
    Serial.printf("[speaker] mode set %d -> %s (%s)\n",
                  n, okm ? "ok" : "failed", audioFx.getModeName());
    return;
  }
  if (arg == "sweep") {
    int maxMode = (int)audioFx.getModeCount() - 1;
    Serial.printf("[speaker] sweeping modes 0..%d (listen for any tone)\n", maxMode);
    for (int m = 0; m <= maxMode; m++) {
      audioFx.waitIdle(2500);
      bool okm = audioFx.setMode((uint8_t)m);
      Serial.printf("[speaker] mode %d (%s) init=%s\n",
                    m, audioFx.getModeName(), okm ? "ok" : "fail");
      if (okm) {
        audioFx.play(SFX_TICK);
        audioFx.waitIdle(1800);
        audioFx.play(SFX_SHUTTER);
        audioFx.waitIdle(2000);
      } else {
        delay(120);
      }
    }
    return;
  }
  if (arg == "burn") {
    int maxMode = (int)audioFx.getModeCount() - 1;
    Serial.printf("[speaker] burn test start (modes 0..%d, 5 rounds)\n", maxMode);
    for (int round = 0; round < 5; round++) {
      Serial.printf("[speaker] burn round %d/5\n", round + 1);
      for (int m = 0; m <= maxMode; m++) {
        audioFx.waitIdle(2500);
        bool okm = audioFx.setMode((uint8_t)m);
        Serial.printf("[speaker] burn mode %d (%s) init=%s\n",
                      m, audioFx.getModeName(), okm ? "ok" : "fail");
        if (!okm) { delay(120); continue; }
        audioFx.play(SFX_TICK);
        audioFx.waitIdle(2200);
        audioFx.play(SFX_SHUTTER);
        audioFx.waitIdle(2400);
        audioFx.play(SFX_REWARD);
        audioFx.waitIdle(3200);
      }
    }
    Serial.println(F("[speaker] burn test complete"));
    return;
  }
  if (arg == "off" || arg == "stop" || arg == "none") {
    audioFx.stop();
    Serial.println(F("[speaker] stopped"));
    return;
  }

  bool ok = false;
  if (arg == "test") {
    ok = audioFx.play(SFX_TICK);
    delay(70);
    ok = audioFx.play(SFX_SHUTTER) || ok;
    delay(90);
    ok = audioFx.play(SFX_REWARD) || ok;
  } else if (arg == "tick") {
    ok = audioFx.play(SFX_TICK);
  } else if (arg == "shutter") {
    ok = audioFx.play(SFX_SHUTTER);
  } else if (arg == "reward") {
    ok = audioFx.play(SFX_REWARD);
  } else if (arg == "tone") {
    ok = audioFx.play(SFX_TONE_LONG);
  } else {
    Serial.println(F("usage: speaker <test|tick|shutter|reward|tone|diag|mode N|sweep|burn|off>"));
    return;
  }
  Serial.printf("[speaker] %s\n", ok ? "triggered" : "busy");
}

static void cmd_encdiv(const String& arg) {
  if (arg.length() == 0) {
    Serial.printf("[encdiv] current = %d edges per detent\n",
                  encoder.getStepsPerDetent());
    Serial.println(F("usage: encdiv <1|2|3|4>   (2 default for Watcher knob)"));
    return;
  }
  int n = arg.toInt();
  encoder.setStepsPerDetent(n);
  Serial.printf("[encdiv] set to %d (actual = %d)\n",
                n, encoder.getStepsPerDetent());
}

static void cmd_encraw() {
  // Raw ISR-edge dump: prints the accumulator every time it CHANGES.
  // Physically turn the knob one detent at a time and count the edges
  // between prints — that number is the right `encdiv` for this hardware.
  Serial.println(F("[encraw] 12 s — turn the knob one detent at a time and "
                   "count the edges between prints. Divisor = that count."));
  unsigned long end   = millis() + 12000;
  int           last  = encoder.peekRawAccum();
  unsigned long lastP = millis();
  Serial.printf("  t=%lums  accum=%d (initial)\n", millis(), last);
  while (millis() < end) {
    int now = encoder.peekRawAccum();
    if (now != last) {
      Serial.printf("  t=%lums  accum=%d  (delta=%+d from last)\n",
                    millis(), now, now - last);
      last  = now;
      lastP = millis();
    } else if (millis() - lastP >= 2000) {
      Serial.printf("  t=%lums  accum=%d  (idle)\n", millis(), now);
      lastP = millis();
    }
    encoder.update();
    haptic.update();
    delay(2);
  }
  int final = encoder.peekRawAccum();
  Serial.printf("[encraw] done — final accum=%d, current divisor=%d\n",
                final, encoder.getStepsPerDetent());
}

static void cmd_knob() {
  Serial.println(F("[knob] live dump for 10s — turn the knob and click; Ctrl-C to stop early"));
  unsigned long end = millis() + 10000;
  int lastDelta = 0;
  bool lastBtn  = watcher_knob_button_is_pressed();
  unsigned long tick = millis();
  while (millis() < end) {
    int d = encoder.getDelta();
    if (d != 0) { lastDelta += d; Serial.printf("  delta=%+d  total=%d\n", d, lastDelta); }
    bool b = watcher_knob_button_is_pressed();
    if (b != lastBtn) {
      Serial.printf("  button -> %s\n", b ? "DOWN" : "up");
      lastBtn = b;
    }
    if (millis() - tick >= 500) {
      tick = millis();
      uint8_t a = digitalRead(PIN_KNOB_A);
      uint8_t bb = digitalRead(PIN_KNOB_B);
      Serial.printf("  [t=%lums] A=%u B=%u btn=%s  accumTotal=%d\n",
                    millis(), a, bb, lastBtn ? "DOWN" : "up", lastDelta);
    }
    encoder.update();
    haptic.update();
    delay(10);
  }
  Serial.printf("[knob] done — net rotation %d  final button %s\n",
                lastDelta, lastBtn ? "DOWN" : "up");
}

static void cmd_btn() {
  Serial.println(F("[btn] live dump for 10s — click the knob"));
  unsigned long end = millis() + 10000;
  bool lastBtn = watcher_knob_button_is_pressed();
  Serial.printf("  initial: %s\n", lastBtn ? "DOWN" : "up");
  int edges = 0;
  while (millis() < end) {
    bool b = watcher_knob_button_is_pressed();
    if (b != lastBtn) {
      edges++;
      Serial.printf("  [t=%lums] edge -> %s\n", millis(), b ? "DOWN" : "up");
      lastBtn = b;
    }
    delay(5);
  }
  Serial.printf("[btn] done — %d edges detected\n", edges);
}

static void cmd_ls(const String& path) {
  String p = path.length() ? path : String("/");
  Serial.printf("[ls] %s\n", p.c_str());
  File d = LittleFS.open(p);
  if (!d) { Serial.println(F("  open failed")); return; }
  if (!d.isDirectory()) {
    Serial.printf("  (file) %u bytes\n", (unsigned)d.size());
    d.close();
    return;
  }
  File e = d.openNextFile();
  int count = 0;
  while (e) {
    Serial.printf("  %s %s  %u bytes\n",
                  e.isDirectory() ? "[dir]" : "     ",
                  e.name(),
                  (unsigned)e.size());
    e = d.openNextFile();
    count++;
  }
  d.close();
  Serial.printf("  %d entries\n", count);
}

static void cmd_cat(const String& path) {
  if (path.length() == 0) { Serial.println(F("usage: cat <path>")); return; }
  File f = LittleFS.open(path, "r");
  if (!f) { Serial.printf("[cat] cannot open %s\n", path.c_str()); return; }
  Serial.printf("[cat] %s (%u bytes)\n", path.c_str(), (unsigned)f.size());
  while (f.available()) Serial.write(f.read());
  Serial.println();
  f.close();
}

// Full automated subsystem sweep. Everything prints a [TAG] so the
// output can be grepped / diffed by the desktop capture script.
static void cmd_test_sweep() {
  Serial.println(F("========== [TEST] SYSTEMATIC SWEEP BEGIN =========="));
  cmd_stats();

  // --- LED ---
  Serial.println(F("[TEST] LED color sweep (R, G, B, W, off, 400ms each)"));
  struct C { const char* n; uint8_t r, g, b; } colors[] = {
    {"red",   255, 0,   0},
    {"green", 0,   255, 0},
    {"blue",  0,   0,   255},
    {"white", 255, 255, 255},
  };
  for (auto& c : colors) {
    Serial.printf("  [TEST] led %s\n", c.n);
    haptic.setSolid(c.r, c.g, c.b);
    delay(400);
  }
  haptic.stop();
  Serial.println(F("  [TEST] led off"));

  // --- Haptic patterns ---
  Serial.println(F("[TEST] haptic pattern sweep"));
  const HapticPattern pats[] = {
    HAPTIC_SOFT_BUZZ, HAPTIC_COUNTDOWN, HAPTIC_HEARTBEAT,
    HAPTIC_CELEBRATION, HAPTIC_SOS
  };
  const char* patNames[] = {"buzz", "countdown", "heartbeat",
                            "celebration", "sos"};
  for (size_t i = 0; i < sizeof(pats) / sizeof(pats[0]); i++) {
    Serial.printf("  [TEST] pattern %s\n", patNames[i]);
    haptic.play(pats[i]);
    // Let each pattern run to completion (max ~500ms) plus margin
    unsigned long pend = millis() + 1200;
    while (millis() < pend) { haptic.update(); delay(10); }
  }

  // --- I2C expander ---
  Serial.printf("[TEST] io expander: %s\n",
                watcher_bsp_ioexp_ok() ? "PASS" : "WARN (not ready)");

  // --- Encoder raw pins ---
  Serial.println(F("[TEST] encoder raw pins (expect both HIGH at rest)"));
  uint8_t ra = digitalRead(PIN_KNOB_A);
  uint8_t rb = digitalRead(PIN_KNOB_B);
  bool    btnNow = watcher_knob_button_is_pressed();
  Serial.printf("  [TEST] A=%u B=%u btn=%s\n", ra, rb, btnNow ? "DOWN" : "up");
  if (ra == 1 && rb == 1) Serial.println(F("  [TEST] encoder pins PASS (both HIGH)"));
  else                     Serial.println(F("  [TEST] encoder pins WARN (not at rest HIGH)"));

  // --- Display fills ---
  Serial.println(F("[TEST] display fill sweep (R, G, B, W, black, bars — 700ms each)"));
  uint16_t fills[] = { COLOR_RED, COLOR_GREEN, COLOR_BLUE,
                       COLOR_WHITE, COLOR_BLACK };
  const char* fillN[] = { "red", "green", "blue", "white", "black" };
  for (size_t i = 0; i < sizeof(fills) / sizeof(fills[0]); i++) {
    Serial.printf("  [TEST] fill %s\n", fillN[i]);
    gui.fillTestColor(fills[i]);
    delay(700);
  }
  Serial.println(F("  [TEST] bars"));
  gui.fillTestBars();
  delay(900);

  // --- Duck frames ---
  Serial.println(F("[TEST] duck frames (0..8, 700ms each)"));
  for (int i = 0; i < DUCK_FRAME_COUNT; i++) {
    Serial.printf("  [TEST] duck %d (%s)\n", i, duck_frameName(i));
    gui.drawDuckFrameCentred(i, 8);
    delay(700);
  }

  // --- Storage ---
  Serial.printf("[TEST] storage used=%u/%u bytes %s\n",
                (unsigned)LittleFS.usedBytes(),
                (unsigned)LittleFS.totalBytes(),
                LittleFS.totalBytes() > 0 ? "PASS" : "FAIL");

  // --- Pet + habits ---
  Serial.printf("[TEST] pet vitality=%d  habits=%d  %s\n",
                pet.getVitality(), habits.getCount(),
                habits.getCount() > 0 ? "PASS" : "WARN (no habits)");

  Serial.println(F("[TEST] sweep complete. display pinned on last duck frame."));
  Serial.println(F("[TEST] send `test off` to resume the app, or `state idle`"));
  displayTestPinned = true;
  Serial.println(F("========== [TEST] SYSTEMATIC SWEEP END =========="));
}

void checkSerialConfig() {
  if (!Serial.available()) return;

  String line = Serial.readStringUntil('\n');
  line.trim();
  if (line.length() == 0) return;

  // Split command + rest for simple `verb arg...` handling.
  int sp = line.indexOf(' ');
  String verb = (sp < 0) ? line : line.substring(0, sp);
  String rest = (sp < 0) ? String("") : line.substring(sp + 1);
  rest.trim();
  verb.toLowerCase();

  // --- legacy single-char commands ---
  if (verb.startsWith("T") && verb.length() > 1) {
    time_t epoch = (time_t)verb.substring(1).toInt();
    rtc.setTime(epoch);
    Serial.println("RTC time set.");
    return;
  }
  if (verb == "l") {
    Serial.printf("Habits (%d):\n", habits.getCount());
    for (int i = 0; i < habits.getCount(); i++) {
      const Habit& h = habits.getHabit(i);
      Serial.printf("  [%d] %s — %d/%d %s, streak=%d\n",
                    i, h.name.c_str(),
                    h.completedToday, h.goalToday, h.unit.c_str(),
                    h.streak);
    }
    return;
  }
  if (verb == "r") {
    pet.setVitality(VITALITY_START);
    pet.save();
    Serial.printf("Pet vitality reset to %d\n", VITALITY_START);
    return;
  }

  // --- new verbs ---
  if (verb == "help" || verb == "?")  { cmd_help(); return; }
  if (verb == "stats")                { cmd_stats(); return; }
  if (verb == "reset")                { Serial.println(F("[reset] restarting...")); delay(50); ESP.restart(); return; }
  if (verb == "pet") {
    int v = rest.toInt();
    pet.setVitality(v);
    pet.save();
    Serial.printf("[pet] vitality=%d  state=%d\n", pet.getVitality(), (int)pet.getState());
    return;
  }
  if (verb == "state") { rest.toLowerCase(); cmd_state(rest); return; }
  if (verb == "disp")  { rest.toLowerCase(); cmd_disp(rest);  return; }
  if (verb == "bright") {
    int v = constrain(rest.toInt(), 0, 255);
    gui.setBrightness((uint8_t)v);
    Serial.printf("[bright] %d/255\n", v);
    return;
  }
  if (verb == "duck")   { rest.toLowerCase(); cmd_duck(rest);   return; }
  if (verb == "preview"){ cmd_preview(); return; }
  if (verb == "caminit")  { cmd_caminit();   return; }
  if (verb == "camstatus"){ cmd_camstatus(); return; }
  if (verb == "cambus")   { cmd_cambus();    return; }
  if (verb == "camsweep") { cmd_camsweep();  return; }
  if (verb == "camblind") { cmd_camblind(rest); return; }
  if (verb == "camcold")  { cmd_camcold(rest);  return; }
  if (verb == "campoke")  { cmd_campoke();   return; }
  if (verb == "camsend")  { cmd_camsend(rest);return; }
  if (verb == "camsensor"){ cmd_camsensor(rest); return; }
  if (verb == "camimg")   { cmd_camimg();    return; }
  if (verb == "caminvoke"){ cmd_caminvoke(); return; }
  if (verb == "camdump")  { cmd_camdump();   return; }
  if (verb == "knob")   { cmd_knob();  return; }
  if (verb == "encraw") { cmd_encraw(); return; }
  if (verb == "encdiv") { cmd_encdiv(rest); return; }
  if (verb == "btn")    { cmd_btn();   return; }
  if (verb == "led")    { rest.toLowerCase(); cmd_led(rest);    return; }
  if (verb == "haptic") { rest.toLowerCase(); cmd_haptic(rest); return; }
  if (verb == "speaker"){ rest.toLowerCase(); cmd_speaker(rest); return; }
  if (verb == "ls")     { cmd_ls(rest);  return; }
  if (verb == "cat")    { cmd_cat(rest); return; }
  if (verb == "sleep")  { enterSleepMode(); return; }
  if (verb == "test") {
    if (rest == "off") { displayTestPinned = false; Serial.println(F("[test] display unpinned")); return; }
    cmd_test_sweep();
    return;
  }

  Serial.printf("Unknown command: '%s'. Type `help` for the list.\n", line.c_str());
}

// ================================================================
//  SLEEP MODE
// ================================================================

// Power-gate the AI chip rail (BSP_PWR_AI_CHIP, expander pin 11).
// We don't go through the SSCMA driver because it caches state and we
// just want a raw bit-flip on the I2C expander.
static bool _setAiPowerRail(bool on) {
  // PCA9535 OUT register is 0x02 / 0x03.
  Wire.beginTransmission(WATCHER_IOEXP_ADDR);
  Wire.write(0x02);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom((int)WATCHER_IOEXP_ADDR, 2) != 2) return false;
  uint16_t cur = (uint16_t)Wire.read() | ((uint16_t)Wire.read() << 8);
  uint16_t next = on ? (cur | BSP_PWR_AI_CHIP) : (cur & ~BSP_PWR_AI_CHIP);
  if (next == cur) return true;
  Wire.beginTransmission(WATCHER_IOEXP_ADDR);
  Wire.write(0x02);
  Wire.write((uint8_t)(next & 0xFF));
  Wire.write((uint8_t)(next >> 8));
  return Wire.endTransmission() == 0;
}

static void enterSleepMode() {
  if (g_sleeping) return;
  Serial.println(F("[Sleep] Long-press detected — entering sleep mode"));
  // Visual ack first, while the screen is still lit.
  gui.drawSleepScreen();
  audioFx.play(SFX_TICK);
  delay(900);

  // Now power down the visible peripherals.
  gui.setBrightness(0);
  haptic.stop();
  // Power-gate the AI chip rail to save the most current. We don't bother
  // calling sscma.* here because sscma.begin() will re-probe on wake if
  // the user explicitly asks for the camera path.
  _setAiPowerRail(false);

  g_sleeping       = true;
  g_sleepEnteredAt = millis();
  Serial.println(F("[Sleep] LCD off, LED off, AI rail gated. "
                   "Tap the knob to wake."));
}

static void exitSleepMode() {
  if (!g_sleeping) return;
  Serial.printf("[Sleep] Wake requested after %lu ms in sleep\n",
                millis() - g_sleepEnteredAt);
  // Restore power rail first so any later sscma.begin() call works.
  _setAiPowerRail(true);
  delay(50);

  // Restore backlight to default.
  gui.setBrightness((uint8_t)((uint32_t)BL_DEFAULT_PERCENT * 255u / 100u));
  gui.drawWakeScreen();
  audioFx.play(SFX_TICK);
  delay(400);

  g_sleeping = false;
  setState(STATE_IDLE);
  Serial.println(F("[Sleep] Awake. Back to IDLE."));
}

// Run one iteration of the sleep loop: light-sleep with timer + GPIO
// wake, then check the button via I2C. Returns true if the user
// requested wake (caller should exitSleepMode()).
static bool _sleepTickShouldWake() {
  // Configure wake sources for this nap.
  //   * Knob rotation pins (KNOB_A/B): real GPIOs, not RTC-IO, so they
  //     cannot wake DEEP sleep — but they DO wake light sleep when they
  //     fall LOW, which is exactly what happens at every detent.
  //   * 500 ms timer: periodic check of the I2C button (the encoder
  //     centre push lives on the PCA9535 expander, which can't wake the
  //     ESP32 by itself).
  gpio_wakeup_enable((gpio_num_t)PIN_KNOB_A, GPIO_INTR_LOW_LEVEL);
  gpio_wakeup_enable((gpio_num_t)PIN_KNOB_B, GPIO_INTR_LOW_LEVEL);
  esp_sleep_enable_gpio_wakeup();
  esp_sleep_enable_timer_wakeup(500ULL * 1000ULL);
  // UART wake-up: any incoming byte on UART0 (Serial) brings us back. We
  // need at least 3 characters to be RX-buffered before the threshold
  // counter triggers — Arduino sends `\r\n` plus the typed char so a
  // single keystroke is enough.
  uart_set_wakeup_threshold(UART_NUM_0, 3);
  esp_sleep_enable_uart_wakeup(UART_NUM_0);

  esp_light_sleep_start();

  // After waking, give I2C a moment then poll the button.
  delayMicroseconds(200);
  bool pressed = watcher_knob_button_is_pressed();
  if (pressed) {
    // Debounce: confirm after 30 ms — discards spurious noise.
    delay(30);
    if (watcher_knob_button_is_pressed()) return true;
  }
  return false;
}

// Day rollover + vitality decay. During sleep, millis() may not advance while in
// light sleep, so we always run vitality (RTC epoch gates the actual loss).
static void tickBackgroundSystems(unsigned long now, bool vitalityEveryWake) {
  if (now - lastDayCheck >= 5000) {
    lastDayCheck = now;
    checkDayRollover();
  } else if (vitalityEveryWake) {
    checkDayRollover();
  }

  if (vitalityEveryWake || (now - lastVitalityDecay >= 30000)) {
    lastVitalityDecay = now;
    pet.tickVitalityDecay(rtc.getTimestamp());
  }
}

static void captureWorkerTask(void*) {
  HabitCamBuffer* fb = nullptr;
  if (camera.isReady()) {
    fb = camera.capture();
  }
  captureTaskFrame = fb;
  captureTaskDone = true;
  captureTaskStarted = false;
  captureTaskHandle = nullptr;
  vTaskDelete(nullptr);
}

// ================================================================
//  STATE HANDLERS
// ================================================================

void handleIdle() {
  unsigned long now = millis();

  // Show morning greeting once per day when hour < 12
  if (!morningGreetShown && rtc.getHour() < 12) {
    pet.setLastDialogueContext(DIALOGUE_IDLE_MORNING);
    morningGreetShown = true;
  } else if (pet.getState() == PET_CRITICAL) {
    pet.setLastDialogueContext(DIALOGUE_CRITICAL);
  } else {
    pet.setLastDialogueContext(DIALOGUE_IDLE_GENERAL);
  }

  // No periodic LED heartbeat feedback; keep idle visually calm.

  if (!displayTestPinned) {
    gui.drawIdle(pet, habits);
  }

  // Long-press (>= 2 s) → enter sleep mode. Checked BEFORE isPressed()
  // because the sleep gesture intentionally swallows the matching
  // release; we do not want it to also trigger habit-select navigation.
  if (encoder.isSleepHeld()) {
    enterSleepMode();
    return;
  }

  // Press → go to habit select
  if (encoder.isPressed()) {
    audioFx.play(SFX_TICK);
    setState(STATE_HABIT_SELECT);
    return;
  }
}

// ---------------------------------------------------------------
void handleHabitSelect() {
  int delta = encoder.getDelta();
  if (delta != 0) {
    audioFx.play(SFX_TICK);
    selectedHabitIdx = (selectedHabitIdx + delta + habits.getCount()) % habits.getCount();
  }

  if (!displayTestPinned) gui.drawHabitSelect(habits, selectedHabitIdx);

  // Short press → open detail
  if (encoder.isPressed()) {
    audioFx.play(SFX_TICK);
    setState(STATE_HABIT_DETAIL);
    return;
  }

  // Long hold → back to idle
  if (encoder.isHeld()) {
    setState(STATE_IDLE);
    return;
  }
}

// ---------------------------------------------------------------
void handleHabitDetail() {
  Habit& h = habits.getHabit(selectedHabitIdx);
  if (!displayTestPinned) gui.drawHabitDetail(h);

  // Short press → start photo capture ritual
  if (encoder.isPressed()) {
    captureCountdown  = 3;
    captureReady      = false;
    captureFlashFired = false;
    captureFlashActive = false;
    captureFlashStartedAt = 0;
    captureLoadingPrimed = false;
    captureLoadingStarted = 0;
    captureTaskStarted = false;
    captureTaskDone = false;
    captureTaskFrame = nullptr;
    captureTaskHandle = nullptr;
    lastCountdownTick = millis();
    audioFx.play(SFX_TICK);
    setState(STATE_CAPTURE_RITUAL);
    return;
  }

  // Long hold → manual +1 increment (for non-photo habits, e.g. Sleep Early)
  if (encoder.isHeld()) {
    habits.logProgress(selectedHabitIdx, 1);
    audioFx.play(SFX_TICK);

    Habit& fresh = habits.getHabit(selectedHabitIdx);
    if (fresh.completedToday >= fresh.goalToday) {
      pet.addVitality(VITALITY_GAIN_PER_HABIT);
      pet.setLastDialogueContext(DIALOGUE_HABIT_COMPLETE);
      celebVitalityGain = VITALITY_GAIN_PER_HABIT;
      celebStreakForMsg  = fresh.streak + 1;   // +1 because rollover hasn't happened yet
      pet.save();
      habits.save();
      setState(STATE_CELEBRATION);
    }
    return;
  }

  // Rotate backwards → go back to habit select
  int delta = encoder.getDelta();
  if (delta < 0) {
    setState(STATE_HABIT_SELECT);
    return;
  }
}

// ---------------------------------------------------------------
void handleCaptureRitual() {
  unsigned long now = millis();

  if (!captureReady) {
    // --- Countdown phase: 3 → 2 → 1 ---
    if (!displayTestPinned) gui.drawCaptureRitual(captureCountdown, nullptr);

    if (now - lastCountdownTick >= (unsigned long)COUNTDOWN_STEP_MS) {
      lastCountdownTick = now;
      captureCountdown--;
      if (captureCountdown > 0) {
        // Build anticipation: tint gets brighter as we approach shutter.
        uint8_t g = (captureCountdown == 1) ? 210 : 150;
        uint8_t b = (captureCountdown == 1) ? 255 : 220;
        audioFx.play(SFX_TICK);
      }

      if (captureCountdown <= 0) {
        captureReady = true;
      }
    }
    return;
  }

  // Show loading animation briefly before calling the blocking capture path.
  if (!captureLoadingPrimed) {
    captureLoadingPrimed = true;
    captureLoadingStarted = now;
  }
  if (!displayTestPinned) gui.drawCaptureRitual(0, nullptr);

  // --- Capture phase (async) ---
  if (!captureTaskStarted && !captureTaskDone) {
    if (!captureFlashFired) {
      // Camera flash notification: single white blink at shutter start.
      haptic.setSolid(255, 255, 255);
      audioFx.play(SFX_SHUTTER);
      captureFlashFired = true;
      captureFlashActive = true;
      captureFlashStartedAt = now;
    }
    BaseType_t ok = xTaskCreatePinnedToCore(
      captureWorkerTask,
      "captureWorker",
      12288,
      nullptr,
      1,
      &captureTaskHandle,
      1
    );
    if (ok == pdPASS) {
      captureTaskStarted = true;
      Serial.println("[Main] capture worker started");
    } else {
      Serial.println("[Main] ERROR: failed to start capture worker");
      captureTaskDone = true;
      captureTaskFrame = nullptr;
    }
  }

  // Keep spinner animating until the capture task completes.
  if (captureFlashActive && (now - captureFlashStartedAt >= 80)) {
    haptic.stop();
    captureFlashActive = false;
  }
  if (!captureTaskDone) {
    return;
  }
  if (captureFlashActive) {
    haptic.stop();
    captureFlashActive = false;
  }
  HabitCamBuffer* fb = captureTaskFrame;
  captureTaskFrame = nullptr;

  // Save the JPEG to LittleFS regardless of capture success
  if (fb) {
    Habit& h = habits.getHabit(selectedHabitIdx);
    String savedPath = storage.saveCapture(rtc.getDate(), h.name,
                                           fb->data, fb->len);
    if (savedPath.length() == 0) {
      Serial.println("[Main] WARNING: Capture save failed (storage full?)");
    }

    // Show decoded JPEG preview on the LCD for a moment
    if (!displayTestPinned) gui.drawCaptureRitual(0, fb);
    camera.returnFrame(fb);
    delay(CAPTURE_PREVIEW_MS);

  } else {
    Serial.println("[Main] Camera unavailable — logging habit without photo");
    if (!displayTestPinned) gui.drawCaptureRitual(0, nullptr);
    delay(800);
  }

  // --- Log the habit (unconditionally — no photo verification) ---
  Habit& h = habits.getHabit(selectedHabitIdx);
  habits.logProgress(selectedHabitIdx, 1);
  audioFx.play(SFX_REWARD);
  pet.addVitality(VITALITY_GAIN_PER_HABIT);
  pet.setLastDialogueContext(DIALOGUE_HABIT_COMPLETE);
  celebVitalityGain = VITALITY_GAIN_PER_HABIT;
  celebStreakForMsg  = h.streak;
  pet.save();
  habits.save();

  setState(STATE_CELEBRATION);
}

// ---------------------------------------------------------------
void handleCelebration() {
  unsigned long elapsed = millis() - stateEnteredAt;

  if (!displayTestPinned) gui.drawCelebration(celebVitalityGain, elapsed);

  // Staged "feel-good" reward audio cues.
  if (celebCueStage == 0) {
    audioFx.play(SFX_REWARD);
    celebCueStage = 1;
  } else if (celebCueStage == 1 && elapsed >= 900) {
    audioFx.play(SFX_TICK);
    celebCueStage = 2;
  } else if (celebCueStage == 2 && elapsed >= 1800) {
    audioFx.play(SFX_TICK);
    celebCueStage = 3;
  }

  // Auto-return to IDLE after CELEBRATION_DURATION_MS
  if (elapsed >= (unsigned long)CELEBRATION_DURATION_MS) {
    // Check for streak milestones and show them on next idle if applicable
    String streakMsg = pet.getStreakDialogue(celebStreakForMsg);
    if (streakMsg.length() > 0) {
      Serial.printf("[Main] Streak milestone: %s\n", streakMsg.c_str());
      // TODO: [Display streak milestone message as a full-screen overlay
      //        for 2 seconds before returning to IDLE]
    }
    setState(STATE_IDLE);
  }
}

// ================================================================
//  SETUP
// ================================================================
void setup() {
  Serial.begin(SERIAL_BAUD);
  delay(300);   // Give USB CDC time to enumerate
  Serial.println("\n=== HabitTracker booting ===");

  // --- Boot diagnostics ---
  Serial.printf("[Boot] chip: %s rev%d  cores=%d  cpu=%uMHz\n",
                ESP.getChipModel(), ESP.getChipRevision(), ESP.getChipCores(),
                (unsigned)ESP.getCpuFreqMHz());
  Serial.printf("[Boot] flash: %lu bytes  sketch: %lu / %lu bytes\n",
                (unsigned long)ESP.getFlashChipSize(),
                (unsigned long)ESP.getSketchSize(),
                (unsigned long)ESP.getFreeSketchSpace());
  Serial.printf("[Boot] heap: total=%lu free=%lu\n",
                (unsigned long)ESP.getHeapSize(),
                (unsigned long)ESP.getFreeHeap());
  Serial.printf("[Boot] psram: total=%lu free=%lu  %s\n",
                (unsigned long)ESP.getPsramSize(),
                (unsigned long)ESP.getFreePsram(),
                ESP.getPsramSize() > 0 ? "OK" : "MISSING (canvas will fail!)");

  // Preconvert Duck frames so the first draw is fast and prints the
  // conversion count -- also a nice smoke test that the data is valid.
  duck_init();

  // --- Storage MUST come first (other modules write to LittleFS) ---
  if (!storage.begin()) {
    Serial.println("FATAL: Storage init failed — attempting format");
    storage.format();
    if (!storage.begin()) {
      Serial.println("FATAL: Storage unrecoverable. Halting.");
      while (true) { delay(1000); }
    }
  }

  // --- Real-time clock ---
  rtc.begin();
  lastKnownDate = rtc.getDate();

  // --- SenseCAP board: IO expander + power rails (required for knob button I2C) ---
  if (!watcher_bsp_begin()) {
    Serial.println("[Main] WARNING: watcher_bsp_begin failed — knob button may not work");
  }

  // --- Load pet and habit state ---
  pet.begin();
  habits.begin();

  // --- Input peripherals ---
  encoder.begin();
  haptic.begin();
  if (audioFx.begin()) {
    Serial.println("[Audio] I2S speaker path ready");
  } else {
    Serial.println("[Audio] WARNING: speaker path init failed");
  }

  // --- Display (must come before camera; they share LEDC timers) ---
  gui.begin();

  // --- Camera (optional — device still works without it) ---
  if (!camera.begin()) {
    Serial.println("[Main] WARNING: Camera not available. Capture will skip photo.");
  }

  // --- SSCMA Himax bridge (real camera path) ----------------------------
  // If this succeeds, the "preview" and habit-capture flows will pull an
  // actual JPEG off the AI chip instead of the pre-decoded stub scene.
  // We run it *quietly* during boot; use `caminit` / `camstatus` from
  // serial to get the noisy version with per-stage logs.
  if (sscma.begin(/*verbose=*/true)) {
    Serial.println("[Main] SSCMA camera online — real frames enabled");
  } else {
    Serial.println("[Main] SSCMA camera NOT responding — capture will use stub scene");
    Serial.println("[Main]   for diagnostics: `camcold 4000`, `cambus`, `caminit`, `camstatus`");
  }

  // --- Perform day rollover if the RTC date is ahead of lastLogDate ---
  checkDayRollover();

  // --- Startup confirmation ---
  audioFx.play(SFX_TICK);

  Serial.printf("[Main] Pet vitality: %d | Habits: %d | Date: %s\n",
                pet.getVitality(), habits.getCount(), rtc.getDate().c_str());
  Serial.println("[Main] Ready. State: IDLE");
  Serial.println("[Main] Type `help` on serial for the test-harness commands.");
}

// ================================================================
//  LOOP
// ================================================================
void loop() {
  unsigned long now = millis();

  // --- Sleep-mode short-circuit ---
  // While in sleep, we skip all the normal state-machine work and the
  // loop runs the light-sleep cycle below.  Serial commands are still
  // processed every wake — this lets the user `state idle` over USB to
  // force the device awake even without touching the knob.
  if (g_sleeping) {
    tickBackgroundSystems(now, true);
    if (Serial.available()) {
      // A serial command came in while sleeping — wake the device so the
      // user can interact with it.
      Serial.println(F("[Sleep] Serial activity — waking"));
      exitSleepMode();
      checkSerialConfig();
      return;
    }
    if (_sleepTickShouldWake()) {
      exitSleepMode();
    }
    return;
  }

  // --- Rate-gate all processing to DISPLAY_REFRESH_MS ---
  // This prevents spinning faster than the display can update.
  if (now - lastRefresh < (unsigned long)DISPLAY_REFRESH_MS) {
    // Still process haptic and encoder at full speed
    haptic.update();
    encoder.update();
    return;
  }
  lastRefresh = now;

  // --- Per-iteration updates ---
  haptic.update();
  encoder.update();
  pet.update(now);

  tickBackgroundSystems(now, false);

  // --- Serial config menu (every 200ms) ---
  if (now - lastSerialCheck >= 200) {
    lastSerialCheck = now;
    checkSerialConfig();
  }

  // --- State machine dispatch ---
  switch (currentState) {
    case STATE_IDLE:           handleIdle();           break;
    case STATE_HABIT_SELECT:   handleHabitSelect();    break;
    case STATE_HABIT_DETAIL:   handleHabitDetail();    break;
    case STATE_CAPTURE_RITUAL: handleCaptureRitual();  break;
    case STATE_CELEBRATION:    handleCelebration();    break;
  }
}
