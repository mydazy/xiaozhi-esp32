# P1-FIX-PLAN — 3 个量产必修 P1 修复方案（草案 · 仅方案不改码）

> 来源：`docs/upstream-compare/RED-ALL.md` 第一档 + `RED-B.md` 置顶三项。
> 基线对照 78/xiaozhi-esp32 **v2.2.4**。本文件**只起草方案，未改一行源码**。
> 三项均碰量产红线（内存安全 / 安全 / OTA），按项目纪律 `先方案 → Jack 确认 → 再实施`。
> 现状均已对源码二次确认（行号以 2026-06-17 HEAD 为准）。

---

## 阅读指引（给 Jack 的一屏话）

- **P1-1**（I2C 栈 UAF）：硬件偶发卡死时会随机踩内存，量产返修最难定位。修法小、风险低，**建议优先**。
- **P1-2**（弱网重连卡主循环）：断网时整机冻结 ≥3 秒（按键/唤醒/语音全不响应）。命中记忆里"治标越修越糟、需架构断根"的老账，**这次要真断根**。
- **P1-3**（远程命令零鉴权）：劫持对话连接即可远程重启/刷机/改人格/改唤醒词。**安全红线**，但要兼容云端现有正常下发，需分级处理。
- 附带 🔴10-13：4 个死码/注释债，与 P1-3 同文件（remote_cmd.cc）或属 OTA 红线（ota.cc），最后一节统一给处理建议。

三项工作量小计见末节「总体实施顺序」。

---

# P1-1：i2c_worker 超时路径栈 UAF

## 现状（已二次确认）

证据文件：`components/mydazy__i2c_bus_worker/i2c_bus_worker.c`

caller 在**自己栈上**创建 binary semaphore，把它和栈上的 `op_result` 地址一起塞进 op 投递给 worker：

```c
// submit_and_wait  :240-247
StaticSemaphore_t sem_buf;                                   // ← 栈上
SemaphoreHandle_t sem = xSemaphoreCreateBinaryStatic(&sem_buf);
if (!sem) return ESP_ERR_NO_MEM;
esp_err_t op_result = ESP_FAIL;                              // ← 栈上
op->result_sem = sem;
op->result_out = &op_result;                                // ← 指向栈
```

超时放弃路径（队列等待超时 + 兜底再等 200ms 仍未拿到 sem）：

```c
// submit_and_wait  :270-273
if (xSemaphoreTake(sem, pdMS_TO_TICKS(200)) != pdTRUE) {
    ESP_LOGE(TAG, "worker stuck > 200ms after queue timeout");
    /* 极端情况下 sem 不能释放（worker 仍持有） — 接受 leak 换 UAF 安全 */
    return ESP_ERR_TIMEOUT;     // ← 函数返回，caller 栈帧回退，sem_buf / op_result 失效
}
```

worker 侧恢复执行后**无条件回写**：

```c
// worker_task  :226-227
if (op.result_out) *op.result_out = ret;       // ← 写已失效栈：内存踩踏
if (op.result_sem) xSemaphoreGive(op.result_sem); // ← give 已失效栈上 sem：踩踏 + 可能 assert
```

## 根因（精确定位）

- **生命周期错配**：`sem_buf` / `op_result` 的生命周期 = caller 的栈帧；但 worker 对它们的引用生命周期 = "op 被 worker 处理完为止"。超时 `return` 让前者先死，后者还活着 → 经典 use-after-return。
- **注释自相矛盾**：`:272` 写"接受 leak 换 UAF 安全"。这句话只有当 sem 在**堆**上才成立（不 delete = 不提前释放堆，worker 还能安全 give）。但 sem 在**栈**上，函数一返回必然失效，"leak"根本不存在，UAF 也换不掉。注释把一个堆方案的安全论证，错套在栈对象上。
- **触发面真实**：codec 失电短接 SDA/SCL、4G-RF 干扰把单次 I2C 事务卡到超过 (队列等待 + 200ms) 时触发。本驱动多处注释自承认 I2C 会卡死，触发条件量产现场存在。

> 说明：`i2c_worker_destroy`（:332-338）也用栈上 `sem_buf` 投 QUIT op，但它紧接着 `xSemaphoreTake(sem, 1000ms)` 同步等到 worker give 完才返回、且只在销毁时一次性调用，不存在"提前 return 让栈失效"路径，**不在本次修复范围**（修复方案保持其行为不变）。

## 修复方案

### 方案 A（首选）：result_sem + result_out 改堆分配，所有权随 op 转移给 worker

把"caller 与 worker 共享的同步对象"从栈搬到堆，并约定**唯一释放者**，用一个 atomic 标志在 caller 放弃时把释放权交还给 worker。

设计要点：

1. 新增一个堆分配的小结构（替代裸 sem + 裸 result_out 指针），随 op 传递：
   ```c
   typedef struct {
       SemaphoreHandle_t sem;        // 堆 binary semaphore（xSemaphoreCreateBinary）
       esp_err_t         result;     // worker 写这里（堆上，不再指向 caller 栈）
       atomic_int        refcnt;     // 2 = caller + worker 各持一份
   } op_sync_t;
   ```
2. `submit_and_wait` 改为：`heap_caps_malloc(MALLOC_CAP_INTERNAL)` 一个 `op_sync_t`，`refcnt=2`，`op->result_sync = sync`。
3. **释放规则（引用计数）**：caller 拿到结果或放弃时 `release(sync)`；worker give 完后也 `release(sync)`。`release` 内 `if (atomic_fetch_sub(&refcnt,1)==1) { vSemaphoreDelete(sem); free(sync); }`。最后一个释放者负责销毁，天然无竞态。
4. worker 回写改为写 `sync->result`、give `sync->sem`、再 `release(sync)`（:226-227 三行替换）。
5. caller 超时放弃：不再 leak，直接 `release(sync)`；worker 后续恢复时持有的是堆对象（仍有效），give + 写 result 都安全，最后 worker 的 release 触发真正销毁。

优点：
- **彻底**消除栈 UAF，且**不再 leak**（旧注释承认的 leak 也一并解决）。
- 释放权用 refcnt 闭合，caller / worker 谁后退出谁清理，无需谁等谁。
- 改动集中在 `submit_and_wait` + worker 回写两处，public API 签名零变化。

代价：
- 每次 I2C 事务多一次堆 malloc/free（internal RAM）。本驱动事务频率为传感器/codec 轮询级（非每帧），开销可接受；如担心碎片可后续换 freelist/对象池，但**首版不做**（极简优先）。

### 方案 B（备选）：超时置 op 失效标志，让 worker 跳过回写

保留栈上 sem，但给 op 加一个**堆上**的 `atomic_bool abandoned`（注意：标志本身不能放栈，否则同样失效）。caller 超时把 `abandoned=true`；worker 回写前先 `if (atomic_load(abandoned)) { /*skip 写 result_out + give*/ }`。

问题：
- 标志一旦放堆，就又要解决"这个堆标志谁来释放"——绕回方案 A 的 refcnt 问题，并没更简单。
- 存在**检查-动作竞态窗口**：worker 读到 `abandoned==false` 后、真正 `give` 之前，caller 才超时翻标志为 true 并返回 → worker 仍 give/写已失效栈。窗口虽窄但真实存在，**不能彻底关闭**，对量产红线不合格。
- sem 仍在栈上，worker 那侧的 give 目标仍可能失效。

> 结论：**采用方案 A**。方案 B 看似改动更小，实则把生命周期问题换了个马甲又引入新竞态，不满足"彻底解决"。

## 涉及函数改动（方案 A）

| 函数 | 改动 |
|---|---|
| `op_t`（:48-58） | `result_sem`/`result_out` 两字段 → 替换为 `op_sync_t *result_sync` |
| 新增 `op_sync_t` 结构 + `op_sync_release()` 内联辅助 | 文件内 static |
| `submit_and_wait`（:238-280） | 堆分配 sync（refcnt=2）；超时路径 `return` 前 `release`；正常路径取 `sync->result` 后 `release` |
| `worker_task` 回写（:226-227） | 写 `sync->result` + give `sync->sem` + `release` |
| `worker_task` OP_QUIT 分支（:160-163） | QUIT 仍走 destroy 的栈 sem 旧路径 → 需兼容：QUIT 不带 sync，保留对 `op.result_sem` 的判空 give（见下） |
| `i2c_worker_destroy`（:332-338） | 维持栈 sem 投 QUIT（同步等待，安全）。需让 op 结构能同时表达"带 sync"和"带裸 sem(QUIT)"两种——最小做法：QUIT 专用字段，或 worker 对 QUIT 特判走旧分支 |

> 实施细节（QUIT 兼容）：QUIT 路径生命周期安全（destroy 同步等待），无需 sync。建议 op 保留一个 `SemaphoreHandle_t quit_sem`（仅 QUIT 用）或在 worker `op.type==OP_QUIT` 分支里只 give 不 release。避免为了统一把 destroy 也改成堆分配（无收益、徒增改动面）。

## 并发正确性论证

竞态窗口 = 「caller 决定放弃」与「worker 回写」的交错。方案 A 用 refcnt 闭合所有交错：

- **情形1 worker 先完成**：worker give → caller 的 `xSemaphoreTake` 成功 → caller 读 `sync->result` → caller release(2→1)；worker 之后 release(1→0) 销毁。✅
- **情形2 caller 先超时**：caller release(2→1) 后返回（栈帧回退，但 sync 在堆，worker 引用仍有效）→ worker 恢复，写 `sync->result`（堆，安全）、give `sync->sem`（堆，安全，无 taker 也合法）→ worker release(1→0) 销毁。✅ **无栈引用、无 leak。**
- **情形3 几乎同时**：两个 `atomic_fetch_sub` 串行化，先到者得 1（不销毁），后到者得 0（销毁）。无论谁先，销毁恰好发生一次，且在两者都不再访问之后。✅
- **sem give 给"无人等待"的 binary semaphore**：FreeRTOS 合法操作（只是把 count 置 1），不会 assert。✅
- **写 `sync->result` 与 caller 读**：情形2 中 caller 在 release 后**不再读** result（它已超时返回 ESP_ERR_TIMEOUT），故不存在并发读写同一字段；情形1 中 caller 读发生在 take 成功之后（happens-after worker 的 give），有 semaphore 内存序保证可见性。✅

> 关键不变式：**sync 对象的销毁，当且仅当 refcnt 归零**，而 refcnt 归零 happens-after caller 与 worker 各自最后一次访问。栈失效不再牵连共享对象。

## 验收标准

构造超时场景（两种，覆盖软超时与真硬件卡死）：

1. **软超时注入（无需硬件）**：临时在 `worker_execute_op` 某 op 类型前插 `vTaskDelay(600ms)`（>队列等待+200ms 兜底），让 caller 必走超时放弃路径。预期：caller 返回 `ESP_ERR_TIMEOUT`；worker 600ms 后回写**不崩**；`heap_caps_check_integrity_all(true)` 干净；连续跑 1000 次无内存增长（证明无 leak）、无踩踏（证明无 UAF）。
2. **真机硬件卡死**：运行中**拔 codec / 短接 SDA-SCL**制造 I2C 卡死，触发 `bus_reset` + 超时路径。预期：`i2c_worker_get_stats` 的 `timeout_count` 增长；系统不重启不踩内存；恢复接线后 I2C 继续工作。
3. **回归**：正常 I2C 读写（传感器轮询 / codec 配置）行为与位级结果不变；`i2c_worker_destroy` 仍能干净退出 worker（QUIT 路径未回归）。

通过判据：上述全部满足 + 无新增 P0/P1。

## 回滚点

- 单文件改动，`git revert` 该 commit 即回到现状（栈 sem + leak 注释）。
- 中间态安全：可先只改 worker 回写 + submit 为堆分配（保持 API），不动 destroy；若堆分配方案出问题，回退仅影响本组件，不波及调用方（API 不变）。

## 工作量评估

- 编码：约 0.5 天（结构 + refcnt + 两处改 + QUIT 兼容）。
- 验证：约 0.5 天（软超时脚本 + 真机拔 codec 实测，**真机需 Jack 配合**）。
- 风险：低（改动局限单组件，API 零变更，有 refcnt 形式化论证兜底）。

---

# P1-2：弱网三态重连主循环同步阻塞

## 现状（已二次确认）

证据文件：`main/application.cc`

`OnAudioChannelClosed` 回调里，Listening/Speaking 断线后投递一个 Schedule 闭包做重连：

```c
// :748-762  （在 OnAudioChannelClosed 内）
Schedule([this]() {
    for (int attempt = 1; attempt <= 3; ++attempt) {
        vTaskDelay(pdMS_TO_TICKS(attempt * 500));   // 500 + 1000 + 1500 = 3000ms
        if (!protocol_) return;
        bool ok = protocol_->OpenAudioChannel();     // 同步！含 TLS 握手，弱网更久
        if (ok) { SetListeningMode(kListeningModeManualStop); return; }
    }
    Board::GetInstance().GetDisplay()->SetChatMessage("system", "");
    SetDeviceState(kDeviceStateIdle);
});
```

Schedule 闭包由**主循环线程串行执行**：

```c
// Schedule  :1258-1264 —— 入队 + 置 MAIN_EVENT_SCHEDULE
// Run/主循环 :401-408 —— 取出 main_tasks_ 顺序 task()
// 主循环优先级 :322 —— vTaskPrioritySet(nullptr, 10)
```

主循环同一个 `while` 还要处理 `MAIN_EVENT_SEND_AUDIO`(音频上行 :375)、`MAIN_EVENT_TOGGLE_CHAT`(按键/触屏 :363)、`MAIN_EVENT_WAKE_WORD_DETECTED`(唤醒 :390)。重连闭包占着主循环跑 `vTaskDelay 3s + 3×OpenAudioChannel` 期间，这些事件**全部排队等待**。

官方 v2.2.4 对照：`OnAudioChannelClosed` 仅 Schedule 回 Idle，**不重连不阻塞**。

## 根因

**异步壳 + 同步核**：`Schedule` 是异步投递（壳对了），但闭包执行体把"退避延时 + 同步连接（含 TLS）"塞进了主循环线程（核错了）。等于把一段最长可达数秒的阻塞代码，放进了全机最关键的单线程事件泵里。命中记忆 `post-prod-stability-2026-06`「弱网主任务阻塞致卡死 P1×2，治标越修越糟、需架构断根」。**调退避参数（把 3s 改小）治标不治本——只要 OpenAudioChannel 同步且在主循环，弱网 TLS 握手照样能卡几秒。**

## 修复方案

把重连**整段**移出主循环线程，放到独立的低优先级一次性任务里执行；主循环只负责"决定要不要重连"和"接收重连结果回主线程提交状态变更"。

### 任务设计

复用项目已有的"低优先级 detached 一次性任务"惯例（参考 activation 任务 `xTaskCreate(... "activation", 4096*2, this, 2, ...)` :439-444；alarm_nvs worker）。

```
OnAudioChannelClosed（仍在协议回调线程）
  └─ 现有窗口/计数判定（user_initiated / server_initiated / 窗口限频）保持不变
  └─ 决定要重连时：不再 Schedule 跑循环，而是
       若已有重连任务在跑 → 直接 return（防重入）
       否则 xTaskCreate 一个 "reconnect" 低优先级任务（prio 2 ~ 3，栈 4KB）
            任务体：for attempt 1..3 { vTaskDelay; if(!protocol_) break;
                       ok = protocol_->OpenAudioChannel();   // 阻塞在本任务，不碍主循环
                       if (ok) { Schedule(回主线程 SetListeningMode); break; } }
                    失败 → Schedule(回主线程 SetChatMessage 清空 + SetDeviceState Idle)
            任务尾：reconnect_task_handle_ = nullptr; vTaskDelete(NULL);
```

要点：
- **延时 + OpenAudioChannel 都在新任务**，主循环完全不被占用。
- **状态变更回主线程**：`SetDeviceState` / `SetListeningMode` 等仍通过 `Schedule` 切回主循环执行（与现有架构一致，避免在重连任务里直接动设备状态机引入新的跨线程写）。
- **防重入**：新增 `std::atomic<TaskHandle_t> reconnect_task_handle_{nullptr}`（或 `std::atomic<bool> reconnecting_`）。OnAudioChannelClosed 判定要重连前先 CAS：已在重连则跳过本次（旧逻辑里"窗口限频 kMaxReconnectInWindow"保留，作为更上层的频率闸）。
- **与 device_state_machine 协作**：重连任务**不直接**调状态机；所有 `SetDeviceState` 经 Schedule 回主线程，沿用现有 `transition_mutex_` 串行化（见 RED-B 纠偏#4，状态机已加锁）。重连期间设备状态保持 close 前的语义，成功才切回 Listening、失败才切 Idle，与现状对外行为一致。
- **protocol_ 生命周期**：任务体每次 OpenAudioChannel 前判 `if(!protocol_) break`（沿用现状 :751-753 的保护），避免关机/切网把 protocol_ 销毁后悬空调用。

> 备选实现 esp_timer 链式回调：可用 3 个递增延时的 `esp_timer_start_once` 串联代替 vTaskDelay。但 OpenAudioChannel 含 TLS 阻塞**不能**放进 esp_timer 回调（esp_timer 派发线程被阻塞会拖垮所有定时器）。故 esp_timer 方案仍要把连接动作再投到某工作线程，反而更绕。**选独立 task 方案**，与 activation 任务同构、最贴合现有代码。

## 如何保证重连期间主循环响应按键/唤醒（<100ms）

- 重连任务 prio 2~3 **低于**主循环 prio 10。主循环一旦有 `MAIN_EVENT_TOGGLE_CHAT`/`WAKE_WORD_DETECTED`/`SEND_AUDIO` 就绪即抢占重连任务运行 → 按键/唤醒/音频上行不再被 3s 阻塞。
- 主循环自身不再承载任何 vTaskDelay 或同步 OpenAudioChannel，事件处理回到毫秒级。
- 唯一需注意：用户在重连期间手动按键发起新对话（ToggleChatState）→ 可能与后台重连竞争开通道。处理：按键路径若发现 `reconnecting_` 为真，可让按键优先（停掉/让重连任务下次循环 `if(!protocol_ || reconnecting已被取消)` 自然退出），或简单地让 OpenAudioChannel 内部幂等（已开则复用）。**首版采用最小策略**：按键照常走，重连任务下一轮检测到通道已开（`protocol_->IsAudioChannelOpened()`）即成功退出，避免重复开。

## 验收标准

1. **断网注入**：设备处于 Listening/Speaking，拔网线 / 关 AP / 4G 进电梯。预期：
   - 触发后**立即**按物理键 / 喊唤醒词，响应延迟 **<100ms**（对比修复前 ≥3s）。
   - 后台重连任务在网络恢复后能自动重新开通道并回 Listening；3 次失败则回 Idle 并清 ChatMessage（行为同现状）。
2. **频率闸回归**：5 分钟窗口内断线重连次数仍受 `kMaxReconnectInWindow=3` 限制（:739），不被新任务绕过。
3. **防重入**：连续快速断线-恢复-断线，不产生多个并行重连任务（用日志/任务列表验证仅一个 "reconnect" 任务存活）。
4. **关机/切网安全**：重连进行中触发关机或 Wi-Fi 重配，protocol_ 销毁，重连任务 `if(!protocol_)` 安全退出，无悬空调用、无崩溃。
5. **无内存回归**：反复断网 200 次，internal/PSRAM min_free 不持续下降（任务正常 vTaskDelete 回收栈）。

通过判据：①的 <100ms 为核心硬指标，其余全满足。

## 回滚点

- 改动集中在 `application.cc` 的 OnAudioChannelClosed 闭包 + 新增一个任务函数 + `application.h` 一个 atomic 字段。`git revert` 即回现状（主循环内同步重连）。
- 兼容性：对外协议、状态机语义不变，回滚无数据/分区影响。

## 工作量评估

- 编码：约 0.5~1 天（抽出重连任务、加防重入字段、状态变更回投 Schedule）。
- 验证：约 1 天（断网注入需 Jack 配合真机：拔网/电梯/关 AP 多场景测响应延迟）。
- 风险：中（触及主循环与状态机协作路径，但状态变更仍走既有 Schedule+锁，风险可控）。**这是记忆里点名的架构债，建议配 regression-check 跑一轮按键/唤醒/音频回归。**

---

# P1-3：remote_cmd 零鉴权远程遥控

## 现状（已二次确认）

证据文件：`main/remote_cmd.cc`、入口 `main/application.cc:863-869`

- 入口 `RemoteCmd::Handle`（:62）对 payload 解析后直接按 `type` 分发，**无任何 token / 签名 / 设备身份 / nonce 校验**。信任边界 = 「已建立的对话连接」（application.cc:863 收到 `type=custom` 即 `remote_cmd_->Handle(payload)`）。
- 命令表（:86-104）共 19 条：`reboot / ota / sleep / reconnect / wakeup / tts / ttai / volume / gain / mic_calibrate / download / flow / music_play / music_stop / music_pause / music_resume / edu_pool / update_prompt / wakeword`。
- 危险命令直写无确认无限流：
  - `update_prompt`（:436-449）直接 `UpdateSystemPrompt`，**无长度上限、无内容审查**，空串=清空人格。
  - `wakeword`（:470-489）直接写 NVS（`Settings("wakeword", true)`），重启生效，可改成用户不会喊的词 → 设备"装聋"。
  - `reboot`（:114）/`ota`（:124）/`sleep`（:328）—— 远程重启 / 触发刷机 / 强制休眠。
  - `tts`/`ttai`（:164/:182）—— 让设备说任意话 / 向 AI 注入任意指令。

## 威胁模型（攻击者能做什么 · 危险分级）

前提：攻击者**劫持或 MITM 了设备↔服务器的对话连接**（或拿到可向该连接注入 `type=custom` 消息的能力）。

| 级别 | 命令 | 攻击者能造成 |
|---|---|---|
| **D0 致命/不可逆** | `reboot`, `ota`, `sleep`, `update_prompt`, `wakeword` | 砖机风险（反复 reboot / 触发刷机）、强制关机 DoS、篡改 AI 人格（prompt 注入/越狱/清空）、改唤醒词使设备永久"装聋"（远程语音 DoS） |
| **D1 内容/骚扰** | `tts`, `ttai` | 让设备对用户说任意话；向 AI 注入指令借设备身份行动 |
| **D2 运维/可恢复** | `volume`, `gain`, `mic_calibrate`, `reconnect`, `wakeup`, `flow`, `music_*`, `edu_pool`, `download` | 改音量/增益、切场景、放任意 URL 音乐（SSRF/流量）、刷场景池。骚扰为主，无持久破坏 |

核心风险集中在 **D0**：这 5 条要么不可逆（reboot/ota/sleep 影响可用性与刷机）、要么持久改变设备行为（update_prompt/wakeword 写状态/NVS）。

## 修复方案

设计原则：**最小侵入 + 不破坏云端现有正常下发 + 危险子集重点设防**。三种思路权衡如下，选「分级 + 危险子集签名/nonce + 来源约束」组合：

| 思路 | 做法 | 优劣 |
|---|---|---|
| **A 全量加签名** | 每条命令都验设备级签名 | 安全最强，但要云端为 19 条全改下发、改动面大、误伤现有正常运维（兼容代价高）。**过度**。 |
| **B 服务端白名单** | 设备只信"服务端已鉴权来源"，端侧不验 | 把信任全压在服务端 + 传输层。但威胁模型正是"连接被劫持/MITM"，服务端白名单挡不住已劫持的连接。**不足以闭合本威胁**。 |
| **C 命令分级 + 危险子集签名/nonce + 来源约束（选用）** | D0/D1 走强校验，D2 维持现状 | 改动面小（只动 5~7 条危险命令）、兼容好（D2 云端零改）、正打在风险点。 |

### 选用方案 C 的具体设计

1. **来源约束（所有命令，零成本）**：`Handle` 入口加判定——仅当来源是"设备主动建立的、已激活的对话连接"时才受理 `type=custom`。当前 application.cc:863 已经只在该连接上路由，此步是把"隐含约束"显式化并加注释，作为最外层闸门（防止未来有别的入口误接 Handle）。

2. **危险命令子集（D0）加设备级校验**：对 `reboot / ota / sleep / update_prompt / wakeword` 这 5 条，要求命令体携带一次性 **nonce + HMAC 签名**：
   - 设备出厂写入一个**每机唯一的密钥**（复用现有设备身份/激活密钥体系，存 NVS 安全区，不硬编码）。
   - 命令体新增 `nonce`（服务端递增或随机，设备侧记录已用 nonce 防重放）+ `sig = HMAC-SHA256(key, type|关键参数|nonce)`。
   - 设备校验：sig 正确且 nonce 未用过 → 执行；否则丢弃并告警日志。
   - **防重放**：维护最近 N 个已用 nonce（或单调递增计数）落 NVS，重复 nonce 拒绝。
   > 若设备暂无现成每机密钥可复用，则**先落地"分级确认 + 限流"作为第一道（见下 3、4），签名作为第二阶段**——避免为引入密钥体系阻塞整个修复。两阶段在末节实施顺序里标注。

3. **危险写操作限流（D0 + 部分 D1）**：`reboot/ota/sleep/update_prompt/wakeword` 加最小频率闸（如同类命令 ≥N 秒一次，复用 esp_timer 或时间戳比较），防"反复 reboot/CheckVersion 打断用户"的 DoS。

4. **危险写操作输入加固**：
   - `update_prompt`：加 prompt **长度上限**（如 ≤2KB）+ 拒绝明显异常内容；空串"清空"行为改为需显式标志而非裸空串误触。
   - `wakeword`：写 NVS 前校验 mode ∈ {afe, custom}、text 长度/字符合法（沿用记忆 `custom-wakeword-chain` 的拼音/长度规则）。

### 不破坏云端现有正常下发的兼容方案

- **D2 命令（音量/增益/音乐/场景等 14 条）零改**：云端现有下发格式与行为完全不变。
- **D0 命令两阶段兼容**：
  - 阶段1（限流+输入加固）：不改命令格式，云端无感；只是设备侧对畸形/高频的危险命令更稳。
  - 阶段2（签名/nonce）：采用**软开关**——设备读 NVS 配置 `require_sig`（默认随固件灰度逐步打开）。未带 sig 的旧云端在灰度初期仍放行（仅告警），灰度确认云端已升级后再强制。避免"固件先上、云端没跟上→所有危险命令被拒"的现场事故。
- 顺带修 `remote_cmd.h` 文档表与实际命令对齐（见 🔴13），让云端按真实命令表对接。

## 安全正确性论证

- **闭合"连接劫持后下发危险命令"**：阶段2 的 HMAC 让攻击者即使能注入 custom 消息，也因无每机密钥无法伪造 D0 命令的 sig → 危险子集被挡。nonce 防重放阻止"录制-重放合法危险命令"。
- **闭合"高频骚扰 DoS"**：限流让 reboot/ota/sleep 即使被伪造也无法高频打断用户。
- **闭合"人格/唤醒词持久篡改"**：update_prompt 长度+内容校验 + wakeword 合法性校验，叠加签名，防越狱/装聋。
- **不引入新攻击面**：来源约束只收窄入口；软开关默认不破坏现网；D2 不动。
- **残余风险（明示）**：阶段1 单独上线时，D0 仍可被已劫持连接下发（只是更难高频、畸形被挡）——**真正闭合需阶段2 签名**。故方案把签名列为必须项，限流/加固列为可先行的缓解。

## 验收标准

1. **伪造命令被拒**：构造一条 `update_prompt`/`wakeword`/`reboot`（无 sig 或 sig 错、或重放旧 nonce），阶段2 开关打开时设备**拒绝执行**并打告警日志；阶段1 时高频伪造被限流挡下。
2. **正常命令通过**：云端按正确密钥+nonce 下发的 D0 命令正常执行；所有 D2 命令（音量/音乐/场景…）行为与修复前完全一致（逐条回归）。
3. **防重放**：同一条合法 D0 命令重复下发第二次被 nonce 去重拒绝。
4. **输入加固**：超长 prompt 被截断/拒绝；非法 wakeword mode/text 被拒，不写 NVS。
5. **灰度兼容**：`require_sig=false` 时旧格式 D0 命令仍执行（仅告警），证明不破坏现网。
6. **限流**：1 秒内连发 5 条 reboot，仅首条受理，其余被限流丢弃。

## 回滚点

- 阶段1（限流+输入加固+文档）：纯 `remote_cmd.cc`/`.h` 改动，`git revert` 即回现状。
- 阶段2（签名/nonce）：受 `require_sig` 软开关保护，出问题可远程/配置关开关降级到阶段1 行为，无需回滚固件。密钥体系若复用现有激活密钥，则不新增分区/OTA 改动；若需新增 NVS 命名空间，属普通 NVS（非红线分区表）。
- **注意**：本项不触碰分区表/Secure Boot；`ota` 命令仍只是触发既有 `Ota::CheckVersion`（源 URL 取本机 NVS，攻击者改不了源），不改 OTA 验签/回滚链路本身。

## 工作量评估

- 阶段1（限流 + update_prompt/wakeword 输入加固 + 文档对齐）：约 0.5 天，低风险，可与 🔴11/12/13 同批落地。
- 阶段2（HMAC 签名 + nonce 防重放 + 软开关 + 灰度）：约 1.5~2 天，**需与云端/服务端协商命令格式与密钥分发**（产品+后端协作点），属跨端约定，建议单独排期。
- 风险：阶段1 低；阶段2 中（依赖每机密钥体系是否就绪 + 云端配合）。

---

# 附带：🔴10-13 处理建议（OTA / remote_cmd 死码 · 因碰红线文件并入本方案）

| # | 项 | 文件:行 | 现状确认 | 建议 |
|---|---|---|---|---|
| 🔴10 | `Ota::RequestSwitch` 死码 | `main/ota.cc:328-331`、`ota.h:19` | 全仓仅"定义+头声明"，**零调用方**（grep 确认）。功能=向 `/switch` 上报切换通道（NFC/iBeacon 用） | **需产品确认是否有 NFC 短期计划**。无计划→删（连同 PostToOta 若再无其它调用方）。属 OTA 红线文件，**走红线 plan 流程**（plan mode 产方案+确认再删），不随手清。 |
| 🔴11 | OnSleep 降级死分支 | `main/remote_cmd.cc:343-346` | `EnterDeepSleep(enable_gyro_wakeup)` 后紧跟 `ESP_LOGW("未实现…降级") + esp_restart()`。若 EnterDeepSleep 真休眠则后两行是死码；若不休眠则远程 sleep 实为重启（行为误导） | **可随 P1-3 改 remote_cmd 时顺手清**。需先确认 `EnterDeepSleep` 当前实现是否真进休眠：真休眠→删 esp_restart 死分支；仅占位→保留 restart 但改注释为"EnterDeepSleep 未实现，暂以重启代替"以消除误导。**先查 EnterDeepSleep 实现再定**（属休眠/电源域语义，确认后再动）。 |
| 🔴12 | OnDownload 空实现 | `main/remote_cmd.cc:252-269` | 解析 files/emoji 后只 `ESP_LOGW("File sync not yet implemented in V2")`，仅 SetEmotion 有副作用 | **可随 P1-3 顺手处理**。二选一：①删 `download` 命令（命令表 :96 + 函数）——若产品无文件同步计划；②保留 emoji 副作用、删 files 死参数并改日志。建议①（极简），但**需产品确认无文件同步需求**。 |
| 🔴13 | remote_cmd.h 文档与代码不符 | `main/remote_cmd.h:18-42` | 文档表列 `update/ota`、`reload`、`audio_debug`（实际无 reload/audio_debug），漏 `edu_pool`/`mic_calibrate`/`flow` 的部分细节 | **必随 P1-3 同步**（P1-3 兼容方案依赖云端按真实命令表对接）。低风险纯注释，与阶段1 同批改。 |

## 总体实施顺序建议

按"性价比 + 红线刹车 + 跨端依赖"排序：

1. **P1-1 i2c_worker 栈 UAF**（最高）—— 改动小、风险低、量产最难定位的内存踩踏。单组件，可独立先上。
2. **P1-3 阶段1 + 🔴11/12/13**（同批）—— remote_cmd.cc/.h 一次过：危险命令限流 + update_prompt/wakeword 输入加固 + OnSleep/OnDownload 死码清理 + 头文件文档对齐。低风险、立即收敛 D0 的高频/畸形面。
3. **P1-2 弱网重连移出主循环**（架构断根）—— 记忆点名的老账，配 regression-check 跑按键/唤醒/音频回归。需真机断网注入实测。
4. **P1-3 阶段2 签名/nonce**（跨端排期）—— 需云端配合命令格式 + 每机密钥分发，软开关灰度。彻底闭合连接劫持。
5. **🔴10 Ota::RequestSwitch**（红线 plan 流程）—— **先产品确认有无 NFC 计划**；确认无后单独走 OTA 红线 plan mode 删除，不并入上面任何普通批次。

> 红线提示：P1-1（内存安全红线）、P1-3 阶段2（安全红线）、🔴10/🔴11（OTA/休眠语义）落地实施时，按项目纪律 `先 plan → Jack 确认 → 再改`，且 P1-1/P1-2 的真机验证（拔 codec / 断网注入）需 Jack 配合。
