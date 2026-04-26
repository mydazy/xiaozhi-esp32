#pragma once

// P30-4G 专属 Backlight：
//   - PWM duty 上限钳到 ~30%（LEDC duty 300/1023），匹配 BL 共用 LDO 的电流限制
//   - 非 0 亮度最小 15%（屏幕过暗时易闪烁）
// 默认亮度（35）通过 ApplyDefaultSettings 写 NVS，不在此覆盖。
//
// 仅本 board 使用，不影响其他 board 的 PwmBacklight 行为。

#include "backlight.h"

#include <driver/ledc.h>

class P30Backlight : public PwmBacklight {
public:
    P30Backlight(gpio_num_t pin, bool output_invert)
        : PwmBacklight(pin, output_invert) {}

    // 覆盖 PwmBacklight：duty 上限 300/1023（≈29% 满量程）
    void SetBrightnessImpl(uint8_t brightness) override {
        // 输入侧再钳一次最低 15%（防止 caller 绕过 SetBrightness 直接走 transition timer 时漂到极低值）
        if (brightness != 0 && brightness < 15) {
            brightness = 15;
        }
        uint32_t duty_cycle = (300 * brightness) / 100;
        ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, duty_cycle);
        ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
    }
};
