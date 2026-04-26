#pragma once

// P30-4G 专属 Button 扩展：
//   - 多时间点长按（700/3000/5000 ms 同时挂回调，用于录音 / 关机提醒 / 关机执行三段）
//   - 多次数连击（3/4/6/9 次同时挂不同回调）
//
// 实现方式：直接调 espressif/button 原生 iot_button_register_cb，
// 利用其 event_args.long_press.press_time / event_args.multiple_clicks.clicks 字段。
// 不修改 main/boards/common/button.cc，其他 board 不受影响。

#include "button.h"

#include <esp_log.h>
#include <iot_button.h>

#include <cstdint>
#include <functional>
#include <map>

class P30Button : public Button {
public:
    using Button::Button;          // 继承构造函数
    using Button::OnLongPress;     // 引入基类无参版长按，避免被下方重载隐藏

    // 多时间点长按：可重复调用注册不同 ms 的回调
    void OnLongPress(std::function<void()> callback, uint16_t press_time_ms) {
        if (button_handle_ == nullptr) return;

        long_press_callbacks_[press_time_ms] = std::move(callback);

        struct Ctx { P30Button* self; uint16_t ms; };
        auto* ctx = new Ctx{this, press_time_ms};

        button_event_args_t args = {};
        args.long_press.press_time = press_time_ms;

        iot_button_register_cb(button_handle_, BUTTON_LONG_PRESS_START, &args,
            [](void* /*handle*/, void* usr_data) {
                auto* c = static_cast<Ctx*>(usr_data);
                auto it = c->self->long_press_callbacks_.find(c->ms);
                if (it != c->self->long_press_callbacks_.end() && it->second) {
                    ESP_LOGI("P30Button", "长按 %u ms 触发", c->ms);
                    it->second();
                }
            }, ctx);
    }

    // 多次数连击：可重复调用注册不同 click_count 的回调
    // 隐藏基类 OnMultipleClick(同签名)。在 P30Button 对象上调用时静态分派到本版本。
    void OnMultipleClick(std::function<void()> callback, uint8_t click_count = 3) {
        if (button_handle_ == nullptr) return;

        multi_click_callbacks_[click_count] = std::move(callback);

        struct Ctx { P30Button* self; uint8_t count; };
        auto* ctx = new Ctx{this, click_count};

        button_event_args_t args = {};
        args.multiple_clicks.clicks = click_count;

        iot_button_register_cb(button_handle_, BUTTON_MULTIPLE_CLICK, &args,
            [](void* /*handle*/, void* usr_data) {
                auto* c = static_cast<Ctx*>(usr_data);
                auto it = c->self->multi_click_callbacks_.find(c->count);
                if (it != c->self->multi_click_callbacks_.end() && it->second) {
                    ESP_LOGI("P30Button", "%u 次连击触发", c->count);
                    it->second();
                }
            }, ctx);
    }

private:
    std::map<uint16_t, std::function<void()>> long_press_callbacks_;
    std::map<uint8_t,  std::function<void()>> multi_click_callbacks_;
};
