#ifndef MANAGED_TIMER_H
#define MANAGED_TIMER_H

#include <lvgl.h>

/**
 * ManagedTimer - LVGL定时器安全封装
 *
 * 解决的问题：
 * 1. 定时器删除后未置nullptr导致野指针
 * 2. 重复删除导致崩溃
 * 3. 析构时忘记删除定时器导致回调访问已释放对象
 *
 * 使用示例：
 *   ManagedTimer timer;
 *   timer.Create(1000, MyCallback, this);  // 创建1秒定时器
 *   timer.Pause();   // 暂停
 *   timer.Resume();  // 恢复
 *   timer.Delete();  // 删除（或析构时自动删除）
 */
class ManagedTimer {
public:
    ManagedTimer() = default;

    ~ManagedTimer() {
        Delete();
    }

    // 禁止拷贝
    ManagedTimer(const ManagedTimer&) = delete;
    ManagedTimer& operator=(const ManagedTimer&) = delete;

    // 允许移动
    ManagedTimer(ManagedTimer&& other) noexcept {
        timer_ = other.timer_;
        other.timer_ = nullptr;
    }

    ManagedTimer& operator=(ManagedTimer&& other) noexcept {
        if (this != &other) {
            Delete();
            timer_ = other.timer_;
            other.timer_ = nullptr;
        }
        return *this;
    }

    /**
     * 创建定时器
     * @param period_ms 周期（毫秒）
     * @param callback 回调函数
     * @param user_data 用户数据
     */
    void Create(uint32_t period_ms, lv_timer_cb_t callback, void* user_data) {
        Delete();  // 先删除旧的
        timer_ = lv_timer_create(callback, period_ms, user_data);
    }

    /**
     * 创建单次定时器（触发一次后自动停止）
     */
    void CreateOnce(uint32_t delay_ms, lv_timer_cb_t callback, void* user_data) {
        Delete();
        timer_ = lv_timer_create(callback, delay_ms, user_data);
        if (timer_) {
            lv_timer_set_repeat_count(timer_, 1);
            lv_timer_set_auto_delete(timer_, false);
        }
    }

    /**
     * 删除定时器
     */
    void Delete() {
        if (timer_) {
            lv_timer_delete(timer_);
            timer_ = nullptr;
        }
    }

    /**
     * 重置定时器（删除后重新创建）
     */
    void Reset(uint32_t period_ms, lv_timer_cb_t callback, void* user_data) {
        Delete();
        Create(period_ms, callback, user_data);
    }

    /**
     * 暂停定时器
     */
    void Pause() {
        if (timer_) {
            lv_timer_pause(timer_);
        }
    }

    /**
     * 恢复定时器
     */
    void Resume() {
        if (timer_) {
            lv_timer_resume(timer_);
        }
    }

    /**
     * 立即触发一次回调
     */
    void Ready() {
        if (timer_) {
            lv_timer_ready(timer_);
        }
    }

    /**
     * 设置周期
     */
    void SetPeriod(uint32_t period_ms) {
        if (timer_) {
            lv_timer_set_period(timer_, period_ms);
        }
    }

    /**
     * 设置重复次数（-1为无限）
     */
    void SetRepeatCount(int32_t count) {
        if (timer_) {
            lv_timer_set_repeat_count(timer_, count);
        }
    }

    /**
     * 检查定时器是否有效
     */
    bool IsValid() const {
        return timer_ != nullptr;
    }

    /**
     * 检查定时器是否暂停
     */
    bool IsPaused() const {
        return timer_ ? lv_timer_get_paused(timer_) : true;
    }

    /**
     * 获取原始指针（谨慎使用）
     */
    lv_timer_t* Get() const {
        return timer_;
    }

private:
    lv_timer_t* timer_ = nullptr;
};

#endif // MANAGED_TIMER_H
