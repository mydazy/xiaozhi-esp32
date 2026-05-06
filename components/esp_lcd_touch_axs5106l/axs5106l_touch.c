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
 *   - Hybrid INT handling: GPIO_NEGEDGE ISR for RF-storm detection (edge counting only,
 *     never reads I2C), LVGL timer (~30 ms) polls level + reads touch frame
 *   - Noise rejection: INT debounce (8 ms) + release debounce (2 frames) + speed filter
 *     (2500 px/s) + jitter-variance gate + INT-edge storm detector (12 edges/s → 2 s mute)
 *   - Software gesture recognition: tap, double-tap, long-press, 4-direction swipe
 *     with trajectory-efficiency gate against RF-synthesised pseudo-swipes
 *   - Sleep / resume (also disables INT ISR to prevent wake from RF noise)
 *   - Debug overlay (AXS5106L_TOUCH_DEBUG_OVERLAY=1): red tracking dot + raw-coordinate log
 */

#include "axs5106l_touch.h"
#include "axs5106l_upgrade.h"

#include <stdlib.h>
#include <string.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <esp_attr.h>
#include <driver/gpio.h>
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

/* v3.0 (2026-05) 灵敏度提升 — 儿童快触场景：
 *   CLICK_MIN_TIME_US 100→60ms（更轻的瞬触可识别）
 *   CLICK_MIN_FRAMES  3→2（≥2 帧 ~60ms @ 30ms LVGL）
 *   CLICK_MAX_MOVE   20→25（手抖容忍）
 *   JITTER_LIMIT_FOR_TAP 24→32（震动容忍）
 *   LONG_PRESS_TIME_US 500ms 保持（与 BOOT 长按节奏一致）
 *
 * 配套 RF 防御不变：storm 检测 + INT 防抖 + trajectory gate。
 * 风险：CLICK_MIN_FRAMES=2 会让单帧 RF 脉冲更易误识别为 tap，
 *      靠 storm 检测 + JITTER_LIMIT 兜底；量产前需做 4G 弱信号实测。 */
#define SWIPE_THRESHOLD      20       /* min travel for swipe (px) */
#define CLICK_MAX_TIME_US    800000   /* max press duration for tap (800 ms) */
#define CLICK_MIN_TIME_US     60000   /* min press duration for tap (60 ms — sensitive light tap) */
#define CLICK_MAX_MOVE       25       /* max travel still considered a tap (px) — 儿童手抖容忍 */
#define LONG_PRESS_TIME_US   400000   /* long-press threshold (400 ms — 比 500 ms 更敏感，儿童 PTT 起手更跟手) */
#define DOUBLE_CLICK_TIME_US 600000   /* max interval between two taps for double-tap (600 ms) */
#define DOUBLE_CLICK_DIST    50       /* max distance between tap positions for double-tap (px) */
#define CLICK_MIN_FRAMES      2       /* tap needs ≥2 PRESSED frames (~60 ms @ 30 ms LVGL) */
#define SWIPE_MIN_TIME_US    150000   /* min swipe duration (150 ms) */
#define SWIPE_MIN_FRAMES     5        /* swipe needs ≥5 PRESSED frames */
#define LONG_PRESS_MIN_FRAMES 12      /* long-press needs ≥12 frames (~360 ms) — match 500 ms threshold */
/* Jitter budget — 儿童手部抖动比成人大，从 24 放宽到 32（每帧 ~6.4 px 平均位移仍允）。 */
#define JITTER_LIMIT_FOR_TAP   32
/* Swipe trajectory efficiency: real fingers travel in (mostly) straight lines,
 * so the per-frame jitter sum stays close to the start-to-end manhattan distance.
 * RF coupling synthesises pseudo-swipes by jumping between random coordinates,
 * inflating jitter_sum to several times manhattan. We require:
 *   jitter_sum ≤ manhattan × SWIPE_TRAJECTORY_RATIO + SWIPE_TRAJECTORY_BIAS
 * with bias allowing minor finger micro-movements on short swipes. */
#define SWIPE_TRAJECTORY_RATIO 2     /* allow up to 2× direct distance for jitter */
#define SWIPE_TRAJECTORY_BIAS  20    /* +20 px buffer for short legitimate swipes */

/* ---------- Debounce / noise ---------- */
/* 8 ms (was 15 ms): real press lasts 50+ ms so 8 ms is invisible to the user,
 * yet still rejects RF spikes which are typically 1-5 ms. 15 ms was adding
 * perceptible lag when stacked with the gesture-layer time gate. */
#define INT_DEBOUNCE_US   8000  /* INT must stay low ≥8 ms to qualify as press */
#define RELEASE_DEBOUNCE  2     /* consecutive no-touch frames before reporting release */
/* 2500 px/s: real fast swipe peaks ~1500-2000 px/s on a 1.83" panel, RF coordinate
 * jumps are typically 5000+ px/s (single-frame teleport across screen). 2500 keeps
 * 25 % margin for vigorous real swipes while denying every RF teleport observed. */
#define MAX_SPEED_PX_S    2500
/* Post-release guard: minimal hold-off so true rapid double-tap stays snappy.
 * The storm detector + INT debounce already shoulder phantom-press defence. */
#define POST_RELEASE_GUARD_US  100000  /* 100 ms */

/* INT-falling-edge storm detection — strongest physical discriminator.
 * A real finger produces 1–3 INT falling edges per second; 4G RF coupling
 * generates 50+ per second. Once the rolling 1-second count crosses the
 * threshold we suppress all touch reads for 5 seconds, which is long enough
 * to outlast a typical 4G TX burst (CSQ poll, MQTT publish, OTA chunk).
 *
 * The ISR runs in IRAM, increments a 32-bit counter (atomic on Xtensa LX7),
 * and never blocks. The LVGL read callback samples + windowizes the counter
 * — no queue, no semaphore, no priority inversion risk. */
#define INT_STORM_WINDOW_US   1000000  /* rolling count window: 1 s */
/* 12 edges/s threshold (was 5):
 *   - 5 was rejecting real presses — finger contact toggles INT 3–5 times/s
 *     during normal touch, occasionally crossing 5 → false storm → 2 s mute
 *     → user perceives "touch dead, must press long".
 *   - 4G TX bursts produce 30–140 edges/s (real measured), so 12 still catches
 *     them with 2.5× margin while passing real fingers untouched.
 *   - LVGL drag-into-click is now blocked by per-widget PRESS_LOCK, so we no
 *     longer need the storm detector to fire on borderline cases. */
#define INT_STORM_THRESHOLD   12       /* edges/s above which RF storm declared */
#define INT_STORM_MUTE_US     2000000  /* mute touch reads for 2 s after storm */

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
    uint8_t  sample_count;          /* PRESSED frames in current press; tap requires ≥CLICK_MIN_FRAMES */
    /* Inter-frame jitter sum: real fingers stay within ≤2 px between LVGL frames;
     * RF-synthesised "stable" coordinates still exhibit 5–30 px hops between
     * consecutive samples. A press whose jitter sum exceeds JITTER_LIMIT_FOR_TAP
     * is downgraded to "unstable" and tap is suppressed (swipe still allowed). */
    uint16_t jitter_sum;
    bool     jitter_unstable;
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
    /* Configuration (immutable after _new) — v3.0+ via i2c_bus_worker */
    i2c_worker_handle_t     worker;
    i2c_worker_dev_t       *dev;
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

    /* INT-edge storm detector. ISR-only writers, LVGL-task readers — 32-bit
     * scalar reads/writes are atomic on Xtensa, no lock needed. */
    volatile uint32_t       int_edge_count;
    uint32_t                int_edge_window_baseline;
    uint64_t                int_edge_window_start_us;
    uint64_t                storm_mute_until_us;
    bool                    isr_installed;

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

static void IRAM_ATTR int_falling_edge_isr(void *arg);
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
    if (cfg == NULL || out == NULL || cfg->worker == NULL) return ESP_ERR_INVALID_ARG;
    *out = NULL;   /* 防御：调用方未初始化指针时不留垃圾 */

    axs5106l_touch_handle_t self = (axs5106l_touch_handle_t)calloc(1, sizeof(struct axs5106l_touch_t));
    if (self == NULL) return ESP_ERR_NO_MEM;

    self->worker   = cfg->worker;
    self->rst_gpio = cfg->rst_gpio;
    self->int_gpio = cfg->int_gpio;
    self->width    = cfg->width;
    self->height   = cfg->height;
    /* v4.0 极简：swap_xy/mirror_* 项目硬编码 false（V2907 firmware 已内部 rotation）。
       移除 cfg 字段以减少调用方判断；如未来需要不同方向请重新 expose。 */
    self->swap_xy  = false;
    self->mirror_x = false;
    self->mirror_y = false;
    /* 回调从 cfg 一次性传入，删除 set_wake_callback / set_gesture_callback 旧 API */
    self->wake_cb     = cfg->wake_cb;
    self->wake_ctx    = cfg->cb_ctx;
    self->gesture_cb  = cfg->gesture_cb;
    self->gesture_ctx = cfg->cb_ctx;
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

    self->dev = i2c_worker_add_device(self->worker, AXS5106L_I2C_ADDR, 400000);
    if (self->dev == NULL) {
        ESP_LOGE(TAG, "i2c_worker_add_device failed");
        ret = ESP_ERR_NO_MEM;
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
    if (self->dev != NULL) i2c_worker_remove_device(self->dev);
    free(self);
    return ret;
}

esp_err_t axs5106l_touch_attach_lvgl(axs5106l_touch_handle_t self)
{
    if (self == NULL) return ESP_ERR_INVALID_ARG;
    if (self->lvgl_indev != NULL) return ESP_OK;  /* idempotent */

    /* INT pin: input + GPIO_NEGEDGE ISR for RF storm detection only (edge counter).
     * Actual touch reads still happen in lvgl_read_cb at ~30 ms cadence by polling
     * INT level + reading I2C. Internal pull-up is the minimum-viable defence;
     * production HW should add 4.7 kΩ external pull-up to VCC-3.3V for RF immunity. */
    gpio_config_t int_cfg = {
        .pin_bit_mask = (1ULL << self->int_gpio),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_NEGEDGE,
    };
    esp_err_t ret = gpio_config(&int_cfg);
    if (ret != ESP_OK) return ret;

    /* Install global ISR service (idempotent — returns ALREADY if installed elsewhere). */
    esp_err_t isr_svc = gpio_install_isr_service(0);
    if (isr_svc != ESP_OK && isr_svc != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "gpio_install_isr_service failed: %s, storm detection disabled",
                 esp_err_to_name(isr_svc));
    } else if (gpio_isr_handler_add(self->int_gpio, int_falling_edge_isr, self) == ESP_OK) {
        self->isr_installed = true;
        self->int_edge_window_start_us = esp_timer_get_time();
        ESP_LOGI(TAG, "INT edge ISR installed for RF storm detection");
    } else {
        ESP_LOGW(TAG, "gpio_isr_handler_add failed, storm detection disabled");
    }

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

/* v4.0 极简：删除 axs5106l_touch_del()
 *   量产固件生命周期内驱动从不释放（与板级 Board 单例同寿）。
 *   保留意味着维护 ISR 移除/I2C device 释放/RST 状态等冗余路径，徒增 bug 面。
 *   如真要做单元测试 mock，请使用 stub 替代而非 del。 */

/* ------------------------------------------------------------------ */
/*  Power                                                              */
/* ------------------------------------------------------------------ */

esp_err_t axs5106l_touch_sleep(axs5106l_touch_handle_t self)
{
    if (self == NULL) return ESP_ERR_INVALID_ARG;
    self->sleeping = true;
    if (self->isr_installed) gpio_intr_disable(self->int_gpio);
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
    /* Reset storm-detection window so any edges latched across sleep
     * don't poison the first second after resume. */
    self->int_edge_window_start_us = esp_timer_get_time();
    self->int_edge_window_baseline = self->int_edge_count;
    self->storm_mute_until_us      = 0;
    if (self->isr_installed) gpio_intr_enable(self->int_gpio);
    return ESP_OK;
}

/* v4.0 极简：删除 4 个 API（迁移至 cfg / 完全弃用）
 *   - axs5106l_touch_set_wake_callback     → cfg.wake_cb（init 时一次性传入）
 *   - axs5106l_touch_set_gesture_callback  → cfg.gesture_cb
 *   - axs5106l_touch_get_lvgl_device       → 外部不需要，删除
 *   - axs5106l_touch_del                   → 量产 N/A（生命周期内不释放）
 */

/* ------------------------------------------------------------------ */
/*  INT-edge storm detector                                            */
/* ------------------------------------------------------------------ */

/* IRAM-resident ISR. Increments a 32-bit counter only — never reads I2C, never
 * logs, never blocks. 32-bit scalar writes are atomic on Xtensa LX7. */
static void IRAM_ATTR int_falling_edge_isr(void *arg)
{
    axs5106l_touch_handle_t self = (axs5106l_touch_handle_t)arg;
    self->int_edge_count++;
}

/* Called from lvgl_read_cb on the LVGL task. Returns true if RF storm is in
 * progress and reads should be suppressed. */
static bool storm_detected(axs5106l_touch_handle_t self, uint64_t now)
{
    if (now < self->storm_mute_until_us) {
        /* Keep baseline pinned to current count during mute, otherwise edges
         * accumulated while muted would land in the next 1 s window and
         * re-trigger the storm immediately on exit. */
        self->int_edge_window_baseline = self->int_edge_count;
        self->int_edge_window_start_us = now;
        return true;
    }
    if (!self->isr_installed) return false;

    if (now - self->int_edge_window_start_us >= INT_STORM_WINDOW_US) {
        uint32_t edges = self->int_edge_count - self->int_edge_window_baseline;
        if (edges >= INT_STORM_THRESHOLD) {
            ESP_LOGW(TAG, "RF storm: %lu INT edges in last %lu ms, mute %lu ms",
                     (unsigned long)edges,
                     (unsigned long)((now - self->int_edge_window_start_us) / 1000),
                     (unsigned long)(INT_STORM_MUTE_US / 1000));
            self->storm_mute_until_us = now + INT_STORM_MUTE_US;
            /* Drop any latched press so LVGL sees a clean release. */
            self->touch.pressed       = false;
            self->touch.press_pending = false;
            self->touch.int_low_since = 0;
            self->touch.last_time     = 0;
            self->int_edge_window_start_us = now;
            self->int_edge_window_baseline = self->int_edge_count;
            return true;
        }
        self->int_edge_window_start_us = now;
        self->int_edge_window_baseline = self->int_edge_count;
    }
    return false;
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

/* v3.0+ I2C helpers — 全部走 i2c_bus_worker
 *   - 重试逻辑保留：worker 内部对单 op 不 retry，driver 层 retry 仍有意义
 *   - bus_reset 改用 worker_bus_reset（worker 调度，不会砸正在传输的其他设备）
 *   - 错误恢复触发条件不变（streak ≥ 3）
 */
static bool write_register(axs5106l_touch_handle_t self, uint8_t reg, const uint8_t *data, size_t len)
{
    if (self->dev == NULL || len > 15) return false;
    uint8_t buf[16];
    buf[0] = reg;
    memcpy(&buf[1], data, len);
    for (int i = 0; i < I2C_RETRIES; i++) {
        if (i2c_worker_write(self->dev, buf, len + 1, I2C_TIMEOUT_MS) == ESP_OK) return true;
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    return false;
}

static bool read_register(axs5106l_touch_handle_t self, uint8_t reg, uint8_t *data, size_t len)
{
    if (self->dev == NULL) return false;
    for (int i = 0; i < I2C_RETRIES; i++) {
        if (i2c_worker_write(self->dev, &reg, 1,   I2C_TIMEOUT_MS) == ESP_OK &&
            i2c_worker_read (self->dev, data, len, I2C_TIMEOUT_MS) == ESP_OK) {
            self->i2c_err_streak = 0;
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    /* Consecutive-failure recovery: strong RF interference can lock SDA low.
     * worker_bus_reset 会在 worker 内部排空队列后做 9 SCL 脉冲，与共享总线上其他
     * driver（音频 codec / sensor / NFC）的 op 严格串行 → 不会砸正在传输的状态机。 */
    if (++self->i2c_err_streak >= 3) {
        ESP_LOGW(TAG, "I2C bus recovery (streak=%d) via worker", self->i2c_err_streak);
        i2c_worker_bus_reset(self->worker);
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
    if (axs5106l_upgrade_init(self->dev, self->rst_gpio, &upgrader) != ESP_OK) {
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

    /* RF storm gate: if INT-edge frequency in the last second exceeds the
     * threshold, drop reads entirely until the storm subsides. */
    if (storm_detected(self, esp_timer_get_time())) {
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
    if (buf[3] == 0xFF && buf[5] == 0xFF) return false;

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

    /* Gate 3: velocity filter — rejects 4G-RF-induced coordinate teleports.
     * Uses microsecond resolution so consecutive frames within the same ms
     * are still rate-checked (previous ms granularity had a 0-ms blind spot). */
    if (self->touch.last_time > 0) {
        uint64_t dt_us = now - self->touch.last_time;
        if (dt_us > 0 && dt_us < 500000) {
            uint32_t dist = (uint32_t)(abs((int)sx - (int)self->touch.last_x) +
                                       abs((int)sy - (int)self->touch.last_y));
            // px/s = dist * 1e6 / dt_us; compare without division: dist * 1e6 > MAX * dt_us
            if ((uint64_t)dist * 1000000ULL > (uint64_t)MAX_SPEED_PX_S * dt_us) return false;
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
        g->pressed         = true;
        g->long_fired      = false;
        g->sample_count    = 1;
        g->jitter_sum      = 0;
        g->jitter_unstable = false;
        g->start_x         = x;
        g->start_y         = y;
        g->last_x          = x;
        g->last_y          = y;
        g->press_time      = now;
        return;
    }

    if (pressed && g->pressed) {
        uint16_t frame_jitter = (uint16_t)(abs(x - g->last_x) + abs(y - g->last_y));
        g->last_x = x;
        g->last_y = y;
        if (g->sample_count < 0xFF) g->sample_count++;
        // 累计每帧位移（含大跳跃帧，因真 swipe 也会出现）。溢出时 saturate 保护。
        // JITTER_PER_FRAME_LIMIT 在此分支不使用——它原仅作"小晃动"的标识，
        // 但 jitter_sum 的语义本就是累加全部位移，分支判断属冗余。
        uint32_t new_sum = (uint32_t)g->jitter_sum + frame_jitter;
        g->jitter_sum = (new_sum < 0xFFFF) ? (uint16_t)new_sum : 0xFFFF;
        if (g->jitter_sum > JITTER_LIMIT_FOR_TAP) g->jitter_unstable = true;
        if (!g->long_fired) {
            int dx = x - g->start_x, dy = y - g->start_y;
            if (now - g->press_time >= LONG_PRESS_TIME_US &&
                abs(dx) + abs(dy) < CLICK_MAX_MOVE &&
                g->sample_count >= LONG_PRESS_MIN_FRAMES) {
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

        if (manhattan < CLICK_MAX_MOVE &&
            dur >= CLICK_MIN_TIME_US && dur < CLICK_MAX_TIME_US &&
            g->sample_count >= CLICK_MIN_FRAMES &&
            !g->jitter_unstable) {
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

        // 任何越过 SWIPE_THRESHOLD 的释放都不应再被视为 tap 链的一部分——
        // 即便后续判定为不合规 swipe（被 trajectory 门拒绝）也要清除 last_click_time，
        // 避免"tap → 拒绝 swipe → tap"被错配为 double-tap。
        if (manhattan >= SWIPE_THRESHOLD) {
            g->last_click_time = 0;
        }

        #if 0
        uint16_t traj_budget = (uint16_t)manhattan * SWIPE_TRAJECTORY_RATIO + SWIPE_TRAJECTORY_BIAS;
        if (manhattan >= SWIPE_THRESHOLD &&
            dur >= SWIPE_MIN_TIME_US &&
            g->sample_count >= SWIPE_MIN_FRAMES &&
            g->jitter_sum <= traj_budget) {
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
        #endif
    }
}
