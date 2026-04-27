# xiaozhi-esp32 项目宪法

> 本文件是项目的最高代码规范与协作准则。所有 Claude Code / Agent / 开发者在本仓库内的一切动作，都必须遵循本文件。
> 与此文件冲突的个人偏好、历史写法、"顺手优化"一律让位于本文件。

---

## 1. 项目概览

**xiaozhi-esp32** 是一个基于 ESP32-S3 的智能硬件 AI 对话终端，当前处于 **1→量产** 阶段，非 0→1 探索。
我们已经为量产付过一次学费（P30 项目 1 万台库存呆滞），**任何改动的风险优先级永远高于"更优雅的实现"**。

### 技术栈（不要自行替换）

| 层 | 方案 |
|---|---|
| 芯片 | ESP32-S3（PSRAM 8MB，Flash 16MB） |
| RTOS | FreeRTOS（ESP-IDF 5.4+ 自带） |
| 构建 | ESP-IDF + CMake（`idf.py build`） |
| 显示 | LVGL（触屏，1.83" 圆角矩形屏 284×240 / 方屏多 SKU） |
| 网络 | WiFi + 4G（Cat.1 模组，ML307 系列）+ BLE 5.0 |
| 音频 | I2S 麦克风 + 扬声器，流式 TTS/STT |
| 协议 | MQTT + WebSocket（与后端实时双向） |
| OTA | 差分升级（基于 ESP-IDF OTA） |
| 业务 | AI 对话、闹钟、番茄钟、智能家居、直播伴侣 |

### 当前阶段的硬约束（全局生效）

1. **硬件批次差异真实存在**：同一版本固件会跑在不同批次 DAC / 触摸 IC / 电池方案上，改动必须考虑兼容性
2. **SKU 多样性**：P30-4G / P30-WiFi / P31（ES7111 DAC）/ 未来 P32，任何通用层改动必须通过所有 SKU 编译
3. **已售出设备不可抛弃**：协议、OTA、存储格式改动必须**向后兼容**，禁止"一刀切升级"
4. **团队 4-6 人，无专职固件团队**：Jack（我）做架构与 P0 决策，代码质量必须靠规范和工具链兜底

---

## 2. 代码风格（C/C++）强约束

### 2.1 基础风格
- **缩进**: 4 个空格，禁止 Tab
- **行宽**: 120 字符（长字符串/日志可放宽）
- **命名**:
  - 函数 / 变量：`snake_case`（ESP-IDF 风格）
  - 类型 / 类 / struct：`PascalCase`
  - 宏 / 常量：`UPPER_SNAKE_CASE`
  - 成员变量：`snake_case_` 结尾加下划线
  - 文件名：`snake_case.cc` / `snake_case.h`
- **头文件保护**: 统一用 `#pragma once`
- **C++ 标准**: C++17，遵循 Google C++ Style，但 ESP-IDF 规范优先

### 2.2 模块化
- **每个外设 / 独立功能** 必须是独立 component（`components/xxx/` 或 `main/xxx/` 子目录）
- 每个 component 必须有 `CMakeLists.txt`、`include/`、对外 API 头文件
- **禁止** 在 `main/` 根目录堆积工具函数，需归类到对应子目录

### 2.3 错误处理（铁律）
- **禁止裸调用 ESP-IDF API**，必须用 `ESP_ERROR_CHECK()` 或显式处理返回值
- 关键路径（网络、I2C、文件）必须有重试逻辑（至少 3 次，带指数退避）
- 错误日志必须含上下文：函数名 + 关键参数 + 错误码
- **禁止** 使用 C++ 异常（`-fno-exceptions`），用返回值或状态码

```cpp
// ❌ 禁止
i2c_master_transmit(bus, data, len, -1);

// ✅ 正确
esp_err_t ret = i2c_master_transmit(bus, data, len, 100);
if (ret != ESP_OK) {
    ESP_LOGE(TAG, "i2c write failed at %s: %d, err=%s",
             __func__, addr, esp_err_to_name(ret));
    return ret;
}
```

### 2.4 内存分配（铁律）
- **禁止裸 `malloc` / `new`**，统一用：
  - `heap_caps_malloc(size, MALLOC_CAP_SPIRAM)` — 大缓冲、图片、可容忍慢的
  - `heap_caps_malloc(size, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA)` — DMA 缓冲
  - `heap_caps_malloc(size, MALLOC_CAP_INTERNAL)` — 性能关键、栈
- 所有分配必须有明确的释放路径，谁分配谁释放
- **大于 4KB 的堆分配** 必须注释说明：用途、生命周期、释放者
- 可用内部 RAM 必须保持 > 60KB（对话期间）

### 2.5 日志
- 统一用 `ESP_LOGE / ESP_LOGW / ESP_LOGI / ESP_LOGD / ESP_LOGV`
- **TAG 命名规范**: `"MODULE_SUBMODULE"`，全大写，下划线分隔
  - 示例: `"AUDIO_CODEC"`, `"NET_MQTT"`, `"UI_POMODORO"`, `"BOARD_P31"`
- 日志级别使用原则：
  - ERROR：系统错误、资源不足、必须告警
  - WARN：可恢复异常、重试
  - INFO：重要状态切换（上线、下线、进入某模式）
  - DEBUG：仅开发阶段，发版前必须通过 Kconfig 关闭
- **禁止在中断/高频路径里打 INFO 及以上日志**（UART 带宽有限）

### 2.6 LVGL 代码
- **所有 LVGL API 调用必须在 LVGL 任务上下文**
- 跨任务改 UI 必须用 `lv_async_call(cb, user_data)` 或专用事件队列
- 禁止在中断回调里直接调用 `lv_*`
- 详见第 4 节 LVGL 规则

---

## 3. RTOS 任务规则（关键）

### 3.1 任务优先级表（固定，改动需 Jack 批准）

| 优先级 | 任务名 | 核心 | 栈大小 | 职责 |
|---|---|---|---|---|
| P12 | audio_input | Core1 | 4096 (internal) | I2S 麦克风采集 |
| P10 | audio_output | Core1 | 4096 (internal) | I2S 扬声器播放 |
| P8  | afe | Core1 | 8192 (internal) | AFE 降噪 / 唤醒词 |
| P7  | opus_codec | Core0 | 8192 (internal) | 编解码 |
| P6  | wake_word / modem | Core0 | 6144 (internal) | 唤醒词回调 / 4G |
| P5  | lvgl | Core1 | 8192 (internal) | LVGL 刷新 |
| P3  | main_loop / application | Core0 | 8192 (internal) | 业务主循环、事件处理 |
| P2  | network / websocket | Core0 | 6144 (可 PSRAM) | WS/MQTT I/O |
| P1  | 后台 / ota / timer | 任意 | 4096 (可 PSRAM) | OTA 上报、一次性任务 |

> **PSRAM 栈陷阱**：持续循环 + Core0 的任务**必须用内部 RAM 栈**。
> flash op（NVS / OTA）会禁用 cache + PSRAM，PSRAM 栈任务此时被调度 → double exception。
> 详见 `~/GitHub/.claude/CLAUDE.md` 的 PSRAM 栈章节。

### 3.2 跨任务通信（铁律）
- **禁止裸全局变量**用于任务间传递数据
- 跨任务通信必须用：
  - `QueueHandle_t` — 数据传递
  - `EventGroupHandle_t` — 状态同步
  - `SemaphoreHandle_t` — 互斥
  - `std::atomic<T>` — 简单标志位
- 所有 queue 必须有上限（典型 8~32），发送方检查 `xQueueSend` 返回值
- 所有互斥锁必须有超时（典型 100ms~500ms），禁止 `portMAX_DELAY` 裸用

### 3.3 临界区
- `portENTER_CRITICAL` / `taskENTER_CRITICAL` 持有时间必须 < 10μs
- 禁止在临界区内：日志、分配内存、调用任何会阻塞的 API
- 优先用 mutex，只有非常短的读写共享变量才用 critical

### 3.4 Watchdog
- 所有任务必须在主循环中 `esp_task_wdt_reset()` 或主动让出 CPU
- 长时间阻塞（> 5s）必须 `esp_task_wdt_delete(NULL)` 退出 wdt 监控
- 禁止关闭 task watchdog（`CONFIG_ESP_TASK_WDT_EN` 必须保持开启）

### 3.5 栈水位
- 每个任务定期用 `uxTaskGetStackHighWaterMark(NULL)` 自检
- 发现水位 < 512 字节 → ESP_LOGW，< 256 字节 → ESP_LOGE 并上报
- `/audit-task` 命令会定期跑全量检查

---

## 4. LVGL 规则

### 4.1 页面抽象（硬性要求）
- **所有新页面必须继承统一 PageBase**
  - 如果当前仓库没有 PageBase，**优先建议创建**，不要在每个页面里复制模板代码
  - PageBase 至少包含：`OnEnter()` / `OnExit()` / `OnKey()` / `OnTick()` 虚函数
- **页面切换必须走路由器** `page_router_switch(page_id)` 或等价接口
- **禁止裸调用** `lv_scr_load()` / `lv_scr_load_anim()`

### 4.2 资源生命周期（最常见 bug 源）
`OnExit()` 必须释放：
- 本页面创建的所有 `lv_obj_t`（或确保 `lv_obj_del(root)` 会级联清理）
- 本页面启动的 `lv_timer_t` → `lv_timer_del`
- 本页面启动的 `lv_anim_t` → `lv_anim_del_all` 或具体 cancel
- 本页面发起的 TTS / 音频播放 → 主动 stop
- 本页面注册的事件回调（mqtt/ws 订阅）→ 解绑

### 4.3 触屏事件回调
- **必须快速返回（< 5ms）**
- 重活（网络请求、文件 IO、长计算）扔到 task queue 或 `lv_async_call`
- 防抖统一在一处实现，不要在每个回调里重复写

### 4.4 任务上下文
- 默认假设：**UI bug 的 80% 根因是任务上下文错了**
- 不确定时，`xTaskGetCurrentTaskHandle()` 和 `lvgl_task_handle` 比较
- `lv_async_call` 是跨任务安全的唯一通路

### 4.5 圆角矩形屏适配
- 屏幕形态：1.83" 圆角矩形屏 284×240，4 角圆弧半径 25px
- 布局策略：矩形布局可用全屏，仅需避让 4 角圆弧内的小三角区
- 字体/图标：不需要 safe area / mask gradient；仅需保证内容不深入圆角弧线区
- 状态栏图标位置：距屏边 ≥ 16px 即可避开圆角弧（参考 UiDisplay::CreateGlobalStatusBar）

---

## 5. Git 提交规则

### 5.1 提交粒度
- **每个 commit 只做一件事**
- bug 修复 + 功能新增 + 重构，必须分成独立 commit
- 禁止 "一天的零碎改动" 合并为 "daily commit"

### 5.2 提交信息格式
```
[模块] 简述（中文，< 50 字）

详细说明：
- 根因：...
- 改动点：...
- 影响范围：...
- 验证方式：...

关联 bug: #xxx
SKU 影响: P30-4G / P30-WiFi / P31 / 全部
```

### 5.3 分支策略
- `main` — 稳定可发布
- `v2` / `dev-v2` — V2 产品线主力开发分支
- `release/x.y.z` — 量产分支，**任何改动必须经过 `/pre-flash-check` 放行**
- `feature/xxx` — 新功能
- `fix/bug-id` — bug 修复

### 5.4 量产分支规则
- 仅允许 P0 bug 修复直接进 release/*
- P1 / P2 必须走 `main → release` 的 PR，且至少 48 小时稳定期
- 任何改动必须 `code-reviewer-p0` agent 审查通过

---

## 6. P0 / P1 / P2 分级标准

### P0（阻断量产）
- 影响量产、导致退货、安全 / 合规相关
- 死机、重启、数据丢失、电池异常、泄漏 Wi-Fi 明文
- **必须根因分析（不接受"重启就好"）+ 至少 5 台设备验证**
- 修复流程：`/diagnose-bug` → `/fix-p0` → 多设备验证 → `/pre-flash-check`

### P1（体验缺陷，可绕过）
- 功能不全、UI 不准、偶发卡顿、不影响核心使用
- 可批量修复、OTA 推送
- 修复流程：常规 PR + `code-reviewer-p0` 审查

### P2（优化项）
- 性能优化、代码整洁、非功能改进
- 进迭代池，按优先级排期
- 禁止 P2 抢占 P0/P1 资源

---

## 7. 禁区清单（硬锁，改动需 Jack 显式批准）

以下模块在当前阶段**已验证稳定**，任何改动（哪怕"只是加个日志"）都必须经过 Jack 显式书面批准：

| 模块 | 路径 | 状态 | 最后稳定版本 |
|---|---|---|---|
| 蓝牙配网 (BluFi) | `main/blufi/`, `main/boards/common/blufi.cpp` | ✅ OK | v2.2.5 |
| WiFi 热点配网 | WiFi connection/provisioning 相关 | ✅ OK | v2.2.5 |
| 下拉切换 (UI 手势) | `main/display/ui/` 手势层 | ✅ OK | v2.2.5 |
| 基础绑定流程 | OTA bind / activate / MQTT 首次握手 | ✅ OK | v2.2.5 |
| 分区表 | `partitions/v2/` | ✅ 冻结 | — |
| 协议兼容层 | BinaryProtocol2 / 3 | ✅ 冻结 | — |

> **禁区规则**：
> 1. Agent 识别到改动触及禁区，立即停下，返回警告
> 2. 即使我（Jack）主动要求改动禁区，也要 **先复述一次"这是禁区，改动风险高"**，等我二次确认
> 3. 禁区不做"顺手优化"、不加"无害的"日志、不重构命名

---

## 8. P30 教训框（醒目）

> ⚠️ **血的教训**：P30 项目 1 万台库存呆滞，根因是上量产前的"最后一次优化"引入了一个偶发的深睡唤醒失败问题。
>
> **铁律**：
> - 宁可保守留一个已知 P1 bug 到下个 OTA，**也绝不引入新的不确定性**进量产固件
> - "这个改动看起来更好" ≠ "这个改动值得现在做"
> - 在 release/* 分支上，**保守 > 激进**，**可追溯 > 优雅**
> - 任何 agent / Claude / 开发者，在 release 分支上提出的"顺手重构"建议，直接拒绝
>
> 量产不是改代码的过程，量产是**不再改代码**的过程。

---

## 9. 与 Agent 协作的约定

- 所有 agent 定义在 `.claude/agents/`，触发场景和工作流详见各 agent 文件
- 斜杠命令定义在 `.claude/commands/`，是"一句话调度"的入口
- 技术知识包定义在 `.claude/skills/`，由 agent 自动加载
- **agent 的输出必须遵循本文件所有规则**，发现矛盾时以本文件为准
- agent 不得修改本文件、`.claude/agents/`、`.claude/commands/`、`.claude/skills/`，这些改动必须 Jack 手动操作

---

## 10. 版本元信息

- 当前版本: v2.2.5（见 `CMakeLists.txt` `PROJECT_VER`）
- IDF 版本: idf54（本仓库用，不是 idf55，注意区分 P30-V2 主线）
- 上游: `xiaozhi-esp32-189`（虾哥开源 v1.9.60 → V1 产品线主线）
- 产品线归属: **V1 产品线衍生仓库 / P31 硬件试验田**
- 最近稳定: v2.2.5（触屏升级 V2907 + edge suppression 实测）
