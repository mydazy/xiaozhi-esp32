#ifndef _SYSTEM_RESET_H
#define _SYSTEM_RESET_H

#include <driver/gpio.h>

class SystemReset {
public:
    SystemReset(gpio_num_t reset_nvs_pin, gpio_num_t reset_factory_pin);
    void CheckButtons(bool force_reset = false);

    // 按键/UI 双击确认后调用，执行顺序：
    //   1) 请求服务器 /reset 解绑（可选，失败不阻塞）
    //   2) NVS 全擦 + 初始化
    //   3) otadata 分区擦除（回退到 factory / ota_0）
    // 执行完后调用方需调用 app.Reboot()（Reboot 内部已切 LDO）。
    static void DoFactoryReset(bool unbind_server = true);

private:
    gpio_num_t reset_nvs_pin_;
    gpio_num_t reset_factory_pin_;

    static bool RequestServerUnbind();
    static void ResetNvsFlash();
    static void ResetToFactory();
    void RestartInSeconds(int seconds);
};


#endif
