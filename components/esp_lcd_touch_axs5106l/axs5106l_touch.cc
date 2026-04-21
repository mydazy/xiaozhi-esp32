/*
 * AXS5106L Touchscreen Driver for ESP32-S3 + LVGL
 *
 * Hardware : JD9853 284×240 + AXS5106L touch controller (I2C 0x63)
 * Firmware : V2907 — landscape rotation pre-applied by chip firmware.
 *            X-axis hardware dead-zone [9..272] compensated in software → [0..283].
 *
 * Features :
 *   - Two-phase init: InitializeHardware (before LVGL) / InitializeInput (after LVGL)
 *   - Automatic firmware upgrade on first boot
 *   - INT-pin polling (no GPIO ISR required)
 *   - Noise rejection: INT debounce (3 ms) + release debounce (2 frames) + speed filter
 *   - Software gesture recognition: tap, double-tap, long-press, 4-direction swipe
 *   - Sleep / resume
 *   - Debug overlay (AXS5106L_TOUCH_DEBUG_OVERLAY=1): red tracking dot + raw-coordinate log
 */

#include "axs5106l_touch.h"
#include "axs5106l_upgrade.h"

#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <algorithm>
#include <cstring>

static const char* TAG = "TOUCH_AXS5106L";

// ---------- I2C registers ----------
#define AXS5106L_I2C_ADDR   0x63
#define AXS5106L_REG_DATA   0x01   // 6-byte touch frame: gesture / num / XH / XL / YH / YL
#define AXS5106L_REG_FW_VER 0x05   // firmware version, 2-byte big-endian
#define AXS5106L_REG_CHIP_ID 0x08  // chip ID, 3 bytes
#define AXS5106L_REG_SLEEP  0x19   // write 0x03 to enter sleep
#define AXS5106L_REG_RESET  0xF0   // write {B3,55,AA,34,01} for soft reset

#define AXS_POINT_NUM(buf)  (buf[1])
#define AXS_POINT_X(buf)    (((uint16_t)(buf[2] & 0x0F) << 8) | buf[3])
#define AXS_POINT_Y(buf)    (((uint16_t)(buf[4] & 0x0F) << 8) | buf[5])

// ---------- Screen limits ----------
#define TOUCH_MAX_X  284
#define TOUCH_MAX_Y  240

// X-axis dead-zone compensation (V2907 measured: left 9 px, right 11 px; Y normal)
#define TOUCH_X_RAW_MIN    9
#define TOUCH_X_RAW_MAX    272
#define TOUCH_X_RAW_RANGE  (TOUCH_X_RAW_MAX - TOUCH_X_RAW_MIN)  // 263

// ---------- Gesture thresholds ----------
#define SWIPE_THRESHOLD      30       // min travel for swipe (px)
#define CLICK_MAX_TIME_US    400000   // max press duration for tap (400 ms)
#define CLICK_MIN_TIME_US    20000    // min press duration for tap, filters RF spikes (20 ms)
#define CLICK_MAX_MOVE       35       // max travel still considered a tap (px)
#define LONG_PRESS_TIME_US   600000   // long-press threshold (600 ms)
#define DOUBLE_CLICK_TIME_US 500000   // max interval between two taps for double-tap (500 ms)
#define DOUBLE_CLICK_DIST    60       // max distance between tap positions for double-tap (px)

// ---------- Debounce / noise ----------
#define INT_DEBOUNCE_US   2000  // INT must stay low ≥2 ms to qualify as press
#define RELEASE_DEBOUNCE  1     // consecutive no-touch frames before reporting release
#define MAX_SPEED_PX_S    3000  // velocity gate: 284px@220DPI, fast swipe ~2600, RF transient >9000

// ---------- I2C ----------
#define I2C_TIMEOUT_MS  100
#define I2C_RETRIES     3

// -------------------------------------------------------------------------

Axs5106lTouch::Axs5106lTouch(i2c_master_bus_handle_t i2c_bus,
                             gpio_num_t rst_gpio, gpio_num_t int_gpio,
                             uint16_t width, uint16_t height,
                             bool swap_xy, bool mirror_x, bool mirror_y)
    : i2c_bus_(i2c_bus),
      rst_gpio_(rst_gpio), int_gpio_(int_gpio),
      width_(width), height_(height),
      swap_xy_(swap_xy), mirror_x_(mirror_x), mirror_y_(mirror_y) {}

Axs5106lTouch::~Axs5106lTouch() { Cleanup(); }

// -------------------------------------------------------------------------
// Public API
// -------------------------------------------------------------------------

bool Axs5106lTouch::InitializeHardware() {
    ESP_LOGI(TAG, "init RST=GPIO%d INT=GPIO%d %dx%d", rst_gpio_, int_gpio_, width_, height_);

    gpio_config_t rst_cfg = {
        .pin_bit_mask = (1ULL << rst_gpio_),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&rst_cfg));
    gpio_set_level(rst_gpio_, 1);

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = AXS5106L_I2C_ADDR,
        .scl_speed_hz = 400000,
        .scl_wait_us = 0,
        .flags = {.disable_ack_check = 0},
    };
    esp_err_t ret = i2c_master_bus_add_device(i2c_bus_, &dev_cfg, &i2c_handle_);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "I2C add device failed: %s", esp_err_to_name(ret));
        return false;
    }

    ResetChip();

    if (CheckAndUpgradeFirmware()) {
        ESP_LOGI(TAG, "firmware upgraded, re-resetting");
        ResetChip();
    }

    // Verify chip is alive (up to 5 attempts)
    uint8_t chip_id[3] = {0};
    for (int i = 0; i < 5; i++) {
        if (ReadRegister(AXS5106L_REG_CHIP_ID, chip_id, 3)) {
            uint8_t mix_or  = chip_id[0] | chip_id[1] | chip_id[2];
            uint8_t mix_and = chip_id[0] & chip_id[1] & chip_id[2];
            if (mix_or != 0 && mix_and != 0xFF) {
                ESP_LOGI(TAG, "chip ID: 0x%02X%02X%02X", chip_id[0], chip_id[1], chip_id[2]);
                break;
            }
        }
        if (i == 4) {
            ESP_LOGE(TAG, "chip ID not responding");
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    uint8_t fw_ver[2] = {0};
    if (ReadRegister(AXS5106L_REG_FW_VER, fw_ver, 2)) {
        ESP_LOGI(TAG, "firmware version: %u", (fw_ver[0] << 8) | fw_ver[1]);
    }
    return true;
}

bool Axs5106lTouch::InitializeInput() {
    // INT pin: polled input, no ISR — LVGL timer calls LvglReadCallback every ~30 ms
    gpio_config_t int_cfg = {
        .pin_bit_mask = (1ULL << int_gpio_),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&int_cfg));

    if (lvgl_indev_) return true;  // idempotent

    lvgl_indev_ = lv_indev_create();
    if (!lvgl_indev_) {
        ESP_LOGE(TAG, "LVGL indev create failed");
        return false;
    }
    lv_indev_set_type(lvgl_indev_, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(lvgl_indev_, LvglReadCallback);
    lv_indev_set_user_data(lvgl_indev_, this);

    ESP_LOGI(TAG, "registered with LVGL");
    return true;
}

void Axs5106lTouch::Sleep() {
    sleeping_ = true;
    const uint8_t cmd = 0x03;
    WriteRegister(AXS5106L_REG_SLEEP, &cmd, 1);
    gpio_set_level(rst_gpio_, 1);
}

void Axs5106lTouch::Resume() {
    sleeping_ = false;
    ResetChip();
    vTaskDelay(pdMS_TO_TICKS(10));
}

void Axs5106lTouch::Cleanup() {
    if (lvgl_indev_) {
        lv_indev_delete(lvgl_indev_);
        lvgl_indev_ = nullptr;
    }
    if (i2c_handle_) {
        i2c_master_bus_rm_device(i2c_handle_);
        i2c_handle_ = nullptr;
    }
    // Keep RST high — shared AUD_VDD line also powers LCD backlight
    gpio_set_level(rst_gpio_, 1);
}

// -------------------------------------------------------------------------
// Private: hardware helpers
// -------------------------------------------------------------------------

void Axs5106lTouch::ResetChip() {
    // Soft reset via I2C, then hardware reset pulse
    const uint8_t rst_cmd[5] = {0xB3, 0x55, 0xAA, 0x34, 0x01};
    WriteRegister(AXS5106L_REG_RESET, rst_cmd, 5);

    gpio_set_level(rst_gpio_, 1);
    esp_rom_delay_us(50);
    gpio_set_level(rst_gpio_, 0);
    esp_rom_delay_us(50);
    vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(rst_gpio_, 1);
    vTaskDelay(pdMS_TO_TICKS(50));
}

bool Axs5106lTouch::WriteRegister(uint8_t reg, const uint8_t* data, size_t len) {
    if (!i2c_handle_ || len > 15) return false;
    uint8_t buf[16];
    buf[0] = reg;
    memcpy(&buf[1], data, len);
    for (int i = 0; i < I2C_RETRIES; i++) {
        if (i2c_master_transmit(i2c_handle_, buf, len + 1, I2C_TIMEOUT_MS) == ESP_OK) return true;
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    return false;
}

bool Axs5106lTouch::ReadRegister(uint8_t reg, uint8_t* data, size_t len) {
    if (!i2c_handle_) return false;
    for (int i = 0; i < I2C_RETRIES; i++) {
        if (i2c_master_transmit(i2c_handle_, &reg, 1, I2C_TIMEOUT_MS) == ESP_OK &&
            i2c_master_receive(i2c_handle_, data, len, I2C_TIMEOUT_MS) == ESP_OK) {
            i2c_err_streak_ = 0;
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    // 连续失败累积：4G RF 弱网时可能锁死 SDA，发 9 个 SCL 脉冲解锁整条总线
    if (++i2c_err_streak_ >= 3) {
        ESP_LOGW(TAG, "I2C bus recovery (streak=%d)", i2c_err_streak_);
        i2c_master_bus_reset(i2c_bus_);
        i2c_err_streak_ = 0;
    }
    return false;
}

bool Axs5106lTouch::CheckAndUpgradeFirmware() {
    Axs5106lUpgrade upgrader(i2c_handle_, rst_gpio_);
    switch (upgrader.CheckAndUpgrade()) {
        case Axs5106lUpgradeResult::Success:
            ESP_LOGI(TAG, "firmware upgrade OK");
            return true;
        case Axs5106lUpgradeResult::NotNeeded:
            ESP_LOGI(TAG, "firmware up to date");
            return false;
        case Axs5106lUpgradeResult::Failed:
            ESP_LOGW(TAG, "firmware upgrade failed");
            return false;
        case Axs5106lUpgradeResult::I2cError:
            ESP_LOGW(TAG, "I2C error, skipping firmware upgrade");
            return false;
    }
    return false;
}

// -------------------------------------------------------------------------
// Private: LVGL read callback
// -------------------------------------------------------------------------

void Axs5106lTouch::LvglReadCallback(lv_indev_t* indev, lv_indev_data_t* data) {
    auto* self = static_cast<Axs5106lTouch*>(lv_indev_get_user_data(indev));
    if (!self || self->sleeping_) {
        data->state = LV_INDEV_STATE_RELEASED;
        return;
    }

#if AXS5106L_TOUCH_DEBUG_OVERLAY
    if (!self->debug_dot_) {
        lv_obj_t* scr = lv_screen_active();
        if (scr) {
            self->debug_dot_ = lv_obj_create(scr);
            lv_obj_remove_style_all(self->debug_dot_);
            lv_obj_set_size(self->debug_dot_, 12, 12);
            lv_obj_set_style_bg_color(self->debug_dot_, lv_color_make(0xFF, 0x30, 0x30), 0);
            lv_obj_set_style_bg_opa(self->debug_dot_, LV_OPA_COVER, 0);
            lv_obj_set_style_radius(self->debug_dot_, LV_RADIUS_CIRCLE, 0);
            lv_obj_add_flag(self->debug_dot_, static_cast<lv_obj_flag_t>(LV_OBJ_FLAG_HIDDEN | LV_OBJ_FLAG_IGNORE_LAYOUT));
            lv_obj_move_foreground(self->debug_dot_);
        }
    }
#endif

    uint16_t x, y;
    if (self->ReadTouch(x, y)) {
        if (!self->touch_.pressed && self->wake_callback_) self->wake_callback_();
        self->touch_.pressed = true;
        self->touch_.release_count = 0;
        self->touch_.last_x = x;
        self->touch_.last_y = y;
        data->point.x = x;
        data->point.y = y;
        data->state = LV_INDEV_STATE_PRESSED;
        self->RecognizeGesture(x, y, true);

#if AXS5106L_TOUCH_DEBUG_OVERLAY
        if (self->debug_dot_) {
            lv_obj_remove_flag(self->debug_dot_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_pos(self->debug_dot_, (int16_t)x - 6, (int16_t)y - 6);
            lv_obj_move_foreground(self->debug_dot_);
        }
#endif
        return;
    }

    if (self->touch_.pressed) {
        self->touch_.release_count++;
        if (self->touch_.release_count >= RELEASE_DEBOUNCE) {
            // Confirmed release
            self->touch_.pressed = false;
            self->touch_.release_count = 0;
            data->point.x = self->touch_.last_x;
            data->point.y = self->touch_.last_y;
            data->state = LV_INDEV_STATE_RELEASED;
            self->RecognizeGesture(0, 0, false);
#if AXS5106L_TOUCH_DEBUG_OVERLAY
            if (self->debug_dot_) lv_obj_add_flag(self->debug_dot_, LV_OBJ_FLAG_HIDDEN);
#endif
        } else {
            // Hold last position during debounce window
            data->point.x = self->touch_.last_x;
            data->point.y = self->touch_.last_y;
            data->state = LV_INDEV_STATE_PRESSED;
        }
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
    }
}

// -------------------------------------------------------------------------
// Private: read one touch frame
// -------------------------------------------------------------------------

bool Axs5106lTouch::ReadTouch(uint16_t& x, uint16_t& y) {
    uint64_t now = esp_timer_get_time();

    // Gate 1: INT must be low (active during touch)
    if (gpio_get_level(int_gpio_) != 0) {
        touch_.int_low_since = 0;
        touch_.last_time = 0;
        return false;
    }
    // Gate 2: INT debounce — only on press edge; skip when already pressed
    if (!touch_.pressed) {
        if (touch_.int_low_since == 0) touch_.int_low_since = now;
        if (now - touch_.int_low_since < INT_DEBOUNCE_US) return false;
    }

    uint8_t buf[6] = {0};
    if (!ReadRegister(AXS5106L_REG_DATA, buf, 6)) return false;

    uint8_t n = AXS_POINT_NUM(buf);
    if (n == 0 || n > 1) return false;  // single-touch only; other values are noise

    uint16_t raw_x = AXS_POINT_X(buf);
    uint16_t raw_y = AXS_POINT_Y(buf);

    if (raw_x == 0xFFF && raw_y == 0xFFF) return false;  // all-ones invalid frame
    if (raw_x > TOUCH_MAX_X + 50 || raw_y > TOUCH_MAX_Y + 50) return false;

#if AXS5106L_TOUCH_DEBUG_OVERLAY
    if (raw_x < raw_stats_.raw_min_x) raw_stats_.raw_min_x = raw_x;
    if (raw_x > raw_stats_.raw_max_x) raw_stats_.raw_max_x = raw_x;
    if (raw_y < raw_stats_.raw_min_y) raw_stats_.raw_min_y = raw_y;
    if (raw_y > raw_stats_.raw_max_y) raw_stats_.raw_max_y = raw_y;
    if (++raw_stats_.sample_count % 20 == 0) {
        ESP_LOGI(TAG, "[calib] raw X=[%u..%u] Y=[%u..%u] n=%lu",
                 raw_stats_.raw_min_x, raw_stats_.raw_max_x,
                 raw_stats_.raw_min_y, raw_stats_.raw_max_y,
                 (unsigned long)raw_stats_.sample_count);
    }
#endif

    // X dead-zone compensation (applied before swap/mirror)
    uint16_t cx = (raw_x <= TOUCH_X_RAW_MIN) ? 0 :
                  (raw_x >= TOUCH_X_RAW_MAX) ? (TOUCH_MAX_X - 1) :
                  (uint16_t)((raw_x - TOUCH_X_RAW_MIN) * (TOUCH_MAX_X - 1) / TOUCH_X_RAW_RANGE);

    uint16_t sx = cx, sy = raw_y;
    if (swap_xy_)  std::swap(sx, sy);
    if (sx >= width_)  sx = width_  - 1;
    if (sy >= height_) sy = height_ - 1;
    if (mirror_x_) sx = width_  - 1 - sx;
    if (mirror_y_) sy = height_ - 1 - sy;

    // Gate 3: velocity filter — rejects 4G-RF-induced position jumps (>3000 px/s)
    if (touch_.last_time > 0) {
        uint32_t dt_ms = (now - touch_.last_time) / 1000;
        if (dt_ms > 0 && dt_ms < 500) {
            uint32_t dist = std::abs((int)sx - (int)touch_.last_x) +
                            std::abs((int)sy - (int)touch_.last_y);
            if (dist * 1000 / dt_ms > MAX_SPEED_PX_S) return false;
        }
    }

    touch_.last_time = now;
    x = sx;
    y = sy;
    return true;
}

// -------------------------------------------------------------------------
// Private: software gesture recognizer
// -------------------------------------------------------------------------

void Axs5106lTouch::RecognizeGesture(int16_t x, int16_t y, bool pressed) {
    uint64_t now = esp_timer_get_time();

    if (pressed && !gesture_.pressed) {
        gesture_.pressed    = true;
        gesture_.long_fired = false;
        gesture_.start_x    = x;
        gesture_.start_y    = y;
        gesture_.last_x     = x;
        gesture_.last_y     = y;
        gesture_.press_time = now;
        return;
    }

    if (pressed && gesture_.pressed) {
        gesture_.last_x = x;
        gesture_.last_y = y;
        if (!gesture_.long_fired) {
            int dx = x - gesture_.start_x, dy = y - gesture_.start_y;
            if (now - gesture_.press_time >= LONG_PRESS_TIME_US &&
                std::abs(dx) + std::abs(dy) < CLICK_MAX_MOVE) {
                gesture_.long_fired = true;
                gesture_.last_click_time = 0;
                ESP_LOGI(TAG, "long-press (%d,%d)", gesture_.start_x, gesture_.start_y);
                if (gesture_callback_)
                    gesture_callback_(TouchGesture::LongPress, gesture_.start_x, gesture_.start_y);
            }
        }
        return;
    }

    if (!pressed && gesture_.pressed) {
        gesture_.pressed = false;

        if (gesture_.long_fired) {
            gesture_.long_fired = false;
            ESP_LOGI(TAG, "long-press release (%d,%d)", gesture_.start_x, gesture_.start_y);
            if (gesture_callback_)
                gesture_callback_(TouchGesture::LongPressRelease, gesture_.start_x, gesture_.start_y);
            return;
        }

        int dx = gesture_.last_x - gesture_.start_x;
        int dy = gesture_.last_y - gesture_.start_y;
        int manhattan = std::abs(dx) + std::abs(dy);
        uint64_t dur  = now - gesture_.press_time;

        if (manhattan < CLICK_MAX_MOVE && dur >= CLICK_MIN_TIME_US && dur < CLICK_MAX_TIME_US) {
            int click_dist = std::abs(gesture_.start_x - gesture_.last_click_x) +
                             std::abs(gesture_.start_y - gesture_.last_click_y);
            bool is_double = gesture_.last_click_time > 0 &&
                             (gesture_.press_time - gesture_.last_click_time) < DOUBLE_CLICK_TIME_US &&
                             click_dist < DOUBLE_CLICK_DIST;
            if (is_double) {
                gesture_.last_click_time = 0;
                ESP_LOGI(TAG, "double-tap (%d,%d)", gesture_.start_x, gesture_.start_y);
                if (gesture_callback_)
                    gesture_callback_(TouchGesture::DoubleClick, gesture_.start_x, gesture_.start_y);
            } else {
                gesture_.last_click_time = now;
                gesture_.last_click_x    = gesture_.start_x;
                gesture_.last_click_y    = gesture_.start_y;
                ESP_LOGI(TAG, "tap (%d,%d)", gesture_.start_x, gesture_.start_y);
                if (gesture_callback_)
                    gesture_callback_(TouchGesture::SingleClick, gesture_.start_x, gesture_.start_y);
            }
            return;
        }

        if (manhattan >= SWIPE_THRESHOLD) {
            gesture_.last_click_time = 0;
            bool horiz = std::abs(dx) > std::abs(dy);
            TouchGesture g;
            const char*  name;
            if (horiz) {
                g    = (dx > 0) ? TouchGesture::SwipeRight : TouchGesture::SwipeLeft;
                name = (dx > 0) ? "swipe-right" : "swipe-left";
            } else {
                g    = (dy > 0) ? TouchGesture::SwipeDown : TouchGesture::SwipeUp;
                name = (dy > 0) ? "swipe-down" : "swipe-up";
            }
            ESP_LOGI(TAG, "%s (%d,%d)->(%d,%d)", name,
                     gesture_.start_x, gesture_.start_y, gesture_.last_x, gesture_.last_y);
            if (gesture_callback_)
                gesture_callback_(g, gesture_.start_x, gesture_.start_y);
        }
    }
}
