#ifndef ALARM_MANAGER_H
#define ALARM_MANAGER_H

#include <cstdint>
#include <string>
#include <functional>
#include <mutex>
#include <atomic>
#include <esp_err.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>

// 闹钟配置（量产精简版）
struct AlarmConfig {
    uint8_t id = 0;           // 槽位 ID (0-7)
    bool enabled = false;     // 是否启用
    uint8_t hour = 0;         // 小时 (0-23)
    uint8_t minute = 0;       // 分钟 (0-59)
    uint8_t repeat_days = 0;  // 重复掩码 (bit0=周日..bit6=周六, 0=仅一次)
    std::string message;      // 提醒内容
};

enum class AlarmNvsOpType : uint8_t {
    Save,
    Remove,
};
// 必须保持 POD：xQueueSend 按字节 memcpy，含 std::string 会在发送方 op 析构后悬垂
struct AlarmNvsOp {
    AlarmNvsOpType type;
    uint8_t id = 0;          // Save: 完整字段; Remove: 仅 id 有效
    bool enabled = false;
    uint8_t hour = 0;
    uint8_t minute = 0;
    uint8_t repeat_days = 0;
    char message[31] = {};   // MCP 入口硬限 ≤30 UTF-8 字节，31 容纳 NUL
};

// 闹钟管理器（单例，线程安全，8 固定槽位，NVS 持久化）
//
// 两个入口：MCP 语音 / 深睡唤醒
// 触发方式：CLOCK_TICK 每秒调用 CheckAndTrigger()
class AlarmManager {
public:
    static AlarmManager& GetInstance();

    AlarmManager(const AlarmManager&) = delete;
    AlarmManager& operator=(const AlarmManager&) = delete;

    // ---- CRUD（线程安全）· AddAlarm 重复 id 即更新 ----
    bool AddAlarm(const AlarmConfig& alarm);
    bool DeleteAlarm(uint8_t id);
    bool GetAlarm(uint8_t id, AlarmConfig& out) const;
    void ClearAllAlarms();

    // 原子分配槽位并添加（MCP 用，避免 GetId→Add 之间的竞争）
    // 返回分配的 id，-1 = 失败
    int AddAlarmAuto(uint8_t hour, uint8_t minute, uint8_t repeat_days,
                     const std::string& message);

    bool SetAlarmEnabled(uint8_t id, bool enabled);

    // ---- 触发（每秒由 CLOCK_TICK 调用）----
    void CheckAndTrigger();
    void SetAlarmCallback(std::function<void(const AlarmConfig&)> cb);

    // ---- 深睡（进入深睡前调用）----
    esp_err_t ConfigureTimerWakeup();

    // 阻塞等 NVS 写队列 drain 完（OTA/关机/深睡前兜底）
    // timeout_ms 到期仍未清空时返回 false，但不阻塞调用方太久
    bool FlushNvs(uint32_t timeout_ms = 3000);

    // 最近闹钟还有多少秒到达（-1=无闹钟，0=已到达）
    // 用于分段唤醒后判断是否需要回睡
    int GetSecondsToNextAlarm();

    // ---- MCP 语音控制 ----
    void RegisterMcpTools();

    // ---- 时间服务 ----
    // OTA/NTP 校时成功后调用，启用闹钟检查
    static void MarkTimeSynced();
    static bool IsTimeSynced();
    // Timer 唤醒标记（允许未校时时用 RTC fallback 触发闹钟）
    static void MarkTimerWakeup();

private:
    AlarmManager();
    ~AlarmManager() = default;

    void LoadAlarms();
    bool SaveAlarmLocked(const AlarmConfig& alarm);
    void RemoveAlarmLocked(uint8_t id);
    bool IsSlotOccupied(uint8_t id) const { return (slot_occupied_ & (1 << id)) != 0; }
    int FindFreeSlotLocked() const;
    int CalcSecondsUntil(const AlarmConfig& alarm) const;
    std::string BuildJsonLocked() const;

    void DoNvsSaveIn(class Settings& settings, const AlarmConfig& alarm);
    void DoNvsRemoveIn(class Settings& settings, uint8_t id);
    static void NvsWorkerTask(void* arg);

    mutable std::mutex mutex_;
    AlarmConfig alarms_[8];
    uint8_t slot_occupied_ = 0;
    bool loaded_ = false;
    std::function<void(const AlarmConfig&)> alarm_callback_;

    // NVS 写异步队列
    QueueHandle_t nvs_queue_ = nullptr;
    TaskHandle_t nvs_worker_task_ = nullptr;
    std::atomic<bool> nvs_worker_running_{false};
    // pending = 队列未入 + worker 正在处理中。FlushNvs 轮询至 0。
    std::atomic<uint32_t> nvs_pending_{0};

    // 防重触发
    time_t last_trigger_time_ = 0;
    // 最近触发的闹钟（供 AI 查询"闹钟响了"时读取）
    AlarmConfig last_triggered_;
    mutable bool has_triggered_ = false;  // BuildJsonLocked 读后清除
};

#endif // ALARM_MANAGER_H
