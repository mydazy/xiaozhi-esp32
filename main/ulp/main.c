/*
 * SPDX-FileCopyrightText: 2023 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */
/* ULP-RISC-V example

   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.

   This code runs on ULP-RISC-V  coprocessor
*/

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include "ulp_riscv.h"
#include "ulp_riscv_utils.h"
#include "ulp_riscv_gpio.h"
#include "ulp_riscv_lock.h"

#define DEBOUNCE_SAMPLES (1 * (1000 / 100))

ulp_riscv_lock_t lock;
bool gpio_level = false;

int debounce_count = 0;

int main (void)
{
    ulp_riscv_gpio_init(GPIO_NUM_0);
    ulp_riscv_gpio_input_enable(GPIO_NUM_0);
    ulp_riscv_gpio_output_enable(GPIO_NUM_0);
    ulp_riscv_gpio_set_output_mode(GPIO_NUM_0, RTCIO_MODE_OUTPUT_OD);
    ulp_riscv_gpio_pullup(GPIO_NUM_0);
    ulp_riscv_gpio_pulldown_disable(GPIO_NUM_0);

    ulp_riscv_gpio_output_level(GPIO_NUM_0, 0);
    ulp_riscv_gpio_output_level(GPIO_NUM_0, 1);

    // /* Must sample within 15 us of the failing edge */
    // ulp_riscv_delay_cycles(5 * ULP_RISCV_CYCLES_PER_US);

    gpio_level = (bool)ulp_riscv_gpio_get_level(GPIO_NUM_0);

    if(gpio_level == 0){
        debounce_count++;
    }
    else{
        debounce_count = 0;
    }

    // if(debounce_count >= 1000000){
    //     gpio_level = ds18b20_read_bit();
    //     debounce_count = 0;
    // }

    if(debounce_count >= DEBOUNCE_SAMPLES){
        debounce_count = 0;
        // gpio_level = (bool)ulp_riscv_gpio_get_level(GPIO_NUM_0);
        // gpio_level_previous = gpio_level;
        ulp_riscv_wakeup_main_processor();
    }

    /* ulp_riscv_halt() is called automatically when main exits */
    return 0;
}
