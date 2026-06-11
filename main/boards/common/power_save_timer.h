#pragma once

#include <functional>
#include <atomic>

#include <esp_timer.h>
#include <esp_pm.h>

class PowerSaveTimer {
public:
    PowerSaveTimer(int cpu_max_freq, int seconds_to_sleep = 20, int seconds_to_shutdown = -1);
    ~PowerSaveTimer();

    void SetEnabled(bool enabled);
    void OnEnterSleepMode(std::function<void()> callback);
    void OnExitSleepMode(std::function<void()> callback);
    void OnShutdownRequest(std::function<void()> callback);
    void WakeUp();
    bool IsInSleepMode() const { return in_sleep_mode_; }   // 省电降亮中（触摸首触不投递判据）

private:
    void PowerSaveCheck();

    esp_timer_handle_t power_save_timer_ = nullptr;
    std::atomic<bool> enabled_{false};
    std::atomic<bool> in_sleep_mode_{false};
    std::atomic<bool> is_wake_word_running_{false};
    std::atomic<int> ticks_{0};
    int cpu_max_freq_;
    int seconds_to_sleep_;
    int seconds_to_shutdown_;

    std::function<void()> on_enter_sleep_mode_;
    std::function<void()> on_exit_sleep_mode_;
    std::function<void()> on_shutdown_request_;
};
