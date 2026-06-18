# RED-B — 窗口2（模块6-10）过度优化 + 安全项汇总清单

> 配套报告：`B-06-protocols` / `B-07-application` / `B-08-display` / `B-09-drivers` / `B-10-ota-settings-mcp`。
> 基线 78/xiaozhi-esp32 **v2.2.4**（我们 ≈ v2.2.x 基础，git 历史经 filter-repo 切断但代码版本接近，共享文件 diff 干净）。标尺=量产稳定。**全程只分析未改一行代码**。
> 处置列待用户逐条确认。触及**电源域/内存安全/4G脏帧守卫/并发**的改动已归各 B-NN 的「🛡️红线保留」节（默认保留，不在本清单）。

## 模块计数总览

| 模块 | 🟢必要 | 🔴过度 | ⚪扩展 | 🔒安全 | 🛡️红线保留 | 报告 |
|---|---|---|---|---|---|---|
| 6 protocols 协议 | 7 | 1 | 7 | 2 | 6 | B-06 |
| 7 application/状态机/弱网重连 | 6 | 2 | 11 | 0 | 4 | B-07 |
| 8 display/UI 控制中心 | 8 | 6 | 7 | — | 3 | B-08 |
| 9 自研驱动（sc7a20h/i2c_worker） | 9 | 1 | 2 | — | 3 | B-09 |
| 10 OTA/settings/mcp_server | 5 | 4 | 9 | 4 | 4 | B-10 |
| **合计** | **35** | **14** | **36** | **6** | **20** | |

结论一句话：**这 5 个模块的"偏离官方"绝大多数是有真机根因背书的量产加固（内存安全/脏帧守卫/并发串行化/死值兜底），不是过度优化。** 真正的 🔴 集中在死代码与注释债（清掉即可，几乎全 P3）。但审计额外挖出 **3 个量产必修的 P1**（其中 1 个是全新发现），见下方置顶。

---

## ⚠️ 最高优先：3 个量产必修 P1（建议优先于其它逐条处置）

> 说明：#1 严格不属"过度优化"分类（它是 🟢必要组件 i2c_worker 内部的实现缺陷），但属本轮审计挖出的最严重隐患，单列置顶。#2 已计入下方 🔴，#3 已计入下方 🔒。

### P1-① i2c_worker 超时放弃路径 → caller 栈上 static 信号量 UAF【全新发现·量产最难定位】
- **现象**：`i2c_worker_submit_and_wait` 在 caller 栈上建 `StaticSemaphore_t sem_buf`（`i2c_bus_worker.c:241-242`）。当单次 I2C 事务硬件超时 >（队列等待 + 200ms 兜底），函数 `return ESP_ERR_TIMEOUT`（`:270-273`）放弃该信号量、调用链一路返回 → **caller 栈帧回退，sem_buf 失效**。
- **后果**：worker 此刻仍卡在 `i2c_master_*`；一旦恢复执行到 `:226-227` 的 `*op.result_out=ret; xSemaphoreGive(op.result_sem)`，写的是已被其它栈帧复用的内存 = **栈 UAF / 随机内存踩踏**。
- **注释自相矛盾**：`:272` 注释自称"接受 leak 换 UAF 安全"，但对**栈上** static 信号量根本换不到——不 delete 只避免提前释放堆，而它在栈上、随函数返回必失效。
- **触发面真实存在**：codec 失电短路 SDA/SCL、4G RF 干扰下 I2C 卡死，驱动自己多处注释提到。
- **✅ 已修复（2026-06-18）**：`result_sem`/`result_out` 及 `write_data`/`read_data` 全部搬进堆分配的 `op_ctx_t`（引用计数=2，caller 与 worker 各 release 一次，最后释放者 free）。caller 超时放弃只释放自己那份，worker 完成时释放另一份 → ctx 在堆上不随 caller 栈失效，杜绝 UAF。独立并发测试 30 万轮（双线程竞争 release）无 double-free / 无 leak。**待真机测 I2C 超时/卡死场景**（codec 失电短路、4G 干扰下）。
- 证据：`components/mydazy__i2c_bus_worker/i2c_bus_worker.c` op_ctx_t(:52) / op_ctx_release(:74) / submit_and_wait(:264) / worker 回写(:251-253)。

### P1-② 弱网三态重连在主循环线程同步阻塞【活体印证已知架构债】
- **现象**：`OnAudioChannelClosed` 用 `Schedule` 投递一个闭包，闭包内 `for(1..3){ vTaskDelay(attempt*500); OpenAudioChannel(); }` —— 累计 `vTaskDelay` 500+1000+1500=**3 秒** + 3 次同步 `OpenAudioChannel()`（含 TLS 握手，弱网下更长）。
- **后果**：`Schedule` 闭包由主循环线程串行执行（`application.cc:401-408`，prio 10）。阻塞期间 `MAIN_EVENT_SEND_AUDIO`（音频上行）/`TOGGLE_CHAT`（按键触屏）/`WAKE_WORD_DETECTED`（唤醒）/后续所有 Schedule 全部排队卡死 → 断网时整机冻结 ≥3s。
- **判定**：🔴 **治标堆叠**（"异步壳 + 同步核"：投递是异步的，执行体把延时+同步连接放进了主循环）。正中项目记忆 `post-prod-stability-2026-06`「弱网主任务阻塞致卡死 P1×2，治标越修越糟、需架构断根」。**调退避参数无效，需把重连移到独立低优先级 task 或 esp_timer 非阻塞回调。**
- **✅ 已修复（2026-06-18）**：改用 esp_timer 退避 + Schedule 回主循环单次连接（仿现有 `ScheduleDelayedWake` 模式）。退避延迟（500/1000/1500ms）落在 esp_timer 不占主循环；连接尝试仍在主循环串行执行 → protocol_ 无跨线程访问（unique_ptr，沿用 `shutting_down_` 保护，避免独立 task 方案的 reset UAF）。主循环不再被 3s vTaskDelay 占用。**待真机测弱网断网恢复**（断网期间按键/触屏/唤醒应即时响应）。
- 残留：单次 OpenAudioChannel 的 TLS 握手仍在主循环（瞬时阻塞），完全异步化需 protocol_ 改 shared_ptr，工作量大、风险高，留待后续单独排期。
- 证据：`application.cc` ScheduleReconnectAttempt/DoReconnectAttempt（SetListeningMode 上方）+ OnAudioChannelClosed(:748)。官方对照 v2.2.4:512-518 仅回 Idle、不重连不阻塞。详见 B-07 深审 §1。

### P1-③ remote_cmd 零鉴权远程遥控通道【安全】
- **现象**：复用对话通道（`type=custom`）下发 ~19 命令做远程运维，入口 `remote_cmd.cc:62` Handle **无任何 token/签名/设备身份校验**，信任边界=「已建立的对话连接」（`application.cc:865`）。
- **后果**：劫持/MITM 对话连接后可远程 `reboot`（砖用户）、`ota`（触发刷机）、`update_prompt`（篡改 AI 人格/prompt 注入，无确认无长度校验）、`wakeword`（改成用户不会喊的词 → 设备"装聋"，远程 DoS 语音）、`sleep`（强制关机）、`tts/ttai`（让设备说任意话/向 AI 注入指令）。**所有危险命令均无二次确认、无频率限流。**
- **缓解方向（不在本轮改）**：危险命令子集（reboot/ota/update_prompt/wakeword）加设备级签名或一次性 nonce；非对话连接来源拒绝；危险写操作加限流。
- 证据：`remote_cmd.cc:62`（入口无鉴权）、`:86-104`（命令表）、`:436-449`（update_prompt 直写）、`:470-489`（wakeword 写 NVS）。详见 B-10 🔒节。

---

## 🔒 安全项汇总（6）

| 模块 | 项 | 风险（攻击者能做什么） | 证据 file:line | 风险级 | 处置(待定) |
|---|---|---|---|---|---|
| OTA/mcp | remote_cmd 零鉴权远程遥控 | 劫持对话连接即可 reboot/ota/改prompt/改唤醒词/强制休眠/说任意话，无确认无限流 | `remote_cmd.cc:62,86-104` | **P1** | 待用户确认 |
| OTA/mcp | update_prompt 无确认无白名单 | 远程把 system prompt 换成任意内容（越狱/诱导），空还能清空；无内容审查无长度上限 | `remote_cmd.cc:436-449` | **P1** | 待用户确认 |
| protocols | 百度 License Key 硬编码默认值 | NVS 未配时落到写死的 32 位 key 编译进二进制（commit `81a11e02b` 产物，与"百度密钥泄露"同源；filter-repo 只清了 git 历史 AK/SK，此 key 仍活在 HEAD 源码） | `websocket_baidu_protocol.cc:115` | P2 | 待用户确认（控制台轮换+改强制NVS注入/无默认值） |
| OTA/mcp | wakeword 远程改写无确认 | 远程改唤醒词模式/文本写 NVS 重启生效 → 设备"装聋"，远程 DoS 语音功能 | `remote_cmd.cc:470-489` | P2 | 待用户确认 |
| protocols | 百度协议注释残留生产端点 | 死注释保留完整 `wss://rtc-aiotgw.exp.bcelive.com/...?a=apprmpwazcyzemj` 域名+appid 明文（AK/SK 已 REDACTED） | `websocket_baidu_protocol.cc:554-556` | P3 | 待用户确认（删注释即可） |
| OTA/mcp | ota 命令无限流 | 远程反复触发 CheckVersion 打断用户（OTA 源 url 取本机 NVS，攻击者改不了源） | `remote_cmd.cc:124-143` | P3 | 待用户确认 |

---

## 🔴 过度优化汇总（14）

| 模块 | 🔴 项 | 我们怎么改的 | 官方 v2.2.4 原实现 | 维护成本/风险 | 处置(待定) |
|---|---|---|---|---|---|
| application | **弱网三态重连主循环阻塞**（=P1-②） | Schedule 闭包内 vTaskDelay 3s + 3 次同步 OpenAudioChannel，全在主循环线程串行 | 仅 Schedule 回 Idle，不重连不阻塞（v2.2.4:512-518） | **P1** 断网冻结按键/唤醒/音频上行 ≥3s；治标，需架构断根 | 待用户确认 |
| application | custom 消息语义重写 | custom → `remote_cmd_->Handle` 命令路由 | custom 仅 ESP_LOGI 日志/扩展占位（v2.2.4:593-601） | P3 占用官方 custom 通道，跨产品线对接需同步约定；不破坏标准消息类型 | 待用户确认 |
| display | 死代码 FinishBootAndShowClock() | 开机 logo 渐出动画函数 24 行，全仓零调用方（实际硬切 SwitchToClockMode） | 无 | P3 死码常驻，误导后人以为开机有渐出 | 待用户确认 |
| display | 死文件 ui/core/managed_timer.h | 159 行 ManagedTimer（功能更全），零 #include（同名相对路径解析到 widgets 版） | 无 | P3 重复实现 159 行死码，改 bug 易改错版本 | 待用户确认 |
| display | 死文件 ui/theme/ui_config.h | 150 行 ScreenConfig 常量全套，零 #include（实际用匿名 ns 的 kColor*）；且注释"2x2 宫格"与真实 3x2、GRID_BTN_SIZE=72 与实现 75 双重脱节 | 无 | P3 150 行死码 + 配置与实现脱节 | 待用户确认 |
| display | 死文件 ui/resources/ui_img_paths.h | 32 行图片路径常量，全仓零引用 | 无 | P3 32 行死码 | 待用户确认 |
| display | 注释与实现不符·开机渐出 | 注释写"Idle 时状态机触发渐出"，实为静态显示无动画（渐出函数是死的） | 无 | P3 注释债 | 待用户确认 |
| display | 注释与实现不符·控制中心呼出 | board 注释仍写"下滑唤起控制中心"，实现已 `if(dy>=0)return` 废弃下滑，改单击状态栏 y<36 | 无 | P3 注释债（轻） | 待用户确认 |
| 自研驱动 | 桌面双击 strike 全链路死代码 | sc7a20h_strike API + update_strike 状态机 + 结构 + 枚举约 80 行；InitializeSc7a20h 从不调用，`t->enabled` 永远 false | N/A 自研 | P3 死码每帧空判 + 维护误导 | 待用户确认（接上双击唤醒 或 删） |
| OTA/mcp | Ota::RequestSwitch 死代码 | 完整定义 /switch 切换通道（NFC/iBeacon 用），全仓无调用方 | 无 | P3 误导有 NFC 切换能力；编译进固件永不执行（安全风险≈0，仅维护成本） | 待用户确认 |
| OTA/mcp | OnSleep 降级死分支 | EnterDeepSleep 后紧跟 `esp_restart()`，注释"未实现降级" | 无 | P3 远程 sleep 实为重启，行为误导；EnterDeepSleep 真休眠则后面是死码 | 待用户确认 |
| OTA/mcp | OnDownload 空实现 | download 命令解析 files/emoji 后只 Log "not yet implemented in V2" | 无 | P3 死功能挂命令表，远程 download 无效果 | 待用户确认 |
| OTA/mcp | remote_cmd.h 文档与代码不符 | 头文件写 update/ota·reload·audio_debug，实际无 reload/audio_debug，漏 edu_pool/mic_calibrate | — | P3 注释陈旧误导对接方（云端按陈旧表下发 reload 被当未知命令丢弃） | 待用户确认 |
| protocols | 百度协议注释残留生产端点（=🔒 P3） | 死注释保留 wss 生产域名+appid 明文 | 无 | P3 信息泄露 + 死注释 | 待用户确认（删注释即可） |

---

## 审计纠偏（与原文档/任务假设不符，已实测订正）

1. **settings GetFloat/SetFloat 用途**：任务假设/`扩展功能清单` 写"用于 Vref 校准（电源域红线）"——**实测错**。该 API 实际用于**音频增益持久化**（input_gain/aec_gain，调用方 `box_audio_codec.cc:23`、`audio_codec.cc:38/65/74`），Vref 校准不经此 API。故它**不属电源域红线**，归 ⚪扩展（实现 ×1000 存 int32 无溢出/精度坑）。
2. **LVGL ManagedTimer double-free 地雷**：记忆 `lvgl-managedtimer-autodelete` 的修复**仍在位**——B-08 实测 widgets 版与 core 版两份 ManagedTimer 的 CreateOnce **均已 `set_auto_delete(false)`**（`widgets/managed_timer.h:39-42`、`core/managed_timer.h:65-68`），display/ 模块**无未防护的 double-free 地雷**。
3. **控制中心析构 UAF**：RAII 成员 ManagedTimer 兜住（对象析构时成员逆序自动 `lv_timer_del`），**非 P 级**，非裸指针。
4. **device_state_machine.cc 归属**：v2.2.4 **已存在**（官方自带，非自研）。我们的改动 = 给 `TransitionTo` 加 `transition_mutex_` 锁（修官方 check-then-act 无锁竞态）+ 补 FatalError/Starting→Idle 转移，属 🟢必要并发加固。
5. **protocol.cc 零定制**：与 v2.2.4 **零差异**，base 协议未改一行。
6. **UDP 打洞真修根因**：MQTT OpenAudioChannel 拿到 UDP 通道发 payload_len=0 空包（`mqtt_protocol.cc:299-306`），让服务器学到 NAT 地址，修了"关 SEND_WAKE_WORD_DATA 后零上行→首句 TTS 无声"，归 🟢必要（非过度）。
