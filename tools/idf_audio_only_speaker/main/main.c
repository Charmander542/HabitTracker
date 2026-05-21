#include <math.h>
#include <stdint.h>
#include <string.h>

#include "driver/i2c.h"
#include "driver/i2s.h"
#include "esp_check.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define TAG "AUDIO_ONLY_IDF"

#define I2C_PORT I2C_NUM_0
#define I2C_SDA 47
#define I2C_SCL 48
#define IOEXP_ADDR 0x21
#define ES8311_ADDR 0x18

#define I2S_PORT I2S_NUM_0
#define I2S_MCLK 10
#define I2S_BCLK 11
#define I2S_WS 12
#define I2S_DOUT 16
#define I2S_DIN 15

#define SAMPLE_RATE 24000
#define CHUNK_SAMPLES 256
#define PI_F 3.14159265358979323846f

#define PWR_SYSTEM (1u << 10)
#define PWR_CODEC_PA (1u << 12)

static esp_err_t i2c_write_reg8(uint8_t addr, uint8_t reg, uint8_t val) {
    uint8_t payload[2] = {reg, val};
    return i2c_master_write_to_device(I2C_PORT, addr, payload, sizeof(payload), pdMS_TO_TICKS(100));
}

static esp_err_t i2c_read_reg8(uint8_t addr, uint8_t reg, uint8_t *val) {
    return i2c_master_write_read_device(I2C_PORT, addr, &reg, 1, val, 1, pdMS_TO_TICKS(100));
}

static esp_err_t ioexp_power_enable(void) {
    uint8_t reg = 0x02;
    uint8_t out[2] = {0};
    ESP_RETURN_ON_ERROR(
        i2c_master_write_read_device(I2C_PORT, IOEXP_ADDR, &reg, 1, out, 2, pdMS_TO_TICKS(100)),
        TAG, "read ioexp out reg failed");
    uint16_t mask = (uint16_t)out[0] | ((uint16_t)out[1] << 8);
    mask |= (PWR_SYSTEM | PWR_CODEC_PA);
    uint8_t payload[3] = {0x02, (uint8_t)(mask & 0xFF), (uint8_t)(mask >> 8)};
    ESP_RETURN_ON_ERROR(
        i2c_master_write_to_device(I2C_PORT, IOEXP_ADDR, payload, sizeof(payload), pdMS_TO_TICKS(100)),
        TAG, "write ioexp out reg failed");
    return ESP_OK;
}

static esp_err_t es8311_init_minimal(void) {
    ESP_RETURN_ON_ERROR(i2c_write_reg8(ES8311_ADDR, 0x00, 0x80), TAG, "reset stage1 failed");
    vTaskDelay(pdMS_TO_TICKS(20));
    ESP_RETURN_ON_ERROR(i2c_write_reg8(ES8311_ADDR, 0x00, 0x00), TAG, "reset stage2 failed");

    // Bring-up sequence aligned with esp_codec_dev ES8311 open/start path.
    i2c_write_reg8(ES8311_ADDR, 0x01, 0x30);
    i2c_write_reg8(ES8311_ADDR, 0x02, 0x00);
    i2c_write_reg8(ES8311_ADDR, 0x03, 0x10);
    i2c_write_reg8(ES8311_ADDR, 0x16, 0x24);
    i2c_write_reg8(ES8311_ADDR, 0x04, 0x10);
    i2c_write_reg8(ES8311_ADDR, 0x05, 0x00);
    i2c_write_reg8(ES8311_ADDR, 0x0B, 0x00);
    i2c_write_reg8(ES8311_ADDR, 0x0C, 0x00);
    i2c_write_reg8(ES8311_ADDR, 0x10, 0x1F);
    i2c_write_reg8(ES8311_ADDR, 0x11, 0x7F);

    // 24k @ 6.144MHz MCLK coeffs.
    i2c_write_reg8(ES8311_ADDR, 0x02, 0x08);
    i2c_write_reg8(ES8311_ADDR, 0x05, 0x00);
    i2c_write_reg8(ES8311_ADDR, 0x03, 0x10);
    i2c_write_reg8(ES8311_ADDR, 0x04, 0x10);
    i2c_write_reg8(ES8311_ADDR, 0x07, 0x00);
    i2c_write_reg8(ES8311_ADDR, 0x08, 0xFF);
    i2c_write_reg8(ES8311_ADDR, 0x06, 0x03);

    i2c_write_reg8(ES8311_ADDR, 0x09, 0x0C);
    i2c_write_reg8(ES8311_ADDR, 0x0A, 0x0C);
    i2c_write_reg8(ES8311_ADDR, 0x13, 0x10);
    i2c_write_reg8(ES8311_ADDR, 0x1B, 0x0A);
    i2c_write_reg8(ES8311_ADDR, 0x1C, 0x6A);
    i2c_write_reg8(ES8311_ADDR, 0x44, 0x50);
    i2c_write_reg8(ES8311_ADDR, 0x17, 0xBF);
    i2c_write_reg8(ES8311_ADDR, 0x0E, 0x02);
    i2c_write_reg8(ES8311_ADDR, 0x12, 0x00);
    i2c_write_reg8(ES8311_ADDR, 0x14, 0x1A);
    i2c_write_reg8(ES8311_ADDR, 0x0D, 0x01);
    i2c_write_reg8(ES8311_ADDR, 0x15, 0x40);
    i2c_write_reg8(ES8311_ADDR, 0x37, 0x08);
    i2c_write_reg8(ES8311_ADDR, 0x45, 0x00);
    i2c_write_reg8(ES8311_ADDR, 0x31, 0x00); // unmute
    i2c_write_reg8(ES8311_ADDR, 0x32, 0xBF); // max-ish DAC volume

    uint8_t vol = 0;
    ESP_RETURN_ON_ERROR(i2c_read_reg8(ES8311_ADDR, 0x32, &vol), TAG, "read DAC volume failed");
    ESP_LOGI(TAG, "ES8311 volume reg 0x32 = 0x%02X", vol);
    return ESP_OK;
}

static esp_err_t i2s_init(void) {
    i2s_config_t cfg = {
        .mode = I2S_MODE_MASTER | I2S_MODE_TX,
        .sample_rate = SAMPLE_RATE,
        .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
        .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
        .communication_format = I2S_COMM_FORMAT_STAND_I2S,
        .intr_alloc_flags = ESP_INTR_FLAG_LEVEL2,
        .dma_buf_count = 8,
        .dma_buf_len = CHUNK_SAMPLES,
        .use_apll = true,
        .tx_desc_auto_clear = true,
        .fixed_mclk = SAMPLE_RATE * 256,
    };
    i2s_pin_config_t pins = {
        .mck_io_num = I2S_MCLK,
        .bck_io_num = I2S_BCLK,
        .ws_io_num = I2S_WS,
        .data_out_num = I2S_DOUT,
        .data_in_num = I2S_DIN,
    };
    ESP_RETURN_ON_ERROR(i2s_driver_install(I2S_PORT, &cfg, 0, NULL), TAG, "i2s_driver_install failed");
    ESP_RETURN_ON_ERROR(i2s_set_pin(I2S_PORT, &pins), TAG, "i2s_set_pin failed");
    ESP_RETURN_ON_ERROR(i2s_set_clk(I2S_PORT, SAMPLE_RATE, I2S_BITS_PER_SAMPLE_16BIT, I2S_CHANNEL_MONO), TAG, "i2s_set_clk failed");
    i2s_zero_dma_buffer(I2S_PORT);
    return ESP_OK;
}

static void play_sine(float freq_hz, float seconds, int16_t amplitude) {
    int16_t buf[CHUNK_SAMPLES];
    size_t bytes_written = 0;
    int total = (int)(seconds * SAMPLE_RATE);
    float phase = 0.0f;
    float step = (2.0f * PI_F * freq_hz) / (float)SAMPLE_RATE;

    while (total > 0) {
        int n = (total > CHUNK_SAMPLES) ? CHUNK_SAMPLES : total;
        for (int i = 0; i < n; ++i) {
            buf[i] = (int16_t)(sinf(phase) * amplitude);
            phase += step;
            if (phase >= 2.0f * PI_F) phase -= 2.0f * PI_F;
        }
        i2s_write(I2S_PORT, buf, n * sizeof(int16_t), &bytes_written, portMAX_DELAY);
        total -= n;
    }
}

void app_main(void) {
    ESP_LOGI(TAG, "Minimal IDF audio-only speaker test boot");

    i2c_config_t i2c_cfg = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = I2C_SDA,
        .scl_io_num = I2C_SCL,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = 400000,
    };
    ESP_ERROR_CHECK(i2c_param_config(I2C_PORT, &i2c_cfg));
    ESP_ERROR_CHECK(i2c_driver_install(I2C_PORT, i2c_cfg.mode, 0, 0, 0));

    ESP_ERROR_CHECK(ioexp_power_enable());
    ESP_LOGI(TAG, "PCA9535 speaker/system rails enabled");
    ESP_ERROR_CHECK(i2s_init());
    ESP_ERROR_CHECK(es8311_init_minimal());

    while (1) {
        ESP_LOGI(TAG, "tone burst");
        play_sine(700.0f, 0.35f, 13000);
        vTaskDelay(pdMS_TO_TICKS(80));
        play_sine(1100.0f, 0.35f, 13000);
        vTaskDelay(pdMS_TO_TICKS(1200));
    }
}
