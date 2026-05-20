/*
 * SPDX-FileCopyrightText: 2026 mydazy
 * SPDX-License-Identifier: Apache-2.0
 *
 * AXS5106L 触摸驱动 v5.0.0 — 详见 docs/p30-touch-flows.html
 * JD9853 284×240 · I²C 0x63 · V2907 firmware（坐标直接输出 · 无 X 死区补偿）
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

/* X 死区补偿已移除（v5.0）· 触摸坐标直接来自 V2907 firmware 输出 */

#define SWIPE_THRESHOLD      20       /* ~2.6mm · 与 Apple 10pt 对齐 */
#define CLICK_MAX_TIME_US    500000   /* 500ms · Apple 标准（800→500）*/
#define CLICK_MIN_TIME_US     50000   /* 50ms · Apple 标准（60→50）*/
#define CLICK_MAX_MOVE       30       /* ~4mm · 容忍儿童手指落屏微移（20→30）*/
#define LONG_PRESS_TIME_US   500000   /* 500ms · 防儿童误触（300→500 与 Apple/iOS 对齐）*/
#define DOUBLE_CLICK_TIME_US 500000   /* 500ms · 实测儿童双击间隔 320~480ms · 300→500 解决"快速双击全被切单击" */
#define DOUBLE_CLICK_DIST    80       /* ~10.6mm · Apple 30pt 物理等效 */
#define CLICK_MIN_FRAMES      2       /* 事件型芯片 INT 只在边沿 latch · 单帧即合法（50ms 时长 + 抖动=0 兜底）*/
#define SINGLE_FRAME_MAX_TIME_US 200000  /* f==1 时 dur 上限 200ms · 拦截 4G 伪 press（被 RF 拉长的单帧噪声）· f>=2 不受限 */
#define RF_ACTIVE_EDGE_HINT      3       /*  本次 press 内 INT 边沿 ≥3 视为 RF 活跃 · 单帧 tap 升级要求 2 帧 */
#define SWIPE_MIN_TIME_US    150000   /* 150ms 不变 */
#define SWIPE_MIN_FRAMES      4       /* 事件型芯片快滑只采 4-5 帧 · 配 manhattan 20px + 150ms 时长足以区分 */
#define LONG_PRESS_MIN_FRAMES 30      /* 480ms @ 16ms · 配 LONG_PRESS_TIME 500ms */
#define JITTER_LIMIT_FOR_TAP 40       /* ~5mm · 容忍儿童手抖 / RF 抖动（20→40）*/
#define RELEASE_DEBOUNCE  2             /* 连续 N 帧无触摸才报松开 ）*/
#define INT_STORM_WINDOW_US      1000000   /* rolling 1s 边沿计数窗（两档共用）*/
#define INT_STORM_HOT_WINDOW_US 30000000   /* 30s post-storm 灵敏模式（两档共用）*/

/* RF_NORMAL: WiFi/无 4G 干扰 · 较宽容 */
#define RF_N_INT_DEBOUNCE_US           5000
#define RF_N_POST_RELEASE_GUARD_US   100000
#define RF_N_MAX_SPEED_PX_S            3000
#define RF_N_STORM_THRESHOLD             20
#define RF_N_STORM_THRESHOLD_HOT         10
#define RF_N_STORM_MUTE_US          1000000
#define RF_N_SWIPE_TRAJ_RATIO             3

#define RF_S_INT_DEBOUNCE_US           6000      /* 量产 v2.2.16: 4→6ms · 进一步过滤 4G TDD 突发伪边沿 */
#define RF_S_POST_RELEASE_GUARD_US   300000      /* 量产 v2.2.16: 200→300ms · 抬手后保护窗加宽 · 抑制 release 后伪 press */
#define RF_S_MAX_SPEED_PX_S            2000      /* 量产 v2.2.16: 2500→2000 · 滑动速度上限收紧 · 真实滑动 200-300px/s 留 6x 余量 */
#define RF_S_STORM_THRESHOLD             25      /* 12→25 · 容忍本底噪声 */
#define RF_S_STORM_THRESHOLD_HOT         14      /* 量产 v2.2.16: 18→14 · HOT 期更敏感（30s 复发窗内更早屏蔽）*/
#define RF_S_STORM_MUTE_US          2000000
#define RF_S_SWIPE_TRAJ_RATIO             2

#define SWIPE_TRAJECTORY_BIAS  20

/* ---------- I2C ---------- */
#define I2C_TIMEOUT_MS  50
#define I2C_RETRIES     3

/* ------------------------------------------------------------------ */
/*  Internal state                                                     */
/* ------------------------------------------------------------------ */

typedef struct {
    bool     pressed;
    uint8_t  release_count;
    int16_t  last_x;
    int16_t  last_y;
    uint64_t last_time;
    uint64_t int_low_since;

    bool     press_pending;
    uint64_t last_release_time;  /* 0 = never released yet */
} touch_state_t;

typedef struct {
    bool     pressed;
    bool     long_fired;
    uint8_t  sample_count;

    uint16_t jitter_sum;
    bool     jitter_unstable;
    int16_t  start_x, start_y;
    int16_t  last_x,  last_y;
    uint64_t press_time;
    uint64_t last_click_time;
    int16_t  last_click_x, last_click_y;
    uint32_t press_edges_baseline;  /* 4G 干扰可观测 · press 起点的 INT 边沿快照 */
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
    bool                    sleeping;

    /* Callbacks ( 5 个独立 cb) */
    void                       *cb_ctx;
    axs5106l_wake_cb_t          on_wake;
    axs5106l_click_cb_t         on_click;
    axs5106l_click_cb_t         on_double_click;
    axs5106l_swipe_cb_t         on_swipe;
    axs5106l_long_press_cb_t    on_long_press;

    uint32_t  rf_int_debounce_us;
    uint32_t  rf_post_release_guard_us;
    uint32_t  rf_max_speed_px_s;
    uint32_t  rf_storm_threshold;
    uint32_t  rf_storm_threshold_hot;
    uint64_t  rf_storm_mute_us;
    uint8_t   rf_swipe_traj_ratio;
    uint8_t   i2c_err_streak;

    volatile uint32_t       int_edge_count;
    uint32_t                int_edge_window_baseline;
    uint64_t                int_edge_window_start_us;
    uint64_t                storm_mute_until_us;
    uint64_t                storm_hot_until_us;
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

static void apply_rf_mode(axs5106l_touch_handle_t self, axs5106l_rf_mode_t mode)
{
    switch (mode) {
    case AXS5106L_RF_STRICT:
        self->rf_int_debounce_us       = RF_S_INT_DEBOUNCE_US;
        self->rf_post_release_guard_us = RF_S_POST_RELEASE_GUARD_US;
        self->rf_max_speed_px_s        = RF_S_MAX_SPEED_PX_S;
        self->rf_storm_threshold       = RF_S_STORM_THRESHOLD;
        self->rf_storm_threshold_hot   = RF_S_STORM_THRESHOLD_HOT;
        self->rf_storm_mute_us         = RF_S_STORM_MUTE_US;
        self->rf_swipe_traj_ratio      = RF_S_SWIPE_TRAJ_RATIO;
        return;
    case AXS5106L_RF_NORMAL:
        break;
    default:
        ESP_LOGW(TAG, "unknown rf_mode=%d, falling back to NORMAL", (int)mode);
        break;
    }
    self->rf_int_debounce_us       = RF_N_INT_DEBOUNCE_US;
    self->rf_post_release_guard_us = RF_N_POST_RELEASE_GUARD_US;
    self->rf_max_speed_px_s        = RF_N_MAX_SPEED_PX_S;
    self->rf_storm_threshold       = RF_N_STORM_THRESHOLD;
    self->rf_storm_threshold_hot   = RF_N_STORM_THRESHOLD_HOT;
    self->rf_storm_mute_us         = RF_N_STORM_MUTE_US;
    self->rf_swipe_traj_ratio      = RF_N_SWIPE_TRAJ_RATIO;
}

esp_err_t axs5106l_touch_init(const axs5106l_touch_config_t *cfg,
                              axs5106l_touch_handle_t *out)
{
    if (cfg == NULL || out == NULL || cfg->worker == NULL) return ESP_ERR_INVALID_ARG;
    *out = NULL;

    axs5106l_touch_handle_t self = (axs5106l_touch_handle_t)calloc(1, sizeof(struct axs5106l_touch_t));
    if (self == NULL) return ESP_ERR_NO_MEM;

    self->worker   = cfg->worker;
    self->rst_gpio = cfg->rst_gpio;
    self->int_gpio = cfg->int_gpio;
    self->width    = cfg->width;
    self->height   = cfg->height;
    /* 项目硬编码：V2907 firmware 已内部 rotation · 不开放 swap/mirror */
    self->swap_xy  = false;
    self->mirror_x = false;
    self->mirror_y = false;

    /* 5 个独立 cb 字段（一次性传入） */
    self->cb_ctx          = cfg->cb_ctx;
    self->on_wake         = cfg->on_wake;
    self->on_click        = cfg->on_click;
    self->on_double_click = cfg->on_double_click;
    self->on_swipe        = cfg->on_swipe;
    self->on_long_press   = cfg->on_long_press;

    /* 应用 RF 抗扰档（4G 板传 STRICT · 其他 NORMAL · 0=NORMAL） */
    apply_rf_mode(self, cfg->rf_mode);
#if AXS5106L_TOUCH_DEBUG_OVERLAY
    self->raw_stats.raw_min_x = 0xFFFF;
    self->raw_stats.raw_min_y = 0xFFFF;
#endif

    ESP_LOGI(TAG, "init RST=GPIO%d INT=GPIO%d %dx%d · rf_mode=%s",
             self->rst_gpio, self->int_gpio, self->width, self->height,
             (cfg->rf_mode == AXS5106L_RF_STRICT) ? "STRICT(4G)" : "NORMAL");

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

    gpio_config_t int_cfg = {
        .pin_bit_mask = (1ULL << self->int_gpio),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_NEGEDGE,
    };
    esp_err_t ret = gpio_config(&int_cfg);
    if (ret != ESP_OK) return ret;

    esp_err_t add_ret = gpio_isr_handler_add(self->int_gpio, int_falling_edge_isr, self);
    if (add_ret == ESP_ERR_INVALID_STATE) {
        gpio_install_isr_service(0);
        add_ret = gpio_isr_handler_add(self->int_gpio, int_falling_edge_isr, self);
    }
    if (add_ret == ESP_OK) {
        self->isr_installed = true;
        self->int_edge_window_start_us = esp_timer_get_time();
        ESP_LOGI(TAG, "INT edge ISR installed for RF storm detection");
    } else {
        ESP_LOGW(TAG, "ISR install failed: %s, storm detection disabled",
                 esp_err_to_name(add_ret));
    }

    self->lvgl_indev = lv_indev_create();
    if (self->lvgl_indev == NULL) {
        ESP_LOGE(TAG, "LVGL indev create failed");
        if (self->isr_installed) {
            gpio_isr_handler_remove(self->int_gpio);
            self->isr_installed = false;
        }
        return ESP_ERR_NO_MEM;
    }
    lv_indev_set_type(self->lvgl_indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(self->lvgl_indev, lvgl_read_cb);
    lv_indev_set_user_data(self->lvgl_indev, self);

    ESP_LOGI(TAG, "registered with LVGL");
    return ESP_OK;
}

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
    self->int_edge_window_start_us = esp_timer_get_time();
    self->int_edge_window_baseline = self->int_edge_count;
    self->storm_mute_until_us      = 0;
    self->storm_hot_until_us       = 0;
    if (self->isr_installed) gpio_intr_enable(self->int_gpio);
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/*  INT-edge storm detector                                            */
/* ------------------------------------------------------------------ */

static void IRAM_ATTR int_falling_edge_isr(void *arg)
{
    axs5106l_touch_handle_t self = (axs5106l_touch_handle_t)arg;
    self->int_edge_count++;
}

static bool storm_detected(axs5106l_touch_handle_t self, uint64_t now)
{
    /* 触摸活跃期跳过 storm 检测 · 否则滑动 / 双击间隔期 INT 抖动会被误判为 RF storm */
    bool recent_release = self->touch.last_release_time != 0 &&
                          (now - self->touch.last_release_time) < 1500000ULL;
    if (self->touch.pressed || self->gesture.long_fired || recent_release) {
        if (now - self->int_edge_window_start_us >= INT_STORM_WINDOW_US) {
            self->int_edge_window_start_us = now;
            self->int_edge_window_baseline = self->int_edge_count;
        }
        self->storm_mute_until_us = 0;
        return false;
    }

    if (now < self->storm_mute_until_us) {
        self->int_edge_window_baseline = self->int_edge_count;
        self->int_edge_window_start_us = now;
        return true;
    }
    if (!self->isr_installed) return false;

    if (now - self->int_edge_window_start_us >= INT_STORM_WINDOW_US) {
        uint32_t edges = self->int_edge_count - self->int_edge_window_baseline;
        uint32_t threshold = (now < self->storm_hot_until_us)
                                ? self->rf_storm_threshold_hot
                                : self->rf_storm_threshold;
        if (edges >= threshold) {
            ESP_LOGW(TAG, "RF storm: %lu INT edges in last %lu ms (thr=%lu%s), mute %lu ms",
                     (unsigned long)edges,
                     (unsigned long)((now - self->int_edge_window_start_us) / 1000),
                     (unsigned long)threshold,
                     (threshold == self->rf_storm_threshold_hot) ? " hot" : "",
                     (unsigned long)(self->rf_storm_mute_us / 1000));
            self->storm_mute_until_us = now + self->rf_storm_mute_us;
            self->storm_hot_until_us  = now + INT_STORM_HOT_WINDOW_US;
            /* Drop any latched press so LVGL sees a clean release. */
            self->touch.pressed       = false;
            self->touch.press_pending = false;
            self->touch.int_low_since = 0;
            self->touch.last_time     = 0;
            self->int_edge_window_start_us = now;
            self->int_edge_window_baseline = self->int_edge_count;
            /* storm 打断进行中的按压：清 gesture 状态机 + 清 last_click 防虚假双击
             * (P0-3.1: 否则下次 click 与 (0,0) 比对距离 < 50 被误判为双击) */
            self->gesture.pressed = false;
            self->gesture.long_fired = false;
            self->gesture.last_click_time = 0;
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

/* v3.0+ I2C helpers — 全部走 i2c_bus_worker */
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
        if (i2c_worker_write_read(self->dev, &reg, 1, data, len, I2C_TIMEOUT_MS) == ESP_OK) {
            self->i2c_err_streak = 0;
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(5));
    }

    if (++self->i2c_err_streak >= 3) {
        ESP_LOGW(TAG, "I2C bus recovery (streak=%d) via worker", self->i2c_err_streak);
        i2c_worker_bus_reset(self->worker);
        self->i2c_err_streak = 0;
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
        if (!self->touch.pressed) {
            bool guard_active = self->touch.last_release_time != 0 &&
                                (now - self->touch.last_release_time) < self->rf_post_release_guard_us;
            if (guard_active && !self->touch.press_pending) {
                self->touch.press_pending = true;
                /* Hold LVGL idle for this frame; pending will resolve next frame. */
                data->state = LV_INDEV_STATE_RELEASED;
                return;
            }
            self->touch.press_pending = false;
            if (self->on_wake != NULL) self->on_wake(self->cb_ctx);
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

    if (!self->touch.pressed) {
        if (gpio_get_level(self->int_gpio) != 0) {
            self->touch.int_low_since = 0;
            self->touch.last_time     = 0;
            return false;
        }
        if (self->touch.int_low_since == 0) self->touch.int_low_since = now;
        if (now - self->touch.int_low_since < self->rf_int_debounce_us) return false;
        esp_rom_delay_us(1);
        if (gpio_get_level(self->int_gpio) != 0) { self->touch.int_low_since = 0; return false; }
        esp_rom_delay_us(1);
        if (gpio_get_level(self->int_gpio) != 0) { self->touch.int_low_since = 0; return false; }
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

    /* X 不做死区补偿 · 直接用 raw 坐标（V2907 firmware 内部已 rotation） */
    uint16_t sx = raw_x, sy = raw_y;
    if (self->swap_xy) {
        uint16_t tmp = sx; sx = sy; sy = tmp;
    }
    if (sx >= self->width)  sx = self->width  - 1;
    if (sy >= self->height) sy = self->height - 1;
    if (self->mirror_x) sx = self->width  - 1 - sx;
    if (self->mirror_y) sy = self->height - 1 - sy;

    if (self->touch.last_time > 0) {
        uint64_t dt_us = now - self->touch.last_time;
        if (dt_us > 0 && dt_us < 500000) {
            uint32_t dist = (uint32_t)(abs((int)sx - (int)self->touch.last_x) +
                                       abs((int)sy - (int)self->touch.last_y));
            // px/s = dist * 1e6 / dt_us; compare without division: dist * 1e6 > MAX * dt_us
            if ((uint64_t)dist * 1000000ULL > (uint64_t)self->rf_max_speed_px_s * dt_us) return false;
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

static inline void fire_click(axs5106l_touch_handle_t self, int16_t x, int16_t y) {
    if (self->on_click) self->on_click(x, y, self->cb_ctx);
}
static inline void fire_double_click(axs5106l_touch_handle_t self, int16_t x, int16_t y) {
    if (self->on_double_click) self->on_double_click(x, y, self->cb_ctx);
}
static inline void fire_long_press(axs5106l_touch_handle_t self, bool is_release, int16_t x, int16_t y) {
    if (self->on_long_press) self->on_long_press(is_release, x, y, self->cb_ctx);
}
static inline void fire_swipe(axs5106l_touch_handle_t self, int16_t dx, int16_t dy) {
    if (self->on_swipe) self->on_swipe(dx, dy, self->cb_ctx);
}

static void recognize_gesture(axs5106l_touch_handle_t self, int16_t x, int16_t y, bool pressed)
{
    uint64_t now = esp_timer_get_time();
    gesture_state_t *g = &self->gesture;

    if (pressed && !g->pressed) {
        g->pressed              = true;
        g->long_fired           = false;
        g->sample_count         = 1;
        g->jitter_sum           = 0;
        g->jitter_unstable      = false;
        g->start_x              = x;
        g->start_y              = y;
        g->last_x               = x;
        g->last_y               = y;
        g->press_time           = now;
        g->press_edges_baseline = self->int_edge_count;
        return;
    }

    if (pressed && g->pressed) {
        uint16_t frame_jitter = (uint16_t)(abs(x - g->last_x) + abs(y - g->last_y));
        g->last_x = x;
        g->last_y = y;
        if (g->sample_count < 0xFF) g->sample_count++;

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
                uint32_t edges_lp = self->int_edge_count - g->press_edges_baseline;
                ESP_LOGI(TAG, "长按 (%d,%d) f=%u j=%u e=%u i2c=%u",
                         g->start_x, g->start_y,
                         (unsigned)g->sample_count, (unsigned)g->jitter_sum,
                         (unsigned)edges_lp, (unsigned)self->i2c_err_streak);
                fire_long_press(self, false, g->start_x, g->start_y);
            }
        }
        return;
    }

    if (!pressed && g->pressed) {
        g->pressed = false;

        if (g->long_fired) {
            g->long_fired = false;
            ESP_LOGI(TAG, "长按松开 (%d,%d)", g->start_x, g->start_y);
            fire_long_press(self, true, g->start_x, g->start_y);
            return;
        }

        int dx = g->last_x - g->start_x;
        int dy = g->last_y - g->start_y;
        int manhattan = abs(dx) + abs(dy);
        uint64_t dur   = now - g->press_time;
        /* 4G 干扰可观测参数 · 每个 release 分支共享 */
        uint32_t edges = self->int_edge_count - g->press_edges_baseline;
        const char *rf_st = (now < self->storm_mute_until_us) ? "mute"
                          : (now < self->storm_hot_until_us)  ? "hot"
                                                              : "ok";
        const char *rf_md = (self->rf_storm_threshold == RF_S_STORM_THRESHOLD) ? "S" : "N";

        /* 4G 伪 press 拦截：RF 注入的单帧坐标会被反复 INT 跳变拉长 dur 到 200-400ms，
         * 但真单帧点击 dur 几乎都 < 150ms（真按 200ms+ 会出多帧或进长按）。
         * 故 f==1 时收紧 dur 上限到 SINGLE_FRAME_MAX_TIME_US，f>=2 保持原 500ms。*/
        uint64_t tap_max = (g->sample_count >= 2) ? CLICK_MAX_TIME_US
                                                  : SINGLE_FRAME_MAX_TIME_US;
        /* RF 活跃（本 press 边沿密集 或 处于 storm 复发窗）→ 要求 ≥2 帧，
         * 滤掉 RF 单帧注入的伪 press；RF 安静时保持单帧灵敏（真实极速单击不受影响）。
         * 注：e≤2 的低边沿伪触摸与真实单击在固件层不可区分，此处无法拦截。*/
        bool rf_active = (edges >= RF_ACTIVE_EDGE_HINT) ||
                         (now < self->storm_hot_until_us);
        uint8_t need_frames = rf_active ? 2 : CLICK_MIN_FRAMES;
        bool tap_ok = (manhattan < CLICK_MAX_MOVE &&
                       dur >= CLICK_MIN_TIME_US && dur < tap_max &&
                       g->sample_count >= need_frames &&
                       !g->jitter_unstable);
        if (!tap_ok) {
            ESP_LOGD(TAG, "tap_ok=0 位移=%d dur=%ums f=%u j=%u unst=%d e=%u rf=%s/%s i2c=%u",
                     manhattan, (unsigned)(dur/1000),
                     (unsigned)g->sample_count, (unsigned)g->jitter_sum,
                     (int)g->jitter_unstable, (unsigned)edges,
                     rf_md, rf_st, (unsigned)self->i2c_err_streak);
        }
        if (tap_ok) {
            int click_dist = abs(g->start_x - g->last_click_x) +
                             abs(g->start_y - g->last_click_y);
            bool is_double = g->last_click_time > 0 &&
                             (g->press_time - g->last_click_time) < DOUBLE_CLICK_TIME_US &&
                             click_dist < DOUBLE_CLICK_DIST;
            if (is_double) {
                g->last_click_time = 0;
                ESP_LOGI(TAG, "双击 (%d,%d) dur=%ums f=%u j=%u e=%u rf=%s/%s i2c=%u",
                         g->start_x, g->start_y, (unsigned)(dur/1000),
                         (unsigned)g->sample_count, (unsigned)g->jitter_sum,
                         (unsigned)edges, rf_md, rf_st, (unsigned)self->i2c_err_streak);
                fire_double_click(self, g->start_x, g->start_y);
            } else {
                g->last_click_time = now;
                g->last_click_x    = g->start_x;
                g->last_click_y    = g->start_y;
                ESP_LOGI(TAG, "单击 (%d,%d) dur=%ums f=%u j=%u e=%u rf=%s/%s i2c=%u",
                         g->start_x, g->start_y, (unsigned)(dur/1000),
                         (unsigned)g->sample_count, (unsigned)g->jitter_sum,
                         (unsigned)edges, rf_md, rf_st, (unsigned)self->i2c_err_streak);
                fire_click(self, g->start_x, g->start_y);
            }
            return;
        }

        if (manhattan >= SWIPE_THRESHOLD) {
            g->last_click_time = 0;
        }

        uint16_t traj_budget = (uint16_t)manhattan * self->rf_swipe_traj_ratio + SWIPE_TRAJECTORY_BIAS;
        if (manhattan >= SWIPE_THRESHOLD &&
            dur >= SWIPE_MIN_TIME_US &&
            g->sample_count >= SWIPE_MIN_FRAMES &&
            g->jitter_sum <= traj_budget) {
            const char *dir = (abs(dx) > abs(dy)) ? (dx > 0 ? "右滑" : "左滑")
                                                  : (dy > 0 ? "下滑" : "上滑");
            ESP_LOGI(TAG, "%s dx=%d dy=%d dur=%ums f=%u j=%u e=%u rf=%s/%s i2c=%u",
                     dir, dx, dy, (unsigned)(dur/1000),
                     (unsigned)g->sample_count, (unsigned)g->jitter_sum,
                     (unsigned)edges, rf_md, rf_st, (unsigned)self->i2c_err_streak);
            fire_swipe(self, (int16_t)dx, (int16_t)dy);
        }
    }
}
