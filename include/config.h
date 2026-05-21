#pragma once
// =============================================================
// config.h — SenseCAP Watcher hardware (from Seeed BSP)
//
// Source of truth: SenseCAP-Watcher-Firmware
//   components/sensecap-watcher/include/sensecap-watcher.h
// Examples: examples/get_started, examples/knob_rgb
// https://github.com/Seeed-Studio/SenseCAP-Watcher-Firmware
// =============================================================

// ---------------------------------------------------------------
// LCD — SPD2010, 412×412, quad SPI on SPI3_HOST
// QSPI pins (same mapping as BSP_SPI3_HOST_* + BSP_LCD_SPI_CS)
// ---------------------------------------------------------------
#define PIN_LCD_QSPI_CS    45
#define PIN_LCD_QSPI_SCK   7
#define PIN_LCD_QSPI_D0    9
#define PIN_LCD_QSPI_D1    1
#define PIN_LCD_QSPI_D2    14
#define PIN_LCD_QSPI_D3    13

#define PIN_LCD_BL         8

// Match BSP: 10-bit LEDC on timer 1, 5 kHz (see sensecap-watcher.c bsp_lcd_backlight_init)
#define BL_LEDC_CHANNEL    1
#define BL_LEDC_TIMER      1
#define BL_LEDC_FREQ_HZ    5000
#define BL_LEDC_RES_BITS   10
#define BL_DEFAULT_PERCENT 85

#define DISPLAY_WIDTH      412
#define DISPLAY_HEIGHT     412
#define DISPLAY_CENTER_X   (DISPLAY_WIDTH / 2)
#define DISPLAY_CENTER_Y   (DISPLAY_HEIGHT / 2)

// Arduino_ESP32QSPI host: set in platformio.ini as -DESP32QSPI_SPI_HOST=SPI3_HOST

// ---------------------------------------------------------------
// Rotary knob — quadrature on ESP32 GPIO (see knob_rgb example)
// ---------------------------------------------------------------
#define PIN_KNOB_A         41
#define PIN_KNOB_B         42

// Knob centre push: PCA9535 IO expander line (BSP_KNOB_BTN), not a GPIO.
// Bit mask matching Espressif IO expander convention (pin index 3 → bit 3).
#define WATCHER_IOEXP_KNOB_BTN_MASK  (1u << 3)

// ---------------------------------------------------------------
// I2C — “General” bus for PCA9535 IO expander (same as BSP)
// ---------------------------------------------------------------
#define PIN_I2C_SDA        47
#define PIN_I2C_SCL        48
#define WATCHER_IOEXP_ADDR 0x21   // ESP_IO_EXPANDER_I2C_PCA9535_ADDRESS_001

// From sensecap-watcher.h — direction / power sequencing
#define DRV_IO_EXP_INPUT_MASK   0x20FFu
#define DRV_IO_EXP_OUTPUT_MASK  0xDF00u

// Power rail bits on expander outputs (BSP_PWR_*); BSP_PWR_START_UP OR mask
#define BSP_PWR_SDCARD      (1u << 8)
#define BSP_PWR_LCD         (1u << 9)
#define BSP_PWR_SYSTEM      (1u << 10)
#define BSP_PWR_AI_CHIP     (1u << 11)
#define BSP_PWR_CODEC_PA    (1u << 12)
#define BSP_PWR_GROVE       (1u << 14)
#define BSP_PWR_BAT_ADC     (1u << 15)
#define BSP_PWR_START_UP    (BSP_PWR_SDCARD | BSP_PWR_LCD | BSP_PWR_SYSTEM | BSP_PWR_AI_CHIP | BSP_PWR_CODEC_PA | BSP_PWR_GROVE | BSP_PWR_BAT_ADC)

// ---------------------------------------------------------------
// WS2812 RGB LED (BSP_RGB_CTRL) — “haptic” substitute (no ERM motor)
// ---------------------------------------------------------------
#define PIN_RGB_LED        40
#define RGB_LED_COUNT      1

// ---------------------------------------------------------------
// Audio codec / speaker (SenseCAP Watcher ES8311 via I2S0)
// ---------------------------------------------------------------
#define AUDIO_SAMPLE_RATE      24000
#define AUDIO_I2S_GPIO_MCLK    10
#define AUDIO_I2S_GPIO_WS      12
#define AUDIO_I2S_GPIO_BCLK    11
#define AUDIO_I2S_GPIO_DOUT    16
#define AUDIO_I2S_GPIO_DIN     15

// ---------------------------------------------------------------
// Pet / habits / timing (unchanged semantics)
// ---------------------------------------------------------------
#define VITALITY_MAX             100
#define VITALITY_MIN               0
#define VITALITY_START            70
#define VITALITY_THRIVING_MIN     80
#define VITALITY_HAPPY_MIN        60
#define VITALITY_TIRED_MIN        40
#define VITALITY_STRUGGLING_MIN   20
#define VITALITY_GAIN_PER_HABIT   10
#define VITALITY_LOSS_PER_MISS     5

#define GOAL_INCREASE_THRESHOLD  100
#define GOAL_STAY_THRESHOLD       50
#define GOAL_STEP_AMOUNT           1

#define HAPTIC_HEARTBEAT_INTERVAL_MS   4000
#define CELEBRATION_DURATION_MS        3000
#define COUNTDOWN_STEP_MS            1000
// How long the decoded capture preview stays on screen before celebration.
// 4 s feels like enough time to enjoy the photo without making the flow drag.
#define CAPTURE_PREVIEW_MS           4000
#define ANIMATION_TICK_MS              50
#define DISPLAY_REFRESH_MS             33

#define PATH_HABITS_JSON   "/habits.json"
#define PATH_PET_CONFIG    "/pet_config.json"
#define PATH_CAPTURES_DIR  "/captures"
#define PATH_LOGS_DIR      "/logs"
#define MAX_CAPTURES        30

#define SERIAL_BAUD     115200
#define NTP_SERVER      "pool.ntp.org"
#define TZ_OFFSET_SEC   (-5 * 3600)
#define DST_OFFSET_SEC  3600

#define DEFAULT_HABIT_COUNT  5
#define MAX_HABIT_COUNT      20

#define ENCODER_DEBOUNCE_MS  8
#define BUTTON_HOLD_MS       600
// Long-hold (>= 2 s) on the encoder button toggles device sleep mode.
// Distinct from the short-hold "back" gesture so users can still hold the
// button briefly to navigate without accidentally putting the watch to sleep.
#define BUTTON_SLEEP_HOLD_MS 2000

#define COLOR_BLACK       0x0000
#define COLOR_WHITE       0xFFFF
#define COLOR_DARK_GREY   0x4208
#define COLOR_MID_GREY    0x8410
#define COLOR_LIGHT_GREY  0xC618
#define COLOR_RED         0xF800
#define COLOR_ORANGE      0xFD20
#define COLOR_YELLOW      0xFFE0
#define COLOR_GREEN       0x07E0
#define COLOR_CYAN        0x07FF
#define COLOR_BLUE        0x001F
#define COLOR_MAGENTA     0xF81F
#define COLOR_SKIN        0xFDD7
#define COLOR_SKIN_DARK   0xDB95
#define COLOR_PINK        0xFB56

// ---------------------------------------------------------------
// Himax SSCMA — AT+SENSOR before AT+SAMPLE (Seeed factory firmware)
//
// See examples/factory_firmware/main/task_flow_module/tf_module_ai_camera.c
// (EVENT_SIMPLE_640_480 / EVENT_PRVIEW_416_416). The spiffs/ folder is only
// audio+image assets; camera logic lives in tf_module_ai_camera.c.
//
// opt_id values match tf_module_ai_camera.h:
//   0 = 240x240 (SSCMA default — blocky, often looks "wrong")
//   1 = 416x416 (YOLO inference grid)
//   2 = 480x480
//   3 = 640x480 (factory "simple photo" path — closest to real scene)
// ---------------------------------------------------------------
#define SSCMA_SENSOR_OPT_240_240   0
#define SSCMA_SENSOR_OPT_416_416 1
#define SSCMA_SENSOR_OPT_480_480  2
#define SSCMA_SENSOR_OPT_640_480  3
#ifndef SSCMA_CAPTURE_SENSOR_OPT
// Default to the most stable mode for repeated capture sessions.
// You can still change at runtime with `camsensor`.
#define SSCMA_CAPTURE_SENSOR_OPT   SSCMA_SENSOR_OPT_240_240
#endif
