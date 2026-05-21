#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>
#include <inttypes.h>
#include <sys/stat.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "nvs_flash.h"
#include "cJSON.h"
#include "sensecap-watcher.h"
#include "esp_jpeg_dec.h"

#define TAG "HT_IDF"
#define HABIT_FILE "/spiffs/habits.json"
#define MAX_HABITS 5
#define CAPTURE_BUF_SZ (256 * 1024)
#define BTN_HOLD_MS 2000
#define BTN_DEBOUNCE_MS 45

typedef enum {
    APP_IDLE = 0,
    APP_HABIT_SELECT,
    APP_HABIT_DETAIL,
    APP_CAPTURE_RITUAL,
    APP_CELEBRATION,
} app_state_t;

typedef struct {
    char name[32];
    int goal;
    int done;
    int streak;
} habit_t;

static habit_t s_habits[MAX_HABITS] = {
    {.name = "Workout", .goal = 1, .done = 0, .streak = 0},
    {.name = "Hydrate", .goal = 8, .done = 0, .streak = 0},
    {.name = "Read", .goal = 1, .done = 0, .streak = 0},
    {.name = "Journal", .goal = 1, .done = 0, .streak = 0},
    {.name = "Sleep", .goal = 1, .done = 0, .streak = 0},
};

static int s_habit_count = MAX_HABITS;
static int s_vitality = 100;
static app_state_t s_state = APP_IDLE;
static int s_selected = 0;
static int s_encoder_accum = 0;
static uint8_t s_prev_enc_state = 0;
static bool s_sleeping = false;
static int64_t s_btn_down_ms = 0;
static int s_btn_release_level = 1;
static int s_btn_stable = 1;
static int s_btn_last_sample = 1;
static int64_t s_btn_last_change_ms = 0;
static bool s_capture_busy = false;
static int64_t s_last_tick_beep_ms = 0;

static lv_obj_t *s_title = NULL;
static lv_obj_t *s_subtitle = NULL;
static lv_obj_t *s_body = NULL;
static lv_obj_t *s_hint = NULL;
static lv_obj_t *s_card = NULL;
static lv_obj_t *s_arc = NULL;
static lv_obj_t *s_spinner = NULL;
static lv_obj_t *s_duck = NULL;
static lv_obj_t *s_symbol = NULL;
static lv_obj_t *s_photo = NULL;
static bool s_ui_ready = false;

static sscma_client_handle_t s_sscma = NULL;
static bool s_camera_ready = false;
static uint8_t *s_capture_buf = NULL;
static int s_capture_len = 0;
static uint8_t s_spinner_frame = 0;
static bool s_has_photo = false;
static lv_img_dsc_t s_photo_dsc = {0};
static uint8_t *s_photo_rgb565 = NULL;

#define CAM_W 240
#define CAM_H 240

static const char *habit_symbol(const char *name) {
    if (strstr(name, "Hydrate")) return "W";
    if (strstr(name, "Read")) return "R";
    if (strstr(name, "Workout")) return "V";
    if (strstr(name, "Journal")) return "J";
    if (strstr(name, "Sleep")) return "Z";
    return "*";
}

static bool decode_jpeg_to_lv_rgb565(const uint8_t *jpeg, int jpeg_len) {
    if (!jpeg || jpeg_len <= 0) return false;
    if (!s_photo_rgb565) {
        s_photo_rgb565 = heap_caps_malloc(CAM_W * CAM_H * 2, MALLOC_CAP_8BIT);
        if (!s_photo_rgb565) return false;
    }

    jpeg_dec_config_t cfg = {
        .output_type = JPEG_RAW_TYPE_RGB565_BE,
        .rotate = JPEG_ROTATE_0D,
    };
    jpeg_dec_handle_t dec = jpeg_dec_open(&cfg);
    if (!dec) return false;

    jpeg_dec_io_t io = {0};
    jpeg_dec_header_info_t out = {0};
    io.inbuf = (uint8_t *)jpeg;
    io.inbuf_len = jpeg_len;
    int ret = jpeg_dec_parse_header(dec, &io, &out);
    if (ret < 0) {
        jpeg_dec_close(dec);
        ESP_LOGE(TAG, "jpeg header parse failed");
        return false;
    }

    io.outbuf = s_photo_rgb565;
    int consumed = io.inbuf_len - io.inbuf_remain;
    io.inbuf = (uint8_t *)jpeg + consumed;
    io.inbuf_len = io.inbuf_remain;
    ret = jpeg_dec_process(dec, &io);
    jpeg_dec_close(dec);
    if (ret < 0) {
        ESP_LOGE(TAG, "jpeg decode failed");
        return false;
    }

    s_photo_dsc.header.always_zero = 0;
    s_photo_dsc.header.w = CAM_W;
    s_photo_dsc.header.h = CAM_H;
    s_photo_dsc.header.cf = LV_IMG_CF_TRUE_COLOR;
    s_photo_dsc.data_size = CAM_W * CAM_H * 2;
    s_photo_dsc.data = s_photo_rgb565;
    return true;
}

static const char *state_name(app_state_t s) {
    switch (s) {
        case APP_IDLE: return "IDLE";
        case APP_HABIT_SELECT: return "HABIT_SELECT";
        case APP_HABIT_DETAIL: return "HABIT_DETAIL";
        case APP_CAPTURE_RITUAL: return "CAPTURE_RITUAL";
        case APP_CELEBRATION: return "CELEBRATION";
        default: return "?";
    }
}

static void set_state(app_state_t next) {
    if (next != s_state) {
        ESP_LOGI(TAG, "state %s -> %s", state_name(s_state), state_name(next));
        s_state = next;
    }
}

static void beep_fx(void) {
    // Audio disabled in this build to avoid ESP-IDF 5.4 I2C
    // old/new driver conflict in the watcher BSP stack.
}

static void tick_beep_if_ready(int64_t now_ms) {
    if ((now_ms - s_last_tick_beep_ms) < 60) {
        return;
    }
    s_last_tick_beep_ms = now_ms;
    beep_fx();
}

static void save_habits(void) {
    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "vitality", s_vitality);
    cJSON *arr = cJSON_AddArrayToObject(root, "habits");
    for (int i = 0; i < s_habit_count; i++) {
        cJSON *h = cJSON_CreateObject();
        cJSON_AddStringToObject(h, "name", s_habits[i].name);
        cJSON_AddNumberToObject(h, "goal", s_habits[i].goal);
        cJSON_AddNumberToObject(h, "done", s_habits[i].done);
        cJSON_AddNumberToObject(h, "streak", s_habits[i].streak);
        cJSON_AddItemToArray(arr, h);
    }
    char *json = cJSON_PrintUnformatted(root);
    FILE *f = fopen(HABIT_FILE, "wb");
    if (f && json) {
        fwrite(json, 1, strlen(json), f);
        fclose(f);
        ESP_LOGI(TAG, "saved habits to %s", HABIT_FILE);
    }
    free(json);
    cJSON_Delete(root);
}

static void load_habits(void) {
    FILE *f = fopen(HABIT_FILE, "rb");
    if (!f) {
        ESP_LOGW(TAG, "no habits file yet");
        save_habits();
        return;
    }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > 64 * 1024) {
        fclose(f);
        return;
    }
    char *buf = calloc((size_t)sz + 1, 1);
    if (!buf) {
        fclose(f);
        return;
    }
    fread(buf, 1, (size_t)sz, f);
    fclose(f);
    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (!root) return;
    cJSON *v = cJSON_GetObjectItem(root, "vitality");
    if (cJSON_IsNumber(v)) s_vitality = v->valueint;
    cJSON *arr = cJSON_GetObjectItem(root, "habits");
    if (cJSON_IsArray(arr)) {
        int n = cJSON_GetArraySize(arr);
        if (n > MAX_HABITS) n = MAX_HABITS;
        for (int i = 0; i < n; i++) {
            cJSON *h = cJSON_GetArrayItem(arr, i);
            if (!cJSON_IsObject(h)) continue;
            cJSON *name = cJSON_GetObjectItem(h, "name");
            cJSON *goal = cJSON_GetObjectItem(h, "goal");
            cJSON *done = cJSON_GetObjectItem(h, "done");
            cJSON *streak = cJSON_GetObjectItem(h, "streak");
            if (cJSON_IsString(name) && name->valuestring) {
                strncpy(s_habits[i].name, name->valuestring, sizeof(s_habits[i].name) - 1);
            }
            if (cJSON_IsNumber(goal)) s_habits[i].goal = goal->valueint;
            if (cJSON_IsNumber(done)) s_habits[i].done = done->valueint;
            if (cJSON_IsNumber(streak)) s_habits[i].streak = streak->valueint;
        }
        s_habit_count = n;
    }
    cJSON_Delete(root);
}

static void ui_set_base_style(void) {
    lv_obj_set_style_bg_color(lv_scr_act(), lv_color_hex(0x0B1020), 0);
    lv_obj_set_style_bg_opa(lv_scr_act(), LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(lv_scr_act(), 0, 0);
}

static void ui_init_objects(void) {
    ui_set_base_style();
    s_card = lv_obj_create(lv_scr_act());
    lv_obj_set_size(s_card, 360, 310);
    lv_obj_center(s_card);
    lv_obj_set_style_bg_color(s_card, lv_color_hex(0x182642), 0);
    lv_obj_set_style_bg_opa(s_card, LV_OPA_90, 0);
    lv_obj_set_style_border_width(s_card, 2, 0);
    lv_obj_set_style_border_color(s_card, lv_color_hex(0x4FA3FF), 0);
    lv_obj_set_style_radius(s_card, 38, 0);
    lv_obj_set_style_pad_all(s_card, 14, 0);

    s_title = lv_label_create(s_card);
    lv_obj_set_style_text_color(s_title, lv_color_hex(0xF7FAFF), 0);
    lv_obj_set_style_text_font(s_title, &lv_font_montserrat_14, 0);
    lv_obj_align(s_title, LV_ALIGN_TOP_MID, 0, 2);

    s_subtitle = lv_label_create(s_card);
    lv_obj_set_style_text_color(s_subtitle, lv_color_hex(0x9DCBFF), 0);
    lv_obj_set_style_text_font(s_subtitle, &lv_font_montserrat_14, 0);
    lv_obj_align(s_subtitle, LV_ALIGN_TOP_MID, 0, 38);

    s_body = lv_label_create(s_card);
    lv_obj_set_width(s_body, 320);
    lv_obj_set_style_text_align(s_body, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(s_body, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(s_body, &lv_font_montserrat_14, 0);
    lv_obj_align(s_body, LV_ALIGN_CENTER, 0, -2);

    s_hint = lv_label_create(s_card);
    lv_obj_set_width(s_hint, 320);
    lv_obj_set_style_text_align(s_hint, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(s_hint, lv_color_hex(0xA9B7D0), 0);
    lv_obj_set_style_text_font(s_hint, &lv_font_montserrat_14, 0);
    lv_obj_align(s_hint, LV_ALIGN_BOTTOM_MID, 0, -12);

    s_arc = lv_arc_create(s_card);
    lv_obj_set_size(s_arc, 126, 126);
    lv_obj_align(s_arc, LV_ALIGN_RIGHT_MID, -8, 10);
    lv_obj_set_style_arc_color(s_arc, lv_color_hex(0x2A3C5D), LV_PART_MAIN);
    lv_obj_set_style_arc_width(s_arc, 10, LV_PART_MAIN);
    lv_obj_set_style_arc_color(s_arc, lv_color_hex(0x67E8F9), LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(s_arc, 12, LV_PART_INDICATOR);
    lv_arc_set_bg_angles(s_arc, 0, 360);
    lv_arc_set_rotation(s_arc, 270);
    lv_obj_remove_style(s_arc, NULL, LV_PART_KNOB);
    lv_obj_clear_flag(s_arc, LV_OBJ_FLAG_CLICKABLE);

    s_spinner = lv_label_create(s_card);
    lv_obj_set_style_text_font(s_spinner, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_spinner, lv_color_hex(0xFCD34D), 0);
    lv_obj_align(s_spinner, LV_ALIGN_CENTER, 0, 56);
    lv_obj_add_flag(s_spinner, LV_OBJ_FLAG_HIDDEN);

    s_duck = lv_label_create(s_card);
    lv_label_set_text(s_duck, "(>' ')>");
    lv_obj_set_style_text_color(s_duck, lv_color_hex(0xFFE066), 0);
    lv_obj_set_style_text_font(s_duck, &lv_font_montserrat_14, 0);
    lv_obj_align(s_duck, LV_ALIGN_LEFT_MID, 16, -30);

    s_symbol = lv_label_create(s_card);
    lv_label_set_text(s_symbol, "*");
    lv_obj_set_style_text_color(s_symbol, lv_color_hex(0x67E8F9), 0);
    lv_obj_set_style_text_font(s_symbol, &lv_font_montserrat_14, 0);
    lv_obj_align(s_symbol, LV_ALIGN_LEFT_MID, 20, 24);

    s_photo = lv_img_create(s_card);
    lv_obj_set_size(s_photo, 140, 140);
    lv_obj_align(s_photo, LV_ALIGN_RIGHT_MID, -8, 10);
    lv_obj_add_flag(s_photo, LV_OBJ_FLAG_HIDDEN);
}

static void update_ui(void) {
    char title[64];
    char subtitle[64];
    char body[256];
    char hint[160];
    int progress = 0;

    snprintf(title, sizeof(title), "Habit Tracker");
    snprintf(subtitle, sizeof(subtitle), "%s", state_name(s_state));
    if (s_state == APP_IDLE) {
        snprintf(subtitle, sizeof(subtitle), "Your duck is waiting");
        snprintf(body, sizeof(body), "Vitality %d%%", s_vitality);
        snprintf(hint, sizeof(hint), "Press to open habits  |  Hold to sleep");
        progress = s_vitality;
        lv_obj_add_flag(s_spinner, LV_OBJ_FLAG_HIDDEN);
    } else if (s_state == APP_HABIT_SELECT) {
        int done = s_habits[s_selected].done;
        int goal = s_habits[s_selected].goal;
        progress = (goal > 0) ? (done * 100) / goal : 0;
        if (progress > 100) progress = 100;
        snprintf(subtitle, sizeof(subtitle), "Choose a habit");
        snprintf(body, sizeof(body), "%s\n%d / %d complete", s_habits[s_selected].name, done, goal);
        snprintf(hint, sizeof(hint), "Rotate to switch  |  Press for details");
        lv_obj_add_flag(s_spinner, LV_OBJ_FLAG_HIDDEN);
    } else if (s_state == APP_HABIT_DETAIL) {
        int done = s_habits[s_selected].done;
        int goal = s_habits[s_selected].goal;
        progress = (goal > 0) ? (done * 100) / goal : 0;
        if (progress > 100) progress = 100;
        snprintf(subtitle, sizeof(subtitle), "Ready to log");
        snprintf(body, sizeof(body), "%s\nStreak %d days", s_habits[s_selected].name, s_habits[s_selected].streak);
        snprintf(hint, sizeof(hint), "Press to capture and log");
        lv_obj_add_flag(s_spinner, LV_OBJ_FLAG_HIDDEN);
    } else if (s_state == APP_CAPTURE_RITUAL) {
        static const char kSpin[4] = {'|', '/', '-', '\\'};
        char spin[8];
        s_spinner_frame = (uint8_t)((s_spinner_frame + 1) & 0x03);
        snprintf(spin, sizeof(spin), "%c", kSpin[s_spinner_frame]);
        lv_label_set_text(s_spinner, spin);
        lv_obj_clear_flag(s_spinner, LV_OBJ_FLAG_HIDDEN);
        progress = 15 + (int)(s_spinner_frame * 20);
        snprintf(subtitle, sizeof(subtitle), "Camera ritual");
        snprintf(body, sizeof(body), "Capturing a real image...");
        snprintf(hint, sizeof(hint), "camera %s  |  last frame %d bytes", s_camera_ready ? "ready" : "offline", s_capture_len);
    } else {
        progress = 100;
        snprintf(subtitle, sizeof(subtitle), "Great job!");
        snprintf(body, sizeof(body), "+1 %s\nStreak %d days", s_habits[s_selected].name, s_habits[s_selected].streak);
        snprintf(hint, sizeof(hint), "Vitality now %d%%", s_vitality);
        lv_obj_add_flag(s_spinner, LV_OBJ_FLAG_HIDDEN);
    }
    ESP_LOGI(TAG, "%s | %s | %s", title, subtitle, body);
    if (!s_ui_ready || !s_title || !s_body || !s_hint || !s_arc || !s_subtitle) return;
    lvgl_port_lock(portMAX_DELAY);
    lv_label_set_text(s_title, title);
    lv_label_set_text(s_subtitle, subtitle);
    lv_label_set_text(s_body, body);
    lv_label_set_text(s_hint, hint);
    lv_label_set_text(s_symbol, habit_symbol(s_habits[s_selected].name));
    if (s_state == APP_IDLE) {
        lv_label_set_text(s_duck, "\\\\_o< quack");
    } else if (s_state == APP_CELEBRATION) {
        lv_label_set_text(s_duck, "\\\\^o^/ yay");
    } else {
        lv_label_set_text(s_duck, "(>' ')>");
    }
    lv_arc_set_value(s_arc, progress);
    if (s_state == APP_IDLE || s_state == APP_CAPTURE_RITUAL) {
        lv_obj_add_flag(s_arc, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_clear_flag(s_arc, LV_OBJ_FLAG_HIDDEN);
    }
    if ((s_state == APP_CELEBRATION || s_state == APP_HABIT_DETAIL) && s_has_photo && s_photo_dsc.data) {
        lv_img_set_src(s_photo, &s_photo_dsc);
        lv_obj_clear_flag(s_photo, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_arc, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_photo, LV_OBJ_FLAG_HIDDEN);
    }
    lvgl_port_unlock();
}

static void update_encoder(void) {
    static const int8_t kQuadTable[16] = {
        0, -1, 1, 0,
        1, 0, 0, -1,
        -1, 0, 0, 1,
        0, 1, -1, 0
    };
    uint8_t a = (uint8_t)gpio_get_level(BSP_KNOB_A);
    uint8_t b = (uint8_t)gpio_get_level(BSP_KNOB_B);
    uint8_t curr = (uint8_t)((a << 1) | b);
    uint8_t idx = (uint8_t)((s_prev_enc_state << 2) | curr);
    int8_t step = kQuadTable[idx & 0x0F];
    static int8_t substep = 0;
    if (step != 0) {
        substep += step;
        if (substep >= 4) {
            s_encoder_accum++;
            substep = 0;
        } else if (substep <= -4) {
            s_encoder_accum--;
            substep = 0;
        }
    }
    s_prev_enc_state = curr;
}

static bool btn_pressed(void) {
    return bsp_exp_io_get_level(BSP_KNOB_BTN) == 0;
}

static bool camera_capture(void) {
    if (!s_sscma) return false;
    sscma_client_reply_t reply = {0};
    esp_err_t err = sscma_client_set_sensor(s_sscma, 1, 0, true);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "set_sensor failed: %s", esp_err_to_name(err));
        return false;
    }
    err = sscma_client_request(s_sscma, "AT+SAMPLE=1\r\n", &reply, true, pdMS_TO_TICKS(8000));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "sample failed: %s", esp_err_to_name(err));
        return false;
    }
    int image_size = 0;
    if (!s_capture_buf) s_capture_buf = malloc(CAPTURE_BUF_SZ);
    if (!s_capture_buf) return false;
    err = sscma_utils_copy_image_from_reply(&reply, (char *)s_capture_buf, CAPTURE_BUF_SZ, &image_size);
    sscma_client_reply_clear(&reply);
    if (err != ESP_OK || image_size <= 0) {
        ESP_LOGW(TAG, "copy image failed: %s", esp_err_to_name(err));
        return false;
    }
    s_capture_len = image_size;
    ESP_LOGI(TAG, "captured image bytes=%d", image_size);
    FILE *f = fopen("/spiffs/last_capture.jpg", "wb");
    if (f) {
        fwrite(s_capture_buf, 1, (size_t)image_size, f);
        fclose(f);
    }
    if (decode_jpeg_to_lv_rgb565(s_capture_buf, image_size)) {
        s_has_photo = true;
        ESP_LOGI(TAG, "decoded camera image for display");
    } else {
        ESP_LOGW(TAG, "failed to decode camera JPEG for display");
    }
    return true;
}

static void start_capture_task(void);

static void app_loop_task(void *arg) {
    int64_t celebration_until = 0;
    while (1) {
        if (s_sleeping) {
            esp_sleep_enable_timer_wakeup(400000);
            esp_light_sleep_start();
            if (btn_pressed()) {
                s_sleeping = false;
                bsp_lcd_brightness_set(100);
                beep_fx();
                set_state(APP_IDLE);
                update_ui();
            }
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        update_encoder();
        int raw_btn = bsp_exp_io_get_level(BSP_KNOB_BTN);
        int64_t now_ms = esp_timer_get_time() / 1000;
        if (raw_btn != s_btn_last_sample) {
            s_btn_last_sample = raw_btn;
            s_btn_last_change_ms = now_ms;
        } else if ((now_ms - s_btn_last_change_ms) >= BTN_DEBOUNCE_MS) {
            s_btn_stable = raw_btn;
        }
        bool pressed = (s_btn_stable != s_btn_release_level);

        if (pressed && s_btn_down_ms == 0) s_btn_down_ms = now_ms;
        if (!pressed && s_btn_down_ms != 0) {
            int64_t held = now_ms - s_btn_down_ms;
            s_btn_down_ms = 0;
            if (held >= BTN_HOLD_MS && s_state == APP_IDLE) {
                s_sleeping = true;
                bsp_lcd_brightness_set(0);
                bsp_rgb_set(0, 0, 0);
                continue;
            }
            if (s_state == APP_IDLE) {
                set_state(APP_HABIT_SELECT);
                beep_fx();
            } else if (s_state == APP_HABIT_SELECT) {
                set_state(APP_HABIT_DETAIL);
                beep_fx();
            } else if (s_state == APP_HABIT_DETAIL) {
                set_state(APP_CAPTURE_RITUAL);
                beep_fx();
                if (!s_capture_busy) {
                    s_capture_busy = true;
                    start_capture_task();
                }
            } else if (s_state == APP_CELEBRATION) {
                set_state(APP_IDLE);
            }
            update_ui();
        }

        if (s_state == APP_HABIT_SELECT) {
            if (s_encoder_accum >= 1) {
                s_encoder_accum = 0;
                s_selected = (s_selected + 1) % s_habit_count;
                tick_beep_if_ready(now_ms);
                update_ui();
            } else if (s_encoder_accum <= -1) {
                s_encoder_accum = 0;
                s_selected = (s_selected - 1 + s_habit_count) % s_habit_count;
                tick_beep_if_ready(now_ms);
                update_ui();
            }
        }

        if (s_state == APP_CELEBRATION && celebration_until > 0 && now_ms > celebration_until) {
            set_state(APP_IDLE);
            update_ui();
        }

        if (s_state == APP_CAPTURE_RITUAL && s_ui_ready) {
            update_ui();
        }
        vTaskDelay(pdMS_TO_TICKS(40));
    }
}

static void capture_task(void *arg) {
    bool ok = false;
    bsp_rgb_set(6, 6, 6);
    ok = camera_capture();
    bsp_rgb_set(0, 0, 0);
    if (ok) {
        s_habits[s_selected].done++;
        if (s_habits[s_selected].done >= s_habits[s_selected].goal) {
            s_habits[s_selected].streak++;
            s_vitality += 5;
            if (s_vitality > 100) s_vitality = 100;
        }
        save_habits();
    }
    set_state(APP_CELEBRATION);
    update_ui();
    s_capture_busy = false;
    vTaskDelete(NULL);
}

static void start_capture_task(void) {
    xTaskCreate(capture_task, "capture_task", 8192, NULL, 4, NULL);
}

static void init_input_baseline(void) {
    int ones = 0;
    int zeros = 0;
    for (int i = 0; i < 20; ++i) {
        int v = bsp_exp_io_get_level(BSP_KNOB_BTN);
        if (v) ones++;
        else zeros++;
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    s_btn_release_level = (ones >= zeros) ? 1 : 0;
    s_btn_stable = s_btn_release_level;
    s_btn_last_sample = s_btn_release_level;
    s_btn_last_change_ms = esp_timer_get_time() / 1000;
    ESP_LOGI(TAG, "button baseline release_level=%d", s_btn_release_level);
}

void app_main(void) {
    ESP_ERROR_CHECK(nvs_flash_init());
    bsp_i2c_bus_init();
    bsp_io_expander_init();
    bsp_rgb_init();
    bsp_exp_io_set_level(BSP_PWR_AI_CHIP, 1);
    bsp_exp_io_set_level(BSP_PWR_CODEC_PA, 1);
    bsp_spiffs_init_default();

    bsp_display_cfg_t disp_cfg = {
        .lvgl_port_cfg = ESP_LVGL_PORT_INIT_CONFIG(),
        .buffer_size = DRV_LCD_H_RES * 40,
        .double_buffer = false,
        .flags = {
            .buff_dma = false,
            .buff_spiram = false,
        },
    };
    lv_disp_t *disp = bsp_lvgl_init_with_cfg(&disp_cfg);
    if (disp) {
        lvgl_port_lock(portMAX_DELAY);
        ui_init_objects();
        lvgl_port_unlock();
        s_ui_ready = true;
    } else {
        ESP_LOGW(TAG, "LVGL init unavailable, running headless UI");
    }

    s_sscma = bsp_sscma_client_init();
    s_camera_ready = (s_sscma != NULL);
    ESP_LOGI(TAG, "camera ready=%d", s_camera_ready);
    init_input_baseline();

    load_habits();
    update_ui();
    xTaskCreate(app_loop_task, "app_loop", 8192, NULL, 5, NULL);

    ESP_LOGI(TAG, "HabitTracker ESP-IDF ready");
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
