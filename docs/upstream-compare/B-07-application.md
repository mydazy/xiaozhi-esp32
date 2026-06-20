# B-07 application/状态机/弱网三态重连 — 对比官网 v2.2.4 识别过度优化

> 基线 v2.2.4；标尺=量产稳定；🟢必要/🔴过度/⚪扩展/🔒安全/🛡️红线保留；只分析不改码。

## 取证范围

| 文件 | diff（v2.2.4→HEAD） | 在 v2.2.4 是否存在 | 说明 |
|---|---|---|---|
| `main/application.cc` | +502 / −54（556 行变更，文件 1567 行） | 存在 | 共享文件，"+"基本是我们加的（基线已实测干净） |
| `main/device_state.h` | **0 行差异** | 存在 | 完全未改，官方原样 |
| `main/device_state_machine.cc` | +23 / −17 | **存在（官方自带，非自研）** | 加锁 + 补转移路径 |
| `main/device_state_machine.h` | +1 | 存在 | 仅加 `transition_mutex_` 成员 |

取证命令：`git diff v2.2.4 HEAD -- <path>`、`git show v2.2.4:<path>`、`git cat-file -e v2.2.4:<path>`。
关键纠偏：任务书提示"device_state_machine 若 v2.2.4 无则属自研深审"——**实测三文件 v2.2.4 均存在**（`git cat-file -e` 全部 EXISTS），故状态机按"改动官方"审 diff，不按自研审。

---

## 🟢 必要（偏离官方但服务量产稳定）

| 项 | 我们怎么改 | 官方 v2.2.4 原实现 | 为何必要 | 证据 file:line |
|---|---|---|---|---|
| 状态机转移加锁 | `TransitionTo` 全程 `lock_guard<mutex> lk(transition_mutex_)` 包住 load→校验→store | 裸 `load()` → 校验 → `store()`，**check-then-act 无锁**（多任务可并发调 `SetDeviceState`） | 红线#3 并发：状态是全局热路径数据，按键/网络/音频多任务都改它；官方裸 atomic 在"读旧态→校验→写新态"间有竞态窗口，可能写入非法态 | `device_state_machine.cc:113-134`；官方 `git show v2.2.4:main/device_state_machine.cc` 同段无锁 |
| 补 `kDeviceStateFatalError` 转移 | 任何状态 → FatalError 无条件允许 | 无此分支，FatalError 转移被 IsValidTransition 拒 | 致命错误必须能从任意态进入（红线/看门狗语义），否则崩溃态被状态机挡住反而更糟 | `device_state_machine.cc:40-42` |
| 补 `Starting→Idle` 转移 | Starting 额外允许直达 Idle | 官方 Starting 只允许 →WifiConfiguring / →Activating | 已激活设备冷启动直接进 Idle 的合法路径，官方缺这条会打印 Invalid transition 并卡住 | `device_state_machine.cc:51-54` |
| 音频发送熔断 | 连续 100 帧 `SendAudio` 失败 → `SetDeviceState(Idle)` + `CloseAudioChannel()` | 失败仅 `break`（不计数、不退出、下轮继续空转） | 弱网路径：发送通道已断但状态机仍 Speaking/Listening，官方逻辑会无限空转占 CPU 且用户无反馈；熔断主动退出并触发重连闭环 | `application.cc:377-387`；官方 `git show v2.2.4:main/application.cc` SEND_AUDIO 段仅 `break`（v2.2.4:222-224） |
| `OnIncomingJson` 增 type/state 校验 | `type` 非 string 直接 drop；`tts.state` 非 string return | 官方同样有 type 校验，但我们补了 sentence_start 等子分支的 `cJSON_IsString` 守卫 | 红线#2：网络帧外部输入先校验再用，防脏 JSON 裸取 valuestring 崩溃 | `application.cc:776-778, 782` |
| `OnIncomingAudio` 加 force 入队 | Speaking 态 `PushPacketToDecodeQueue(.., true)` | 官方 `PushPacketToDecodeQueue(packet)`（无 force 参数） | 弱网下解码队列可能满，force 保证 TTS 不丢首帧（与音频管线改动配套） | `application.cc:690-693`；官方 v2.2.4:498-500 无第二参 |

---

## 🔴 过度（偏离官方又不服务稳定 / 治标无效 / 堆复杂度）

| 项 | 我们怎么改 | 官方 v2.2.4 原实现 | 为何判过度 | 维护成本/风险 | 证据 file:line |
|---|---|---|---|---|---|
| **弱网三态重连在主循环线程阻塞** | `OnAudioChannelClosed` 里 `Schedule` 一个闭包，闭包内 `for attempt 1..3 { vTaskDelay(attempt*500); OpenAudioChannel(); }` —— 累计 `vTaskDelay` 500+1000+1500=**3 秒**，再叠加 3 次同步 `OpenAudioChannel()` 网络耗时 | 官方 `OnAudioChannelClosed` 仅 `Schedule([]{ SetChatMessage(""); SetDeviceState(Idle); })`，**不重连、不阻塞**，断了就回 Idle 等用户/唤醒重发 | **治标且制造新阻塞**：`Schedule` 闭包由主循环线程在 `MAIN_EVENT_SCHEDULE` 串行执行（`application.cc:401-408`），主循环优先级 10。`vTaskDelay(3s)`+同步连接期间，主循环被独占，`MAIN_EVENT_SEND_AUDIO`(音频上行)/`TOGGLE_CHAT`(按键触屏)/`WAKE_WORD_DETECTED`(唤醒)/后续所有 Schedule 全部排队卡死。正中项目记忆 `post-prod-stability-2026-06`「弱网主任务阻塞致卡死 P1×2，治标越修越糟、需架构断根」 | 主循环卡顿 ≥3s（最坏含连接超时更长）；用户感知=按键无响应/唤醒不灵/界面冻结。**P1 高频**（弱网现场必现）。维护上：退避/限流/三态判定堆了 60 行复杂度（`application.cc:706-771`），但底座（主循环同步重连）是错的，越调参越糟 | `application.cc:748-762`（vTaskDelay+OpenAudioChannel 闭包）+ `:401-408`（Schedule 在主循环串行执行）+ `:322`（主循环 prio 10）。官方对照 v2.2.4:512-518 |
| custom 消息语义重写 | 官方"custom=显示文本/调试打印"→ 我们改成 `remote_cmd_->Handle(payload)` 命令路由 | 官方：`custom` 仅 `ESP_LOGI("Received custom message")` + 校验 payload 后无业务动作（v2.2.4:593-601，纯日志/扩展占位） | 改了官方 `custom` 消息语义。官方 custom 本就是"自定义扩展占位"无固定语义，**协议兼容性风险低**（不破坏 tts/stt/mcp/system 等标准类型）；但语义被我们独占后，若将来对接走官方 custom 约定的服务端会冲突 | 协议耦合：设备端 custom 与服务端必须同约定 RemoteCmd 格式，跨产品线复用需同步。**P3 潜在**（非崩溃，属协议约定债） | `application.cc:863-869`；官方 `git show v2.2.4:main/application.cc` custom 段(v2.2.4:593-601)仅日志 |

> 说明：custom 重写判 🔴 偏轻——它**不破坏官方标准消息类型**（tts/stt/llm/mcp/system/alert 分支与官方一致，见 `application.cc:780-862`），只占用了官方语义模糊的 custom 通道。属"协议约定债"而非"破坏兼容"，故风险定 P3。

---

## ⚪ 扩展（官方没有的纯业务功能，只登记不评判）

| 项 | 能力 | 证据 file:line |
|---|---|---|
| `SendTextToTts` | 文本驱动 TTS 朗读：自动开通道 + Speaking 态先 AbortSpeaking 抢占；协议 fallback 链（TTS→AI→WakeWordInvoke 三级降级） | `application.cc:1282-1307` |
| `SendTextToAI` | 文本送 AI 对话（通道已开才发，否则 WakeWordInvoke 兜底） | `application.cc:1309-1315` |
| `UpdateSystemPrompt` | **运行期换 AI 人设**（教育卡核心）：自动开通道 + `protocol_->UpdateSystemPrompt(model_type, prompt)`，当前协议不实现则返回 false | `application.cc:1323-1330` |
| `ScheduleDelayedWake` | 延迟唤醒：`esp_timer_start_once` 到点 Schedule 一个 `WakeWordInvoke`；timer **复用同一 handle**（stop 后复用不重建，无 double-free） | `application.cc:1332-1351` |
| 教育卡 `[主_辅]` 标记提取 | 每句 TTS `sentence_start` 文本解析 `[苹果_píngguǒ]`，提取主/辅 → 投递 UI ShowEduCard（state+FontGIF 三重守护） | `application.cc:69-104, 805-808` |
| stt → OGG_POPUP 识别确认提示音 | 服务器 stt 回包=语音已识别 → 三重门控（`stt_popup_enabled_` && 非 ManualStop && 非 skip_next）播提示音 | `application.cc:815-828` |
| 休眠判据扩展 | 官方 3 条基础上加 MusicPlaying（防 MP3 中断）+ Pomodoro active（防专注 25min 黑屏打断） | `application.cc:1479-1490`；官方 `git show v2.2.4:main/application.cc` CanEnterSleepMode 仅 3 条(v2.2.4:1052-1066) |
| 多协议自动选型 | URL 含 `bcelive`→百度 BRTC / `joyinside`→JoyAI / 其它→标准 WS | `application.cc:667-680`；官方仅 MQTT/WS 二选一(v2.2.4:481-486) |
| ShowActivationCode 二维码绑定 | 激活码额外生成 `mydazy.cn/ota/bind?mac=` 二维码 | `application.cc:903-909` |
| `RemoteCmd` / `FlowEngine` 注入 | 初始化远程命令处理器 + 直播伴侣流程引擎 | `application.cc:664-665` |
| `tts_streaming_` 标志 | TTS 流式状态跟踪（start/stop 切 atomic），官方无此标志 | `application.cc:784, 790`；声明 `application.h:187` |

---

## 🔒 安全项

无。本模块未发现零鉴权 / 硬编码密钥等量产安全问题。
（custom→RemoteCmd 路由本身不引入鉴权缺口，命令鉴权属 RemoteCmd 模块职责，不在本模块范围。）

---

## 🛡️ 红线保留（触及 电源域 / 内存安全 / 4G脏帧守卫，默认保留只标不动）

| 项 | 说明 | 证据 |
|---|---|---|
| 教育卡字符串解析边界守卫 | `ParseEduCard` 对 LLM 文本（网络外部输入）严格校验：先确认 `[`/`]` 存在且 rp>lp，再找 `_` 分隔符（npos/越界/多分隔符全拒），`substr` 下标恒在 `[0,size)` 内，加长度上限 main≤32/top≤24。属红线#2 内存安全 | `application.cc:49-66, 69-77` |
| `OnIncomingJson` 全分支 cJSON 类型守卫 | 每个 valuestring 取值前 `cJSON_IsString` 校验，脏 JSON 不裸解引用。属红线#2（网络帧外部输入先校验） | `application.cc:776-778, 782, 802, 817, 831, 843, 858` |
| 音频发送熔断 | 弱网路径主动退出，间接服务 4G 弱网稳定。属红线相关弱网保护 | `application.cc:377-387` |
| 状态机 transition 加锁 | 并发热路径数据加锁，红线#3 | `device_state_machine.cc:113-134` |

> 以上即便看着像"额外复杂度"，因触及内存安全/并发/弱网红线，**默认保留，不建议删**。

---

## 深审发现（逐点，带 file:line + 风险级）

### 1. 【P1 高频】弱网重连在主循环线程同步阻塞（本轮焦点·明确判 🔴 治标）
- `Schedule` 把闭包 push 进 `main_tasks_`，由主循环 `Run()` 在 `MAIN_EVENT_SCHEDULE` 分支**串行同步执行**（`application.cc:401-408`：`for (auto& task : tasks) task();`），主循环优先级 vTaskPrioritySet=10（`:322`）。
- 弱网重连闭包（`application.cc:748-762`）内含 `vTaskDelay(attempt*500)` × 3 = 累计 **3000ms** + 3 次同步 `OpenAudioChannel()`（含 TCP/TLS 握手，弱网下可达秒级）。
- **阻塞期间主循环无法处理**：`MAIN_EVENT_SEND_AUDIO`（音频上行 `:375`）、`MAIN_EVENT_TOGGLE_CHAT`（按键/触屏 `:363`）、`MAIN_EVENT_WAKE_WORD_DETECTED`（唤醒 `:390`）、`MAIN_EVENT_STATE_CHANGED`、以及后续所有 `Schedule` 任务全部排队。
- **判定**：🔴 治标堆叠，非断根。退避(500/1000/1500)+5min 限流(`kReconnectWindowUs=300s`/`kMaxReconnectInWindow=3`)+三态判定(用户退/服务器告别/真弱网)共 60 行（`:706-771`），调度框架是异步的（Schedule 投递），**但执行体把 vTaskDelay+同步连接放进了主循环线程**，等价于在主任务里阻塞。正中记忆 `post-prod-stability-2026-06` 警告。
- **断根方向（仅记录不改码）**：重连应跑在独立低优先级 task（自带 vTaskDelay 不碍主循环），或用 esp_timer 定时回调触发非阻塞 `OpenAudioChannel`，主循环只收结果。当前实现是"异步壳 + 同步核"，壳对了核错了。

### 2. 【P3 潜在】`reconnect_attempts_in_window_` / `reconnect_window_start_us_` 裸 int 无 atomic/锁
- 声明为裸 `int` / `int64_t`（`application.h:180-181`），无 atomic、无锁。
- 读改写发生在 `OnAudioChannelClosed` 回调（`application.cc:734-740`：`++reconnect_attempts_in_window_`、`= 0` 赋值、窗口比较）。
- 该回调在**多个协议多处触发**（`websocket_baidu_protocol.cc` 就有 5 处：:351/:649/:982/:1517 等，分布不同函数；mqtt/ws/joyai 各有触发点），若 protocol 内部存在多线程触发同一回调，则裸 int RMW 是 data race（红线#3）。
- **风险定 P3**：回调本体只做 `++` 和阈值判断（窗口极小），且单次断线通常单线程触发；但严格按红线#3「多任务共享 RMW 必须 atomic/锁」，此处不合规。对照：同段其它共享标志（`user_initiated_close_` 等 `:184-187`）都正确用了 `std::atomic<bool>`，唯独这两个计数器漏了。
- 注：`audio_send_fail_count_`（`:179` 裸 int）只在主循环线程读写（`:378-386`），单线程安全，无需 atomic。

### 3. 【P3 潜在】弱网重连闭包内 `protocol_` 裸指针二次解引用的 TOCTOU 窗口
- 闭包内 `if (!protocol_) return;`（`:751`）检查后才 `protocol_->OpenAudioChannel()`（`:754`）。
- `protocol_` 是 `unique_ptr`，检查与使用之间隔着上一轮 `vTaskDelay`。若关机/切网在此窗口 reset 了 `protocol_`，存在 TOCTOU。
- **风险定 P3**：实际 `protocol_` reset 也走主循环线程（`ResetProtocol`/`Reboot` 经 Schedule），与本闭包同线程串行，不会并发——当前线程模型下安全。但依赖"都在主循环"这一隐含约束，注释已说明（`:752` "延迟期间 protocol_ 已被销毁：放弃重连"），属防御性检查，保留合理。

### 4. 【无风险·确认】状态机加锁正确，无死锁
- `transition_mutex_` 与原有 `mutex_`（保护 listeners_）是两把独立锁（`device_state_machine.h:70-71`）。`TransitionTo` 持 `transition_mutex_` 期间调 `NotifyStateChange`——核对：`NotifyStateChange` 在 `transition_mutex_` 临界区**外**调用（`:130` unlock 后 `:133` 才 Notify），不嵌套持锁，无死锁。✓

### 5. 【无风险·确认】MAIN_EVENT 事件分发无并发隐患
- 事件位通过 `xEventGroupSetBits`/`xEventGroupWaitBits`（`:340` `pdTRUE` 自动 clear）操作，FreeRTOS 事件组自带原子性，set/clear 无需额外锁。✓
- `main_tasks_` 出队加锁（`:402-404` `unique_lock(mutex_)` + `std::move` 后 unlock 再执行），入队加锁（`:1259-1262`），生产者-消费者正确。✓

### 6. 【无风险·确认】教育卡解析边界安全
- 见🛡️红线保留。`find` 系列全判 npos，`substr` 下标因 `lp<sep<rp` 恒合法，长度上限防超长串。异常路径（无`[`/无`]`/无`_`/多`_`/空字段/超长）全部 return nullopt。✓

### 7. 【无风险·确认】ScheduleDelayedWake timer 生命周期正确
- `delayed_wake_timer_` 首次 `esp_timer_create` 后**复用同一 handle**（`:1333` 先 stop，`:1336` 仅 nullptr 时才 create，`:1350` start_once），不 delete 不重建，无 double-free / 野指针。符合 ESP-IDF timer 复用规范。✓

---

## 小结

🟢 6 项　🔴 2 项　⚪ 11 项　🔒 0 项　🛡️ 4 项

**一句话结论**：状态机改动（加锁+补转移）与音频熔断是干净的量产加固（🟢），教育卡/文本原语/多协议是稳健的业务扩展（⚪，边界处理合规）；但**弱网三态重连是"异步壳+同步核"的治标堆叠（🔴 P1）——退避调度框架对了，却把 vTaskDelay 3 秒+同步 OpenAudioChannel 放进主循环线程串行执行，断网时直接冻结按键/唤醒/音频上行，正是项目记忆反复警告的"主任务阻塞致卡死、越修越糟"的活体证据**，需架构断根（移到独立 task 或 esp_timer 非阻塞回调），不是再调退避参数。
