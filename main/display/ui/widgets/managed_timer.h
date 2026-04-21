#pragma once

#include <lvgl.h>

/**
 * ManagedTimer — LVGL 定时器 RAII 封装
 *
 * 析构自动 lv_timer_del，防止对象释放后回调访问野指针。
 * 用法：
 *   ManagedTimer timer;
 *   timer.Create(1000, cb, user_data);  // 1s 周期
 *   timer.Pause(); timer.Resume();      // 暂停/恢复
 */
class ManagedTimer {
public:
    ManagedTimer() = default;
    ~ManagedTimer() { Delete(); }

    // 禁止拷贝
    ManagedTimer(const ManagedTimer&) = delete;
    ManagedTimer& operator=(const ManagedTimer&) = delete;

    // 移动构造
    ManagedTimer(ManagedTimer&& other) noexcept : timer_(other.timer_) { other.timer_ = nullptr; }
    ManagedTimer& operator=(ManagedTimer&& other) noexcept {
        if (this != &other) { Delete(); timer_ = other.timer_; other.timer_ = nullptr; }
        return *this;
    }

    void Create(uint32_t period_ms, lv_timer_cb_t cb, void* user_data) {
        Delete();
        timer_ = lv_timer_create(cb, period_ms, user_data);
    }

    // 单次触发（自动 repeat_count=1，回调后 LVGL 停止但不删除）
    void CreateOnce(uint32_t delay_ms, lv_timer_cb_t cb, void* user_data) {
        Delete();
        timer_ = lv_timer_create(cb, delay_ms, user_data);
        if (timer_) lv_timer_set_repeat_count(timer_, 1);
    }

    void Delete() {
        if (timer_) { lv_timer_del(timer_); timer_ = nullptr; }
    }

    void Pause()  { if (timer_) lv_timer_pause(timer_); }
    void Resume() { if (timer_) lv_timer_resume(timer_); }

    lv_timer_t* Get() const { return timer_; }

private:
    lv_timer_t* timer_ = nullptr;
};
