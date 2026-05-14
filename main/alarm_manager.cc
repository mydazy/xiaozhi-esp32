#include "alarm_manager.h"
#include "settings.h"
#include "mcp_server.h"
#include "audio/alarm_ringer.h"

#include <esp_log.h>
#include <esp_sleep.h>
#include <sys/time.h>
#include <cstring>
#include <climits>
#include <cJSON.h>

#define TAG "AlarmMgr"

static const char* NVS_NS = "alarms";
// 多核共享标志 · atomic 防撕裂 (Core 0 OTA 写 / main_loop 读)
static std::atomic<bool> s_time_synced{false};
static std::atomic<bool> s_timer_wakeup{false};  // Timer 唤醒标记（保留供 future 查询）
// RTC 时间合法性下限 · 防 RTC clear / 异常重启返回 1970 等
constexpr int kRtcSanityYear = 2024;

AlarmManager& AlarmManager::GetInstance() {
    static AlarmManager instance;
    return instance;
}

AlarmManager::AlarmManager() {
    for (int i = 0; i < 8; i++) alarms_[i].id = i;

    // 先初始化 NVS 异步队列和 worker task, 后续 LoadAlarms
    // 队列深度 16: 8 slot × 2 op type, 冗余 2 倍应对快速连续点击
    nvs_queue_ = xQueueCreate(16, sizeof(AlarmNvsOp));
    if (nvs_queue_) {
        nvs_worker_running_.store(true);
        // 栈 4608B：Settings open + nvs_commit + lvgl_port_lock + ESP_LOGI vprintf 边界
        xTaskCreatePinnedToCore(NvsWorkerTask, "alarm_nvs", 4608, this,
                                2 /* 低优, 不抢实时路径 */,
                                &nvs_worker_task_, 0 /* Core 0 */);
    } else {
        ESP_LOGE(TAG, "创建 NVS 队列失败, 闹钟持久化将退化为同步写");
    }

    LoadAlarms();
}

// ============================================================================
// NVS 持久化（内部方法，调用者已持锁或在构造函数中）
// ============================================================================

void AlarmManager::LoadAlarms() {
    if (loaded_) return;

    Settings settings(NVS_NS, false);

    for (int i = 0; i < 8; i++) {
        char key[16];
        snprintf(key, sizeof(key), "alarm_%d", i);

        int32_t data = settings.GetInt(key, -1);
        if (data < 0) continue;

        slot_occupied_ |= (1 << i);
        alarms_[i].enabled     = (data >> 24) & 0x01;
        alarms_[i].hour        = (data >> 16) & 0x1F;
        alarms_[i].minute      = (data >>  8) & 0x3F;
        alarms_[i].repeat_days = data & 0x7F;

        snprintf(key, sizeof(key), "msg_%d", i);
        alarms_[i].message = settings.GetString(key, "");
    }

    loaded_ = true;
    ESP_LOGI(TAG, "闹钟加载完成，占用: 0x%02X", slot_occupied_);
}

// 持业务锁调用，只改内存 + enqueue，不在此函数内做 NVS 写
// 真正的 NVS 写由 NvsWorkerTask 消费队列时执行 (DoNvsSaveIn)
bool AlarmManager::SaveAlarmLocked(const AlarmConfig& alarm) {
    if (alarm.id >= 8) return false;

    // 1. 同步更新内存 (调用者已持 mutex_)
    alarms_[alarm.id] = alarm;
    slot_occupied_ |= (1 << alarm.id);

    ESP_LOGI(TAG, "保存闹钟 %d: %02d:%02d en=%d repeat=0x%02X",
             alarm.id, alarm.hour, alarm.minute, alarm.enabled, alarm.repeat_days);

    // 2. 异步 enqueue NVS 写
    if (nvs_queue_ == nullptr) {
        // 降级：队列创建失败（极罕见 boot OOM）· 持锁路径不做同步 NVS（防死锁）
        // 此时只保住内存态 · 闹钟在重启后会丢 · 上层应感知 nvs_queue_ 创建失败 LOG
        ESP_LOGW(TAG, "NVS 队列不可用，闹钟 %d 仅内存生效 · 重启会丢失", alarm.id);
        return true;
    }
    AlarmNvsOp op{AlarmNvsOpType::Save, alarm};
    nvs_pending_.fetch_add(1, std::memory_order_acq_rel);
    if (xQueueSend(nvs_queue_, &op, 0) != pdTRUE) {
        nvs_pending_.fetch_sub(1, std::memory_order_acq_rel);
        ESP_LOGE(TAG, "NVS 队列满, 闹钟 %d 写入丢失", alarm.id);
        return false;
    }
    return true;
}

// 持业务锁调用，只改内存 + enqueue
void AlarmManager::RemoveAlarmLocked(uint8_t id) {
    if (id >= 8) return;

    // 1. 同步更新内存
    alarms_[id] = AlarmConfig();
    alarms_[id].id = id;
    slot_occupied_ &= ~(1 << id);

    ESP_LOGI(TAG, "删除闹钟 %d", id);

    // 2. 异步 enqueue NVS 删除
    if (nvs_queue_ == nullptr) {
        // 同 Save 降级：持锁路径不做同步 NVS · 内存已清 · NVS 残留下次启动覆盖
        ESP_LOGW(TAG, "NVS 队列不可用，闹钟 %d 内存清除 · NVS 残留", id);
        return;
    }
    AlarmNvsOp op{AlarmNvsOpType::Remove, {}};
    op.alarm.id = id;
    nvs_pending_.fetch_add(1, std::memory_order_acq_rel);
    // 队列满时丢弃 remove 影响小 (内存已清, 下次启动 LoadAlarms 读回 NVS 残留
    // 是退化问题不是安全问题, 下次 Save 会自然覆盖)
    if (xQueueSend(nvs_queue_, &op, 0) != pdTRUE) {
        nvs_pending_.fetch_sub(1, std::memory_order_acq_rel);
    }
}

int AlarmManager::FindFreeSlotLocked() const {
    for (int i = 0; i < 8; i++) {
        if (!IsSlotOccupied(i)) return i;
    }
    return -1;
}

// ============================================================================
// 公开 CRUD（线程安全）
// ============================================================================

bool AlarmManager::AddAlarm(const AlarmConfig& alarm) {
    // repeat_days 只用 7 bit · 公共 API 防 0xFF 静默 trunc
    if (alarm.id >= 8 || alarm.hour > 23 || alarm.minute > 59 ||
        alarm.repeat_days > 0x7F) {
        ESP_LOGW(TAG, "无效闹钟: id=%d %02d:%02d repeat=0x%02X",
                 alarm.id, alarm.hour, alarm.minute, alarm.repeat_days);
        return false;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    return SaveAlarmLocked(alarm);
}

bool AlarmManager::DeleteAlarm(uint8_t id) {
    if (id >= 8) return false;
    std::lock_guard<std::mutex> lock(mutex_);
    if (!IsSlotOccupied(id)) return false;
    RemoveAlarmLocked(id);
    return true;
}

bool AlarmManager::GetAlarm(uint8_t id, AlarmConfig& out) const {
    if (id >= 8) return false;
    std::lock_guard<std::mutex> lock(mutex_);
    if (!IsSlotOccupied(id)) return false;
    out = alarms_[id];
    return true;
}

void AlarmManager::ClearAllAlarms() {
    std::lock_guard<std::mutex> lock(mutex_);
    for (int i = 0; i < 8; i++) {
        if (IsSlotOccupied(i)) RemoveAlarmLocked(i);
    }
    ESP_LOGI(TAG, "已清除所有闹钟");
}

int AlarmManager::AddAlarmAuto(uint8_t hour, uint8_t minute, uint8_t repeat_days,
                               const std::string& message) {
    if (hour > 23 || minute > 59) return -1;

    std::lock_guard<std::mutex> lock(mutex_);
    int id = FindFreeSlotLocked();
    if (id < 0) return -1;

    AlarmConfig a;
    a.id          = static_cast<uint8_t>(id);
    a.enabled     = true;
    a.hour        = hour;
    a.minute      = minute;
    a.repeat_days = repeat_days;
    a.message     = message;

    return SaveAlarmLocked(a) ? id : -1;
}

bool AlarmManager::SetAlarmEnabled(uint8_t id, bool enabled) {
    if (id >= 8) return false;
    std::lock_guard<std::mutex> lock(mutex_);
    if (!IsSlotOccupied(id)) return false;
    alarms_[id].enabled = enabled;
    return SaveAlarmLocked(alarms_[id]);
}

// ============================================================================
// 触发检查（CLOCK_TICK 每秒调用）
// ============================================================================

void AlarmManager::CheckAndTrigger() {
    time_t now;
    time(&now);
    struct tm ti;
    localtime_r(&now, &ti);

    int hour   = ti.tm_hour;
    int minute = ti.tm_min;
    int wday   = ti.tm_wday;

    // tm_wday 越界防御 · RTC 损坏可能返回 -1 · `1<<wday` 是 UB
    if (wday < 0 || wday > 6) return;

    // 时间门只看 (synced) || (RTC year ≥ 2024)
    // 删除原 s_timer_wakeup 必要条件 → EXT0 按键开机时 RTC 年份合理就允许触发
    // 防漏闹钟：用户 06:59:55 按键开机 + NTP 完成需 10s · 否则 7:00 闹钟错过
    if (!s_time_synced.load(std::memory_order_acquire) &&
        ti.tm_year + 1900 < kRtcSanityYear) {
        return;
    }

    // 持锁收集触发闹钟，不调回调（避免死锁）
    AlarmConfig triggered[8];
    int count = 0;
    std::function<void(const AlarmConfig&)> cb;

    {
        std::lock_guard<std::mutex> lock(mutex_);

        // dedup 移入锁内 · last_trigger_time_ 不再锁外裸读
        // 删除 (now - last) >= 0 子句 · NTP 回拨超 60s 不重复触发
        if (last_trigger_time_ > 0 && (now - last_trigger_time_) < 60) {
            return;
        }

        for (int i = 0; i < 8; i++) {
            if (!IsSlotOccupied(i) || !alarms_[i].enabled) continue;
            if (alarms_[i].hour != hour || alarms_[i].minute != minute) continue;

            // 检查周几（repeat_days=0 表示一次性，任何日期都触发）
            if (alarms_[i].repeat_days != 0 &&
                !(alarms_[i].repeat_days & (1 << wday))) continue;

            ESP_LOGI(TAG, "闹钟 %d 触发: %02d:%02d「%s」",
                     i, hour, minute, alarms_[i].message.c_str());
            triggered[count++] = alarms_[i];

            // 一次性闹钟：触发后直接删除
            if (alarms_[i].repeat_days == 0) {
                RemoveAlarmLocked(i);
                ESP_LOGI(TAG, "一次性闹钟 %d 已自动删除", i);
            }
        }

        if (count > 0) {
            last_trigger_time_ = now;
            last_triggered_ = triggered[count - 1];  // 记录最近触发的
            has_triggered_ = true;
        }

        cb = alarm_callback_;
    }

    // 锁外回调（回调可能调用 Application::Schedule/PlaySound）
    for (int i = 0; i < count; i++) {
        if (cb) cb(triggered[i]);
    }
}

void AlarmManager::SetAlarmCallback(std::function<void(const AlarmConfig&)> cb) {
    std::lock_guard<std::mutex> lock(mutex_);
    alarm_callback_ = cb;
}

// ============================================================================
// 时间计算（调用者已持锁）
// ============================================================================

int AlarmManager::CalcSecondsUntil(const AlarmConfig& alarm) const {
    if (!alarm.enabled) return INT_MAX;

    time_t now;
    time(&now);
    struct tm ti;
    localtime_r(&now, &ti);

    int now_sec   = ti.tm_hour * 3600 + ti.tm_min * 60 + ti.tm_sec;
    int alarm_sec = alarm.hour * 3600 + alarm.minute * 60;

    // 一次性闹钟：今天剩余或明天
    if (alarm.repeat_days == 0) {
        int diff = alarm_sec - now_sec;
        return (diff > 0) ? diff : diff + 86400;
    }

    // 重复闹钟：找最近匹配日（最多看 7 天）
    for (int offset = 0; offset < 7; offset++) {
        int check_wday = (ti.tm_wday + offset) % 7;
        if (!(alarm.repeat_days & (1 << check_wday))) continue;

        int diff = offset * 86400 + alarm_sec - now_sec;
        if (diff > 0) return diff;
    }

    return INT_MAX;
}

// ============================================================================
// 深睡唤醒
// ============================================================================

esp_err_t AlarmManager::ConfigureTimerWakeup() {
    std::lock_guard<std::mutex> lock(mutex_);

    int min_sec = INT_MAX;
    for (int i = 0; i < 8; i++) {
        if (!IsSlotOccupied(i) || !alarms_[i].enabled) continue;
        int s = CalcSecondsUntil(alarms_[i]);
        if (s < min_sec) min_sec = s;
    }

    if (min_sec == INT_MAX) {
        ESP_LOGI(TAG, "无启用闹钟，跳过定时唤醒");
        return ESP_ERR_NOT_FOUND;
    }

    // 分段策略（RC 漂移 5% · 详见 docs/p30-alarm-flows.html § 6）：
    //   ≤30min: 不睡  ·  ≤4h: 提前 30min 醒  ·  >4h: 分段 2h
    constexpr int ADVANCE_SHORT_SEC = 1800;   // 短睡提前 30 分钟
    constexpr int SEGMENT_INTERVAL  = 7200;   // 分段间隔 2 小时
    constexpr int SEGMENT_THRESHOLD = 14400;  // > 4 小时启用分段

    int sleep_sec;

    if (min_sec <= ADVANCE_SHORT_SEC) {
        // 闹钟不足 30 分钟，立即唤醒
        sleep_sec = 0;
        ESP_LOGI(TAG, "闹钟 %d 秒后到达，立即唤醒", min_sec);
    } else if (min_sec <= SEGMENT_THRESHOLD) {
        // 4 小时内：提前 30 分钟唤醒（覆盖 ~12 分钟漂移 + 18 分钟联网校时）
        sleep_sec = min_sec - ADVANCE_SHORT_SEC;
        ESP_LOGI(TAG, "闹钟 %d 秒后，提前 %d 秒唤醒，睡 %d 秒",
                 min_sec, ADVANCE_SHORT_SEC, sleep_sec);
    } else {
        // > 4 小时：分段睡眠，先睡 2 小时，唤醒后系统会自动联网校时再重新深睡
        sleep_sec = SEGMENT_INTERVAL;
        ESP_LOGI(TAG, "闹钟 %d 秒后（>4h），分段睡眠 %d 秒（每 2h 唤醒校时）",
                 min_sec, sleep_sec);
    }

    if (sleep_sec == 0) {
        // 不睡了，直接返回，让系统保持运行等待闹钟触发
        ESP_LOGI(TAG, "闹钟临近，跳过深睡");
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t ret = esp_sleep_enable_timer_wakeup((uint64_t)sleep_sec * 1000000ULL);
    ESP_LOGI(TAG, "定时唤醒已配置: %d 秒后 (%s)", sleep_sec, esp_err_to_name(ret));
    return ret;
}

int AlarmManager::GetSecondsToNextAlarm() {
    std::lock_guard<std::mutex> lock(mutex_);

    int min_sec = INT_MAX;
    for (int i = 0; i < 8; i++) {
        if (!IsSlotOccupied(i) || !alarms_[i].enabled) continue;
        int s = CalcSecondsUntil(alarms_[i]);
        if (s < min_sec) min_sec = s;
    }
    return (min_sec == INT_MAX) ? -1 : min_sec;
}

// ============================================================================
// MCP 语音控制
// ============================================================================

void AlarmManager::RegisterMcpTools() {
    // 幂等守卫 · 防意外二次调用导致 MCP 工具重复注册
    static bool registered = false;
    if (registered) return;
    registered = true;

    auto& mcp = McpServer::GetInstance();

    mcp.AddTool("self.alarm.add",
        "设闹钟。先问清提醒事项，别用『闹钟/起床』敷衍。"
        "**message 必须是 AI 写好的完整唤醒提示词**（设备到点裸用 · 不再拼前缀）。"
        "**硬上限 ≤10 汉字（30 字节）**，超长返回 message_too_long 让 AI 精简重试。"
        "示例（按场景自由组合）："
        "  起床：『起床上学』『早安主人』『该起床啦』；"
        "  日程：『下午开会』『产品评审会』『该开会了』；"
        "  生活：『提醒买菜』『记得吃药』『接孩子放学』；"
        "  保健：『该吃感冒药』『起来喝水』。"
        "repeat_days：0=一次，0x7F=每天，0x3E=工作日，0x41=周末。",
        PropertyList({
            Property("message", kPropertyTypeString),
            Property("hour", kPropertyTypeInteger, 0, 23),
            Property("minute", kPropertyTypeInteger, 0, 59),
            Property("repeat_days", kPropertyTypeInteger, 0, 127)
        }),
        [this](const PropertyList& props) -> ReturnValue {
            std::string msg = props["message"].value<std::string>();
            // trim 头尾空白判空（防 message="  " 这种全空格）
            auto first = msg.find_first_not_of(" \t\n\r");
            if (first == std::string::npos) {
                return std::string("missing_message: 需要先询问主人要提醒什么具体事情才能设闹钟。"
                                   "请先问『要提醒您什么呢？』拿到答复后再调用本工具。");
            }
            // 硬限制：message ≤10 汉字（30 UTF-8 字节）· 设备到点裸用此字符串作唤醒词
            if (msg.size() > 30) {
                return std::string("message_too_long: 唤醒词最多 10 个汉字，请精简后重试。"
                                   "例：『下午三点参加产品评审会』→『产品评审会』。");
            }

            int id = AddAlarmAuto(
                static_cast<uint8_t>(props["hour"].value<int>()),
                static_cast<uint8_t>(props["minute"].value<int>()),
                static_cast<uint8_t>(props["repeat_days"].value<int>()),
                msg);

            if (id < 0) return std::string("添加失败，已达上限(8个)");

            char buf[160];
            snprintf(buf, sizeof(buf), "已设置提醒[%d]「%s」%02d:%02d",
                     id, msg.c_str(),
                     props["hour"].value<int>(), props["minute"].value<int>());
            return std::string(buf);
        });

    mcp.AddTool("self.alarm.remove",
        "删闹钟，id=0-7。",
        PropertyList({ Property("id", kPropertyTypeInteger, 0, 7) }),
        [this](const PropertyList& props) -> ReturnValue {
            return DeleteAlarm(props["id"].value<int>())
                ? std::string("已删除") : std::string("闹钟不存在");
        });

    mcp.AddTool("self.alarm.list",
        "查闹钟列表。返回 alarms[] + count + next_alarm_in + last_triggered（最近触发的 time/message）。"
        "**仅在用户主动询问**『有什么闹钟 / 下一个几点响 / 设过哪些提醒』时调用。"
        "（响铃唤醒词『提醒+事项』/『再次提醒』已含完整语义 · AI 直接据此提醒即可 · 不必调本工具反查）",
        PropertyList(),
        [this](const PropertyList&) -> ReturnValue {
            std::lock_guard<std::mutex> lock(mutex_);
            return BuildJsonLocked();
        });

    mcp.AddTool("self.alarm.dismiss",
        "关掉正在响的闹钟。**响铃时**用户说『知道了 / 关掉 / 别响了 / 起来了 』等"
        "表示已收到提醒的话术时立即调用。"
        "用户说『再睡 N 分钟』：先调本工具，再 self.alarm.add 设 N 分钟一次性闹钟。"
        "返回 no_alarm_ringing 说明此刻没响铃，按正常对话处理。",
        PropertyList(),
        [](const PropertyList&) -> ReturnValue {
            if (!AlarmRinger::GetInstance().IsRinging()) {
                return std::string("no_alarm_ringing");
            }
            AlarmRinger::GetInstance().Stop("voice");
            return std::string("已关闭闹钟");
        });

    ESP_LOGI(TAG, "MCP 闹钟工具已注册");
}

std::string AlarmManager::BuildJsonLocked() const {
    cJSON* root = cJSON_CreateObject();
    if (!root) return "{}";

    cJSON* arr = cJSON_CreateArray();
    int count = 0;

    for (int i = 0; i < 8; i++) {
        if (!IsSlotOccupied(i)) continue;

        cJSON* item = cJSON_CreateObject();
        cJSON_AddNumberToObject(item, "id", i);

        char ts[8];
        snprintf(ts, sizeof(ts), "%02d:%02d", alarms_[i].hour, alarms_[i].minute);
        cJSON_AddStringToObject(item, "time", ts);
        cJSON_AddBoolToObject(item, "enabled", alarms_[i].enabled);
        cJSON_AddNumberToObject(item, "repeat_days", alarms_[i].repeat_days);
        if (!alarms_[i].message.empty()) {
            cJSON_AddStringToObject(item, "message", alarms_[i].message.c_str());
        }
        cJSON_AddItemToArray(arr, item);
        count++;
    }

    cJSON_AddItemToObject(root, "alarms", arr);
    cJSON_AddNumberToObject(root, "count", count);

    // 最近触发的闹钟（AI 用来判断提醒内容）
    if (has_triggered_) {
        cJSON* trig = cJSON_CreateObject();
        char ts[8];
        snprintf(ts, sizeof(ts), "%02d:%02d", last_triggered_.hour, last_triggered_.minute);
        cJSON_AddStringToObject(trig, "time", ts);
        cJSON_AddStringToObject(trig, "message",
            last_triggered_.message.empty() ? "闹钟" : last_triggered_.message.c_str());
        cJSON_AddItemToObject(root, "just_triggered", trig);
        has_triggered_ = false;  // 读一次后清除
    }

    // 下一个闹钟倒计时
    int min_sec = INT_MAX;
    for (int i = 0; i < 8; i++) {
        if (!IsSlotOccupied(i) || !alarms_[i].enabled) continue;
        int s = CalcSecondsUntil(alarms_[i]);
        if (s < min_sec) min_sec = s;
    }
    if (min_sec != INT_MAX) {
        char ns[16];
        snprintf(ns, sizeof(ns), "%02d:%02d:%02d",
                 min_sec / 3600, (min_sec % 3600) / 60, min_sec % 60);
        cJSON_AddStringToObject(root, "next_alarm_in", ns);
    } else {
        cJSON_AddStringToObject(root, "next_alarm_in", "none");
    }

    char* str = cJSON_PrintUnformatted(root);
    std::string result = str ? str : "{}";
    if (str) cJSON_free(str);
    cJSON_Delete(root);
    return result;
}

// ============================================================================
// 时间服务
// ============================================================================

void AlarmManager::MarkTimeSynced() {
    if (s_time_synced.exchange(true, std::memory_order_release)) return;
    ESP_LOGI(TAG, "时间已校准，闹钟检查已启用");
}

void AlarmManager::MarkTimerWakeup() {
    s_timer_wakeup.store(true, std::memory_order_release);
    ESP_LOGI(TAG, "Timer 唤醒标记");
}

bool AlarmManager::IsTimeSynced() {
    return s_time_synced.load(std::memory_order_acquire);
}


// ============================================================================
// 异步 NVS worker (单例生命周期, 不退出)
// ============================================================================

void AlarmManager::DoNvsSaveIn(Settings& settings, const AlarmConfig& alarm) {
    int32_t data = ((alarm.enabled ? 1 : 0) << 24) |
                   ((alarm.hour   & 0x1F)   << 16) |
                   ((alarm.minute & 0x3F)   <<  8) |
                   (alarm.repeat_days & 0x7F);

    char key[16];
    snprintf(key, sizeof(key), "alarm_%d", alarm.id);
    settings.SetInt(key, data);

    snprintf(key, sizeof(key), "msg_%d", alarm.id);
    if (!alarm.message.empty()) {
        settings.SetString(key, alarm.message);
    } else {
        settings.EraseKey(key);
    }
}

void AlarmManager::DoNvsRemoveIn(Settings& settings, uint8_t id) {
    if (id >= 8) return;
    char key[16];
    snprintf(key, sizeof(key), "alarm_%d", id);  settings.EraseKey(key);
    snprintf(key, sizeof(key), "msg_%d", id);    settings.EraseKey(key);
}

// 批量 drain + 合并 + 单次 commit：
// 用户连点 8 次开关 → 1 次 flash_op + 1 次 lvgl_port_lock，代替 8 次。
// 合并规则：同 id 后到的 op 覆盖先到的（last op wins）· 不分 Save / Remove 优先级。
void AlarmManager::NvsWorkerTask(void* arg) {
    AlarmManager* self = static_cast<AlarmManager*>(arg);
    AlarmNvsOp op;
    while (self->nvs_worker_running_.load()) {
        // 500ms 超时：合并窗口 200ms 之后入队的 op 不再卡 5s · FlushNvs 也能及时清空
        if (xQueueReceive(self->nvs_queue_, &op, pdMS_TO_TICKS(500)) != pdTRUE) {
            continue;
        }

        // 200ms 连击窗口：让用户连点的后续 op 堆进队列一次性合并
        vTaskDelay(pdMS_TO_TICKS(200));

        // 按 id 索引合并（0=无操作, 1=save, 2=remove）
        uint8_t action[8] = {0};
        AlarmConfig snapshot[8] = {};

        auto apply = [&](const AlarmNvsOp& o) {
            uint8_t id = o.alarm.id;
            if (id >= 8) return;
            if (o.type == AlarmNvsOpType::Save) {
                action[id] = 1;
                snapshot[id] = o.alarm;
            } else {  // Remove
                action[id] = 2;
            }
        };
        apply(op);
        uint32_t drained = 1;
        while (xQueueReceive(self->nvs_queue_, &op, 0) == pdTRUE) {
            apply(op);
            drained++;
        }

        // 单次 Settings 作用域 → 析构时 1 次 lvgl_port_lock + 1 次 nvs_commit
        {
            Settings settings(NVS_NS, true);
            for (uint8_t id = 0; id < 8; id++) {
                if (action[id] == 1) {
                    self->DoNvsSaveIn(settings, snapshot[id]);
                } else if (action[id] == 2) {
                    self->DoNvsRemoveIn(settings, id);
                }
            }
        }
        ESP_LOGI(TAG, "NVS flush: %u ops 合并为 1 次 commit", (unsigned)drained);

        // pending 清零（FlushNvs 轮询退出）
        uint32_t prev = self->nvs_pending_.load(std::memory_order_acquire);
        if (prev >= drained) {
            self->nvs_pending_.fetch_sub(drained, std::memory_order_acq_rel);
        } else {
            self->nvs_pending_.store(0, std::memory_order_release);
        }
    }
    // 注：当前实现 nvs_worker_running_ 永远 true · while 永不退出 · 此处 vTaskDelete 不可达
    // 保留 atomic 字段方便未来 shutdown 时优雅退出（写 false + 等任务消亡）
}

// 兜底：阻塞等 worker drain 完所有排队写（OTA/关机/深睡前调用）
bool AlarmManager::FlushNvs(uint32_t timeout_ms) {
    const TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);
    while (nvs_pending_.load(std::memory_order_acquire) > 0) {
        if ((int32_t)(deadline - xTaskGetTickCount()) <= 0) {
            ESP_LOGW(TAG, "FlushNvs 超时 %ums，仍有 %u pending",
                     (unsigned)timeout_ms,
                     (unsigned)nvs_pending_.load(std::memory_order_acquire));
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    return true;
}
