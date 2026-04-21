---
name: freertos-patterns
description: FreeRTOS 任务设计模式、队列用法、event group、死锁排查、栈大小估算、跨核通信。场景触发词：FreeRTOS、xTaskCreate、xQueue、xSemaphore、mutex、event group、死锁、栈溢出、优先级反转。自动被 rtos-task-auditor / firmware-debugger 加载。
---

# FreeRTOS 在 ESP32-S3 上的设计模式

本 skill 由 Claude Code 在遇到任务 / 并发 / 栈 / 同步相关问题时自动加载。

---

## 一、任务创建（选择正确 API）

### 1.1 API 选择表

| API | 栈位置 | 适用场景 |
|---|---|---|
| `xTaskCreate` | 内部 RAM | 中小栈（< 8KB）、默认选择 |
| `xTaskCreatePinnedToCore` | 内部 RAM | 绑定 core、ESP32 双核必须 |
| `xTaskCreateWithCaps` | 任意 CAP | 大栈 / PSRAM 栈 |
| `xTaskCreatePinnedToCoreWithCaps` | 任意 CAP + 绑核 | 大栈 + 绑核场景 |
| `xTaskCreateStatic` | 静态预分配 | 关键路径，避免动态分配失败 |

### 1.2 PSRAM 栈的陷阱（🔴 关键）

**不是所有任务都能用 PSRAM 栈**：

| 任务特征 | 栈位置 | 理由 |
|---|---|---|
| 持续循环 + Core0 | 🔴 **必须内部 RAM** | flash op（NVS/OTA）会禁用 cache + PSRAM |
| 实时 DMA/AFE | 🔴 内部 RAM | PSRAM 延迟不稳定 |
| TLS 握手（首次 I/O） | 🔴 内部 RAM | 栈需求爆发 + 关键路径 |
| WiFi/BT 初始化 | 🔴 内部 RAM | IDF 内部要求 |
| 一次性任务 + TLS/HTTP | ✅ 可 PSRAM | 偶发，容忍 flash op 冲突 |
| 长阻塞在 socket | ✅ 可 PSRAM | 主要时间在等，调度少 |
| 低频检查（几秒一次） | ✅ 可 PSRAM | 调度频率低 |

**Double Exception 症状**：
- Core 1 backtrace `ipc_task → spi_flash_op_block_func`
- Core 0 SP=0x60100000 或 `|<-CORRUPTED`
- = **必然的 PSRAM 栈 + flash op 碰撞**

### 1.3 栈大小估算
- **启动栈**（boot、init）：8KB 起
- **网络 I/O**（TLS、HTTP）：6KB
- **业务逻辑**：4KB
- **纯数据搬运 / 简单循环**：2KB
- **LVGL**：8KB
- 实测：`uxTaskGetStackHighWaterMark(NULL)` 定期采样，留 ≥ 512 字节余量

---

## 二、队列（Queue）

### 2.1 创建
```cpp
QueueHandle_t q = xQueueCreate(32, sizeof(MyMsg));  // 容量 32
// 不要 xQueueCreate(0, ...) 或 xQueueCreate(INT_MAX, ...)
```

### 2.2 发送
```cpp
// 带超时
if (xQueueSend(q, &msg, pdMS_TO_TICKS(100)) != pdPASS) {
    ESP_LOGW(TAG, "queue full, dropping msg");
}

// 中断里
BaseType_t yield = pdFALSE;
xQueueSendFromISR(q, &msg, &yield);
if (yield) portYIELD_FROM_ISR();
```

### 2.3 接收
```cpp
MyMsg msg;
if (xQueueReceive(q, &msg, pdMS_TO_TICKS(500)) == pdPASS) {
    // process
}
// ❌ 禁止 xQueueReceive(q, &msg, portMAX_DELAY)
```

### 2.4 队列模式
- **命令队列**（事件驱动）：main_loop 消费命令
- **数据管道**（生产-消费）：audio_input → afe → opus_codec
- **通知队列**（状态变化）：event 广播

---

## 三、信号量 / 互斥锁

### 3.1 Mutex（推荐）
```cpp
// 创建
SemaphoreHandle_t mux = xSemaphoreCreateMutex();

// 加锁（带超时）
if (xSemaphoreTake(mux, pdMS_TO_TICKS(100)) == pdTRUE) {
    // 临界区
    xSemaphoreGive(mux);
}
```
- 优势：自动优先级继承（防优先级反转）
- 不要 `xSemaphoreCreateBinary` 当 mutex 用

### 3.2 RAII 包装（推荐）
```cpp
class MutexGuard {
    SemaphoreHandle_t& m_;
    bool locked_;
public:
    MutexGuard(SemaphoreHandle_t& m, TickType_t to = pdMS_TO_TICKS(100))
        : m_(m), locked_(xSemaphoreTake(m, to) == pdTRUE) {}
    ~MutexGuard() { if (locked_) xSemaphoreGive(m_); }
    bool ok() const { return locked_; }
};

{
    MutexGuard g(mux);
    if (!g.ok()) { ESP_LOGW(TAG, "mutex timeout"); return ESP_ERR_TIMEOUT; }
    // 临界区自动释放
}
```

### 3.3 Counting Semaphore
- 计数资源（连接池、buffer 池）
- 生产-消费节流

### 3.4 临界区（portENTER_CRITICAL）
- **仅用于** < 10μs 的极短临界区
- 禁止在临界区内：
  - 调用阻塞 API
  - 日志
  - 分配内存
- 跨核场景用 `portMUX_TYPE`

---

## 四、Event Group（多事件同步）

### 4.1 典型场景
"等 WiFi 连接 **且** MQTT 上线 **且** OTA 空闲"：
```cpp
#define BIT_WIFI_CONN  BIT0
#define BIT_MQTT_CONN  BIT1
#define BIT_OTA_IDLE   BIT2
EventGroupHandle_t net_events = xEventGroupCreate();

// 等所有条件（AND）
EventBits_t bits = xEventGroupWaitBits(
    net_events,
    BIT_WIFI_CONN | BIT_MQTT_CONN | BIT_OTA_IDLE,
    pdFALSE,  // 不清除
    pdTRUE,   // AND
    pdMS_TO_TICKS(10000)
);

// 设置位
xEventGroupSetBits(net_events, BIT_WIFI_CONN);
// 清除位
xEventGroupClearBits(net_events, BIT_WIFI_CONN);
```

### 4.2 和 Queue 的区别
- Queue：传数据
- Event Group：传**状态**（比较 bit mask）

---

## 五、死锁排查

### 5.1 经典死锁
```
Task A: take(mux1) → take(mux2)
Task B: take(mux2) → take(mux1)
```

### 5.2 排查
- `xSemaphoreGetMutexHolder(mux)` 返回当前持有者
- 加日志记录 take/give 时间戳
- `vTaskList` 看 blocked 任务及其等待资源

### 5.3 预防
- **固定加锁顺序**：所有任务按同一顺序拿锁
- **尽量用单锁**，多锁必须文档说明顺序
- **避免 take + 调用其他函数**（可能内部再 take）

---

## 六、优先级反转

### 6.1 场景
- 高优先级 H 等 低优先级 L 持有的 mutex
- 中优先级 M 抢占 L
- H 饥饿
### 6.2 解决
- 用 **mutex**（带 priority inheritance），不要 binary semaphore
- 尽量减少低优先级任务持有 mutex 的时间

---

## 七、跨核通信

### 7.1 基本原则
- FreeRTOS queue / semaphore 都是**跨核安全**的
- `portENTER_CRITICAL` 单核版 → 用 `portENTER_CRITICAL_SAFE` 跨核版
- `std::atomic` 优于 volatile

### 7.2 IPC
- `esp_ipc_call(core, func, arg)` —  在指定 core 执行函数
- 场景：Core0 发消息让 Core1 做某事

---

## 八、Watchdog

### 8.1 Task Watchdog
- 默认 5s 超时
- 所有注册了 wdt 的任务必须 `esp_task_wdt_reset()`
- 退出监控：`esp_task_wdt_delete(NULL)`（谨慎，容易忘记再加）

### 8.2 Interrupt Watchdog
- ISR 执行时间不能超过限制
- 超时 = 硬件重启，不可恢复

---

## 九、常见反模式（立即打回）

| 反模式 | 问题 | 正确做法 |
|---|---|---|
| `vTaskDelay(portMAX_DELAY)` | 永不唤醒 | 有超时，失败后上报 |
| `while (1) { ... }` 无 delay | CPU 饥饿 | 加 `vTaskDelay(1)` 或事件驱动 |
| 队列无上限 | 内存爆炸 | `xQueueCreate(n, ...)` n ≤ 32 |
| 全局变量跨任务裸读写 | 数据竞争 | `std::atomic` 或 mutex |
| 中断里调 malloc / lock | Panic | ISR 只做最小工作，通知任务 |
| PSRAM 栈 + Core0 持续循环 | Double Exception | 内部 RAM 栈 |

---

## 十、调试技巧

### 10.1 任务列表
```cpp
char buf[2048];
vTaskList(buf);  // 需 CONFIG_FREERTOS_USE_TRACE_FACILITY
ESP_LOGI(TAG, "\nName          State Prio Stack  Num Core\n%s", buf);
```

### 10.2 运行时统计
```cpp
char buf[2048];
vTaskGetRunTimeStats(buf);  // 需 CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS
ESP_LOGI(TAG, "\nName          Ticks       %%\n%s", buf);
```

### 10.3 栈水位常态监控
```cpp
static void monitor_task(void* arg) {
    TaskHandle_t target = (TaskHandle_t)arg;
    while (1) {
        UBaseType_t wm = uxTaskGetStackHighWaterMark(target);
        if (wm < 512) ESP_LOGE(TAG, "%s stack LOW: %u", pcTaskGetName(target), wm);
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}
```

---

## 参考

- FreeRTOS 官方：https://www.freertos.org/
- ESP-IDF FreeRTOS：https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/api-reference/system/freertos.html
- 本项目 CLAUDE.md 第 3 节 RTOS 任务规则
