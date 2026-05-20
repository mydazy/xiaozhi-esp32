# 04 · 应用主流程 / 状态机 / MCP 子系统 缺陷报告

> 审计范围：main 根级 24 个文件（main / application / device_state_machine / device_state_event /
> device_state / activity_type / scene_type / mcp_server / flow_engine / edu_scene_pool /
> alarm_manager / pomodoro_manager / settings / system_info 的 .cc/.h）。
> 审计方法：逐文件精读 + 跨文件调用链分析。审计时间基线 2026-05-20。

## 子系统概述

本子系统是 P30 固件的应用层心脏，由四块构成：

1. **启动主流程**（main.cc → Application::Initialize/Run）：NVS 初始化 → 默认事件循环 →
   单例 Application 初始化（display/audio/codec/MusicPlayer/网络回调/Alarm/Pomodoro/MCP 工具）→
   进入 `Run()` 永不返回的事件位循环（`xEventGroupWaitBits`，13 个事件位）。

2. **设备状态机**（DeviceStateMachine + DeviceStateEventManager）：12 态枚举 + 白名单迁移表 +
   观察者回调。状态变更经两条路径广播：① 直接 StateCallback（同步在调用方上下文）；
   ② esp_event 默认循环异步分发给 FlowEngine 等订阅方。

3. **MCP 子系统**（McpServer + McpTool/Property）：JSON-RPC 2.0 工具注册与调用，
   含 common tools（音量/亮度/AEC/音乐/休眠/唤醒词）与 user-only tools。
   工具回调统一 `Schedule` 到主任务执行。

4. **业务管理器**：FlowEngine（直播伴侣/教学脚本，HTTP 拉脚本 + 状态机驱动的 TTS 链）、
   AlarmManager（8 槽闹钟 + 异步 NVS worker + 深睡唤醒）、PomodoroManager（esp_timer 1Hz 番茄钟）、
   EduScenePool（摇一摇启蒙场景池）、Settings（NVS RAII 封装）、SystemInfo（堆/CPU/温度监控）。

跨任务并发面较广：main 主任务（prio 10）、esp_event 默认循环任务、activation 任务、
flow_load 任务、alarm_nvs worker、各 esp_timer 回调任务。多处共享态的同步是审计重点。

---

## P0（必崩 / 砖机 / 安全）

### P0-1 OnIncomingJson 对 type/state/command 字段未做 null 与类型校验即解引用
- 判级理由：服务端或中间人下发一条 `type` 缺失或非字符串的 JSON，必然空指针解引用崩溃；
  音频通道是设备主链路、攻击面直接暴露在网络侧，属"必然崩溃 + 可被远端触发"。
- 文件：application.cc:756-766、819-823
- 代码：
```cpp
auto type = cJSON_GetObjectItem(root, "type");
if (strcmp(type->valuestring, "tts") == 0) {        // type 可能为 nullptr → 崩
    auto state = cJSON_GetObjectItem(root, "state");
    if (strcmp(state->valuestring, "start") == 0) {  // state 可能为 nullptr / 非字符串 → 崩
```
- 根因：`cJSON_GetObjectItem` 返回 nullptr（字段缺失）或返回非字符串节点（`valuestring==nullptr`）时，
  直接 `strcmp(node->valuestring, ...)` 解引用空指针。`type`、`state`、`command` 三处均无
  `cJSON_IsString()` 守卫（对比 `text`/`emotion` 处都有 `cJSON_IsString` 守卫，明显遗漏）。
- 触发条件与影响面：任意一条畸形 JSON（`{"jsonrpc":...}` 之外的协议帧 / 服务端 bug / 弱网截断 /
  恶意服务端）→ OnIncomingJson 在协议接收任务上下文崩溃 → 设备重启。可被远端反复触发形成 DoS。
- 修复建议：在 `strcmp` 前统一守卫：
```cpp
auto type = cJSON_GetObjectItem(root, "type");
if (!cJSON_IsString(type)) { ESP_LOGW(TAG,"msg missing type"); return; }
...
auto state = cJSON_GetObjectItem(root, "state");
if (!cJSON_IsString(state)) return;
```
  对 `command` 同理（虽然其 strcmp 前已判 `cJSON_IsString(command)`，但 line 819 的 `command` 获取后
  在 `if (cJSON_IsString(command))` 内安全，复核确认 command 分支 OK；重点修 type/state）。

---

## P1（高频崩溃 / 体验严重退化）

### P1-1 EduScenePool 每次"摇一摇"都同步写 NVS（call_counts blob + commit），高频 flash 磨损 + 阻塞
- 判级理由：摇一摇是儿童高频交互（产品核心玩法），每次调用 `GetRandomWithCount()` 都
  `nvs_set_blob + nvs_commit` 一次。儿童连续摇晃可在数月内逼近 NVS 扇区擦写寿命，且
  commit 是同步 flash 操作会卡住调用线程。属"高频路径 + 长期可靠性退化"。
- 文件：edu_scene_pool.cc:149-161、117-123
- 代码：
```cpp
EduPick EduScenePool::GetRandomWithCount() {
    ...
    if (call_counts_[idx] < UINT16_MAX) call_counts_[idx]++;
    SaveCountsToNvs();        // 每次摇一摇都 nvs_set_blob + nvs_commit
    return EduPick{names_[idx], call_counts_[idx]};
}
```
- 根因：计数仅用于"云端按编号轮换内容"的去重提示，并非强一致需求，却做了每次同步持久化。
  无节流（防抖 / 批量 / 退出时落盘）。同步 commit 在儿童快速连摇时叠加阻塞。
- 触发条件与影响面：儿童反复摇一摇 → 每秒可能数次 NVS commit → flash 磨损加速 + 触发线程卡顿。
  与 AlarmManager 已采用的"异步队列 + 合并 commit"模式形成鲜明对比，说明此处是遗漏。
- 修复建议：① 计数累计在内存，定时（如 60s）或达到阈值（如每 +10 次）才落盘一次；
  ② 或参考 AlarmManager 走异步 worker；③ 至少加"距上次保存 < N 秒则跳过"的节流。

### P1-2 SwitchProtocol 在调用方上下文内串行执行 CheckNewVersion，可阻塞分钟级
- 判级理由：`SwitchProtocol()` 内联调用 `CheckNewVersion()`，后者是带指数退避、最多 10 次重试
  （10→20→40…秒）的阻塞循环，弱网下可阻塞数分钟。若该调用发生在主任务/事件任务，期间
  整个事件循环停摆（UI 冻结、闹钟检查停、看门狗风险），属"体验严重退化"。
- 文件：application.cc:1334-1356、573-646
- 代码：
```cpp
bool Application::SwitchProtocol() {
    ...
    ota_ = std::make_unique<Ota>();
    CheckNewVersion();   // 内含 MAX_RETRY=10、retry_delay 指数翻倍的阻塞 while 循环
    InitializeProtocol();
```
- 根因：复用了为"激活后台任务"设计的阻塞式 `CheckNewVersion`，但 SwitchProtocol（remote_cmd "reload"
  触发）的执行上下文未隔离到独立后台任务。`SwitchProtocol` 内还有 `vTaskDelay(500)+vTaskDelay(200)`。
- 触发条件与影响面：远程 reload 指令 + 弱网/服务器不可达 → 调用线程被阻塞最长数分钟。
  若是 main 主任务则全系统假死。[待确认] 需确认 remote_cmd 调用 SwitchProtocol 的具体任务上下文。
- 修复建议：将 SwitchProtocol 的重活（CheckNewVersion + InitializeProtocol）移入专用后台任务执行，
  并给 CheckNewVersion 增加"总时限"或"reload 模式只取一次配置不重试"的快速路径。

### P1-3 状态机 TransitionTo 非原子（check-then-act），多任务并发迁移有竞争
- 判级理由：`current_state_` 虽是 atomic，但 `TransitionTo` 是"load → 校验 → store → 通知"的
  复合操作且不持锁。状态迁移由多个任务发起（main 主任务的各 Handle*、FlowEngine 在 esp_event
  任务内 `app_->SetDeviceState`、OnAudioChannelClosed 的 Schedule 等），并发下可产生非法迁移落地
  或丢通知/双通知，导致 UI 与音频管线状态错乱。结合 P30 对话主链路，归 P1。
- 文件：device_state_machine.cc:112-135
- 代码：
```cpp
bool DeviceStateMachine::TransitionTo(DeviceState new_state) {
    DeviceState old_state = current_state_.load();   // 读
    if (old_state == new_state) return true;
    if (!IsValidTransition(old_state, new_state)) { ... return false; } // 校验
    current_state_.store(new_state);                 // 写（与读之间无锁，可被打断）
    NotifyStateChange(old_state, new_state);
```
- 根因：迁移合法性依赖"读到的 old_state"，但读和写之间无互斥。两个任务同时读到同一 old_state，
  各自校验通过后先后 store，第二个 store 的 from→to 实际已非法（基于已被改写的状态）。
  `mutex_` 只保护 listeners_，未保护状态迁移本身。
- 触发条件与影响面：对话切换瞬间（如 Speaking→Idle 与弱网重连 Listening 并发）、
  FlowEngine 在事件任务推 Idle 与主任务推 Listening 撞车 → 状态/通知错乱、偶发 UI 卡在错误界面或
  音频处理器开关错位。
- 修复建议：用 `mutex_`（或新增专用锁）包裹"load→校验→store→Post 通知数据准备"全过程；
  或改为 `compare_exchange` 配合迁移表校验循环，确保 check-act 原子。

---

## P2（偶发 / 边缘场景）

### P2-1 listening_mode_ 为非原子普通成员，跨任务读写存在数据竞争
- 判级理由：`listening_mode_`（application.h:163）被多任务读写：主任务 SetListeningMode/各 Handle*、
  FlowEngine 经 `ForceListeningMode()` 在 esp_event 任务内直接写、OnIncomingJson 的 schedule 读。
  违反项目规范"多核共享变量用 std::atomic"。后果是偶发读到撕裂/过期值，影响监听模式分支。
- 文件：application.h:163；application.cc:1324-1327、126(ForceListeningMode 内联);
  flow_engine.cc:124/169/242/268(经 ForceListeningMode)
- 代码：
```cpp
ListeningMode listening_mode_ = kListeningModeAutoStop;   // 非 atomic
void ForceListeningMode(ListeningMode mode) { listening_mode_ = mode; } // 可被任意任务调用
```
- 根因：ListeningMode 是枚举，赋值看似原子但标准不保证，且 FlowEngine 的 `ForceListeningMode`
  从 esp_event 任务上下文写入，与主任务读写并发。
- 触发条件与影响面：FlowEngine 暂停/恢复与主任务进入 Listening 撞车 → 偶发监听模式取错值
  （AutoStop vs ManualStop），表现为对话该自动停却没停或反之。
- 修复建议：改为 `std::atomic<ListeningMode>`；或统一所有写入都经 Schedule 投到主任务串行化。

### P2-2 FlowEngine 在 esp_event 回调任务内调用 SetDeviceState，叠加 P1-3 竞争
- 判级理由：`OnDeviceStateChanged` 在 DeviceStateEventManager 的 esp_event 默认循环任务上下文执行，
  其中直接 `app_->SetDeviceState(kDeviceStateIdle)`（flow_engine.cc:175、437、541-543）。这等于在
  "状态变更通知"过程中再触发新的状态迁移，跨任务且可重入。偶发场景下与主任务迁移竞争。
- 文件：flow_engine.cc:171-176、421-443(PlayCurrentItem 内 SetDeviceState)、538-543
- 代码：
```cpp
void FlowEngine::OnDeviceStateChanged(DeviceState prev, DeviceState curr) {
    ...
    if (curr == kDeviceStateListening) {
        app_->Schedule([this]() { if (IsRunning()) app_->SetDeviceState(kDeviceStateIdle); });
```
（部分路径经 Schedule 已规避，但 Stop()/PlayCurrentItem() 内的 SetDeviceState 直接在事件任务执行）
- 根因：状态变更广播链 → FlowEngine 回调 → 再发状态迁移，路径上有的经 Schedule 串行化、
  有的直接执行，不一致。
- 触发条件与影响面：脚本播放结束 / 停止 与用户交互并发 → 偶发状态错乱（与 P1-3 同源）。
- 修复建议：FlowEngine 内所有 `SetDeviceState` 统一经 `app_->Schedule` 投回主任务，确保单线程串行迁移。

### P2-3 MCP common tools 的 lambda 以引用捕获局部引用变量 board（生命周期可疑）
- 判级理由：`auto& board = Board::GetInstance();` 是 AddCommonTools 的栈上引用变量，多个工具
  lambda 以 `[&board]` 捕获之并长期存入 `tools_`。AddCommonTools 返回后该局部引用离开作用域，
  lambda 持有的是悬垂引用。实践中因 Board 是静态单例、编译器多半把引用物化为单例地址而"碰巧能跑"，
  但属标准 UB，编译器/优化变化下可崩。判 P2（潜在 + 实践多数不复现）。
- 文件：mcp_server.cc:54、62/70/78/140/157/166 等多处 `[&board]`
- 代码：
```cpp
auto& board = Board::GetInstance();           // 局部引用
AddTool("self.get_device_status", ...,
    [&board](const PropertyList&) -> ReturnValue {  // 捕获局部引用 → AddCommonTools 返回后悬垂
        return board.GetDeviceStatusJson();
    });
```
- 根因：应捕获 Board 单例本身而非局部引用别名。对比 `[backlight]`/`[display]`（按值捕获指针）是安全的。
- 触发条件与影响面：编译器/优化级别变化后可能崩溃；当前看似稳定但是定时炸弹。
- 修复建议：lambda 内改为 `auto& board = Board::GetInstance();`（每次调用现取），或捕获
  `Board::GetInstance()` 的地址/值，移除对局部 `board` 引用变量的引用捕获。

### P2-4 AlarmManager 重复闹钟仅在一分钟窗口内逐秒匹配，跨分钟全程错过则漏触发
- 判级理由：闹钟匹配 `hour==now && minute==now`，仅在目标分钟的 60 个 tick 内有效，靠 60s dedup
  防重。若设备在该整分钟内全程无法跑 CLOCK_TICK（重 OTA/长阻塞/深睡误差），该次闹钟被永久错过。
  属边缘但对"起床闹钟"是体验事故。
- 文件：alarm_manager.cc:210-277、241-247
- 代码：
```cpp
if (alarms_[i].hour != hour || alarms_[i].minute != minute) continue;  // 仅当前分钟匹配
```
- 根因：用"等于当前分钟"作触发条件而非"已越过预定时刻且未触发过"。无"错过补触发"窗口。
- 触发条件与影响面：闹钟分钟恰逢设备长阻塞（如 SwitchProtocol 阻塞见 P1-2、资产下载、深睡漂移
  落在该分钟之后）→ 闹钟静默丢失。
- 修复建议：改为记录"上次成功触发的 epoch 分钟"，匹配条件改为"当前 epoch 时刻 ≥ 预定时刻
  且预定时刻所在分钟 > 上次触发分钟"，给出 ±N 分钟补触发窗口。

### P2-5 McpServer::AddCommonTools 期间 tools_ 被清空再回填，与 tools/list / tools/call 有窗口竞争
- 判级理由：AddCommonTools 先 `tools_ = std::move(...)` 清空，逐个 AddTool（各自独立加锁），
  最后再 insert 回原列表。期间若有 MCP 请求到达，`GetToolsList`/`DoToolCall` 会持锁看到不完整列表。
  初始化阶段网络协议尚未 Start，实际窗口很小，故 P2。
- 文件：mcp_server.cc:49-53、283-287；DoToolCall 555-561、GetToolsList 489-524
- 根因：非原子的"清空-逐项添加-回填"序列在并发可见。注释自述"启动期单线程跑"假设成立则无害，
  但 `tools_mutex_` 的粒度无法覆盖整个重排过程。
- 触发条件与影响面：仅当 MCP 请求与 AddCommonTools 并发（例如初始化重构后协议提前 Start）→
  工具列表/调用临时缺项，返回 Unknown tool。
- 修复建议：整个 AddCommonTools 重排过程持单把锁；或改为在已有 tools_ 头部直接 insert，
  避免 move-out 的中间空窗。

### P2-6 delayed_wake_timer_ 一次性定时器内的 pending_wake_text_ 跨任务读写无同步
- 判级理由：`ScheduleDelayedWake` 在调用方写 `pending_wake_text_`（std::string），定时器回调
  （esp_timer 任务）读它并投 Schedule。两次连续 ScheduleDelayedWake 与回调触发并发 → string 读写
  竞争，可能崩溃或读到半串。低频，P2。
- 文件：application.cc:1303-1322、171(成员)
- 代码：
```cpp
pending_wake_text_ = wake_text;            // 调用方任务写
esp_timer_start_once(delayed_wake_timer_, delay_us);
// 回调（定时器任务）：app->WakeWordInvoke(app->pending_wake_text_);  读
```
- 根因：esp_timer 先 stop 再改 text 再 start，但 stop 不保证回调未在途；多次调用间无锁保护 string。
- 修复建议：pending_wake_text_ 写入与定时器启停纳入互斥；或回调里 copy 后立即用，
  并保证 stop→改值→start 在持锁区内。

---

## P3（潜在远期风险）

### P3-1 main_tasks_ 调度队列（std::deque）无大小上限，违反防 OOM 红线
- 判级理由：项目规范明确"所有 deque/vector/队列必须有 size 上限检查（防 OOM）"。
  `main_tasks_` 无上限。正常每轮 Run 会清空，但若某 task 阻塞主循环、同时其他任务高频 Schedule，
  可无限堆积耗尽内部 RAM。远期/异常路径风险。
- 文件：application.h:158；application.cc:1227-1233、398-405
- 代码：
```cpp
std::deque<std::function<void()>> main_tasks_;   // 无上限
void Application::Schedule(...) { main_tasks_.push_back(std::move(callback)); ... }
```
- 修复建议：加上限（如 64/128），超限丢弃最旧或拒绝并 LOGE，对齐 ESP32 内存红线清单。

### P3-2 Settings 写接口用 ESP_ERROR_CHECK，NVS 满/损坏时直接 abort 重启
- 判级理由：`SetString/SetInt/SetBool` 与析构 commit 均 `ESP_ERROR_CHECK`，NVS 分区满或损坏时
  会 abort → 重启。配合 P1-1 高频写场景，远期可演变为"NVS 满即砖循环"。
- 文件：settings.cc:14、33、42、63、84
- 代码：
```cpp
void Settings::SetString(...) { ESP_ERROR_CHECK(nvs_set_str(...)); dirty_=true; }
~Settings() { if (read_write_ && dirty_) ESP_ERROR_CHECK(nvs_commit(nvs_handle_)); }
```
- 修复建议：写接口改为返回 esp_err_t 并上层降级处理（LOGW + 跳过），不要在数据写失败时 abort 整机。

### P3-3 main 主任务被设为优先级 10，与项目优先级表 P3=main_loop 冲突
- 判级理由：`Run()` 起手 `vTaskPrioritySet(nullptr, 10)`，与规范"P3=main_loop、P10=audio_output"
  矛盾。主循环抢占音频输出优先级，长期可能影响实时音频。属架构纪律偏差，需确认是否有意。
- 文件：application.cc:319
- 修复建议：[待确认] 与音频实时性团队确认主循环目标优先级；若规范为准应降到 P3。

### P3-4 device_state_event 与 main.cc 各自 esp_event_loop_create_default，初始化顺序耦合
- 判级理由：main.cc:26 与 DeviceStateEventManager 构造（device_state_event.cc:59）都创建默认事件循环，
  均靠 `ESP_ERR_INVALID_STATE` 容错。可工作但属隐式顺序依赖，违反规范"复用已有事件循环不另起"。
- 文件：main.cc:26-29；device_state_event.cc:58-62
- 修复建议：统一在 main.cc 创建一次，DeviceStateEventManager 仅 register handler 不再尝试 create。

### P3-5 McpTool::Call 中 cJSON* 返回值路径会 cJSON_Delete 调用方传入的对象，所有权约定隐晦
- 判级理由：当工具返回 `cJSON*` 时，`Call()` 内部 `cJSON_Delete(json)`（mcp_server.h:295-300），
  即工具回调让出所有权。当前工具（screen.get_info）符合，但约定未在 ReturnValue 文档化，
  未来新工具若返回不打算转移所有权的 cJSON* 会双重释放。远期维护风险。
- 文件：mcp_server.h:295-301
- 修复建议：在 ReturnValue/AddTool 注释明确"返回 cJSON* 即转移所有权，由框架删除"；
  或改用 unique_ptr 语义。

### P3-6 ActivationTask 后台任务对 ota_ 的访问与主任务 HandleActivationDoneEvent 存在所有权切换窗口
- 判级理由：activation 任务内创建并使用 `ota_`，完成后置 `MAIN_EVENT_ACTIVATION_DONE`，
  主任务 HandleActivationDoneEvent 读 `ota_->HasServerTime()` 后 `ota_.reset()`。两任务对同一
  unique_ptr 的读/重置依赖事件位先后顺序而非锁。正常路径有序，但唤醒词在激活中打断
  （HandleWakeWordDetectedEvent → SetDeviceState Idle）等异常路径下时序假设可能被破坏。
- 文件：application.cc:430-441、461-491、493-513
- 修复建议：明确 ota_ 的单一所有者任务，或对 ota_ 访问加锁/原子指针，避免跨任务裸用 unique_ptr。

### P3-7 FlowEngine LoadScript 失败/为空时未清 loading_，依赖 LoadScriptTask 尾部兜底
- 判级理由：`LoadScript` 多个失败 return（脚本大小异常 line 321-327、解析失败等）直接返回 false，
  `loading_` 的清理依赖 `LoadScriptTask` 末尾 `else { lc->loading_ = false; }`（line 278-280）。
  路径成立但耦合脆弱：若未来有人在 LoadScript 内提前 return 且改动 Task 结构，易残留 loading_=true
  导致后续 Start 永久被拒。
- 文件：flow_engine.cc:286-337、255-284
- 修复建议：在 LoadScript 的每个失败出口或用 RAII guard 统一清 loading_，不依赖远端调用者兜底。

---

## 统计

| 等级 | 数量 | 条目 |
|------|------|------|
| P0   | 1    | P0-1 |
| P1   | 3    | P1-1 / P1-2 / P1-3 |
| P2   | 6    | P2-1 ~ P2-6 |
| P3   | 7    | P3-1 ~ P3-7 |
| **合计** | **17** | |

> 标注 [待确认] 项：P1-2（SwitchProtocol 调用任务上下文）、P3-3（主循环目标优先级）。
> 行号基于审计时仓库快照，修复前请二次核对。
