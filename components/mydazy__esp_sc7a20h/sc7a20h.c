/*
 * SPDX-FileCopyrightText: 2026 mydazy
 * SPDX-License-Identifier: Apache-2.0
 *
 * SC7A20H 驱动 v5.0.0 — 详见 docs/p30-sc7a20h-flows.html
 */

#include "sc7a20h.h"

#include <stdlib.h>
#include <string.h>
#include <esp_log.h>
#include <esp_sleep.h>
#include <esp_timer.h>
#include <driver/rtc_io.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

static const char *TAG = "sc7a20h";

/* ============== I²C / 量程常量（量产锁定 · 不开放运行时切换）============== */
#define ADDR_7BIT        0x19
#define I2C_SPEED_HZ     400000
#define WHO_AM_I_VAL     0x11
#define I2C_TIMEOUT_MS   50
#define MG_PER_LSB       2          /* ±4g HR 12-bit · 精确整数 */
#define THS_STEP_MG      32         /* INT1_THS 步长 @ ±4g */
#define DUR_STEP_MS      10         /* INT1_DURATION 步长 @ ODR 100 Hz */

/* ============== 寄存器地址 ============== */
#define REG_WHO_AM_I       0x0F
#define REG_CTRL_REG1      0x20
#define REG_CTRL_REG2      0x21
#define REG_CTRL_REG3      0x22
#define REG_CTRL_REG4      0x23
#define REG_CTRL_REG5      0x24
#define REG_CTRL_REG6      0x25
#define REG_OUT_X_L        0x28
#define REG_INT1_CFG       0x30
#define REG_INT1_SRC       0x31
#define REG_INT1_THS       0x32
#define REG_INT1_DURATION  0x33

/* ============== motion_task 参数 ============== */
#define MOTION_PERIOD_MS   100
#define MOTION_STACK       2560
#define MOTION_PRIO        1
#define MOTION_CORE        1
#define SHAKE_WINDOW_MAX   16        /* 滑窗上限 · 实际由用户传 window_ms/100 决定 */

/* ============== Driver state ============== */
typedef struct {
    bool                    enabled;
    int32_t                 thresh_sq;   /* deviation² (mg²) */
    int                     window;      /* 帧数 = window_ms / 100 */
    int                     target;
    int64_t                 cooldown_us;
    int32_t                 ring[SHAKE_WINDOW_MAX];
    int                     idx;
    int64_t                 last_us;
    sc7a20h_shake_cb_t      cb;
    void                   *ctx;
} shake_state_t;

typedef enum { STRIKE_IDLE, STRIKE_WAIT_GAP } strike_phase_t;

typedef struct {
    bool                    enabled;
    int32_t                 peak_sq;     /* peak² (mg²) */
    int64_t                 min_gap_us;
    int64_t                 max_gap_us;
    int64_t                 cooldown_us;
    strike_phase_t          phase;
    int64_t                 peak1_us;
    int64_t                 last_us;
    sc7a20h_strike_cb_t     cb;
    void                   *ctx;
} strike_state_t;

struct sc7a20h_dev_t {
    i2c_worker_handle_t   worker;
    i2c_worker_dev_t     *dev;
    TaskHandle_t          motion_task;
    shake_state_t         shake;
    strike_state_t        strike;
};

/* ============== 内部 I²C 助手（全部走 worker · 防 4G RF 共线污染）============== */
static esp_err_t write_reg(struct sc7a20h_dev_t *d, uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = {reg, val};
    return i2c_worker_write(d->dev, buf, 2, I2C_TIMEOUT_MS);
}

static esp_err_t read_burst(struct sc7a20h_dev_t *d, uint8_t reg,
                            uint8_t *buf, size_t len)
{
    uint8_t addr = reg | 0x80;  /* MSB=1 触发 sensor 自动递增寄存器 */
    return i2c_worker_write_read(d->dev, &addr, 1, buf, len, I2C_TIMEOUT_MS);
}

static esp_err_t read_xyz_mg(struct sc7a20h_dev_t *d, int16_t *x, int16_t *y, int16_t *z)
{
    uint8_t buf[6];
    esp_err_t r = read_burst(d, REG_OUT_X_L, buf, 6);
    if (r != ESP_OK) return r;
    /* 12-bit 左对齐 · 算术右移 4 还原 signed 12-bit · 乘 MG_PER_LSB 得 mg */
    int16_t rx = (int16_t)((buf[1] << 8) | buf[0]) >> 4;
    int16_t ry = (int16_t)((buf[3] << 8) | buf[2]) >> 4;
    int16_t rz = (int16_t)((buf[5] << 8) | buf[4]) >> 4;
    *x = (int16_t)(rx * MG_PER_LSB);
    *y = (int16_t)(ry * MG_PER_LSB);
    *z = (int16_t)(rz * MG_PER_LSB);
    return ESP_OK;
}

/* ============== motion_task — 共享数据流 · 双算法独立判定 ============== */
static void update_shake(shake_state_t *s, int32_t dev, int64_t now_us)
{
    if (!s->enabled || !s->cb) return;

    s->ring[s->idx] = dev;
    s->idx = (s->idx + 1) % s->window;

    int strong = 0;
    for (int i = 0; i < s->window; ++i) {
        if (s->ring[i] > s->thresh_sq) strong++;
    }
    if (strong >= s->target && (now_us - s->last_us) > s->cooldown_us) {
        s->last_us = now_us;
        ESP_LOGI(TAG, "摇一摇触发 (强动帧=%d/%d)", strong, s->window);
        s->cb(s->ctx);
    }
}

static void update_strike(strike_state_t *t, int32_t dev, int64_t now_us)
{
    if (!t->enabled || !t->cb) return;
    if ((now_us - t->last_us) < t->cooldown_us) return;

    bool is_peak = (dev > t->peak_sq);

    switch (t->phase) {
        case STRIKE_IDLE:
            if (is_peak) {
                t->peak1_us = now_us;
                t->phase = STRIKE_WAIT_GAP;
            }
            break;
        case STRIKE_WAIT_GAP: {
            int64_t gap = now_us - t->peak1_us;
            if (gap > t->max_gap_us) {
                /* 超时 · 若当前帧仍是峰值则视为新第一击，否则回 idle */
                t->phase = is_peak ? STRIKE_WAIT_GAP : STRIKE_IDLE;
                if (is_peak) t->peak1_us = now_us;
            } else if (is_peak && gap >= t->min_gap_us) {
                t->last_us = now_us;
                t->phase = STRIKE_IDLE;
                ESP_LOGI(TAG, "桌面双击触发 (间隔=%lldms)", gap / 1000);
                t->cb(t->ctx);
            }
            break;
        }
    }
}

static void motion_task_entry(void *arg)
{
    struct sc7a20h_dev_t *d = (struct sc7a20h_dev_t *)arg;
    TickType_t last_wake = xTaskGetTickCount();
    const TickType_t period = pdMS_TO_TICKS(MOTION_PERIOD_MS);
    const int32_t gravity_sq = 1000 * 1000;

    for (;;) {
        vTaskDelayUntil(&last_wake, period);

        int16_t x, y, z;
        if (read_xyz_mg(d, &x, &y, &z) != ESP_OK) continue;

        /* 模长平方 - 重力平方 · 整数运算避免 sqrt */
        int32_t mag_sq = (int32_t)x * x + (int32_t)y * y + (int32_t)z * z;
        int32_t dev    = mag_sq - gravity_sq;
        if (dev < 0) dev = -dev;

        int64_t now_us = esp_timer_get_time();
        update_shake (&d->shake,  dev, now_us);
        update_strike(&d->strike, dev, now_us);
    }
}

static esp_err_t ensure_motion_task(struct sc7a20h_dev_t *d)
{
    if (d->motion_task) return ESP_OK;
    BaseType_t r = xTaskCreatePinnedToCore(
        motion_task_entry, "sc7a20_motion", MOTION_STACK, d,
        MOTION_PRIO, &d->motion_task, MOTION_CORE);
    if (r != pdPASS) {
        d->motion_task = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
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

    uint8_t reg_id = REG_WHO_AM_I, id = 0;
    if (i2c_worker_write_read(d->dev, &reg_id, 1, &id, 1, I2C_TIMEOUT_MS) != ESP_OK ||
        id != WHO_AM_I_VAL) {
        ESP_LOGE(TAG, "WHO_AM_I=0x%02X (expect 0x%02X)", id, WHO_AM_I_VAL);
        i2c_worker_remove_device(d->dev);
        free(d);
        return NULL;
    }

    /* 拿起灵敏度 mg/ms → 寄存器 LSB（编译期常量整除 · 无浮点） */
    uint8_t ths = (uint8_t)((pickup_thresh_mg + THS_STEP_MG / 2) / THS_STEP_MG);
    if (ths > 0x7F) ths = 0x7F;
    uint8_t dur = (uint8_t)(pickup_duration_ms / DUR_STEP_MS);
    if (dur > 0x7F) dur = 0x7F;

    /* 10 寄存器原子序列 — 防音频/触摸 ISR 中途插入 */
    i2c_worker_lock_session(worker, 200);

    write_reg(d, REG_CTRL_REG1, 0x00);            /* power-down · 配置前必先 down */
    vTaskDelay(pdMS_TO_TICKS(10));
    write_reg(d, REG_CTRL_REG4, 0x10 | 0x88);     /* ±4g + BDU + HR */
    write_reg(d, REG_CTRL_REG2, 0x01);            /* HPF → INT1 */
    /* LIR_INT1=0 不锁存 — v4.0.1 量产修复秒醒：codec 失电短路 SDA/SCL 致
       INT1_SRC 清不掉 · 改为动态电平后即使瞬态运动也不会钉死 INT1 */
    write_reg(d, REG_CTRL_REG5, 0x00);
    write_reg(d, REG_CTRL_REG3, 0x40);            /* AOI1 → INT1 */
    write_reg(d, REG_CTRL_REG6, 0x02);            /* INT1 active LOW */
    write_reg(d, REG_INT1_THS, ths);
    write_reg(d, REG_INT1_DURATION, dur);
    write_reg(d, REG_INT1_CFG, 0x2A);             /* XHIE | YHIE | ZHIE */
    write_reg(d, REG_CTRL_REG1, 0x50 | 0x07);     /* ODR 100Hz + XYZ enable */

    i2c_worker_unlock_session(worker);
    vTaskDelay(pdMS_TO_TICKS(10));

    ESP_LOGI(TAG, "init ok (pickup=%umg/%ums · THS=0x%02X DUR=0x%02X)",
             pickup_thresh_mg, pickup_duration_ms, ths, dur);
    return d;
}

esp_err_t sc7a20h_wakeup(sc7a20h_handle_t h, gpio_num_t int1_gpio)
{
    if (!h) return ESP_ERR_INVALID_ARG;

    /* 兜底清 latch — LIR_INT1=0 已是主防线，此处失败仅警告不阻塞唤醒 */
    uint8_t reg = REG_INT1_SRC, src = 0;
    esp_err_t clr = i2c_worker_write_read(h->dev, &reg, 1, &src, 1, I2C_TIMEOUT_MS);
    if (clr != ESP_OK) {
        ESP_LOGW(TAG, "INT1_SRC clear failed (%s) · I2C 可能已 reset",
                 esp_err_to_name(clr));
    } else if (src & 0x40) {
        ESP_LOGI(TAG, "INT1_SRC stale latch cleared (src=0x%02X)", src);
    }

    esp_err_t ret = esp_sleep_enable_ext1_wakeup_io(
        (1ULL << int1_gpio), ESP_EXT1_WAKEUP_ANY_LOW);
    if (ret != ESP_OK) return ret;
    rtc_gpio_pullup_en(int1_gpio);
    rtc_gpio_pulldown_dis(int1_gpio);

    ESP_LOGI(TAG, "wakeup armed on GPIO%d", int1_gpio);
    return ESP_OK;
}

esp_err_t sc7a20h_shake(sc7a20h_handle_t h,
                        uint16_t deviation_mg,
                        uint16_t window_ms,
                        uint8_t  target_frames,
                        uint16_t cooldown_ms,
                        sc7a20h_shake_cb_t cb,
                        void *ctx)
{
    if (!h || !cb) return ESP_ERR_INVALID_ARG;
    if (h->shake.enabled) return ESP_ERR_INVALID_STATE;

    int win = window_ms / MOTION_PERIOD_MS;
    if (win < 1)               win = 1;
    if (win > SHAKE_WINDOW_MAX) win = SHAKE_WINDOW_MAX;

    int64_t thresh_sq64 = (int64_t)deviation_mg * (int64_t)deviation_mg;
    h->shake.thresh_sq   = (thresh_sq64 > INT32_MAX) ? INT32_MAX : (int32_t)thresh_sq64;
    h->shake.window      = win;
    h->shake.target      = (target_frames < 1) ? 1 : (target_frames > win ? win : target_frames);
    h->shake.cooldown_us = (int64_t)cooldown_ms * 1000;
    h->shake.idx         = 0;
    h->shake.last_us     = 0;
    memset(h->shake.ring, 0, sizeof(h->shake.ring));
    h->shake.cb          = cb;
    h->shake.ctx         = ctx;
    h->shake.enabled     = true;

    esp_err_t r = ensure_motion_task(h);
    if (r != ESP_OK) { h->shake.enabled = false; return r; }

    ESP_LOGI(TAG, "摇一摇启用 (阈值=%umg 窗=%dms 目标帧=%d/%d 冷却=%ums)",
             deviation_mg, win * MOTION_PERIOD_MS, h->shake.target, win, cooldown_ms);
    return ESP_OK;
}

esp_err_t sc7a20h_strike(sc7a20h_handle_t h,
                         uint16_t peak_mg,
                         uint16_t min_gap_ms,
                         uint16_t max_gap_ms,
                         uint16_t cooldown_ms,
                         sc7a20h_strike_cb_t cb,
                         void *ctx)
{
    if (!h || !cb) return ESP_ERR_INVALID_ARG;
    if (h->strike.enabled) return ESP_ERR_INVALID_STATE;

    h->strike.peak_sq     = (int32_t)peak_mg * peak_mg;
    h->strike.min_gap_us  = (int64_t)min_gap_ms * 1000;
    h->strike.max_gap_us  = (int64_t)max_gap_ms * 1000;
    h->strike.cooldown_us = (int64_t)cooldown_ms * 1000;
    h->strike.phase       = STRIKE_IDLE;
    h->strike.peak1_us    = 0;
    h->strike.last_us     = 0;
    h->strike.cb          = cb;
    h->strike.ctx         = ctx;
    h->strike.enabled     = true;

    esp_err_t r = ensure_motion_task(h);
    if (r != ESP_OK) { h->strike.enabled = false; return r; }

    ESP_LOGI(TAG, "桌面双击启用 (峰值=%umg 间隔=[%u,%u]ms 冷却=%ums)",
             peak_mg, min_gap_ms, max_gap_ms, cooldown_ms);
    return ESP_OK;
}
