/*
 * SPDX-FileCopyrightText: 2023 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */

#include "rtc_wake_stub.h"

#include <cinttypes>
#include "esp_sleep.h"
#include "esp_cpu.h"
#include "esp_rom_sys.h"
#include "esp_wake_stub.h"
#include "sdkconfig.h"

extern "C" {



// wakeup_cause stored in RTC memory
static uint32_t wakeup_cause;

// wakeup_time from CPU start to wake stub
static uint32_t wakeup_time;

// wake up stub function stored in RTC memory
void wake_stub(void)
{
    // Get wakeup time.
    wakeup_time = esp_cpu_get_cycle_count() / esp_rom_get_cpu_ticks_per_us();

    wakeup_cause = esp_wake_stub_get_wakeup_cause();

    ESP_RTC_LOGI("wake stub: going to deep sleep, wakeup cost %lu us, wakeup cause %lu", 
                 (unsigned long)wakeup_time, (unsigned long)wakeup_cause);

    if (wakeup_cause == ESP_SLEEP_WAKEUP_EXT0) {
        ESP_RTC_LOGI("wake ext0");
    }
    else if(wakeup_cause == ESP_SLEEP_WAKEUP_EXT1) {
        ESP_RTC_LOGI("wake ext1");
    }
    else if(wakeup_cause == ESP_SLEEP_WAKEUP_TIMER) {
        ESP_RTC_LOGI("wake timer");
    }

    esp_default_wake_deep_sleep();
}

} // extern "C"

