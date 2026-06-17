# RED-ALL — 对比官网总清单（窗口1+2 合并 · 待用户逐项确认）

> 合并自：窗口1（模块1-5，原写在 `PROGRESS.md`）+ 窗口2（模块6-10，`RED-B.md`）。
> 基线 78/xiaozhi-esp32 **v2.2.4**；标尺=量产稳定；**全程只分析、未改一行代码**。
> **一句话**：偏离官方的绝大多数是有真机根因背书的量产加固（内存/脏帧/并发/过放/死值兜底），**不是过度优化**；真正的 🔴 几乎全是死代码与注释债（清掉即可）。但审计额外挖出 **3 个量产必修 P1**。

---

## 🚨 第一档：量产必修 P1（3 项，优先于一切，非"过度优化"但最严重）

| # | 项 | 现象 | 风险 | 断根方向 | 证据 |
|---|----|------|------|---------|------|
| **P1-1** | i2c_worker 超时路径栈 UAF（全新发现） | 超时 `return` 后 caller 栈帧回退，worker 恢复时仍回写已失效的栈上信号量 | 栈 UAF / 随机内存踩踏；codec 失电、4G-RF 卡 I2C 时触发 | result_sem 改堆分配 + worker 释放，或超时置 op 失效标志 | `i2c_bus_worker.c:241-280,226-227` |
| **P1-2** | 弱网三态重连主循环同步阻塞 | Schedule 闭包内 `vTaskDelay 3s + 3×OpenAudioChannel` 全在主循环线程串行 | 断网时按键/唤醒/音频上行冻结 ≥3s；治标越修越糟 | 重连移到独立低优先级 task 或 esp_timer 非阻塞回调 | `application.cc:748-762,401-408` |
| **P1-3** | remote_cmd 零鉴权远程遥控 | ~19 命令入口无 token/签名/设备身份，信任边界=已建对话连接 | 劫持连接即可 reboot/ota/改prompt/改唤醒词/强制休眠，无确认无限流 | 危险命令子集加签名/nonce + 非对话来源拒绝 + 限流 | `remote_cmd.cc:62,86-104,436-449,470-489` |

> P1-2 同时计入下方🔴#6，P1-3 同时计入下方🔒#1。三者**均需架构级修，本轮未改**。

---

## 🔒 第二档：安全项（6，按风险降序）

| # | 模块 | 项 | 攻击者能做什么 | 级别 | 处置(待定) |
|---|------|----|--------------|------|-----------|
| 🔒1 | OTA/mcp | remote_cmd 零鉴权（=P1-3） | reboot/ota/改prompt/改唤醒词/强制休眠/说任意话 | **P1** | 危险命令加签名+限流 |
| 🔒2 | OTA/mcp | update_prompt 无确认无白名单 | 远程把 AI system prompt 换任意内容（越狱/诱导），可清空 | **P1** | 加确认+长度/内容审查 |
| 🔒3 | protocols | 百度 License Key 硬编码默认值 | NVS 未配时落到写死 32 位 key 编进二进制（与"百度密钥泄露"同源，filter-repo 没清 HEAD 这处） | P2 | 控制台轮换 + 改强制 NVS 注入、去默认值 |
| 🔒4 | OTA/mcp | wakeword 远程改写无确认 | 改唤醒词写 NVS 重启生效→设备"装聋"，远程 DoS 语音 | P2 | 加确认 |
| 🔒5 | protocols | 百度协议注释残留生产端点 | 死注释留 `wss://...bcelive.com/...?a=apprmpwazcyzemj` 域名+appid 明文（AK/SK 已 REDACTED） | P3 | 删注释 |
| 🔒6 | OTA/mcp | ota 命令无限流 | 反复触发 CheckVersion 打断用户（源 url 取本机 NVS，改不了源） | P3 | 加限流 |

---

## 🔴 第三档：过度优化（19 项 = 窗口1×5 + 窗口2×14）

### 3A. 死代码 / 注释债（13 项，全 ≤P3，**建议一次性批准清理**，不碰红线、无功能影响）

| # | 模块 | 项 | 内容 | 处置 |
|---|------|----|------|------|
| 🔴1 | wake | 3 个死接口声明 | `audio_service.h:131/134/135` 声明零实现零调用 | 删 3 行声明 |
| 🔴2 | touch | 中值滤波死代码 | `#if 0` 的 median3 整套 + 字段 | 清理整套 |
| 🔴3 | display | `FinishBootAndShowClock()` 死码 | 开机渐出 24 行零调用（实际硬切） | 删 |
| 🔴4 | display | 死文件 `ui/core/managed_timer.h` | 159 行零 #include | 删 |
| 🔴5 | display | 死文件 `ui/theme/ui_config.h` | 150 行零 #include + 配置与实现脱节 | 删 |
| 🔴6 | display | 死文件 `ui/resources/ui_img_paths.h` | 32 行零引用 | 删 |
| 🔴7 | display | 注释债·开机渐出 | 注释写"状态机触发渐出"实为静态 | 改注释 |
| 🔴8 | display | 注释债·控制中心呼出 | 注释写"下滑唤起"实为单击状态栏 y<36 | 改注释 |
| 🔴9 | 驱动 | 桌面双击 strike 死链路 | ~80 行 API+状态机，`enabled` 永 false | 接上双击唤醒 或 删 |
| 🔴10 | OTA/mcp | `Ota::RequestSwitch` 死码 | /switch 切换通道（NFC/iBeacon）零调用 | 删（确认无短期 NFC 计划）|
| 🔴11 | OTA/mcp | OnSleep 降级死分支 | EnterDeepSleep 后紧跟 esp_restart，注释"未实现降级" | 清理误导 |
| 🔴12 | OTA/mcp | OnDownload 空实现 | 解析后只 Log "not yet implemented in V2" | 删命令 或 实现 |
| 🔴13 | OTA/mcp | remote_cmd.h 文档与代码不符 | 头文件命令表陈旧（写 reload/audio_debug 实无，漏 edu_pool/mic_calibrate） | 同步注释 |

> 🔒5 百度注释残留同属死注释，归到安全档不重复。

### 3B. 治标 / 调参（4 项，**需 A/B 实测或硬件确认后再决定回退**）

| # | 模块 | 项 | 我们怎么改的 | 风险/疑点 | 处置 |
|---|------|----|------------|----------|------|
| 🔴14 | audio | underrun 主动 PLC 补偿(~60行) | 队列瞬时欠载时合成 PLC 假帧填充 | 治标（真因疑在内存争抢/保活）+60 行复杂度 | A/B 实测弱网断续率，收益不显著则回退 |
| 🔴15 | audio | SetOutputVolume ×0.95 折扣 | 音量下发前打 0.95 折，magic number 无注释 | "设100实际95"留坑 | 补注释说明依据 或 回退；**需硬件确认 PA 保护诉求** |
| 🔴16 | touch | RF 参数反复微调 | RF_S/N_* 带 v2.2.16 N→M 演进痕迹 | memory 已定性"收敛不掉别再调" | **冻结参数停止微调**（机制保留）|
| 🔴17 | application | custom 消息语义重写（=P1 无关） | custom→remote_cmd 命令路由 | 占用官方 custom 通道，跨产品线对接需同步约定 | 待决策（保留则文档化约定）|

> 🔴6 弱网阻塞（=P1-2）和 🔴(百度注释=🔒5) 已在上面档位，不重复列。

---

## 🛡️ 第四档：默认保留（红线区 · 已确认实现正确 · 不动）

4G 脏帧越界守卫（09-P0 系列）· 运行期过放保护（08-P0-A）· I2C 总线串行化 worker（除 P1-1 缺陷外机制正确）· afe UAF 退出防护 · mp3 player join 防护 · device_state_machine `transition_mutex_` 并发加固 · UDP 打洞空包（修首句 TTS 无声）· wake 链路（已对齐 v2.2.4 零差异）。

---

## 📐 第五档：审计纠偏（6 条 · 订正既有文档/假设 · 存档）

1. **settings GetFloat/SetFloat** 实为**音频增益持久化**（input_gain/aec_gain），**非** Vref 校准、不属电源域红线 → 改归 ⚪扩展。
2. **LVGL ManagedTimer double-free** 修复仍在位：widgets 版 + core 版 CreateOnce 均已 `set_auto_delete(false)`，display/ 无地雷。
3. **控制中心析构** RAII ManagedTimer 兜住，非 UAF、非 P 级。
4. **device_state_machine.cc** v2.2.4 官方自带（非自研），我们仅加锁+补转移。
5. **protocol.cc** 与 v2.2.4 零差异，base 协议未改。
6. **UDP 打洞** 是 🟢必要真修（payload_len=0 空包让服务器学 NAT 地址），非过度。

---

## 统计与建议处置顺序

- **总账**：3 个 P1（必修）+ 6 🔒安全（2 个 P1 级）+ 19 🔴过度（13 死码注释债 + 4 调参 + 弱网阻塞/custom）+ 6 纠偏。
- **建议顺序**：
  1. 🚨 **P1-1/2/3 排期架构修**（最高，独立于"过度优化"）——尤其 P1-1 栈 UAF 量产最难定位。
  2. 🔴 **3A 死码/注释债 13 项一次性清理**（低风险，立即可批）。
  3. 🔒 **安全项 3-6 逐个评估**（百度 key 轮换最紧）。
  4. 🔴 **3B 调参 4 项**——回退前需弱网/硬件实测，别凭空回退。
  5. 🛡️ 红线区**不动**。
