/*
 * SPDX-FileCopyrightText: 2026 mydazy
 * SPDX-License-Identifier: Apache-2.0
 *
 * AXS5106L Touchscreen Driver for ESP32-S3 + LVGL — C implementation.
 *
 * Hardware : JD9853 284×240 + AXS5106L touch controller (I2C 0x63)
 * Firmware : V2907 — landscape rotation pre-applied by chip firmware.
 *            X-axis hardware dead-zone [9..272] compensated in software → [0..283].
 *
 * Features :
 *   - Two-phase init: axs5106l_touch_new (before LVGL) / axs5106l_touch_attach_lvgl (after LVGL)
 *   - Automatic firmware upgrade on first boot
 *   - INT-pin polling (no GPIO ISR required)
 *   - Noise rejection: INT debounce (3 ms) + release debounce (2 frames) + speed filter
 *   - Software gesture recognition: tap, double-tap, long-press, 4-direction swipe
 *   - Sleep / resume
 *   - Debug overlay (AXS5106L_TOUCH_DEBUG_OVERLAY=1): red tracking dot + raw-coordinate log
 */

#include "axs5106l_touch.h"
#include "axs5106l_upgrade.h"

#include <stdlib.h>
#include <string.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

static const char *TAG = "axs5106l_touch";

/* ---------- I2C registers ---------- */
#define AXS5106L_I2C_ADDR    0x63
#define AXS5106L_REG_DATA    0x01   /* 6-byte touch frame: gesture / num / XH / XL / YH / YL */
#define AXS5106L_REG_FW_VER  0x05   /* firmware version, 2-byte big-endian */
#define AXS5106L_REG_CHIP_ID 0x08   /* chip ID, 3 bytes */
#define AXS5106L_REG_SLEEP   0x19   /* write 0x03 to enter sleep */
#define AXS5106L_REG_RESET   0xF0   /* write {B3,55,AA,34,01} for soft reset */

#define AXS_POINT_NUM(buf)  ((buf)[1])
#define AXS_POINT_X(buf)    (((uint16_t)((buf)[2] & 0x0F) << 8) | (buf)[3])
#define AXS_POINT_Y(buf)    (((uint16_t)((buf)[4] & 0x0F) << 8) | (buf)[5])

/* ---------- Screen limits ---------- */
#define TOUCH_MAX_X  284
#define TOUCH_MAX_Y  240

/* X-axis dead-zone compensation (V2907 measured: left 9 px, right 11 px; Y normal) */
#define TOUCH_X_RAW_MIN    9
#define TOUCH_X_RAW_MAX    272
#define TOUCH_X_RAW_RANGE  (TOUCH_X_RAW_MAX - TOUCH_X_RAW_MIN)  /* 263 */

/* ---------- Gesture thresholds ---------- */
/* On compact 1.83"/284-px panels, the previous 30 px swipe + 35 px tap pairing
 * left a 30~35 px ambiguous zone that biased toward tap. 25/24 split makes
 * small swipes register reliably without enlarging the tap envelope. */
#define SWIPE_THRESHOLD      25       /* min travel for swipe (px) */
#define CLICK_MAX_TIME_US    500000   /* max press duration for tap (500 ms — slow-lift tolerance) */
#define CLICK_MIN_TIME_US    40000    /* min press duration for tap, filters RF spikes (40 ms) */
#define CLICK_MAX_MOVE       24       /* max travel still considered a tap (px) */
#define LONG_PRESS_TIME_US   600000   /* long-press threshold (600 ms) */
#define DOUBLE_CLICK_TIME_US 500000   /* max interval between two taps for double-tap (500 ms) */
#define DOUBLE_CLICK_DIST    60       /* max distance between tap positions for double-tap (px) */

/* ---------- Debounce / noise ---------- */
#define INT_DEBOUNCE_US   2000  /* INT must stay low ≥2 ms to qualify as press */
#define RELEASE_DEBOUNCE  1     /* consecutive no-touch frames before reporting release */
#define MAX_SPEED_PX_S    3000  /* velocity gate: 284px@220DPI, fast swipe ~2600, RF transient >9000 */
/* RF storm protection: a press arriving within this window after the
 * previous release must persist for one additional LVGL frame before being
 * reported to LVGL. Long-idle-to-press transitions are unaffected — a single
 * tap, a long swipe, or first touch after >200 ms quiet has zero added latency. */
#define POST_RELEASE_GUARD_US  200000  /* 200 ms */

/* ---------- I2C ---------- */
#define I2C_TIMEOUT_MS  100
#define I2C_RETRIES     3

/* ------------------------------------------------------------------ */
/*  Internal state                                                     */
/* ------------------------------------------------------------------ */

typedef struct {
    bool     pressed;
    uint8_t  release_count;
    int16_t  last_x;
    int16_t  last_y;
    uint64_t last_time;          /* for velocity filter */
    uint64_t int_low_since;      /* for INT debounce */
    /* RF storm protection (see POST_RELEASE_GUARD_US): if a press edge
     * arrives within the guard window of the previous release, hold it as
     * pending for one frame; only report PRESSED if the next frame still
     * sees a valid touch. */
    bool     press_pending;
    uint64_t last_release_time;  /* 0 = never released yet */
} touch_state_t;

typedef struct {
    bool     pressed;
    bool     long_fired;
    int16_t  start_x, start_y;
    int16_t  last_x,  last_y;
    uint64_t press_time;
    uint64_t last_click_time;
    int16_t  last_click_x, last_click_y;
} gesture_state_t;

#if AXS5106L_TOUCH_DEBUG_OVERLAY
typedef struct {
    uint16_t raw_min_x;
    uint16_t raw_max_x;
    uint16_t raw_min_y;
    uint16_t raw_max_y;
    uint32_t sample_count;
} raw_stats_t;
#endif

struct axs5106l_touch_t {
    /* Configuration (immutable after _new) */
    i2c_master_bus_handle_t i2c_bus;
    i2c_master_dev_handle_t i2c_handle;
    gpio_num_t              rst_gpio;
    gpio_num_t              int_gpio;
    uint16_t                width;
    uint16_t                height;
    bool                    swap_xy;
    bool                    mirror_x;
    bool                    mirror_y;

    /* Runtime */
    lv_indev_t             *lvgl_indev;
    bool                    sleeping;     /* set by sleep/resume; read by LVGL cb */

    /* Callbacks */
    axs5106l_wake_cb_t      wake_cb;
    void                   *wake_ctx;
    axs5106l_gesture_cb_t   gesture_cb;
    void                   *gesture_ctx;

    /* I2C bus-recovery counter: when 4G RF locks SDA, consecutive read failures
     * accumulate; on threshold, i2c_master_bus_reset() sends 9 SCL pulses to unlock. */
    uint8_t                 i2c_err_streak;

    touch_state_t           touch;
    gesture_state_t         gesture;

#if AXS5106L_TOUCH_DEBUG_OVERLAY
    raw_stats_t             raw_stats;
    lv_obj_t               *debug_dot;
#endif
};

/* ------------------------------------------------------------------ */
/*  Forward declarations                                               */
/* ------------------------------------------------------------------ */

static void reset_chip(axs5106l_touch_handle_t self);
static bool check_and_upgrade_firmware(axs5106l_touch_handle_t self);
static bool write_register(axs5106l_touch_handle_t self, uint8_t reg, const uint8_t *data, size_t len);
static bool read_register(axs5106l_touch_handle_t self, uint8_t reg, uint8_t *data, size_t len);
static bool read_touch(axs5106l_touch_handle_t self, uint16_t *out_x, uint16_t *out_y);
static void recognize_gesture(axs5106l_touch_handle_t self, int16_t x, int16_t y, bool pressed);
static void lvgl_read_cb(lv_indev_t *indev, lv_indev_data_t *data);

/* ------------------------------------------------------------------ */
/*  Lifecycle                                                          */
/* ------------------------------------------------------------------ */

esp_err_t axs5106l_touch_new(const axs5106l_touch_config_t *cfg,
                             axs5106l_touch_handle_t *out)
{
    if (cfg == NULL || out == NULL || cfg->i2c_bus == NULL) return ESP_ERR_INVALID_ARG;

    axs5106l_touch_handle_t self = (axs5106l_touch_handle_t)calloc(1, sizeof(struct axs5106l_touch_t));
    if (self == NULL) return ESP_ERR_NO_MEM;

    self->i2c_bus  = cfg->i2c_bus;
    self->rst_gpio = cfg->rst_gpio;
    self->int_gpio = cfg->int_gpio;
    self->width    = cfg->width;
    self->height   = cfg->height;
    self->swap_xy  = cfg->swap_xy;
    self->mirror_x = cfg->mirror_x;
    self->mirror_y = cfg->mirror_y;
#if AXS5106L_TOUCH_DEBUG_OVERLAY
    self->raw_stats.raw_min_x = 0xFFFF;
    self->raw_stats.raw_min_y = 0xFFFF;
#endif

    ESP_LOGI(TAG, "init RST=GPIO%d INT=GPIO%d %dx%d",
             self->rst_gpio, self->int_gpio, self->width, self->height);

    gpio_config_t rst_cfg = {
        .pin_bit_mask = (1ULL << self->rst_gpio),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    esp_err_t ret = gpio_config(&rst_cfg);
    if (ret != ESP_OK) goto fail;
    gpio_set_level(self->rst_gpio, 1);

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = AXS5106L_I2C_ADDR,
        .scl_speed_hz    = 400000,
        .scl_wait_us     = 0,
        .flags           = { .disable_ack_check = 0 },
    };
    ret = i2c_master_bus_add_device(self->i2c_bus, &dev_cfg, &self->i2c_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "I2C add device failed: %s", esp_err_to_name(ret));
        goto fail;
    }

    reset_chip(self);

    if (check_and_upgrade_firmware(self)) {
        ESP_LOGI(TAG, "firmware upgraded, re-resetting");
        reset_chip(self);
    }

    /* Verify chip is alive (up to 5 attempts). */
    uint8_t chip_id[3] = {0};
    bool alive = false;
    for (int i = 0; i < 5; i++) {
        if (read_register(self, AXS5106L_REG_CHIP_ID, chip_id, 3)) {
            uint8_t mix_or  = chip_id[0] | chip_id[1] | chip_id[2];
            uint8_t mix_and = chip_id[0] & chip_id[1] & chip_id[2];
            if (mix_or != 0 && mix_and != 0xFF) {
                ESP_LOGI(TAG, "chip ID: 0x%02X%02X%02X", chip_id[0], chip_id[1], chip_id[2]);
                alive = true;
                break;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    if (!alive) {
        ESP_LOGE(TAG, "chip ID not responding");
        ret = ESP_ERR_NOT_FOUND;
        goto fail;
    }

    uint8_t fw_ver[2] = {0};
    if (read_register(self, AXS5106L_REG_FW_VER, fw_ver, 2)) {
        ESP_LOGI(TAG, "firmware version: %u", ((uint16_t)fw_ver[0] << 8) | fw_ver[1]);
    }

    *out = self;
    return ESP_OK;

fail:
    if (self->i2c_handle != NULL) i2c_master_bus_rm_device(self->i2c_handle);
    free(self);
    return ret;
}

esp_err_t axs5106l_touch_attach_lvgl(axs5106l_touch_handle_t self)
{
    if (self == NULL) return ESP_ERR_INVALID_ARG;
    if (self->lvgl_indev != NULL) return ESP_OK;  /* idempotent */

    /* INT pin: polled input, no ISR — LVGL timer calls lvgl_read_cb every ~30 ms. */
    gpio_config_t int_cfg = {
        .pin_bit_mask = (1ULL << self->int_gpio),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    esp_err_t ret = gpio_config(&int_cfg);
    if (ret != ESP_OK) return ret;

    self->lvgl_indev = lv_indev_create();
    if (self->lvgl_indev == NULL) {
        ESP_LOGE(TAG, "LVGL indev create failed");
        return ESP_ERR_NO_MEM;
    }
    lv_indev_set_type(self->lvgl_indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(self->lvgl_indev, lvgl_read_cb);
    lv_indev_set_user_data(self->lvgl_indev, self);

    ESP_LOGI(TAG, "registered with LVGL");
    return ESP_OK;
}

esp_err_t axs5106l_touch_del(axs5106l_touch_handle_t self)
{
    if (self == NULL) return ESP_ERR_INVALID_ARG;

    if (self->lvgl_indev != NULL) {
        lv_indev_delete(self->lvgl_indev);
        self->lvgl_indev = NULL;
    }
    if (self->i2c_handle != NULL) {
        i2c_master_bus_rm_device(self->i2c_handle);
        self->i2c_handle = NULL;
    }
    /* Keep RST high — shared AUD_VDD line also powers LCD backlight. */
    gpio_set_level(self->rst_gpio, 1);
    free(self);
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/*  Power                                                              */
/* ------------------------------------------------------------------ */

esp_err_t axs5106l_touch_sleep(axs5106l_touch_handle_t self)
{
    if (self == NULL) return ESP_ERR_INVALID_ARG;
    self->sleeping = true;
    const uint8_t cmd = 0x03;
    write_register(self, AXS5106L_REG_SLEEP, &cmd, 1);
    gpio_set_level(self->rst_gpio, 1);
    return ESP_OK;
}

esp_err_t axs5106l_touch_resume(axs5106l_touch_handle_t self)
{
    if (self == NULL) return ESP_ERR_INVALID_ARG;
    self->sleeping = false;
    reset_chip(self);
    vTaskDelay(pdMS_TO_TICKS(10));
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/*  Callbacks                                                          */
/* ------------------------------------------------------------------ */

void axs5106l_touch_set_wake_callback(axs5106l_touch_handle_t self,
                                      axs5106l_wake_cb_t cb,
                                      void *user_ctx)
{
    if (self == NULL) return;
    self->wake_cb  = cb;
    self->wake_ctx = user_ctx;
}

void axs5106l_touch_set_gesture_callback(axs5106l_touch_handle_t self,
                                         axs5106l_gesture_cb_t cb,
                                         void *user_ctx)
{
    if (self == NULL) return;
    self->gesture_cb  = cb;
    self->gesture_ctx = user_ctx;
}

lv_indev_t *axs5106l_touch_get_lvgl_device(axs5106l_touch_handle_t self)
{
    return (self != NULL) ? self->lvgl_indev : NULL;
}

/* ------------------------------------------------------------------ */
/*  Hardware helpers                                                   */
/* ------------------------------------------------------------------ */

static void reset_chip(axs5106l_touch_handle_t self)
{
    /* Soft reset via I2C, then hardware reset pulse. */
    const uint8_t rst_cmd[5] = {0xB3, 0x55, 0xAA, 0x34, 0x01};
    write_register(self, AXS5106L_REG_RESET, rst_cmd, 5);

    gpio_set_level(self->rst_gpio, 1);
    esp_rom_delay_us(50);
    gpio_set_level(self->rst_gpio, 0);
    esp_rom_delay_us(50);
    vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(self->rst_gpio, 1);
    vTaskDelay(pdMS_TO_TICKS(50));
}

static bool write_register(axs5106l_touch_handle_t self, uint8_t reg, const uint8_t *data, size_t len)
{
    if (self->i2c_handle == NULL || len > 15) return false;
    uint8_t buf[16];
    buf[0] = reg;
    memcpy(&buf[1], data, len);
    for (int i = 0; i < I2C_RETRIES; i++) {
        if (i2c_master_transmit(self->i2c_handle, buf, len + 1, I2C_TIMEOUT_MS) == ESP_OK) return true;
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    return false;
}

static bool read_register(axs5106l_touch_handle_t self, uint8_t reg, uint8_t *data, size_t len)
{
    if (self->i2c_handle == NULL) return false;
    for (int i = 0; i < I2C_RETRIES; i++) {
        if (i2c_master_transmit(self->i2c_handle, &reg, 1, I2C_TIMEOUT_MS) == ESP_OK &&
            i2c_master_receive(self->i2c_handle, data, len, I2C_TIMEOUT_MS) == ESP_OK) {
            self->i2c_err_streak = 0;
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    /* Consecutive-failure recovery: strong RF interference can lock SDA low.
     * Reset the bus (issues 9 SCL pulses internally) per the I2C recovery spec. */
    if (++self->i2c_err_streak >= 3) {
        ESP_LOGW(TAG, "I2C bus recovery (streak=%d)", self->i2c_err_streak);
        i2c_master_bus_reset(self->i2c_bus);
        self->i2c_err_streak = 0;
        /* Clear transient touch state so a phantom press cannot survive across the reset. */
        self->touch.int_low_since = 0;
        self->touch.last_time     = 0;
        self->touch.press_pending = false;
    }
    return false;
}

static bool check_and_upgrade_firmware(axs5106l_touch_handle_t self)
{
    axs5106l_upgrade_handle_t upgrader = NULL;
    if (axs5106l_upgrade_init(self->i2c_handle, self->rst_gpio, &upgrader) != ESP_OK) {
        ESP_LOGW(TAG, "upgrade init failed");
        return false;
    }
    bool upgraded = false;
    switch (axs5106l_upgrade_run(upgrader)) {
        case AXS5106L_UPGRADE_SUCCESS:
            ESP_LOGI(TAG, "firmware upgrade OK");
            upgraded = true;
            break;
        case AXS5106L_UPGRADE_NOT_NEEDED:
            ESP_LOGI(TAG, "firmware up to date");
            break;
        case AXS5106L_UPGRADE_FAILED:
            ESP_LOGW(TAG, "firmware upgrade failed");
            break;
        case AXS5106L_UPGRADE_I2C_ERROR:
            ESP_LOGW(TAG, "I2C error, skipping firmware upgrade");
            break;
    }
    axs5106l_upgrade_del(upgrader);
    return upgraded;
}

/* ------------------------------------------------------------------ */
/*  LVGL read callback                                                 */
/* ------------------------------------------------------------------ */

static void lvgl_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    axs5106l_touch_handle_t self = (axs5106l_touch_handle_t)lv_indev_get_user_data(indev);
    if (self == NULL || self->sleeping) {
        data->state = LV_INDEV_STATE_RELEASED;
        return;
    }

#if AXS5106L_TOUCH_DEBUG_OVERLAY
    if (self->debug_dot == NULL) {
        lv_obj_t *scr = lv_screen_active();
        if (scr != NULL) {
            self->debug_dot = lv_obj_create(scr);
            lv_obj_remove_style_all(self->debug_dot);
            lv_obj_set_size(self->debug_dot, 12, 12);
            lv_obj_set_style_bg_color(self->debug_dot, lv_color_make(0xFF, 0x30, 0x30), 0);
            lv_obj_set_style_bg_opa(self->debug_dot, LV_OPA_COVER, 0);
            lv_obj_set_style_radius(self->debug_dot, LV_RADIUS_CIRCLE, 0);
            lv_obj_add_flag(self->debug_dot, (lv_obj_flag_t)(LV_OBJ_FLAG_HIDDEN | LV_OBJ_FLAG_IGNORE_LAYOUT));
            lv_obj_move_foreground(self->debug_dot);
        }
    }
#endif

    uint16_t x = 0, y = 0;
    uint64_t now = esp_timer_get_time();
    if (read_touch(self, &x, &y)) {
        /* Press edge: if the previous release was very recent, require one
         * extra frame of valid touch before reporting. Idle-to-press skips
         * this — first tap and long swipes have zero added latency. */
        if (!self->touch.pressed) {
            bool guard_active = self->touch.last_release_time != 0 &&
                                (now - self->touch.last_release_time) < POST_RELEASE_GUARD_US;
            if (guard_active && !self->touch.press_pending) {
                self->touch.press_pending = true;
                /* Hold LVGL idle for this frame; pending will resolve next frame. */
                data->state = LV_INDEV_STATE_RELEASED;
                return;
            }
            self->touch.press_pending = false;
            if (self->wake_cb != NULL) self->wake_cb(self->wake_ctx);
        }
        self->touch.pressed       = true;
        self->touch.release_count = 0;
        self->touch.last_x        = x;
        self->touch.last_y        = y;
        data->point.x = x;
        data->point.y = y;
        data->state   = LV_INDEV_STATE_PRESSED;
        recognize_gesture(self, x, y, true);

#if AXS5106L_TOUCH_DEBUG_OVERLAY
        if (self->debug_dot != NULL) {
            lv_obj_remove_flag(self->debug_dot, LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_pos(self->debug_dot, (int16_t)x - 6, (int16_t)y - 6);
            lv_obj_move_foreground(self->debug_dot);
        }
#endif
        return;
    }

    /* No valid touch this frame. */
    if (self->touch.press_pending) {
        /* Pending press lost on the very next frame → single-frame RF spike. Drop it. */
        self->touch.press_pending = false;
        data->state = LV_INDEV_STATE_RELEASED;
        return;
    }

    if (self->touch.pressed) {
        self->touch.release_count++;
        if (self->touch.release_count >= RELEASE_DEBOUNCE) {
            /* Confirmed release. */
            self->touch.pressed           = false;
            self->touch.release_count     = 0;
            self->touch.last_release_time = now;
            data->point.x = self->touch.last_x;
            data->point.y = self->touch.last_y;
            data->state   = LV_INDEV_STATE_RELEASED;
            recognize_gesture(self, 0, 0, false);
#if AXS5106L_TOUCH_DEBUG_OVERLAY
            if (self->debug_dot != NULL) lv_obj_add_flag(self->debug_dot, LV_OBJ_FLAG_HIDDEN);
#endif
        } else {
            /* Hold last position during debounce window. */
            data->point.x = self->touch.last_x;
            data->point.y = self->touch.last_y;
            data->state   = LV_INDEV_STATE_PRESSED;
        }
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
    }
}

/* ------------------------------------------------------------------ */
/*  Read one touch frame                                               */
/* ------------------------------------------------------------------ */

static bool read_touch(axs5106l_touch_handle_t self, uint16_t *out_x, uint16_t *out_y)
{
    uint64_t now = esp_timer_get_time();

    /* Gate 1: INT must be low (active during touch). */
    if (gpio_get_level(self->int_gpio) != 0) {
        self->touch.int_low_since = 0;
        self->touch.last_time     = 0;
        return false;
    }
    /* Gate 2: INT debounce — only on press edge; skip when already pressed. */
    if (!self->touch.pressed) {
        if (self->touch.int_low_since == 0) self->touch.int_low_since = now;
        if (now - self->touch.int_low_since < INT_DEBOUNCE_US) return false;
    }

    uint8_t buf[6] = {0};
    if (!read_register(self, AXS5106L_REG_DATA, buf, 6)) return false;

    uint8_t n = AXS_POINT_NUM(buf);
    if (n == 0 || n > 1) return false;  /* single-touch only; other values are noise */

    uint16_t raw_x = AXS_POINT_X(buf);
    uint16_t raw_y = AXS_POINT_Y(buf);

    if (raw_x == 0xFFF && raw_y == 0xFFF) return false;  /* all-ones invalid frame */
    if (raw_x > TOUCH_MAX_X + 50 || raw_y > TOUCH_MAX_Y + 50) return false;

#if AXS5106L_TOUCH_DEBUG_OVERLAY
    if (raw_x < self->raw_stats.raw_min_x) self->raw_stats.raw_min_x = raw_x;
    if (raw_x > self->raw_stats.raw_max_x) self->raw_stats.raw_max_x = raw_x;
    if (raw_y < self->raw_stats.raw_min_y) self->raw_stats.raw_min_y = raw_y;
    if (raw_y > self->raw_stats.raw_max_y) self->raw_stats.raw_max_y = raw_y;
    if (++self->raw_stats.sample_count % 20 == 0) {
        ESP_LOGI(TAG, "[calib] raw X=[%u..%u] Y=[%u..%u] n=%lu",
                 self->raw_stats.raw_min_x, self->raw_stats.raw_max_x,
                 self->raw_stats.raw_min_y, self->raw_stats.raw_max_y,
                 (unsigned long)self->raw_stats.sample_count);
    }
#endif

    /* X dead-zone compensation (applied before swap/mirror). */
    uint16_t cx = (raw_x <= TOUCH_X_RAW_MIN) ? 0 :
                  (raw_x >= TOUCH_X_RAW_MAX) ? (TOUCH_MAX_X - 1) :
                  (uint16_t)((raw_x - TOUCH_X_RAW_MIN) * (TOUCH_MAX_X - 1) / TOUCH_X_RAW_RANGE);

    uint16_t sx = cx, sy = raw_y;
    if (self->swap_xy) {
        uint16_t tmp = sx; sx = sy; sy = tmp;
    }
    if (sx >= self->width)  sx = self->width  - 1;
    if (sy >= self->height) sy = self->height - 1;
    if (self->mirror_x) sx = self->width  - 1 - sx;
    if (self->mirror_y) sy = self->height - 1 - sy;

    /* Gate 3: velocity filter — rejects 4G-RF-induced position jumps (>3000 px/s). */
    if (self->touch.last_time > 0) {
        uint32_t dt_ms = (uint32_t)((now - self->touch.last_time) / 1000);
        if (dt_ms > 0 && dt_ms < 500) {
            uint32_t dist = (uint32_t)(abs((int)sx - (int)self->touch.last_x) +
                                       abs((int)sy - (int)self->touch.last_y));
            if (dist * 1000 / dt_ms > MAX_SPEED_PX_S) return false;
        }
    }

    self->touch.last_time = now;
    *out_x = sx;
    *out_y = sy;
    return true;
}

/* ------------------------------------------------------------------ */
/*  Software gesture recognizer                                        */
/* ------------------------------------------------------------------ */

static inline void fire_gesture(axs5106l_touch_handle_t self, axs5106l_gesture_t g, int16_t x, int16_t y)
{
    if (self->gesture_cb != NULL) self->gesture_cb(g, x, y, self->gesture_ctx);
}

static void recognize_gesture(axs5106l_touch_handle_t self, int16_t x, int16_t y, bool pressed)
{
    uint64_t now = esp_timer_get_time();
    gesture_state_t *g = &self->gesture;

    if (pressed && !g->pressed) {
        g->pressed    = true;
        g->long_fired = false;
        g->start_x    = x;
        g->start_y    = y;
        g->last_x     = x;
        g->last_y     = y;
        g->press_time = now;
        return;
    }

    if (pressed && g->pressed) {
        g->last_x = x;
        g->last_y = y;
        if (!g->long_fired) {
            int dx = x - g->start_x, dy = y - g->start_y;
            if (now - g->press_time >= LONG_PRESS_TIME_US &&
                abs(dx) + abs(dy) < CLICK_MAX_MOVE) {
                g->long_fired      = true;
                g->last_click_time = 0;
                ESP_LOGI(TAG, "long-press (%d,%d)", g->start_x, g->start_y);
                fire_gesture(self, AXS5106L_GESTURE_LONG_PRESS, g->start_x, g->start_y);
            }
        }
        return;
    }

    if (!pressed && g->pressed) {
        g->pressed = false;

        if (g->long_fired) {
            g->long_fired = false;
            ESP_LOGI(TAG, "long-press release (%d,%d)", g->start_x, g->start_y);
            fire_gesture(self, AXS5106L_GESTURE_LONG_PRESS_RELEASE, g->start_x, g->start_y);
            return;
        }

        int dx = g->last_x - g->start_x;
        int dy = g->last_y - g->start_y;
        int manhattan = abs(dx) + abs(dy);
        uint64_t dur  = now - g->press_time;

        if (manhattan < CLICK_MAX_MOVE && dur >= CLICK_MIN_TIME_US && dur < CLICK_MAX_TIME_US) {
            int click_dist = abs(g->start_x - g->last_click_x) +
                             abs(g->start_y - g->last_click_y);
            bool is_double = g->last_click_time > 0 &&
                             (g->press_time - g->last_click_time) < DOUBLE_CLICK_TIME_US &&
                             click_dist < DOUBLE_CLICK_DIST;
            if (is_double) {
                g->last_click_time = 0;
                ESP_LOGI(TAG, "double-tap (%d,%d)", g->start_x, g->start_y);
                fire_gesture(self, AXS5106L_GESTURE_DOUBLE_CLICK, g->start_x, g->start_y);
            } else {
                g->last_click_time = now;
                g->last_click_x    = g->start_x;
                g->last_click_y    = g->start_y;
                ESP_LOGI(TAG, "tap (%d,%d)", g->start_x, g->start_y);
                fire_gesture(self, AXS5106L_GESTURE_SINGLE_CLICK, g->start_x, g->start_y);
            }
            return;
        }

        if (manhattan >= SWIPE_THRESHOLD) {
            g->last_click_time = 0;
            bool horiz = abs(dx) > abs(dy);
            axs5106l_gesture_t kind;
            const char *name;
            if (horiz) {
                kind = (dx > 0) ? AXS5106L_GESTURE_SWIPE_RIGHT : AXS5106L_GESTURE_SWIPE_LEFT;
                name = (dx > 0) ? "swipe-right" : "swipe-left";
            } else {
                kind = (dy > 0) ? AXS5106L_GESTURE_SWIPE_DOWN : AXS5106L_GESTURE_SWIPE_UP;
                name = (dy > 0) ? "swipe-down" : "swipe-up";
            }
            ESP_LOGI(TAG, "%s (%d,%d)->(%d,%d)", name,
                     g->start_x, g->start_y, g->last_x, g->last_y);
            fire_gesture(self, kind, g->start_x, g->start_y);
        }
    }
}
