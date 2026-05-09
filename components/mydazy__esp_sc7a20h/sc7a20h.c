/*
 * SPDX-FileCopyrightText: 2026 mydazy
 * SPDX-License-Identifier: Apache-2.0
 *
 * SC7A20H 极简驱动 — v4.0.0
 *
 * 设计：编译期硬编码项目参数，运行时无切换分支，全 I2C 走 worker。
 */

#include "sc7a20h.h"

#include <stdlib.h>
#include <esp_log.h>
#include <esp_sleep.h>
#include <esp_timer.h>
#include <driver/rtc_io.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

static const char *TAG = "sc7a20h";

/* ============== 项目硬编码参数（不开放给上层）============== */
#define ADDR_7BIT        0x19
#define I2C_SPEED_HZ     400000
#define WHO_AM_I_VAL     0x11
#define I2C_TIMEOUT_MS   100
#define MG_PER_LSB       2          /* ±4g HR 12-bit: 2 mg/LSB（精确整数） */
#define THS_STEP_MG      32         /* INT1_THS 步长 @ ±4g */
#define DUR_STEP_MS      10         /* INT1_DURATION 步长 @ ODR 100 Hz */

/* ============== Register map ============== */
#define REG_WHO_AM_I       0x0F
#define REG_CTRL_REG1      0x20    /* ODR + axis enable */
#define REG_CTRL_REG2      0x21    /* HPF for INT1 */
#define REG_CTRL_REG3      0x22    /* INT1 routing */
#define REG_CTRL_REG4      0x23    /* range + BDU + HR */
#define REG_CTRL_REG5      0x24    /* latch INT1 */
#define REG_CTRL_REG6      0x25    /* INT polarity */
#define REG_OUT_X_L        0x28
#define REG_INT1_CFG       0x30
#define REG_INT1_SRC       0x31
#define REG_INT1_THS       0x32
#define REG_INT1_DURATION  0x33

/* ============== 摇一摇检测参数（量产实测调优·硬编码） ============== */
#define SHAKE_PERIOD_MS    100
#define SHAKE_THRESH_MG_SQ (1500 * 1500)   /* 偏离 1g 阈值的平方 */
#define SHAKE_GRAVITY_SQ   (1000 * 1000)   /* (1g)² */
#define SHAKE_WINDOW       6               /* 600ms 滑动窗口 */
#define SHAKE_TARGET       3               /* 窗口内 ≥3 帧强动 = 摇 */
#define SHAKE_COOLDOWN_US  (1500 * 1000LL) /* 触发后冷却防连发 */

/* ============== Driver state ============== */
struct sc7a20h_dev_t {
    i2c_worker_handle_t   worker;
    i2c_worker_dev_t     *dev;
    TaskHandle_t          shake_task;
    sc7a20h_shake_cb_t    shake_cb;
    void                 *shake_ctx;
};

/* ============== Low-level helpers (worker-routed) ============== */
static esp_err_t write_reg(struct sc7a20h_dev_t *d, uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = {reg, val};
    return i2c_worker_write(d->dev, buf, 2, I2C_TIMEOUT_MS);
}

static esp_err_t read_burst(struct sc7a20h_dev_t *d, uint8_t reg,
                            uint8_t *buf, size_t len)
{
    uint8_t addr = reg | 0x80;  /* MSB=1: 自动递增寄存器地址 */
    return i2c_worker_write_read(d->dev, &addr, 1, buf, len, I2C_TIMEOUT_MS);
}

/* ============== Public API ============== */

sc7a20h_handle_t sc7a20h_init(i2c_worker_handle_t worker,
                              uint16_t pickup_thresh_mg,
                              uint16_t pickup_duration_ms)
{
    if (!worker) return NULL;

    struct sc7a20h_dev_t *d = calloc(1, sizeof(*d));
    if (!d) return NULL;

    d->worker = worker;
    d->dev    = i2c_worker_add_device(worker, ADDR_7BIT, I2C_SPEED_HZ);
    if (!d->dev) { free(d); return NULL; }

    /* WHO_AM_I 校验 — 单次读，不锁会话（worker 已串行单 op） */
    uint8_t reg_id = REG_WHO_AM_I, id = 0;
    if (i2c_worker_write_read(d->dev, &reg_id, 1, &id, 1, I2C_TIMEOUT_MS) != ESP_OK ||
        id != WHO_AM_I_VAL) {
        ESP_LOGE(TAG, "WHO_AM_I=0x%02X (expect 0x%02X)", id, WHO_AM_I_VAL);
        i2c_worker_remove_device(d->dev);
        free(d);
        return NULL;
    }

    /* mg → 寄存器值（编译期常量整除，无浮点） */
    uint8_t ths = (uint8_t)((pickup_thresh_mg + THS_STEP_MG / 2) / THS_STEP_MG);
    if (ths > 0x7F) ths = 0x7F;
    uint8_t dur = (uint8_t)(pickup_duration_ms / DUR_STEP_MS);
    if (dur > 0x7F) dur = 0x7F;

    /* 10 寄存器初始化序列必须原子（防音频/触摸 ISR 中途插入） */
    i2c_worker_lock_session(worker, 200);

    write_reg(d, REG_CTRL_REG1, 0x00);                  /* power down */
    vTaskDelay(pdMS_TO_TICKS(10));
    write_reg(d, REG_CTRL_REG4, 0x10 | 0x88);           /* ±4g + BDU + HR */
    write_reg(d, REG_CTRL_REG2, 0x01);                  /* HPF → INT1 */
    write_reg(d, REG_CTRL_REG5, 0x08);                  /* latch INT1 */
    write_reg(d, REG_CTRL_REG3, 0x40);                  /* AOI1 → INT1 */
    write_reg(d, REG_CTRL_REG6, 0x02);                  /* INT1 active LOW */
    write_reg(d, REG_INT1_THS, ths);
    write_reg(d, REG_INT1_DURATION, dur);
    write_reg(d, REG_INT1_CFG, 0x2A);                   /* XHIE | YHIE | ZHIE */
    write_reg(d, REG_CTRL_REG1, 0x50 | 0x07);           /* ODR 100Hz + XYZ */

    i2c_worker_unlock_session(worker);
    vTaskDelay(pdMS_TO_TICKS(10));

    ESP_LOGI(TAG, "ready (pickup=%umg/%ums, THS=0x%02X DUR=0x%02X)",
             pickup_thresh_mg, pickup_duration_ms, ths, dur);
    return d;
}

esp_err_t sc7a20h_read_mg(sc7a20h_handle_t h, int16_t *x, int16_t *y, int16_t *z)
{
    if (!h || !x || !y || !z) return ESP_ERR_INVALID_ARG;
    uint8_t buf[6];
    esp_err_t r = read_burst(h, REG_OUT_X_L, buf, 6);
    if (r != ESP_OK) return r;

    /* 12-bit left-aligned → 算术右移 4 → 乘 MG_PER_LSB（编译期常量） */
    int16_t raw_x = (int16_t)((buf[1] << 8) | buf[0]) >> 4;
    int16_t raw_y = (int16_t)((buf[3] << 8) | buf[2]) >> 4;
    int16_t raw_z = (int16_t)((buf[5] << 8) | buf[4]) >> 4;
    *x = (int16_t)(raw_x * MG_PER_LSB);
    *y = (int16_t)(raw_y * MG_PER_LSB);
    *z = (int16_t)(raw_z * MG_PER_LSB);
    return ESP_OK;
}

esp_err_t sc7a20h_arm_wakeup(sc7a20h_handle_t h, gpio_num_t int1_gpio)
{
    if (!h) return ESP_ERR_INVALID_ARG;

    /* Clear INT1_SRC latch — 防止 stale 事件让 deep_sleep_start 立即唤醒。
       I2C 失败不阻塞唤醒注册（总线可能已被调用方关电）。 */
    uint8_t reg = REG_INT1_SRC, src = 0;
    (void)i2c_worker_write_read(h->dev, &reg, 1, &src, 1, I2C_TIMEOUT_MS);

    esp_err_t ret = esp_sleep_enable_ext1_wakeup_io(
        (1ULL << int1_gpio), ESP_EXT1_WAKEUP_ANY_LOW);
    if (ret != ESP_OK) return ret;
    rtc_gpio_pullup_en(int1_gpio);
    rtc_gpio_pulldown_dis(int1_gpio);

    ESP_LOGI(TAG, "wakeup armed on GPIO%d", int1_gpio);
    return ESP_OK;
}

/* ============== 摇一摇后台检测任务 ============== */
static void shake_task_entry(void *arg)
{
    struct sc7a20h_dev_t *d = (struct sc7a20h_dev_t *)arg;
    int32_t window[SHAKE_WINDOW] = {0};
    int     wi = 0;
    int64_t last_shake_us = 0;

    TickType_t last_wake = xTaskGetTickCount();
    const TickType_t period = pdMS_TO_TICKS(SHAKE_PERIOD_MS);

    for (;;) {
        vTaskDelayUntil(&last_wake, period);

        int16_t x, y, z;
        if (sc7a20h_read_mg(d, &x, &y, &z) != ESP_OK) continue;

        /* 模长平方 - 重力平方（无 sqrt） */
        int32_t mag_sq = (int32_t)x * x + (int32_t)y * y + (int32_t)z * z;
        int32_t dev    = mag_sq - SHAKE_GRAVITY_SQ;
        if (dev < 0) dev = -dev;
        window[wi] = dev;
        wi = (wi + 1) % SHAKE_WINDOW;

        int strong = 0;
        for (int i = 0; i < SHAKE_WINDOW; ++i) {
            if (window[i] > SHAKE_THRESH_MG_SQ) strong++;
        }

        int64_t now_us = esp_timer_get_time();
        if (strong >= SHAKE_TARGET && (now_us - last_shake_us) > SHAKE_COOLDOWN_US) {
            last_shake_us = now_us;
            ESP_LOGI(TAG, "shake detected (strong=%d/%d)", strong, SHAKE_WINDOW);
            if (d->shake_cb) d->shake_cb(d->shake_ctx);
        }
    }
}

esp_err_t sc7a20h_start_shake_detect(sc7a20h_handle_t h,
                                     sc7a20h_shake_cb_t cb,
                                     void *user_ctx)
{
    if (!h || !cb) return ESP_ERR_INVALID_ARG;
    if (h->shake_task) return ESP_ERR_INVALID_STATE;

    h->shake_cb  = cb;
    h->shake_ctx = user_ctx;

    /* Core 1 与 LVGL 共核但低优先级；栈 2560 实测水位线 ~1KB */
    BaseType_t r = xTaskCreatePinnedToCore(
        shake_task_entry, "sc7a20_shake", 2560, h, 1, &h->shake_task, 1);
    if (r != pdPASS) {
        h->shake_task = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
