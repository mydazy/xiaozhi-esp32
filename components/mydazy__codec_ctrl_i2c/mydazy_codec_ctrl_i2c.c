/*
 * SPDX-FileCopyrightText: 2026 MyDazy
 * SPDX-License-Identifier: Apache-2.0
 *
 * MyDazy 版 audio_codec_ctrl_i2c — 把 esp_codec_dev 的所有 I2C 访问
 * 转发到 i2c_bus_worker 单线程串行化。
 *
 * 派生自 espressif/esp_codec_dev/platform/audio_codec_ctrl_i2c.c，
 * 保持 audio_codec_ctrl_if_t 接口 1:1 兼容。
 */

#include "mydazy_codec_ctrl_i2c.h"

#include <stdlib.h>
#include <string.h>
#include "esp_log.h"
#include "esp_codec_dev_defaults.h"     /* ESP_CODEC_DEV_OK / ESP_CODEC_DEV_* */

#define TAG "MyDazyCodecI2C"
#define DEFAULT_I2C_TIMEOUT_MS  100

typedef struct {
    audio_codec_ctrl_if_t   base;
    bool                    is_open;
    i2c_worker_dev_t       *dev;        /* 通过 worker 注册的设备 */
} mydazy_codec_i2c_ctrl_t;

static int _open(const audio_codec_ctrl_if_t *ctrl, void *cfg, int cfg_size)
{
    if (ctrl == NULL || cfg == NULL || cfg_size != sizeof(mydazy_codec_i2c_cfg_t)) {
        return ESP_CODEC_DEV_INVALID_ARG;
    }
    mydazy_codec_i2c_ctrl_t *self = (mydazy_codec_i2c_ctrl_t *)ctrl;
    mydazy_codec_i2c_cfg_t  *c    = (mydazy_codec_i2c_cfg_t *)cfg;

    if (c->worker == NULL) {
        ESP_LOGE(TAG, "worker is NULL");
        return ESP_CODEC_DEV_INVALID_ARG;
    }

    /* esp_codec_dev 习惯传 8-bit addr（高 7 bit + R/W bit=0），转 7-bit */
    uint16_t addr_7bit = (c->addr >> 1) & 0x7F;
    uint32_t scl_speed = c->scl_speed_hz ? c->scl_speed_hz : 100000;

    /* 幂等：已注册过则直接视为已打开，避免上游二次 open 重复注册设备泄漏 */
    if (self->dev != NULL) {
        self->is_open = true;
        return ESP_CODEC_DEV_OK;
    }

    self->dev = i2c_worker_add_device(c->worker, addr_7bit, scl_speed);
    if (self->dev == NULL) {
        ESP_LOGE(TAG, "i2c_worker_add_device failed (addr=0x%02X)", addr_7bit);
        return ESP_CODEC_DEV_DRV_ERR;
    }
    self->is_open = true;   /* open 回调自身负责置位，符合 audio_codec_ctrl_if_t 约定 */
    return ESP_CODEC_DEV_OK;
}

static bool _is_open(const audio_codec_ctrl_if_t *ctrl)
{
    if (ctrl == NULL) return false;
    return ((mydazy_codec_i2c_ctrl_t *)ctrl)->is_open;
}

static int _read_reg(const audio_codec_ctrl_if_t *ctrl, int addr, int addr_len, void *data, int data_len)
{
    if (ctrl == NULL || data == NULL) return ESP_CODEC_DEV_INVALID_ARG;
    mydazy_codec_i2c_ctrl_t *self = (mydazy_codec_i2c_ctrl_t *)ctrl;
    if (!self->is_open) return ESP_CODEC_DEV_WRONG_STATE;

    /* 与上游 _i2c_master_read_reg 完全一致：寄存器地址 1-2 字节大端 */
    uint8_t addr_data[2] = {0};
    if (addr_len > 1) {
        addr_data[0] = (uint8_t)(addr >> 8);
        addr_data[1] = (uint8_t)(addr & 0xFF);
    } else {
        addr_data[0] = (uint8_t)(addr & 0xFF);
    }

    esp_err_t ret = i2c_worker_write_read(
        self->dev, addr_data, addr_len, data, data_len, DEFAULT_I2C_TIMEOUT_MS);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Fail to read (ret=%d)", (int)ret);
        return ESP_CODEC_DEV_READ_FAIL;
    }
    return ESP_CODEC_DEV_OK;
}

static int _write_reg(const audio_codec_ctrl_if_t *ctrl, int addr, int addr_len, void *data, int data_len)
{
    if (ctrl == NULL || data == NULL) return ESP_CODEC_DEV_INVALID_ARG;
    mydazy_codec_i2c_ctrl_t *self = (mydazy_codec_i2c_ctrl_t *)ctrl;
    if (!self->is_open) return ESP_CODEC_DEV_WRONG_STATE;

    /* 与上游 _i2c_master_write_reg 一致：拼成单条 transaction
       上游限制 len<=4（只支持小数据写） — 我们保持同样限制以兼容 */
    int len = addr_len + data_len;
    if (len > 4) {
        ESP_LOGE(TAG, "write payload > 4 bytes not supported");
        return ESP_CODEC_DEV_NOT_SUPPORT;
    }

    uint8_t buf[4] = {0};
    int i = 0;
    if (addr_len > 1) {
        buf[i++] = (uint8_t)(addr >> 8);
        buf[i++] = (uint8_t)(addr & 0xFF);
    } else {
        buf[i++] = (uint8_t)(addr & 0xFF);
    }
    const uint8_t *src = (const uint8_t *)data;
    while (i < len) buf[i++] = *src++;

    esp_err_t ret = i2c_worker_write(self->dev, buf, len, DEFAULT_I2C_TIMEOUT_MS);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Fail to write (ret=%d)", (int)ret);
        return ESP_CODEC_DEV_WRITE_FAIL;
    }
    return ESP_CODEC_DEV_OK;
}

static int _close(const audio_codec_ctrl_if_t *ctrl)
{
    if (ctrl == NULL) return ESP_CODEC_DEV_INVALID_ARG;
    mydazy_codec_i2c_ctrl_t *self = (mydazy_codec_i2c_ctrl_t *)ctrl;
    if (self->dev) {
        i2c_worker_remove_device(self->dev);
        self->dev = NULL;
    }
    self->is_open = false;
    return ESP_CODEC_DEV_OK;
}

const audio_codec_ctrl_if_t *mydazy_codec_new_i2c_ctrl(const mydazy_codec_i2c_cfg_t *cfg)
{
    if (cfg == NULL) {
        ESP_LOGE(TAG, "cfg is NULL");
        return NULL;
    }

    mydazy_codec_i2c_ctrl_t *self = (mydazy_codec_i2c_ctrl_t *)calloc(1, sizeof(*self));
    if (!self) {
        ESP_LOGE(TAG, "no memory");
        return NULL;
    }

    self->base.open     = _open;
    self->base.is_open  = _is_open;
    self->base.read_reg = _read_reg;
    self->base.write_reg = _write_reg;
    self->base.close    = _close;

    int ret = _open(&self->base, (void *)cfg, sizeof(mydazy_codec_i2c_cfg_t));
    if (ret != ESP_CODEC_DEV_OK) {
        free(self);
        return NULL;
    }
    self->is_open = true;
    return &self->base;
}
