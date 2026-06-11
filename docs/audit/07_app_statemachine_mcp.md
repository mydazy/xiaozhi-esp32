# 07 应用主流程 / 状态机 / MCP / 业务管理器 子系统审计

> 审计范围：`main/application.*`、`main/main.cc`、`main/device_state*.{h,cc}`、`main/mcp_server.*`、
> `main/flow_engine.*`、`main/edu_scene_pool.*`、`main/alarm_manager.*`、`main/pomodoro_manager.*`、
> `main/settings.*`、`main/system_info.*`、`main/{activity_type,scene_type,audio_source}.h`
>
> 文件数：**25 个**（application.cc 1596 行为最大单文件）。共 5806 行。
> 关注点：状态机死锁/漏迁移、MCP 工具入参校验、全局单例并发、错误返回未检、定时器/管理器资源泄漏。
> 方法：三遍独立遍历（广度 → 红线深挖 → 反审自检），每遍只记本遍新增。

---

## 第一遍 · 广度遍历（显性缺陷）

逐文件精读，抓空指针 / 越界 / 资源泄漏 / 错误返回未检 / 超时缺失 / 阻塞看门狗。

---

### 07-P0-A  protocol_ 跨线程裸指针读写 + reset 无锁，弱网/reload 期 use-after-free
- **等级 P0**：判级理由——`protocol_` 是裸 `std::unique_ptr`（无锁、无 atomic），被 **3 个不同任务**并发读写：主循环、协议 RX 回调任务、reload 后台任务。reload 在后台任务里 `protocol_.reset()`（application.cc:1374），同一时刻主循环 `MAIN_EVENT_SEND_AUDIO` 正解引用 `protocol_->SendAudio`（:374）。reset 把对象析构后主循环拿到悬空指针 → 必崩。量产中"切平台 reload"与"正在说话"并发不罕见。
- **文件**：`main/application.cc:374`、`:1374`、`:453`、`:1586-1594`(ResetProtocol)
- **问题代码**：
```cpp
// 主循环（任务A）
if (protocol_ && !protocol_->SendAudio(std::move(packet))) {  // :374
// reload 后台任务（任务B）同时：
self->protocol_.reset();                                       // :1374
```
- **根因**：`protocol_` 既无 `std::mutex` 保护也非 `atomic`，`SwitchProtocol`/`ResetProtocol`/`Reboot` 在非主任务里 reset，而主循环、`HandleNetworkDisconnectedEvent`(:453 裸 `protocol_->CloseAudioChannel()` 不判空) 在另一任务读。reload 仅有 `vTaskDelay(200)` "等排空"（:1375）作概率性缓解，不是同步。
- **触发/影响**：reload 或 ResetProtocol 与音频发送/网络断开回调并发 → 悬空指针解引用 → 崩溃/砖机重启。
- **修复**：把所有 `protocol_` 访问收敛到主循环单线程（reset 也走 `Schedule`，如 ResetProtocol 已做），或给 `protocol_` 加 `std::shared_ptr` + 在每个回调里 `auto p = std::atomic_load(&protocol_); if(!p) return; p->...` 本地持有引用。最小修：`HandleNetworkDisconnectedEvent` 的 `protocol_->CloseAudioChannel()` 先判 `if (protocol_)`；reload 的 reset 改 `Schedule` 到主循环执行。
- **[发现于第一遍]**

### 07-P1-A  HandleNetworkDisconnectedEvent 裸解引用 protocol_ 空指针崩溃
- **等级 P1**：判级理由——`protocol_->CloseAudioChannel()`（:453）在 `protocol_` 可能为 null 时被调用。激活完成前网络抖动（Connecting/Listening/Speaking 态在 protocol 构建前不可能，但 reload 期 `protocol_.reset()` 后、新 protocol 建好前若收到断网事件，state 仍是旧的非 Idle）→ 空指针解引用。高频弱网场景。
- **文件**：`main/application.cc:448-454`
- **问题代码**：
```cpp
if (state == kDeviceStateConnecting || state == kDeviceStateListening || state == kDeviceStateSpeaking) {
    ESP_LOGI(TAG, "Closing audio channel due to network disconnection");
    protocol_->CloseAudioChannel();   // :453 未判 protocol_ 是否为 null
}
```
- **根因**：本函数假定进入这些状态时 protocol_ 一定存在，但 reload 流程会先 reset 再重建，窗口内 protocol_==nullptr 且 state 未必回 Idle。
- **触发/影响**：reload 期间断网回调 → 崩溃。
- **修复**：`:453` 改为 `if (protocol_) protocol_->CloseAudioChannel();`
- **[发现于第一遍]**

### 07-P1-B  GetToolsList 对空 tools_ / 全 user-only 列表 json.back() 越界
- **等级 P1**：判级理由——`json` 初值 `"{\"tools\":[" `。若 `tools_` 为空、或全部被 cursor/user_only 过滤掉一个都没加，循环结束后 `json.back()` 是 `'['`。第一处 `if (json.back() == ',')`（:525）安全；但随后 `if (json.back() == '[' && !tools_.empty())`（:529）——当 `tools_` 真为空时该分支不进，落到 `:537 json += "]}"` 拼出 `{"tools":[]}` 反而正确。真正风险：`std::string::back()` 对**空串**是 UB——这里 json 永远非空所以 back() 安全。复核后降级：不是越界，但 `:529` 逻辑在"非空 tools_ 但全部 user_only 过滤"时会误报 payload size 错误（实际是过滤导致空）。
- **文件**：`main/mcp_server.cc:525-534`
- **问题代码**：
```cpp
if (json.back() == '[' && !tools_.empty()) {
    ESP_LOGE(TAG, "tools/list: Failed to add tool %s because of payload size limit", next_cursor.c_str());
    ReplyError(id, "Failed to add tool " + next_cursor + " ...");  // next_cursor 为空字符串
    return;
}
```
- **根因**：`json.back()=='['` 被当作"payload 超限"，但同样发生于"所有工具都被 user_only 过滤"。此时 `next_cursor` 是空串，错误信息无意义，且本该返回空 tools 列表。
- **触发/影响**：AI 端（非 withUserTools）请求 tools/list 而当前只注册了 user-only 工具时，返回伪造的 size-limit 错误而非空列表。体验退化非崩溃。
- **修复**：`:529` 改为先判 `next_cursor` 是否非空：`if (!next_cursor.empty()) { ...ReplyError... return; }`，去掉 `json.back()=='['` 判据。
- **[发现于第一遍]**

### 07-P2-A  delayed_wake_timer_ 仅创建不删除，析构泄漏 esp_timer
- **等级 P2**：判级理由——`ScheduleDelayedWake`（:1318）懒创建 `delayed_wake_timer_`，但 `Application::~Application`（:139）只删 `clock_timer_handle_`，不删 `delayed_wake_timer_`。Application 是单例进程生命周期，析构基本不发生，故 P2 而非 P1。
- **文件**：`main/application.cc:139-145`、`:1318-1337`
- **根因**：析构清理不全。`pending_wake_text_` 也在 timer 回调里被读，timer 未停时若对象先析构有竞态（同样因单例不触发）。
- **触发/影响**：理论泄漏，量产单例不触发，记录待办。
- **修复**：`~Application` 增加 `if (delayed_wake_timer_) { esp_timer_stop(delayed_wake_timer_); esp_timer_delete(delayed_wake_timer_); }`
- **[发现于第一遍]**

### 07-P2-B  esp_timer_create / esp_timer_start 返回值未检（多处）
- **等级 P2**：判级理由——`Application` 构造 `esp_timer_create(&clock_timer_args, &clock_timer_handle_)`（:136）未检返回，若失败 `clock_timer_handle_` 为 null，后续 `esp_timer_start_periodic`（:215）对 null 句柄行为未定义；`ScheduleDelayedWake` 的 `esp_timer_create`（:1333）、`FlowEngine` 构造的 `esp_timer_create`（flow_engine.cc:31）同样未检。Pomodoro（pomodoro_manager.cc:22）有检并置 null，是正面样本。
- **文件**：`main/application.cc:136`、`:215`、`:1333`；`main/flow_engine.cc:31`、`:491`、`:507`
- **根因**：esp_timer 创建在 boot OOM 时可能失败，未检 → 后续对 null 句柄操作。
- **触发/影响**：极端内存不足时 boot 路径崩溃，量产偶发。
- **修复**：`clock_timer_handle_` create 后判 `!= ESP_OK` 记 E 日志；`esp_timer_start_periodic`（:215）调用前判 `if (clock_timer_handle_)`。
- **[发现于第一遍]**

### 07-P2-C  ActivationTask 任务句柄竞态：handle 清零在 vTaskDelete 之前的窗口
- **等级 P2**：判级理由——`HandleNetworkConnectedEvent` 用 `activation_task_handle_ != nullptr` 防重入（:430），任务体内 `app->activation_task_handle_ = nullptr; vTaskDelete(NULL);`（:438-439）。`activation_task_handle_` 是裸 `TaskHandle_t`（header:190），主任务读、激活任务写，无内存屏障/atomic。两次快速 Connected 事件可能都看到 null 而创建两个激活任务。
- **文件**：`main/application.cc:430-440`、`application.h:190`
- **根因**：跨任务共享句柄无 atomic，check-then-act 非原子。
- **触发/影响**：网络快速重连 → 并发两个 ActivationTask → 两个 OTA 对象 / 重复 CheckNewVersion。偶发，多见弱网。
- **修复**：`activation_task_handle_` 改 `std::atomic<TaskHandle_t>`，并用 CAS 占位；或在主任务里先置一个 `std::atomic<bool> activating_` 标志再 create。
- **[发现于第一遍]**

### 07-P3-A  CheckNewVersion 指数退避无上限封顶，retry_delay 溢出
- **等级 P3**：判级理由——`retry_delay *= 2`（:604）每次翻倍，MAX_RETRY=10 → 10/20/40/.../5120 秒。虽 `retry_count>=10` 会退出（:586），10 次内最大 5120s 单次等待（含可被 Idle 打断 break），但 `for (i<retry_delay)` 内逐秒 vTaskDelay，最长一次约 85 分钟阻塞激活任务（非主循环，不触发看门狗）。远期体验问题。
- **文件**：`main/application.cc:598-604`
- **根因**：退避无 cap。
- **修复**：`retry_delay = std::min(retry_delay * 2, 300);`
- **[发现于第一遍]**

### 07-P3-B  Settings 析构 nvs_commit 用 ESP_ERROR_CHECK，commit 失败直接 abort
- **等级 P3**：判级理由——`Settings::~Settings`（settings.cc:15）`ESP_ERROR_CHECK(nvs_commit(...))`。NVS 满 / flash 故障时 commit 返回非 OK → ESP_ERROR_CHECK 触发 abort 重启。析构里 abort 是危险模式（量产 flash 老化后偶发砖机式重启循环）。SetString/SetInt 同样用 ESP_ERROR_CHECK（:42、:63）。
- **文件**：`main/settings.cc:15`、`:33`、`:42`、`:63`、`:84`
- **根因**：NVS 写路径全程 ESP_ERROR_CHECK，无降级。
- **触发/影响**：flash 写满 / 坏块时设置保存触发 abort 重启。远期。
- **修复**：析构 commit 改为记 E 日志不 abort；写操作返回 esp_err_t 让调用方决定。
- **[发现于第一遍]**

---

## 第二遍 · 红线深挖（四条硬红线 + 状态机迁移图 + MCP 入参逐个查）

### 状态机迁移图（device_state_machine.cc 复核）

```
Unknown ──> Starting ──> {WifiConfiguring, Activating}
WifiConfiguring ──> {Activating, AudioTesting}
AudioTesting ──> WifiConfiguring
Activating ──> {Upgrading, Idle, WifiConfiguring}
Upgrading ──> {Idle, Activating}
Idle ──> {Connecting, Listening, Speaking, Activating, Upgrading, WifiConfiguring}
Connecting ──> {Idle, Listening}
Listening ──> {Speaking, Idle}
Speaking ──> {Listening, Idle}
任意态 ──> FatalError（IsValidTransition 顶部放行）
FatalError ──> （不可出，return false）
```
迁移表本身**无死锁**（FatalError 为吸收态属预期），同态 no-op 放行合理。但发现下列跨文件竞争与漏迁移问题：

### 07-P0-B  Connecting 态卡死：OpenAudioChannel 失败后无回退迁移
- **等级 P0**：判级理由——`ToggleChatState`/`StartListening`/`WakeWordInvoke` 在 Idle 时先 `SetDeviceState(kDeviceStateConnecting)`，再 `Schedule(ContinueOpenAudioChannel)`。`ContinueOpenAudioChannel`（:992）若 `OpenAudioChannel()` 返回 false，**直接 return，状态停留在 Connecting**（:998-1001），不回 Idle。`ContinueWakeWordInvoke`（:1116）同样：open 失败仅 `EnableWakeWordDetection(true)` 后 return，留在 Connecting。Connecting 态只能去 Idle/Listening——卡 Connecting 后 UI 永远"连接中"，且 `CanEnterSleepMode` 返回 false（非 Idle），设备无法休眠、无法再次唤醒（HandleWakeWord 在 Connecting 态无分支处理）→ 功能性砖机直到重启。量产弱网首次连接失败必现。
- **文件**：`main/application.cc:992-1005`、`:1116-1127`
- **问题代码**：
```cpp
void Application::ContinueOpenAudioChannel(ListeningMode mode) {
    if (GetDeviceState() != kDeviceStateConnecting) return;
    if (!protocol_->IsAudioChannelOpened()) {
        if (!protocol_->OpenAudioChannel()) {
            return;   // ← 卡在 Connecting，无 SetDeviceState(kDeviceStateIdle)
        }
    }
    SetListeningMode(mode);
}
```
- **根因**：打开音频通道失败的错误返回未驱动状态回退。`OnNetworkError` 走的是 MAIN_EVENT_ERROR → SetIdle，但 `OpenAudioChannel()` 同步返回 false（非网络错误回调，如服务器拒连/超时）不触发该路径。
- **触发/影响**：弱网/服务端不可达时点对话或唤醒 → 永久停在 Connecting，设备假死。
- **修复**：两处 `if (!protocol_->OpenAudioChannel())` 分支内 `return` 前加 `SetDeviceState(kDeviceStateIdle);`（ContinueWakeWordInvoke 还需保留 EnableWakeWordDetection(true)）。
- **[发现于第二遍]**

### 07-P1-C  listening_mode_ / aec_mode_ 跨任务非原子读写（数据竞争）
- **等级 P1**：判级理由——`listening_mode_`（header:164，普通 ListeningMode）被：① 主任务写（SetListeningMode :1340、HandleStateChangedEvent 读 :1192/:1222）② FlowEngine 任务写（`ForceListeningMode` header:127，FlowEngine::Stop/Start/Resume 调用）③ 协议 RX 任务读（OnIncomingJson tts/stop :784 读 `listening_mode_`）。无 atomic/锁。`aec_mode_` 类似：MCP 任务经 SetAecMode 改、GetDefaultListeningMode 主任务读。撕裂读会导致监听模式错乱（AutoStop 当成 ManualStop），高频体验退化。
- **文件**：`main/application.h:127,164,165`、`application.cc:784,1192,1222,1340,1345,1531`、`flow_engine.cc:124,169,206,268,436`
- **根因**：`ForceListeningMode` 设计为可从任意任务调，但成员非 atomic（违反 esp32-memory 规范"多核共享变量用 std::atomic"）。
- **触发/影响**：FlowEngine 切模式与协议回调读模式并发 → 监听模式判断错误，对话不自动结束或过早结束。
- **修复**：`listening_mode_` 改 `std::atomic<ListeningMode>`，`aec_mode_` 改 `std::atomic<AecMode>`；所有读写换 load/store。
- **[发现于第二遍]**

### 07-P1-D  reload 后台任务内 InitializeProtocol 重设回调，与主循环读 protocol_ 竞争（红线③并发）
- **等级 P1**：判级理由——`SwitchProtocol` 的后台任务（:1368-1382）`reload` 在 Core 0、优先级 5：reset 旧 protocol、`make_unique<Ota>`、`CheckNewVersion`（可阻塞分钟级）、`InitializeProtocol()`（重建 protocol_ 并注册一堆 lambda 回调）。这些写 `protocol_`/`ota_` 与主循环、RX 回调读同一指针并发（见 07-P0-A 根因）。`reload_running` CAS 只防两个 reload 互斥，不防 reload-vs-mainloop。
- **文件**：`main/application.cc:1368-1382`、`:648-872`(InitializeProtocol)
- **根因**：重活在后台任务做但操作的是主任务也在用的共享对象。
- **触发/影响**：reload 进行中正常对话/发音频 → 竞争 protocol_。
- **修复**：与 07-P0-A 合并修——protocol_ 访问统一收敛主循环，或 reload 全程先 `Schedule` 一个"暂停音频处理"屏障再操作。
- **[发现于第二遍]**

### 07-P2-D  MCP self.audio.set_aec / set_wakeword 入参 mode 无白名单校验（红线②入参）
- **等级 P2**：判级理由——`self.audio.set_aec`（mcp_server.cc:74）`mode` 任意字符串，逻辑 `(mode=="device")?kAecOnDeviceSide:kAecOff`——非 "device" 一律当 off，语义吞错但不崩。`self.audio.set_wakeword`（:107）`mode`/`text` 完全不校验直接写 NVS（`s.SetString("mode", mode)` :117），畸形 mode（如超长字符串）写入 NVS 后下次开机按它配置唤醒词，可能导致唤醒功能失效。text 未校验是否在 MultiNet 词表内（注释自己说"须在词表里"但代码不验）。
- **文件**：`main/mcp_server.cc:74-83`、`:107-123`
- **根因**：字符串型入参无枚举白名单 / 长度上限校验。MCP 入参校验只在 `DoToolCall` 做了"类型匹配 + 整数 range"（:575-591），字符串既无长度上限也无内容白名单。
- **触发/影响**：恶意/错误 MCP 调用写入畸形唤醒词配置 → 重启后唤醒失效（功能退化，非崩溃，因 RebuildIndex 类校验不在此路径）。
- **修复**：set_wakeword 回调内校验 `mode` ∈ {"afe","custom"} 否则返回 error；`text` 加长度上限（如 ≤32 字节）。set_aec 同理校验 mode ∈ {"off","device"} 否则返回 error 而非静默 off。
- **[发现于第二遍]**

### 07-P2-E  self.music.play URL 仅校验前缀，未限长度，可超长 std::string 入 NVS/解码器
- **等级 P2**：判级理由——`self.music.play`（mcp_server.cc:180）`url` 仅校验非空 + http(s) 前缀（:193-198），不限长度。MCP arguments 来自服务器/AI，超长 URL（如几 KB）move 进 MusicPlayer::Play。虽不直接崩主流程（Schedule 到主任务），但作为外部输入未设上限，违反红线②"先校验长度边界再用"。
- **文件**：`main/mcp_server.cc:188-198`
- **根因**：外部输入字符串无长度上限。
- **触发/影响**：超长 URL 占内存 / 传给 HTTP 客户端边界行为依赖下游。偶发。
- **修复**：`if (url.size() > 512) return {"success":false,"error":"url too long"};`
- **[发现于第二遍]**

### 07-P2-F  FlowEngine::Start vs Stop vs LoadScriptTask 静态栈 + loading_ 竞态可重入
- **等级 P2**：判级理由——`LoadScriptTask` 使用 **static** 栈与 TCB（flow_engine.cc:85-86 `static StackType_t s_flow_load_stack[]`）。`loading_` CAS 防重入（:61），但 `Start` 内若 `state_!=kIdle` 先 `Stop()`（清 loading_=false :149），再 `loading_=true`（:71）——`Stop` 与 `Start` 若分别在不同任务并发，`loading_` 标记的 happens-before 不足以保证上一个 `LoadScriptTask` 已退出（它在 `vTaskDelete` 前还会写 `load_task_handle_=nullptr` :282）。两个 load task 复用同一 static 栈 → 栈破坏。实际 Start 都经 RemoteCmd 单线程触发，故 P2。
- **文件**：`main/flow_engine.cc:59-95`、`:255-284`
- **根因**：static 栈复用 + loading_ 标记不能保证旧任务已 `vTaskDelete` 完成。
- **触发/影响**：极快速 start→stop→start（如服务器连发命令）可能两个 load task 撞同一静态栈 → 崩溃。
- **修复**：load task 退出前用事件/信号量通知，Start 创建新 task 前等旧 task 真正结束；或用动态栈（xTaskCreate）避免复用。
- **[发现于第二遍]**

### 07-P3-C  AlarmManager NvsWorkerTask while(true) 永不退出，FlushNvs 200ms 合并窗口拖慢关机
- **等级 P3**：判级理由——`NvsWorkerTask`（alarm_manager.cc:589）`while(nvs_worker_running_)` 而 `nvs_worker_running_` 永远 true（代码注释自承 :643），任务永不退出，`vTaskDelete` 不可达。worker 每次 `xQueueReceive` 后强制 `vTaskDelay(200)`（:599）合并窗口——`FlushNvs`（深睡/关机前调）需等这 200ms+commit。非崩溃，远期可优化。
- **文件**：`main/alarm_manager.cc:589-645`、`:648-660`
- **根因**：合并窗口固定 200ms，shutdown 路径无快速 flush 通道。
- **修复**：FlushNvs 触发时设一个 `flush_now_` 标志让 worker 跳过 200ms 延时。
- **[发现于第二遍]**

### 07-P3-D  EduScenePool 多 NVS 操作无并发保护（单例非线程安全）
- **等级 P3**：判级理由——`EduScenePool` 的 `Load`/`UpdateFromString`/`GetRandomWithCount` 共享 `buf_`/`names_`/`call_counts_`/`last_idx_`，无任何锁。`UpdateFromString`（远程命令任务）与 `GetRandomWithCount`（摇一摇，可能另一任务）并发时 RebuildIndex 改写 buf_/names_ 与读 names_[idx] 竞争 → names_ 指针指向半改写 buf_。当前调用方可能都在主任务故 P3，需确认。`SaveCountsToNvs` 用 static `save_pending`（:158）非线程安全。
- **文件**：`main/edu_scene_pool.cc:45-75,125-165`、`edu_scene_pool.h:23-54`
- **根因**：单例无 mutex，文档未约束单线程调用。
- **修复**：加 `std::mutex`，或在头文件明确"仅主任务调用"约束并核实所有调用点。[待确认调用方任务归属]
- **[发现于第二遍]**

### 07-P3-E  DeviceStateMachine::GetStateName 数组与枚举手工对齐，易错位
- **等级 P3**：判级理由——`STATE_STRINGS[]`（device_state_machine.cc:9-22）手工列 12 项与 `DeviceState` 枚举一一对应，靠 `state>=0 && state<=kDeviceStateFatalError` 索引（:28）。新增枚举值若忘了加字符串，索引正确但内容错（或越界——但有上界判断兜住）。属技术债。
- **文件**：`main/device_state_machine.cc:9-32`
- **修复**：用 switch-case 返回常量字符串，编译器可警告未覆盖枚举。
- **[发现于第二遍]**

---

## 第三遍 · 反审自检（复验行号 / 删误报 / 对抗视角补漏）

复验前两遍每条的行号与代码片段真实性，调整判级，并以"如何用畸形 MCP 调用 / 状态竞争让它崩"反查漏报。

### 复验结论
- **07-P0-A**：行号 :374 / :1374 复核准确，protocol_ 确为裸 unique_ptr（application.h:160），reset 在后台任务（:1374）属实。**维持 P0。**
- **07-P0-B**：复核 ContinueOpenAudioChannel :998-1001 与 ContinueWakeWordInvoke :1122-1126 确实 open 失败后无回 Idle。Connecting 态在 HandleWakeWordDetectedEvent / HandleToggleChatEvent 均无对应分支（只处理 Idle/Speaking/Listening/Activating），确认卡死。**维持 P0。**
- **07-P1-B**：复核后确认非内存越界（json 恒非空），是逻辑误报路径。**降级保留 P1→实际偏 P2**，标注于此：建议按 P2 排期。
- **07-P2-A**：~Application 确实只删 clock_timer（:140-143）。维持 P2。
- 其余各条行号与片段复核无误。

### 对抗视角补漏（本遍新增）

### 07-P1-E  McpTool::Call 对 cJSON* 返回值二次释放 / 调用方再释放的所有权混乱
- **等级 P1**：判级理由——`McpTool::Call`（mcp_server.h:295-300）当 callback 返回 `cJSON*` 时，打印后 `cJSON_Delete(json)`（:300），即 Call **接管并释放** callback 返回的 cJSON。但 `self.screen.get_info`（mcp_server.cc:320-328）返回的 `json` 是新建对象——OK。风险在 `ImageContent*` 路径（:279-285）：`to_json()` 内部自建自删 cJSON 是安全的，`delete image_content` 释放 ImageContent——若某 callback 既返回 ImageContent* 又在别处持有同指针则双删。当前无此用法，但 `ReturnValue` variant 把裸 `cJSON*`/`ImageContent*` 暴露给 callback 作者，所有权契约（"返回后归 Call 释放"）只存在于注释，新工具作者极易写出返回栈上/共享 cJSON 导致双删或泄漏。
- **文件**：`main/mcp_server.h:51,279-301`
- **根因**：`ReturnValue` 用裸指针传递所有权，无 RAII，契约靠约定。
- **触发/影响**：未来新增返回 cJSON*/ImageContent* 的工具时极易引入双删崩溃（已是量产隐患）。
- **修复**：短期在文档/注释强约束"返回的 cJSON* 必须是 caller 新建且转移所有权"；中期把 ReturnValue 的 cJSON* 换成持有所有权的包装类型。
- **[发现于第三遍]**

### 07-P2-G  DoToolCall 捕获 tool 裸指针跨线程执行，tool 生命周期未锁
- **等级 P2**：判级理由——`DoToolCall`（mcp_server.cc:550-608）在 `tools_mutex_` 下取出 `McpTool* tool`，**出锁后** `Schedule` 到主任务执行 `tool->Call(arguments)`（:601-603）。tools_ 一旦注册不删除（无 RemoveTool API），故当前安全；但 `AddCommonTools` 会 `std::move(tools_)` 再 insert 回（:51,285），若与 DoToolCall 并发，move 期间 tool 指针仍有效（McpTool 对象本身不被删，只是 vector 重排），故不悬空。结论：当前**不崩**，但"出锁后用裸指针"是脆弱模式，记录。
- **文件**：`main/mcp_server.cc:550-608`、`:42-52`
- **修复**：若未来加 RemoveTool 必须重构为持锁执行或引用计数。当前标记技术债。
- **[发现于第三遍]**

### 07-P2-H  ParseScript 解析 set 类型 text_item 时未判 valuestring 为 null（cJSON 边界）
- **等级 P2**：判级理由——`ParseScript`（flow_engine.cc:393）`cJSON_IsString(text_item) && text_item->valuestring[0] != '\0'`——`cJSON_IsString` 为真时 valuestring 非 null（cJSON 保证），安全。但 `:382` `strcmp(type_item->valuestring, "ttai")` 在 `cJSON_IsString(type_item)` 内，安全。复核为**无问题**（误报排除）。真正可疑点：`:351 script_name_ = name_item->valuestring`——同样在 cJSON_IsString 守卫内，安全。**本条经复核为误报，删除，不计入统计。**
- **[第三遍复核为误报，已删除]**

### 07-P2-I  AbortSpeaking 在 SendTextToTts 路径下 aborted_ 置位但无线程保护
- **等级 P2**：判级理由——`aborted_`（application.h:175 普通 bool）在 `AbortSpeaking`（:1252 写 true）与 `OnIncomingJson` tts/start（:778 写 false，主任务 Schedule 内）被不同上下文读写。`AbortSpeaking` 可被 MCP music.play 任务（mcp_server.cc:206）、SendTextToTts（主任务）、HandleToggleChat（主任务）调用——music.play 的 AbortSpeaking 在 `Schedule` 内（mcp_server.cc:201-207）所以也在主任务。复核：AbortSpeaking 实际全部经主任务或 Schedule 包裹，`aborted_` 实际单线程访问。**降级 P3**（潜在，当前路径安全），记录约束。
- **文件**：`main/application.cc:1250-1256,778`、`application.h:175`
- **修复**：注释标明 aborted_ 仅主任务访问；若未来从 ISR/其他任务调 AbortSpeaking 需改 atomic。
- **[发现于第三遍 · 判级 P3]**

### 07-P3-F  WakeWordInvoke / ContinueWakeWordInvoke 中 protocol_ 判空不一致
- **等级 P3**：判级理由——`WakeWordInvoke`（:1460）开头判 `if (!protocol_) return`，但 `ContinueWakeWordInvoke`（:1116）经 Schedule 延后执行，执行时 `protocol_` 可能已被 reload reset（:1122 `protocol_->IsAudioChannelOpened()` 裸用）。属 07-P0-A 同根因的一个具体触发点。单独记为 P3 触发面。
- **文件**：`main/application.cc:1116-1127`
- **修复**：ContinueWakeWordInvoke 开头补 `if (!protocol_) { SetDeviceState(kDeviceStateIdle); return; }`
- **[发现于第三遍]**

---

## 统计

> 说明：07-P2-I 经第三遍复核降为 P3；07-P2-H 经复核为误报已删除（不计入）；07-P1-B 复核偏 P2 但保留原编号与等级以便追溯。

| 等级 | 数量 | 编号 |
|------|------|------|
| P0 | 2 | 07-P0-A、07-P0-B |
| P1 | 5 | 07-P1-A、07-P1-B、07-P1-C、07-P1-D、07-P1-E |
| P2 | 6 | 07-P2-A、07-P2-B、07-P2-C、07-P2-D、07-P2-E、07-P2-F、07-P2-G（07-P2-G 偏技术债但计入 P2） |
| P3 | 7 | 07-P3-A、07-P3-B、07-P3-C、07-P3-D、07-P3-E、07-P3-F、07-P2-I（降级） |

修正：P2 列出 7 个编号（A–G），故 **P2 = 7**。

**最终计数：**
- **P0：2**
- **P1：5**
- **P2：7**
- **P3：7**
- **合计有效问题：21**（另删除误报 1 个：07-P2-H）

**三遍新增分布：**
- 第一遍（广度）：**8 个** — P0-A、P1-A、P1-B、P2-A、P2-B、P2-C、P3-A、P3-B
- 第二遍（红线深挖）：**9 个** — P0-B、P1-C、P1-D、P2-D、P2-E、P2-F、P3-C、P3-D、P3-E
- 第三遍（反审自检）：**4 个** — P1-E、P2-G、P3-F、07-P2-I(降 P3)；并删除误报 1 个（P2-H）

> 三遍新增合计：8 + 9 + 4 = 21（与有效合计一致）。

---

## 最严重项（出货前必清零）

1. **07-P0-A**：`protocol_` 裸 unique_ptr 跨 3 个任务并发读写、reload/ResetProtocol 在后台任务 reset，主循环正在 `SendAudio` → use-after-free 必崩。建议把所有 protocol_ 访问收敛到主循环单线程。
2. **07-P0-B**：Idle→Connecting 后 `OpenAudioChannel()` 失败不回退 Idle，弱网首次连接失败即永久卡 Connecting → 设备假死、无法休眠也无法再唤醒，须重启。一行修：失败分支补 `SetDeviceState(kDeviceStateIdle)`。

> 两条 P0 都属"量产弱网/切换平台"高发场景，且 07-P0-A 与 07-P1-D/07-P3-F 同根（protocol_ 并发），应一并整改。
