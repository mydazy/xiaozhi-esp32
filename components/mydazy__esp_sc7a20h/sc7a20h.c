/*
 * SPDX-FileCopyrightText: 2026 mydazy
 * SPDX-License-Identifier: Apache-2.0
 *
 * SC7A20H 3-axis accelerometer driver (Silan Microelectronics).
 */

#include "sc7a20h.h"

#include <stdlib.h>
#include <string.h>
#include <esp_log.h>
#include <esp_check.h>
#include <esp_sleep.h>
#include <esp_timer.h>
#include <driver/rtc_io.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

static const char *TAG = "sc7a20h";

/* ========================================================================
 * Register map (LIS2DH12-compatible)
 * ====================================================================== */
#define REG_WHO_AM_I       0x0F
#define REG_CTRL_REG1      0x20  /* ODR + axis enable + low-power */
#define REG_CTRL_REG2      0x21  /* High-pass filter */
#define REG_CTRL_REG3      0x22  /* INT1 routing */
#define REG_CTRL_REG4      0x23  /* Full-scale + resolution */
#define REG_CTRL_REG5      0x24  /* FIFO / latch control */
#define REG_CTRL_REG6      0x25  /* INT2 / interrupt polarity */
#define REG_OUT_X_L        0x28
#define REG_INT1_CFG       0x30
#define REG_INT1_SRC       0x31  /* Read clears latched INT1 (LIR_INT1) */
#define REG_INT1_THS       0x32
#define REG_INT1_DURATION  0x33

#define DEVICE_ID          0x11  /* WHO_AM_I expected value */
#define AXES_ENABLE        0x07  /* CTRL_REG1: enable X+Y+Z */
#define INT1_AOI1          0x40  /* CTRL_REG3: route AOI1 to INT1 pin */
#define I2C_TIMEOUT_MS     100

/* ========================================================================
 * Driver state
 * ====================================================================== */
struct sc7a20h_dev_t {
    i2c_master_bus_handle_t bus;
    i2c_master_dev_handle_t dev;
    sc7a20h_range_t         range;
    sc7a20h_odr_t           odr;
    sc7a20h_wakeup_cb_t     wakeup_cb;
    void                   *wakeup_ctx;
    bool                    initialised;
};

/* ========================================================================
 * Low-level I2C helpers
 * ====================================================================== */
static esp_err_t write_reg(struct sc7a20h_dev_t *d, uint8_t reg, uint8_t value)
{
    uint8_t buf[2] = {reg, value};
    return i2c_master_transmit(d->dev, buf, 2, I2C_TIMEOUT_MS);
}

static esp_err_t read_reg(struct sc7a20h_dev_t *d, uint8_t reg, uint8_t *value)
{
    return i2c_master_transmit_receive(d->dev, &reg, 1, value, 1, I2C_TIMEOUT_MS);
}

static esp_err_t read_regs(struct sc7a20h_dev_t *d, uint8_t reg,
                           uint8_t *buf, size_t len)
{
    /* Burst read: set MSB to enable register-address auto-increment. */
    uint8_t reg_addr = reg | 0x80;
    return i2c_master_transmit_receive(d->dev, &reg_addr, 1, buf, len, I2C_TIMEOUT_MS);
}

static float sensitivity_mg_per_lsb(sc7a20h_range_t range)
{
    /* High-resolution mode (12-bit). */
    switch (range) {
        case SC7A20H_RANGE_2G:  return 1.0f;
        case SC7A20H_RANGE_4G:  return 2.0f;
        case SC7A20H_RANGE_8G:  return 4.0f;
        case SC7A20H_RANGE_16G: return 12.0f;
        default:                return 2.0f;
    }
}

/* ========================================================================
 * Lifecycle
 * ====================================================================== */
esp_err_t sc7a20h_create(i2c_master_bus_handle_t i2c_bus,
                         const sc7a20h_config_t *cfg,
                         sc7a20h_handle_t *out)
{
    ESP_RETURN_ON_FALSE(i2c_bus && cfg && out, ESP_ERR_INVALID_ARG, TAG, "bad arg");

    struct sc7a20h_dev_t *d = calloc(1, sizeof(*d));
    ESP_RETURN_ON_FALSE(d, ESP_ERR_NO_MEM, TAG, "alloc");

    d->bus   = i2c_bus;
    d->range = cfg->range;
    d->odr   = cfg->odr;

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = cfg->i2c_addr,
        .scl_speed_hz    = 400000,
    };
    esp_err_t ret = i2c_master_bus_add_device(i2c_bus, &dev_cfg, &d->dev);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "i2c_master_bus_add_device failed: %d", ret);
        free(d);
        return ret;
    }

    /* Verify WHO_AM_I. */
    uint8_t id = 0;
    ret = read_reg(d, REG_WHO_AM_I, &id);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "WHO_AM_I read failed: %d", ret);
        goto fail;
    }
    if (id != DEVICE_ID) {
        ESP_LOGE(TAG, "WHO_AM_I mismatch: got 0x%02X, expected 0x%02X", id, DEVICE_ID);
        ret = ESP_ERR_NOT_FOUND;
        goto fail;
    }
    ESP_LOGI(TAG, "Detected SC7A20H (WHO_AM_I = 0x%02X)", id);

    /* Disable output before reconfiguring. */
    ESP_GOTO_ON_ERROR(write_reg(d, REG_CTRL_REG1, 0x00), fail, TAG, "ctrl1");
    vTaskDelay(pdMS_TO_TICKS(10));

    /* CTRL_REG4: full-scale + Block-Data-Update + high resolution. */
    ESP_GOTO_ON_ERROR(write_reg(d, REG_CTRL_REG4,
                                (uint8_t)cfg->range | 0x80), fail, TAG, "ctrl4");
    /* CTRL_REG2: enable high-pass filter for INT1. */
    ESP_GOTO_ON_ERROR(write_reg(d, REG_CTRL_REG2, 0x01), fail, TAG, "ctrl2");
    /* CTRL_REG5: latch INT1 request. */
    ESP_GOTO_ON_ERROR(write_reg(d, REG_CTRL_REG5, 0x08), fail, TAG, "ctrl5");
    /* CTRL_REG3: route AOI1 to INT1 pin. */
    ESP_GOTO_ON_ERROR(write_reg(d, REG_CTRL_REG3, INT1_AOI1), fail, TAG, "ctrl3");
    /* CTRL_REG6: INT1 polarity = active LOW (bit1 H_LACTIVE=1).
     * Idle level on the INT pin is HIGH; pulled LOW when an event fires. */
    ESP_GOTO_ON_ERROR(write_reg(d, REG_CTRL_REG6, 0x02), fail, TAG, "ctrl6");
    /* CTRL_REG1: enable XYZ + apply ODR. */
    ESP_GOTO_ON_ERROR(write_reg(d, REG_CTRL_REG1,
                                (uint8_t)cfg->odr | AXES_ENABLE), fail, TAG, "ctrl1b");
    vTaskDelay(pdMS_TO_TICKS(10));

    d->initialised = true;
    *out = d;
    ESP_LOGI(TAG, "Initialised (range=0x%02X, ODR=0x%02X)",
             (uint8_t)cfg->range, (uint8_t)cfg->odr);
    return ESP_OK;

fail:
    if (d->dev) {
        i2c_master_bus_rm_device(d->dev);
    }
    free(d);
    return ret;
}

esp_err_t sc7a20h_del(sc7a20h_handle_t h)
{
    if (!h) return ESP_OK;
    if (h->initialised) {
        write_reg(h, REG_CTRL_REG1, 0x00);  /* Power down, ignore error. */
    }
    if (h->dev) {
        i2c_master_bus_rm_device(h->dev);
    }
    free(h);
    return ESP_OK;
}

/* ========================================================================
 * Data readout
 * ====================================================================== */
esp_err_t sc7a20h_get_raw_acce(sc7a20h_handle_t h, sc7a20h_raw_acce_t *raw)
{
    ESP_RETURN_ON_FALSE(h && raw, ESP_ERR_INVALID_ARG, TAG, "bad arg");

    uint8_t buf[6] = {0};
    esp_err_t ret = read_regs(h, REG_OUT_X_L, buf, sizeof(buf));
    if (ret != ESP_OK) return ret;

    /* 12-bit left-aligned -> shift right by 4 to recover signed value. */
    raw->x = (int16_t)((buf[1] << 8) | buf[0]) >> 4;
    raw->y = (int16_t)((buf[3] << 8) | buf[2]) >> 4;
    raw->z = (int16_t)((buf[5] << 8) | buf[4]) >> 4;
    return ESP_OK;
}

esp_err_t sc7a20h_get_acce(sc7a20h_handle_t h, sc7a20h_acce_t *acce)
{
    ESP_RETURN_ON_FALSE(h && acce, ESP_ERR_INVALID_ARG, TAG, "bad arg");

    sc7a20h_raw_acce_t raw = {0};
    esp_err_t ret = sc7a20h_get_raw_acce(h, &raw);
    if (ret != ESP_OK) return ret;

    float s = sensitivity_mg_per_lsb(h->range);
    acce->x = raw.x * s;
    acce->y = raw.y * s;
    acce->z = raw.z * s;
    return ESP_OK;
}

/* ========================================================================
 * Motion detection
 * ====================================================================== */
esp_err_t sc7a20h_set_motion_detection(sc7a20h_handle_t h,
                                       bool enable,
                                       const sc7a20h_motion_config_t *mcfg)
{
    ESP_RETURN_ON_FALSE(h, ESP_ERR_INVALID_ARG, TAG, "bad arg");

    if (!enable) {
        esp_err_t ret = write_reg(h, REG_INT1_CFG, 0x00);
        if (ret == ESP_OK) ESP_LOGI(TAG, "Motion detection disabled");
        return ret;
    }

    sc7a20h_motion_config_t defaults = SC7A20H_DEFAULT_MOTION_CONFIG();
    const sc7a20h_motion_config_t *cfg = mcfg ? mcfg : &defaults;

    ESP_RETURN_ON_ERROR(write_reg(h, REG_INT1_THS, cfg->threshold), TAG, "ths");
    ESP_RETURN_ON_ERROR(write_reg(h, REG_INT1_DURATION, cfg->duration), TAG, "dur");

    /* INT1_CFG: high-event OR over enabled axes. */
    uint8_t int_cfg = 0;
    if (cfg->enable_x) int_cfg |= 0x02;  /* XHIE */
    if (cfg->enable_y) int_cfg |= 0x08;  /* YHIE */
    if (cfg->enable_z) int_cfg |= 0x20;  /* ZHIE */
    ESP_RETURN_ON_ERROR(write_reg(h, REG_INT1_CFG, int_cfg), TAG, "cfg");

    ESP_LOGI(TAG, "Motion detection enabled (threshold=0x%02X, duration=0x%02X)",
             cfg->threshold, cfg->duration);
    return ESP_OK;
}

esp_err_t sc7a20h_set_wakeup_callback(sc7a20h_handle_t h,
                                      sc7a20h_wakeup_cb_t cb,
                                      void *user_ctx)
{
    ESP_RETURN_ON_FALSE(h, ESP_ERR_INVALID_ARG, TAG, "bad arg");
    h->wakeup_cb  = cb;
    h->wakeup_ctx = user_ctx;
    return ESP_OK;
}

/* ========================================================================
 * Power management / runtime tuning
 * ====================================================================== */
esp_err_t sc7a20h_enter_power_down(sc7a20h_handle_t h)
{
    ESP_RETURN_ON_FALSE(h, ESP_ERR_INVALID_ARG, TAG, "bad arg");
    esp_err_t ret = write_reg(h, REG_CTRL_REG1, 0x00);
    if (ret == ESP_OK) {
        vTaskDelay(pdMS_TO_TICKS(10));
        ESP_LOGI(TAG, "Entered power-down mode");
    }
    return ret;
}

esp_err_t sc7a20h_exit_power_down(sc7a20h_handle_t h)
{
    ESP_RETURN_ON_FALSE(h, ESP_ERR_INVALID_ARG, TAG, "bad arg");
    esp_err_t ret = write_reg(h, REG_CTRL_REG1, (uint8_t)h->odr | AXES_ENABLE);
    if (ret == ESP_OK) {
        vTaskDelay(pdMS_TO_TICKS(10));
        ESP_LOGI(TAG, "Exited power-down mode");
    }
    return ret;
}

esp_err_t sc7a20h_set_range(sc7a20h_handle_t h, sc7a20h_range_t range)
{
    ESP_RETURN_ON_FALSE(h, ESP_ERR_INVALID_ARG, TAG, "bad arg");
    esp_err_t ret = write_reg(h, REG_CTRL_REG4, (uint8_t)range | 0x80);
    if (ret == ESP_OK) h->range = range;
    return ret;
}

esp_err_t sc7a20h_set_odr(sc7a20h_handle_t h, sc7a20h_odr_t odr)
{
    ESP_RETURN_ON_FALSE(h, ESP_ERR_INVALID_ARG, TAG, "bad arg");
    esp_err_t ret = write_reg(h, REG_CTRL_REG1, (uint8_t)odr | AXES_ENABLE);
    if (ret == ESP_OK) h->odr = odr;
    return ret;
}

/* ========================================================================
 * Latch clearing
 * ====================================================================== */
esp_err_t sc7a20h_clear_motion_latch(sc7a20h_handle_t h, uint8_t *src)
{
    ESP_RETURN_ON_FALSE(h, ESP_ERR_INVALID_ARG, TAG, "bad arg");
    uint8_t val = 0;
    esp_err_t ret = read_reg(h, REG_INT1_SRC, &val);
    if (ret == ESP_OK && src) *src = val;
    return ret;
}

/* ========================================================================
 * Deep-sleep wakeup
 * ====================================================================== */
esp_err_t sc7a20h_config_deep_sleep_wakeup(sc7a20h_handle_t h, gpio_num_t int1_gpio)
{
    ESP_RETURN_ON_FALSE(h, ESP_ERR_INVALID_ARG, TAG, "bad arg");

    /* Read INT1_SRC first to clear any latched event. Without this, a stale
     * latch keeps INT1 asserted (LOW) and ESP_EXT1_WAKEUP_ANY_LOW fires
     * immediately after esp_deep_sleep_start(). Errors are non-fatal:
     * the I2C bus may already be torn down by the caller. */
    uint8_t int1_src = 0;
    esp_err_t r = read_reg(h, REG_INT1_SRC, &int1_src);
    if (r == ESP_OK) {
        ESP_LOGI(TAG, "INT1_SRC cleared (was 0x%02X)", int1_src);
    } else {
        ESP_LOGW(TAG, "INT1_SRC read failed (%s); latch may persist",
                 esp_err_to_name(r));
    }

    esp_err_t ret = esp_sleep_enable_ext1_wakeup_io(
        (1ULL << int1_gpio), ESP_EXT1_WAKEUP_ANY_LOW);
    ESP_RETURN_ON_ERROR(ret, TAG, "ext1 wakeup");
    ESP_RETURN_ON_ERROR(rtc_gpio_pullup_en(int1_gpio), TAG, "rtc pullup");
    ESP_RETURN_ON_ERROR(rtc_gpio_pulldown_dis(int1_gpio), TAG, "rtc pulldown");

    ESP_LOGI(TAG, "Deep-sleep wakeup configured on GPIO%d", int1_gpio);
    return ESP_OK;
}

/* ========================================================================
 * Convenience: built-in 500 ms debounced wakeup logger
 * ====================================================================== */
static void default_motion_log_cb(void *user_ctx)
{
    static int64_t last_us = 0;
    int64_t now = esp_timer_get_time();
    if (now - last_us > 500000) {  /* 500 ms debounce */
        last_us = now;
        ESP_LOGI(TAG, "Motion event");
    }
    (void)user_ctx;
}

esp_err_t sc7a20h_create_with_motion_detection(i2c_master_bus_handle_t i2c_bus,
                                               const sc7a20h_config_t *cfg,
                                               const sc7a20h_motion_config_t *mcfg,
                                               sc7a20h_handle_t *out)
{
    sc7a20h_config_t default_cfg = SC7A20H_DEFAULT_CONFIG();
    if (!cfg) cfg = &default_cfg;

    esp_err_t ret = sc7a20h_create(i2c_bus, cfg, out);
    if (ret != ESP_OK) return ret;

    ret = sc7a20h_set_motion_detection(*out, true, mcfg);
    if (ret != ESP_OK) {
        sc7a20h_del(*out);
        *out = NULL;
        return ret;
    }

    sc7a20h_set_wakeup_callback(*out, default_motion_log_cb, NULL);
    ESP_LOGI(TAG, "Motion-detect ready (with default debounced log callback)");
    return ESP_OK;
}
