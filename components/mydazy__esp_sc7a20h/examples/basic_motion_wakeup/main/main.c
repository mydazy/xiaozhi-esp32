/*
 * SPDX-FileCopyrightText: 2026 mydazy
 * SPDX-License-Identifier: Apache-2.0
 *
 * SC7A20H — basic motion-wakeup example.
 *
 * Demonstrates the typical handheld / wearable use case:
 *   1. initialise the SC7A20H over I2C,
 *   2. enable motion detection on the INT1 line with a 500 ms debounced log,
 *   3. configure deep-sleep wake-up on INT1,
 *   4. enter deep sleep — the next motion event resets the MCU.
 *
 * Wiring (ESP32-S3 reference):
 *   SC7A20H VCC -> 3V3
 *   SC7A20H GND -> GND
 *   SC7A20H SDA -> GPIO_SDA (4.7 k pull-up to 3V3)
 *   SC7A20H SCL -> GPIO_SCL (4.7 k pull-up to 3V3)
 *   SC7A20H INT1 -> GPIO_INT1 (any RTC-capable GPIO)
 */

#include "sc7a20h.h"

#include <esp_log.h>
#include <esp_sleep.h>
#include <esp_err.h>
#include <driver/i2c_master.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

static const char *TAG = "sc7a20h_example";

/* ---- Pin map — change to match your board ---- */
#define I2C_PORT     I2C_NUM_0
#define GPIO_SDA     GPIO_NUM_6
#define GPIO_SCL     GPIO_NUM_7
#define GPIO_INT1    GPIO_NUM_3

void app_main(void)
{
    /* 1. Bring up the I2C master bus. */
    i2c_master_bus_handle_t bus;
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = I2C_PORT,
        .sda_io_num = GPIO_SDA,
        .scl_io_num = GPIO_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_ERROR_CHECK(i2c_new_master_bus(&bus_cfg, &bus));

    /* 2. Create driver + enable motion detection in one call. */
    sc7a20h_handle_t sensor = NULL;
    sc7a20h_config_t cfg = SC7A20H_DEFAULT_CONFIG();
    if (sc7a20h_create_with_motion_detection(bus, &cfg, NULL, &sensor) != ESP_OK) {
        ESP_LOGE(TAG, "Sensor init failed — check wiring / address");
        return;
    }

    /* 3. Read a few samples in milli-g. */
    for (int i = 0; i < 5; ++i) {
        sc7a20h_acce_t acc;
        if (sc7a20h_get_acce(sensor, &acc) == ESP_OK) {
            ESP_LOGI(TAG, "X = %7.1f mg   Y = %7.1f mg   Z = %7.1f mg",
                     acc.x, acc.y, acc.z);
        }
        vTaskDelay(pdMS_TO_TICKS(200));
    }

    /* 4. Configure deep-sleep wake-up on INT1 and sleep. */
    ESP_ERROR_CHECK(sc7a20h_config_deep_sleep_wakeup(sensor, GPIO_INT1));
    ESP_LOGI(TAG, "Entering deep sleep — move the device to wake.");
    esp_deep_sleep_start();
}
