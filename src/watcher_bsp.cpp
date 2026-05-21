// =============================================================
// watcher_bsp.cpp — PCA9535 + power rails for SenseCAP Watcher
//
// Ported from Seeed BSP logic in sensecap-watcher.c (bsp_io_expander_init
// and output sequencing). Without this, the knob button (expander pin 3)
// does not read correctly after cold boot.
// =============================================================

#include <Wire.h>
#include "config.h"
#include "watcher_bsp.h"

static bool s_ioexpOk = false;

static bool pca9535_write16(uint8_t reg, uint16_t value) {
  Wire.beginTransmission(WATCHER_IOEXP_ADDR);
  Wire.write(reg);
  Wire.write((uint8_t)(value & 0xFF));
  Wire.write((uint8_t)(value >> 8));
  return Wire.endTransmission() == 0;
}

static bool pca9535_read16(uint8_t reg, uint16_t* out) {
  Wire.beginTransmission(WATCHER_IOEXP_ADDR);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom((int)WATCHER_IOEXP_ADDR, 2) != 2) return false;
  uint8_t lo = Wire.read();
  uint8_t hi = Wire.read();
  *out = (uint16_t)lo | ((uint16_t)hi << 8);
  return true;
}

bool watcher_bsp_begin() {
  s_ioexpOk = false;
  Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL);
  Wire.setClock(400000);

  // Direction: 0 = output for rails on PCA95xx driver with dir_out_bit_zero;
  // Seeed masks resolve to 0x20FF for the direction register after BSP setup.
  uint16_t direction = (uint16_t)(~DRV_IO_EXP_OUTPUT_MASK & 0xFFFFu);
  if (!pca9535_write16(0x06, direction)) {
    Serial.println("[WatcherBSP] PCA9535 direction write failed (check I2C pins 47/48)");
    return false;
  }

  // All expander-controlled outputs low, then power sequencing
  if (!pca9535_write16(0x02, 0x0000u)) {
    Serial.println("[WatcherBSP] PCA9535 output clear failed");
    return false;
  }

  if (!pca9535_write16(0x02, (uint16_t)BSP_PWR_SYSTEM)) {
    Serial.println("[WatcherBSP] PCA9535 BSP_PWR_SYSTEM failed");
    return false;
  }
  delay(120);

  if (!pca9535_write16(0x02, (uint16_t)(BSP_PWR_SYSTEM | BSP_PWR_START_UP))) {
    Serial.println("[WatcherBSP] PCA9535 BSP_PWR_START_UP failed");
    return false;
  }
  delay(60);

  s_ioexpOk = true;
  Serial.println("[WatcherBSP] IO expander + rails initialised");
  return true;
}

bool watcher_bsp_ioexp_ok() { return s_ioexpOk; }

bool watcher_knob_button_is_pressed() {
  if (!s_ioexpOk) return false;
  uint16_t in = 0;
  if (!pca9535_read16(0x00, &in)) return false;
  // Active low — matches get_started example: bsp_exp_io_get_level(BSP_KNOB_BTN)==0
  return (in & WATCHER_IOEXP_KNOB_BTN_MASK) == 0;
}
