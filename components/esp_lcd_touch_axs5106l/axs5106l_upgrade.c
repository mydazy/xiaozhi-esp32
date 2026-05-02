/*
 * SPDX-FileCopyrightText: 2026 mydazy
 * SPDX-License-Identifier: Apache-2.0
 *
 * AXS5106L touch-controller firmware upgrade module — C implementation.
 */

#include "axs5106l_upgrade.h"

#include <stdlib.h>
#include <string.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

static const char *TAG = "axs5106l_upgrade";

/* Embedded firmware image (raw byte stream included from a generated header). */
static const uint8_t kFirmwareData[] = {
#include "axs5106l_firmware.h"
};

/* Offset within the firmware image where the version word lives. */
#define FIRMWARE_VERSION_OFFSET  0x400

/* I2C transaction parameters. */
#define I2C_TIMEOUT_MS           100
#define I2C_MAX_RETRIES          3

/* Upgrade-flow parameters. */
#define UPGRADE_RETRY_TIMES      1
#define DEBUG_MODE_RETRY_TIMES   3
#define WRITE_TIMEOUT_MS         10

struct axs5106l_upgrade_t {
    i2c_master_dev_handle_t i2c_handle;
    gpio_num_t              rst_gpio;
};

/* ------------------------------------------------------------------ */
/*  Lifecycle                                                          */
/* ------------------------------------------------------------------ */

esp_err_t axs5106l_upgrade_init(i2c_master_dev_handle_t i2c_handle,
                                gpio_num_t rst_gpio,
                                axs5106l_upgrade_handle_t *out)
{
    if (i2c_handle == NULL || out == NULL) return ESP_ERR_INVALID_ARG;

    axs5106l_upgrade_handle_t h = (axs5106l_upgrade_handle_t)calloc(1, sizeof(struct axs5106l_upgrade_t));
    if (h == NULL) return ESP_ERR_NO_MEM;

    h->i2c_handle = i2c_handle;
    h->rst_gpio   = rst_gpio;
    *out = h;
    return ESP_OK;
}

void axs5106l_upgrade_del(axs5106l_upgrade_handle_t h)
{
    if (h != NULL) free(h);
}

/* ------------------------------------------------------------------ */
/*  Delay helpers                                                      */
/* ------------------------------------------------------------------ */

static inline void delay_ms(uint16_t ms)
{
    vTaskDelay(pdMS_TO_TICKS(ms));
}

static inline void delay_us(uint16_t us)
{
    /* Busy wait: required when us < 1 RTOS tick. */
    uint64_t start = esp_timer_get_time();
    while (esp_timer_get_time() - start < us) { }
}

/* ------------------------------------------------------------------ */
/*  Low-level I2C primitives                                           */
/* ------------------------------------------------------------------ */

static bool i2c_write_reg(axs5106l_upgrade_handle_t h, uint8_t reg, const uint8_t *data, size_t len)
{
    if (h->i2c_handle == NULL || len > 64) return false;

    uint8_t buf[65];
    buf[0] = reg;
    memcpy(&buf[1], data, len);

    for (int retry = 0; retry < I2C_MAX_RETRIES; retry++) {
        if (i2c_master_transmit(h->i2c_handle, buf, len + 1, I2C_TIMEOUT_MS) == ESP_OK) {
            return true;
        }
        delay_ms(5);
    }
    return false;
}

static bool i2c_read_reg(axs5106l_upgrade_handle_t h, uint8_t reg, uint8_t *data, size_t len)
{
    if (h->i2c_handle == NULL) return false;

    for (int retry = 0; retry < I2C_MAX_RETRIES; retry++) {
        if (i2c_master_transmit(h->i2c_handle, &reg, 1, I2C_TIMEOUT_MS) == ESP_OK &&
            i2c_master_receive(h->i2c_handle, data, len, I2C_TIMEOUT_MS) == ESP_OK) {
            return true;
        }
        delay_ms(5);
    }
    return false;
}

static bool i2c_read_regs(axs5106l_upgrade_handle_t h, const uint8_t *reg, size_t reg_len,
                          uint8_t *data, size_t data_len)
{
    if (h->i2c_handle == NULL) return false;

    for (int retry = 0; retry < I2C_MAX_RETRIES; retry++) {
        if (i2c_master_transmit(h->i2c_handle, reg, reg_len, I2C_TIMEOUT_MS) == ESP_OK &&
            i2c_master_receive(h->i2c_handle, data, data_len, I2C_TIMEOUT_MS) == ESP_OK) {
            return true;
        }
        delay_ms(5);
    }
    return false;
}

/* ------------------------------------------------------------------ */
/*  Reset                                                              */
/* ------------------------------------------------------------------ */

static void hardware_reset(axs5106l_upgrade_handle_t h)
{
    gpio_set_level(h->rst_gpio, 1);
    delay_us(50);
    gpio_set_level(h->rst_gpio, 0);
    delay_us(50);
    delay_ms(20);
    gpio_set_level(h->rst_gpio, 1);
}

static void software_reset(axs5106l_upgrade_handle_t h)
{
    uint8_t rst_cmd[5] = {0xB3, 0x55, 0xAA, 0x34, 0x01};
    i2c_write_reg(h, 0xF0, rst_cmd, 5);
    hardware_reset(h);
}

/* ------------------------------------------------------------------ */
/*  Upgrade flow stages                                                */
/* ------------------------------------------------------------------ */

static bool enter_debug_mode(axs5106l_upgrade_handle_t h)
{
    uint8_t debug_cmd[1]  = {0x55};
    uint8_t write_buf[3]  = {0x80, 0x7f, 0xd1};
    uint8_t read_buf[1]   = {0x00};

    for (int retry = 0; retry < DEBUG_MODE_RETRY_TIMES; retry++) {
        software_reset(h);

        /* Wait window: 500 us < delay < 4 ms. */
        delay_us(800);

        i2c_write_reg(h, 0xAA, debug_cmd, 1);

        /* delay >= 50 us before the readback. */
        delay_us(100);

        if (i2c_read_regs(h, write_buf, 3, read_buf, 1) && read_buf[0] == 0x28) {
            ESP_LOGI(TAG, "entered debug mode");
            return true;
        }
    }

    ESP_LOGE(TAG, "failed to enter debug mode");
    return false;
}

static bool unlock_flash(axs5106l_upgrade_handle_t h)
{
    uint8_t unlock_cmd[3] = {0x6F, 0xFF, 0xFF};
    i2c_write_reg(h, 0x90, unlock_cmd, 3);

    unlock_cmd[1] = 0xDA;
    unlock_cmd[2] = 0x18;
    i2c_write_reg(h, 0x90, unlock_cmd, 3);

    return true;
}

static bool erase_flash(axs5106l_upgrade_handle_t h)
{
    uint8_t clear_flag[3] = {0x6F, 0xD9, 0x0C};
    uint8_t erase_cmd[3]  = {0x6F, 0xD6, 0x77};
    uint8_t write_buf[3]  = {0x80, 0x7F, 0xD9};
    uint8_t read_buf[1]   = {0x00};

    i2c_write_reg(h, 0x90, clear_flag, 3);
    i2c_write_reg(h, 0x90, erase_cmd, 3);

    /* Poll for erase completion (timeout: 300 ms). */
    for (int i = 0; i < 30; i++) {
        delay_ms(WRITE_TIMEOUT_MS);

        if (i2c_read_regs(h, write_buf, 3, read_buf, 1)) {
            if (read_buf[0] & 0x04) {  /* bit 2 == 1 means done */
                erase_cmd[2] = 0x00;
                i2c_write_reg(h, 0x90, erase_cmd, 3);
                ESP_LOGI(TAG, "flash erase complete");
                return true;
            }
        }
    }

    erase_cmd[2] = 0x00;
    i2c_write_reg(h, 0x90, erase_cmd, 3);
    ESP_LOGE(TAG, "flash erase timeout");
    return false;
}

static bool write_flash(axs5106l_upgrade_handle_t h, const uint8_t *data, size_t len)
{
    uint8_t cmd[3] = {0x6F, 0xD4, 0x00};

    /* Configure write parameters. */
    i2c_write_reg(h, 0x90, cmd, 3);

    cmd[1] = 0xD5;
    i2c_write_reg(h, 0x90, cmd, 3);

    cmd[1] = 0xD2;
    cmd[2] = (len - 1) & 0xFF;
    i2c_write_reg(h, 0x90, cmd, 3);

    cmd[1] = 0xD3;
    cmd[2] = ((len - 1) >> 8) & 0xFF;
    i2c_write_reg(h, 0x90, cmd, 3);

    cmd[1] = 0xD6;
    cmd[2] = 0xF4;
    i2c_write_reg(h, 0x90, cmd, 3);

    /* Byte-by-byte write (slow mode, broadest compatibility). */
    cmd[1] = 0xD7;
    for (size_t i = 0; i < len; i++) {
        cmd[2] = data[i];
        i2c_write_reg(h, 0x90, cmd, 3);

        if ((i + 1) % 1024 == 0) {
            ESP_LOGI(TAG, "write progress: %u / %u", (unsigned)(i + 1), (unsigned)len);
        }
    }

    cmd[1] = 0xD6;
    cmd[2] = 0x00;
    i2c_write_reg(h, 0x90, cmd, 3);

    ESP_LOGI(TAG, "firmware write complete (%u bytes)", (unsigned)len);
    return true;
}

static bool do_upgrade(axs5106l_upgrade_handle_t h)
{
    ESP_LOGI(TAG, "starting upgrade (firmware size: %u bytes)", (unsigned)sizeof(kFirmwareData));

    if (!enter_debug_mode(h))                               return false;
    if (!unlock_flash(h))      { ESP_LOGE(TAG, "flash unlock failed"); return false; }
    if (!erase_flash(h))                                    return false;
    if (!write_flash(h, kFirmwareData, sizeof(kFirmwareData))) return false;

    /* Verification omitted by default — slow on byte-by-byte path. */
    return true;
}

/* ------------------------------------------------------------------ */
/*  Public API                                                         */
/* ------------------------------------------------------------------ */

bool axs5106l_upgrade_get_chip_version(axs5106l_upgrade_handle_t h, uint16_t *version)
{
    if (h == NULL || version == NULL) return false;

    uint8_t fw_ver[2] = {0};
    if (!i2c_read_reg(h, 0x05, fw_ver, 2)) return false;
    *version = ((uint16_t)fw_ver[0] << 8) | fw_ver[1];
    return true;
}

uint16_t axs5106l_upgrade_get_embedded_version(void)
{
    if (sizeof(kFirmwareData) < FIRMWARE_VERSION_OFFSET + 2) return 0;
    return ((uint16_t)kFirmwareData[FIRMWARE_VERSION_OFFSET] << 8) |
            kFirmwareData[FIRMWARE_VERSION_OFFSET + 1];
}

axs5106l_upgrade_result_t axs5106l_upgrade_run(axs5106l_upgrade_handle_t h)
{
    if (h == NULL) return AXS5106L_UPGRADE_I2C_ERROR;

    /* 1. Read the version currently running on the chip. */
    uint16_t chip_version = 0;
    if (!axs5106l_upgrade_get_chip_version(h, &chip_version)) {
        ESP_LOGW(TAG, "cannot read chip firmware version; will attempt upgrade anyway "
                      "(may be a blank chip)");
    } else {
        ESP_LOGI(TAG, "chip firmware version: V%u", chip_version);
    }

    /* 2. Read the embedded version. */
    uint16_t embedded_version = axs5106l_upgrade_get_embedded_version();
    ESP_LOGI(TAG, "embedded firmware version: V%u", embedded_version);

    /* 3. Compare; skip if equal. */
    if (chip_version == embedded_version && chip_version != 0) {
        ESP_LOGI(TAG, "chip firmware up to date; no upgrade needed");
        return AXS5106L_UPGRADE_NOT_NEEDED;
    }

    /* 4. Run the upgrade. */
    ESP_LOGI(TAG, "starting firmware upgrade: V%u -> V%u", chip_version, embedded_version);

    for (int retry = 0; retry < UPGRADE_RETRY_TIMES; retry++) {
        if (do_upgrade(h)) {
            software_reset(h);
            delay_ms(50);

            uint16_t new_version = 0;
            if (axs5106l_upgrade_get_chip_version(h, &new_version)) {
                if (new_version == embedded_version) {
                    ESP_LOGI(TAG, "firmware upgrade succeeded; new version: V%u", new_version);
                    return AXS5106L_UPGRADE_SUCCESS;
                }
                ESP_LOGW(TAG, "version mismatch after upgrade: expected V%u, got V%u",
                         embedded_version, new_version);
            }
            ESP_LOGW(TAG, "post-upgrade version verification failed; retrying...");
        }
    }

    ESP_LOGE(TAG, "firmware upgrade failed");
    return AXS5106L_UPGRADE_FAILED;
}
