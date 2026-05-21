#include <math.h>
#include <stdint.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "sensecap-watcher.h"

#define TAG "WATCHER_SPK_TEST"
#define SAMPLE_RATE 16000
#define CHUNK_SAMPLES 512
#define PI_F 3.14159265358979323846f

static void play_sine(float freq_hz, float seconds, int16_t amplitude)
{
    int16_t samples[CHUNK_SAMPLES];
    size_t bytes_written = 0;
    const int total_samples = (int)(seconds * SAMPLE_RATE);
    float phase = 0.0f;
    const float phase_step = (2.0f * PI_F * freq_hz) / (float)SAMPLE_RATE;

    for (int i = 0; i < total_samples; i += CHUNK_SAMPLES)
    {
        const int block = (total_samples - i > CHUNK_SAMPLES) ? CHUNK_SAMPLES : (total_samples - i);
        for (int n = 0; n < block; ++n)
        {
            samples[n] = (int16_t)(sinf(phase) * amplitude);
            phase += phase_step;
            if (phase > 2.0f * PI_F)
            {
                phase -= 2.0f * PI_F;
            }
        }
        bsp_i2s_write(samples, block * sizeof(int16_t), &bytes_written, portMAX_DELAY);
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "Starting SenseCAP Watcher speaker test");
    ESP_ERROR_CHECK(bsp_codec_init());
    ESP_ERROR_CHECK(bsp_codec_set_fs(SAMPLE_RATE, 16, 1));
    ESP_ERROR_CHECK(bsp_codec_volume_set(100, NULL));
    ESP_ERROR_CHECK(bsp_codec_mute_set(false));

    while (1)
    {
        ESP_LOGI(TAG, "BEEP pattern A (800Hz/1200Hz)");
        play_sine(800.0f, 0.25f, 12000);
        vTaskDelay(pdMS_TO_TICKS(60));
        play_sine(1200.0f, 0.25f, 12000);

        int16_t silence[CHUNK_SAMPLES];
        memset(silence, 0, sizeof(silence));
        size_t bytes_written = 0;
        for (int i = 0; i < 8; ++i)
        {
            bsp_i2s_write(silence, sizeof(silence), &bytes_written, portMAX_DELAY);
        }
        vTaskDelay(pdMS_TO_TICKS(1500));
    }
}
