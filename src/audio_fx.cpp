#include "audio_fx.h"

#include <math.h>
#include <driver/i2s.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <Wire.h>
#include <Arduino.h>

#include "config.h"

namespace {
constexpr float PI_F = 3.14159265f;
constexpr int kBufSamples = 128;

TaskHandle_t g_audioTask = nullptr;
volatile bool g_audioBusy = false;
bool g_i2sReady = false;
uint8_t g_codecAddr = 0;
uint8_t g_audioMode = 0;
int g_currentSampleRate = AUDIO_SAMPLE_RATE;
int g_currentMclkHz = AUDIO_SAMPLE_RATE * 256;

struct FxTaskArgs {
  SoundFx fx;
};

bool codecWrite(uint8_t reg, uint8_t val) {
  if (!g_codecAddr) return false;
  Wire.beginTransmission(g_codecAddr);
  Wire.write(reg);
  Wire.write(val);
  return Wire.endTransmission() == 0;
}

bool codecRead(uint8_t reg, uint8_t* out) {
  if (!g_codecAddr || !out) return false;
  Wire.beginTransmission(g_codecAddr);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom((int)g_codecAddr, 1) != 1) return false;
  *out = (uint8_t)Wire.read();
  return true;
}

bool ensureCodecPowerRail() {
  // PCA9535 output register (low byte @0x02, high byte @0x03)
  // We need SYSTEM + CODEC_PA rails high for speaker path.
  Wire.beginTransmission(WATCHER_IOEXP_ADDR);
  Wire.write(0x02);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom((int)WATCHER_IOEXP_ADDR, 2) != 2) return false;
  uint16_t out = (uint16_t)Wire.read() | ((uint16_t)Wire.read() << 8);
  out |= (uint16_t)(BSP_PWR_SYSTEM | BSP_PWR_CODEC_PA);

  Wire.beginTransmission(WATCHER_IOEXP_ADDR);
  Wire.write(0x02);
  Wire.write((uint8_t)(out & 0xFF));
  Wire.write((uint8_t)(out >> 8));
  return Wire.endTransmission() == 0;
}

struct Es8311Coeff {
  int mclk;
  int rate;
  uint8_t pre_div;
  uint8_t pre_multi;
  uint8_t adc_div;
  uint8_t dac_div;
  uint8_t fs_mode;
  uint8_t lrck_h;
  uint8_t lrck_l;
  uint8_t bclk_div;
  uint8_t adc_osr;
  uint8_t dac_osr;
};

// Borrowed from known-working ES8311 coefficient table (ADF driver).
const Es8311Coeff kEsCoeff16k[] = {
  {12288000,16000,0x03,0x01,0x01,0x01,0x00,0x00,0xff,0x04,0x10,0x10},
  { 8192000,16000,0x02,0x01,0x01,0x01,0x00,0x00,0xff,0x04,0x10,0x10},
  { 6144000,16000,0x03,0x02,0x01,0x01,0x00,0x00,0xff,0x04,0x10,0x10},
  { 4096000,16000,0x01,0x01,0x01,0x01,0x00,0x00,0xff,0x04,0x10,0x10},
  { 3072000,16000,0x03,0x04,0x01,0x01,0x00,0x00,0xff,0x04,0x10,0x10},
  // 24kHz entries from Espressif ES8311 coeff table
  {12288000,24000,0x02,0x01,0x01,0x01,0x00,0x00,0xff,0x04,0x10,0x10},
  {18432000,24000,0x03,0x01,0x01,0x01,0x00,0x00,0xff,0x04,0x10,0x10},
  { 6144000,24000,0x01,0x01,0x01,0x01,0x00,0x00,0xff,0x04,0x10,0x10},
  { 3072000,24000,0x01,0x02,0x01,0x01,0x00,0x00,0xff,0x04,0x10,0x10},
  { 1536000,24000,0x01,0x04,0x01,0x01,0x00,0x00,0xff,0x04,0x10,0x10},
};

const Es8311Coeff* findCoeff(int mclk, int rate) {
  for (const auto& c : kEsCoeff16k) {
    if (c.mclk == mclk && c.rate == rate) return &c;
  }
  return nullptr;
}

bool applyClockCoeff(const Es8311Coeff& c) {
  uint8_t reg = 0;
  if (!codecRead(0x02, &reg)) return false;
  reg &= 0x07;
  reg |= (uint8_t)((c.pre_div - 1) << 5);
  uint8_t pm = 0;
  if      (c.pre_multi == 1) pm = 0;
  else if (c.pre_multi == 2) pm = 1;
  else if (c.pre_multi == 4) pm = 2;
  else if (c.pre_multi == 8) pm = 3;
  reg |= (uint8_t)(pm << 3);
  codecWrite(0x02, reg);

  codecWrite(0x05, (uint8_t)(((c.adc_div - 1) << 4) | (c.dac_div - 1)));
  codecWrite(0x03, (uint8_t)((c.fs_mode << 6) | c.adc_osr));
  codecWrite(0x04, c.dac_osr);
  codecWrite(0x07, c.lrck_h);
  codecWrite(0x08, c.lrck_l);

  if (!codecRead(0x06, &reg)) return false;
  reg &= 0xE0;
  reg |= (uint8_t)((c.bclk_div < 19) ? (c.bclk_div - 1) : c.bclk_div);
  codecWrite(0x06, reg);
  return true;
}

bool initEs8311(int sampleRate, int mclkHz) {
  // Probe common 7-bit addresses seen in Watcher integrations.
  const uint8_t candidates[] = {0x18, 0x10, 0x11};
  g_codecAddr = 0;
  for (uint8_t a : candidates) {
    Wire.beginTransmission(a);
    if (Wire.endTransmission() == 0) {
      g_codecAddr = a;
      break;
    }
  }
  if (!g_codecAddr) {
    Serial.println("[Audio] ES8311 probe failed (no ACK on 0x18/0x10/0x11)");
    return false;
  }
  Serial.printf("[Audio] ES8311 detected @ 0x%02X\n", g_codecAddr);

  // Minimal bring-up sequence adapted from ES8311 reference driver.
  if (!codecWrite(0x01, 0x30)) return false;
  codecWrite(0x02, 0x00);
  codecWrite(0x03, 0x10);
  codecWrite(0x16, 0x24);
  codecWrite(0x04, 0x10);
  codecWrite(0x05, 0x00);
  codecWrite(0x0B, 0x00);
  codecWrite(0x0C, 0x00);
  codecWrite(0x10, 0x1F);
  codecWrite(0x11, 0x7F);
  codecWrite(0x00, 0x80);
  delay(10);
  codecWrite(0x00, 0x00); // slave mode
  codecWrite(0x01, 0x3F); // enable clocks
  const Es8311Coeff* coeff = findCoeff(mclkHz, sampleRate);
  if (!coeff) {
    Serial.printf("[Audio] no ES8311 coeff for mclk=%d rate=%d\n", mclkHz, sampleRate);
    return false;
  }
  if (!applyClockCoeff(*coeff)) return false;

  // Match the working BSP open/start sequence for stable DAC output.
  codecWrite(0x09, 0x0C);
  codecWrite(0x0A, 0x0C);
  codecWrite(0x13, 0x10);
  codecWrite(0x1B, 0x0A);
  codecWrite(0x1C, 0x6A);
  codecWrite(0x44, 0x50); // internal reference (ADCL + DACR)
  codecWrite(0x17, 0xBF);
  codecWrite(0x0E, 0x02);
  codecWrite(0x12, 0x00);
  codecWrite(0x14, 0x1A);
  codecWrite(0x0D, 0x01);
  codecWrite(0x15, 0x40);
  codecWrite(0x37, 0x08);
  codecWrite(0x45, 0x00);
  codecWrite(0x31, 0x00); // unmute
  // Match the known-good IDF path (logs showed 0xBF when audible).
  codecWrite(0x32, 0xBF);

  uint8_t v = 0;
  if (!codecRead(0x32, &v)) return false;
  Serial.printf("[Audio] ES8311 DAC volume reg=0x%02X\n", v);
  return true;
}

struct ModeCfg {
  i2s_comm_format_t fmt;
  i2s_channel_fmt_t ch;
  int sampleRate;
  int bclk;
  int ws;
  int dout;
  int mclk;
  const char* name;
};

const ModeCfg kModes[] = {
  // Canonical Watcher mapping (DOUT=16, MCLK=10)
  { I2S_COMM_FORMAT_STAND_I2S, I2S_CHANNEL_FMT_ONLY_LEFT,  24000, 11, 12, 16, 10, "i2s-left-d16-b11-w12-m10"   },
  { I2S_COMM_FORMAT_STAND_MSB, I2S_CHANNEL_FMT_RIGHT_LEFT, 24000, 11, 12, 16, 10, "msb-stereo-d16-b11-w12-m10" },
  { I2S_COMM_FORMAT_STAND_I2S, I2S_CHANNEL_FMT_RIGHT_LEFT, 24000, 11, 12, 16, 10, "i2s-stereo-d16-b11-w12-m10" },
  { I2S_COMM_FORMAT_STAND_I2S, I2S_CHANNEL_FMT_ONLY_RIGHT, 24000, 11, 12, 16, 10, "i2s-right-d16-b11-w12-m10"  },
  { I2S_COMM_FORMAT_STAND_MSB, I2S_CHANNEL_FMT_ONLY_LEFT,  24000, 11, 12, 16, 10, "msb-left-d16-b11-w12-m10"   },
  { I2S_COMM_FORMAT_STAND_MSB, I2S_CHANNEL_FMT_ONLY_RIGHT, 24000, 11, 12, 16, 10, "msb-right-d16-b11-w12-m10"  },
  // Alternate mapping seen in some board forks (DOUT=15)
  { I2S_COMM_FORMAT_STAND_I2S, I2S_CHANNEL_FMT_RIGHT_LEFT, 24000, 11, 12, 15, 10, "i2s-stereo-d15-b11-w12-m10" },
  { I2S_COMM_FORMAT_STAND_MSB, I2S_CHANNEL_FMT_RIGHT_LEFT, 24000, 11, 12, 15, 10, "msb-stereo-d15-b11-w12-m10" },
  // No MCLK variants (some codecs derive clock from BCLK/LRCK path)
  { I2S_COMM_FORMAT_STAND_I2S, I2S_CHANNEL_FMT_RIGHT_LEFT, 24000, 11, 12, 16, -1, "i2s-stereo-d16-b11-w12-no-mclk" },
  { I2S_COMM_FORMAT_STAND_MSB, I2S_CHANNEL_FMT_RIGHT_LEFT, 24000, 11, 12, 16, -1, "msb-stereo-d16-b11-w12-no-mclk" },
  { I2S_COMM_FORMAT_STAND_I2S, I2S_CHANNEL_FMT_RIGHT_LEFT, 24000, 11, 12, 15, -1, "i2s-stereo-d15-b11-w12-no-mclk" },
  { I2S_COMM_FORMAT_STAND_MSB, I2S_CHANNEL_FMT_RIGHT_LEFT, 24000, 11, 12, 15, -1, "msb-stereo-d15-b11-w12-no-mclk" },
  // BCLK/WS swapped probe set (some firmware variants label these opposite).
  { I2S_COMM_FORMAT_STAND_I2S, I2S_CHANNEL_FMT_ONLY_LEFT,  24000, 12, 11, 16, 10, "i2s-left-d16-b12-w11-m10"   },
  { I2S_COMM_FORMAT_STAND_MSB, I2S_CHANNEL_FMT_RIGHT_LEFT, 24000, 12, 11, 16, 10, "msb-stereo-d16-b12-w11-m10" },
  { I2S_COMM_FORMAT_STAND_I2S, I2S_CHANNEL_FMT_RIGHT_LEFT, 24000, 12, 11, 16, 10, "i2s-stereo-d16-b12-w11-m10" },
  { I2S_COMM_FORMAT_STAND_I2S, I2S_CHANNEL_FMT_ONLY_RIGHT, 24000, 12, 11, 16, 10, "i2s-right-d16-b12-w11-m10"  },
  { I2S_COMM_FORMAT_STAND_MSB, I2S_CHANNEL_FMT_ONLY_LEFT,  24000, 12, 11, 16, 10, "msb-left-d16-b12-w11-m10"   },
  { I2S_COMM_FORMAT_STAND_MSB, I2S_CHANNEL_FMT_ONLY_RIGHT, 24000, 12, 11, 16, 10, "msb-right-d16-b12-w11-m10"  },
  { I2S_COMM_FORMAT_STAND_I2S, I2S_CHANNEL_FMT_RIGHT_LEFT, 24000, 12, 11, 15, 10, "i2s-stereo-d15-b12-w11-m10" },
  { I2S_COMM_FORMAT_STAND_MSB, I2S_CHANNEL_FMT_RIGHT_LEFT, 24000, 12, 11, 15, 10, "msb-stereo-d15-b12-w11-m10" },
};
constexpr uint8_t kModeCount = (uint8_t)(sizeof(kModes) / sizeof(kModes[0]));

bool installI2sForMode(uint8_t mode) {
  if (mode >= kModeCount) return false;
  const ModeCfg& m = kModes[mode];

  // Avoid runtime tear-down/reinstall once active; this can race DMA/ISR on
  // Arduino's legacy I2S driver and trigger LoadProhibited panics.
  if (g_i2sReady && mode != g_audioMode) {
    Serial.printf("[Audio] runtime mode switch blocked (%u -> %u)\n",
                  (unsigned)g_audioMode, (unsigned)mode);
    return false;
  }

  if (g_i2sReady) {
    i2s_stop(I2S_NUM_0);
    delay(10);
    i2s_zero_dma_buffer(I2S_NUM_0);
    delay(10);
    i2s_driver_uninstall(I2S_NUM_0);
    g_i2sReady = false;
    delay(18);
  }

  Serial.printf("[Audio] try mode %u: %s | bclk=%d ws=%d dout=%d mclk=%d\n",
                (unsigned)mode, m.name, m.bclk, m.ws, m.dout, m.mclk);

  i2s_config_t cfg = {};
  cfg.mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX | I2S_MODE_RX);
  cfg.sample_rate = m.sampleRate;
  cfg.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT;
  cfg.channel_format = m.ch;
  cfg.communication_format = m.fmt;
  cfg.intr_alloc_flags = ESP_INTR_FLAG_LEVEL2 | ESP_INTR_FLAG_IRAM;
  cfg.dma_buf_count = 6;
  cfg.dma_buf_len = 128;
  cfg.use_apll = true;
  cfg.tx_desc_auto_clear = true;
  cfg.fixed_mclk = (m.mclk < 0) ? 0 : (m.sampleRate * 256);

  i2s_pin_config_t pins = {};
  pins.mck_io_num = m.mclk;
  pins.bck_io_num = m.bclk;
  pins.ws_io_num = m.ws;
  pins.data_out_num = m.dout;
  pins.data_in_num = AUDIO_I2S_GPIO_DIN;

  if (i2s_driver_install(I2S_NUM_0, &cfg, 0, nullptr) != ESP_OK) return false;
  if (i2s_set_pin(I2S_NUM_0, &pins) != ESP_OK) {
    i2s_driver_uninstall(I2S_NUM_0);
    return false;
  }
  i2s_channel_t ch = (m.ch == I2S_CHANNEL_FMT_RIGHT_LEFT) ? I2S_CHANNEL_STEREO : I2S_CHANNEL_MONO;
  if (i2s_set_clk(I2S_NUM_0, m.sampleRate, I2S_BITS_PER_SAMPLE_16BIT, ch) != ESP_OK) {
    i2s_driver_uninstall(I2S_NUM_0);
    return false;
  }
  i2s_zero_dma_buffer(I2S_NUM_0);
  g_currentSampleRate = m.sampleRate;
  g_currentMclkHz = (m.mclk < 0) ? 0 : (m.sampleRate * 256);
  return true;
}

bool modeIsStereo() {
  if (g_audioMode >= kModeCount) return false;
  return kModes[g_audioMode].ch == I2S_CHANNEL_FMT_RIGHT_LEFT;
}

bool writeToneHz(int hz, int ms, float amp) {
  if (!g_i2sReady || hz <= 0 || ms <= 0) return false;
  int16_t buf[kBufSamples * 2];
  const bool stereo = modeIsStereo();
  const int totalSamples = (g_currentSampleRate * ms) / 1000;
  int written = 0;
  float phase = 0.0f;
  const float phaseInc = 2.0f * PI_F * (float)hz / (float)g_currentSampleRate;

  while (written < totalSamples) {
    int n = min(kBufSamples, totalSamples - written);
    for (int i = 0; i < n; i++) {
      float s = sinf(phase) * amp;
      if (s > 1.0f) s = 1.0f;
      if (s < -1.0f) s = -1.0f;
      int16_t v = (int16_t)(s * 32767.0f);
      if (stereo) {
        buf[i * 2 + 0] = v;
        buf[i * 2 + 1] = v;
      } else {
        buf[i] = v;
      }
      phase += phaseInc;
      if (phase > 2.0f * PI_F) phase -= 2.0f * PI_F;
    }
    size_t bytesWritten = 0;
    size_t txBytes = stereo ? (size_t)(n * 2 * sizeof(int16_t)) : (size_t)(n * sizeof(int16_t));
    esp_err_t err = i2s_write(I2S_NUM_0, buf, txBytes, &bytesWritten, pdMS_TO_TICKS(50));
    if (err != ESP_OK) return false;
    written += stereo
      ? (int)(bytesWritten / (2 * sizeof(int16_t)))
      : (int)(bytesWritten / sizeof(int16_t));
  }
  return true;
}

void writeSilenceMs(int ms) {
  if (!g_i2sReady || ms <= 0) return;
  const int16_t z[kBufSamples] = {0};
  int totalSamples = (g_currentSampleRate * ms) / 1000;
  while (totalSamples > 0) {
    int n = min(kBufSamples, totalSamples);
    size_t bytesWritten = 0;
    i2s_write(I2S_NUM_0, z, n * sizeof(int16_t), &bytesWritten, pdMS_TO_TICKS(20));
    totalSamples -= n;
  }
}

void playFxBlocking(SoundFx fx) {
  switch (fx) {
    case SFX_TICK:
      writeToneHz(1200, 180, 0.92f);
      break;
    case SFX_SHUTTER:
      writeToneHz(2200, 110, 0.95f);
      writeSilenceMs(15);
      writeToneHz(900, 180, 0.93f);
      break;
    case SFX_REWARD:
      writeToneHz(660, 260, 0.90f);
      writeSilenceMs(40);
      writeToneHz(880, 260, 0.92f);
      writeSilenceMs(40);
      writeToneHz(1320, 340, 0.95f);
      break;
    case SFX_TONE_LONG:
      writeToneHz(1000, 3500, 0.98f);
      break;
    default:
      break;
  }
}

void audioTask(void* arg) {
  FxTaskArgs* a = (FxTaskArgs*)arg;
  g_audioBusy = true;
  if (a) {
    playFxBlocking(a->fx);
    delete a;
  }
  g_audioBusy = false;
  g_audioTask = nullptr;
  vTaskDelete(nullptr);
}
}  // namespace

bool AudioFx::begin() {
  if (g_i2sReady && _ready) {
    _ready = true;
    return true;
  }
  // Community reports on ES8311 boards show PA/amp enable gating can be
  // separate from codec init. On Watcher-class designs, driving GPIO46 high
  // is safe (shared as SD CS, idle-high) and can unmute external amp paths.
  pinMode(46, OUTPUT);
  digitalWrite(46, HIGH);
  delay(2);
  Serial.println("[Audio] forced GPIO46 HIGH (PA/CS gate)");
  if (!ensureCodecPowerRail()) {
    Serial.println("[Audio] WARNING: failed to force CODEC_PA rail");
  }
  // IDF-style robustness: try the configured mode first, then sweep all
  // known wire-format/pin variants until one fully initializes.
  uint8_t first = g_audioMode;
  bool ok = false;
  for (uint8_t pass = 0; pass < kModeCount; pass++) {
    uint8_t mode = (uint8_t)((first + pass) % kModeCount);
    if (!installI2sForMode(mode)) {
      continue;
    }
    if (!initEs8311(g_currentSampleRate, g_currentMclkHz)) {
      i2s_driver_uninstall(I2S_NUM_0);
      g_i2sReady = false;
      continue;
    }
    g_audioMode = mode;
    g_i2sReady = true;
    ok = true;
    Serial.printf("[Audio] active mode %u (%s)\n", (unsigned)g_audioMode, kModes[g_audioMode].name);
    break;
  }
  _ready = ok;
  if (!_ready) {
    return false;
  }

  // Prime DAC path after codec startup (mirrors IDF behavior where a short
  // initial stream often avoids a silent first playback).
  writeSilenceMs(30);
  writeToneHz(900, 180, 0.90f);
  writeSilenceMs(20);
  return true;
}

bool AudioFx::isBusy() const {
  return g_audioBusy;
}

bool AudioFx::play(SoundFx fx) {
  if (!_ready || fx == SFX_NONE) return false;
  if (g_audioTask || g_audioBusy) return false;
  FxTaskArgs* args = new FxTaskArgs{fx};
  if (!args) return false;
  BaseType_t ok = xTaskCreatePinnedToCore(audioTask, "audioFx", 4096, args, 1, &g_audioTask, 1);
  if (ok != pdPASS) {
    delete args;
    g_audioTask = nullptr;
    return false;
  }
  return true;
}

void AudioFx::stop() {
  if (g_i2sReady) {
    i2s_zero_dma_buffer(I2S_NUM_0);
  }
}

bool AudioFx::waitIdle(uint32_t timeoutMs) {
  const unsigned long start = millis();
  while (isBusy()) {
    if (millis() - start > timeoutMs) return false;
    delay(5);
  }
  return true;
}

bool AudioFx::setMode(uint8_t mode) {
  if (mode >= kModeCount) return false;
  if (g_i2sReady && mode != g_audioMode) {
    Serial.println("[Audio] mode change rejected while running (restart to re-probe)");
    return false;
  }
  // Avoid reconfiguring I2S while DMA callbacks are active.
  waitIdle(2500);
  g_audioMode = mode;
  _ready = false;
  return begin();
}

uint8_t AudioFx::getMode() const {
  return g_audioMode;
}

uint8_t AudioFx::getModeCount() const {
  return kModeCount;
}

const char* AudioFx::getModeName() const {
  if (g_audioMode >= kModeCount) return "unknown";
  return kModes[g_audioMode].name;
}

