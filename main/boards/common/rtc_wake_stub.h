/*
 * SPDX-FileCopyrightText: 2023 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Deep sleep wake stub function
 * 
 * This function runs immediately as soon as the chip wakes up - before any normal
 * initialisation, bootloader, or ESP-IDF code has run.
 * 
 * Wake stub code must be carefully written:
 * 1) The wake stub code can only access data loaded in RTC memory.
 * 2) The wake stub code can only call functions implemented in ROM or loaded into RTC Fast Memory.
 * 3) RTC memory must include any read-only data (.rodata) used by the wake stub.
 */
void wake_stub(void);

#ifdef __cplusplus
}
#endif

