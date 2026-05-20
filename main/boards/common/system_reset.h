#ifndef _SYSTEM_RESET_H
#define _SYSTEM_RESET_H

#include <driver/gpio.h>

class SystemReset {
public:
    SystemReset(gpio_num_t reset_nvs_pin, gpio_num_t reset_factory_pin); // 构造函数私有化
    void CheckButtons();

    // 静态重载 · 业务层运行时复位（9 连击+双击确认 / MCP 远程命令 / 配网超时 等）
    static void CheckButtons(bool force_factory_reset);

private:
    gpio_num_t reset_nvs_pin_;
    gpio_num_t reset_factory_pin_;

    static void ResetNvsFlash();
    static void ResetToFactory();
    static void RestartInSeconds(int seconds);
};


#endif
