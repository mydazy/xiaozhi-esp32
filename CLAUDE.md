# MyDazy P30 项目规则（量产纪律 · 优先级高于全局 CLAUDE.md）

> 本文件覆盖全局 `~/.../CLAUDE.md` 中的通用规则，仅适用于 mydazy-p30-v32 仓库。
> 架构 SSOT：`docs/p30-architecture.md`（权威源）+ `docs/p30-architecture.html`（视觉版）

## 项目身份
- 产品形态：ESP32-S3 智能 AI 对话终端，1.83" 圆角矩形屏 284×240 + 触摸 + 语音
- 阶段：1→量产（非 0→1）；P30 项目曾因量产前激进改动产生 1 万台呆滞库存——**铁律：保守 > 激进**
- 上游：xiaozhi-esp32（虾哥开源，本仓库 V1 衍生）
- ESP-IDF：5.5（不是 5.4）
- 三 SKU：P30-WiFi（WiFi+加速计+触摸）/ P30-4G（双网+加速计+触摸+GNSS）/ P31（双网+触摸+加速计+NFC+iBeacon+GNSS+耳机）

## 量产纪律（不动 / 不抽 / 必登记）

### 不动状态机
- 12 态 `DeviceState` 不新增枚举（`device_state.h`）
- 22 项功能盘点结果：0 项需要新增状态（见 `docs/p30-architecture.html` § 一.3）
- 真要碰之前先去 § 一.5.0 第一性原理重审，并提交决策记录

### 不抽基类
- 不抽 `ActivitySession` 基类（理由：会话型/事件型/服务型形状不同，违 SRP）
- 不抽 `SceneNavigator` 基类（理由：LVGL screen 已经是 Scene 抽象）
- 真有第 4 个会话型 Activity 时再考虑（见 § 一.5.6 阶段 2 触发判据）

### 必登记三维取值
- 每个新功能必须在 § 一.5.2 落位表登记一行：(A: DeviceState) × (B: ActivityType) × (C: SceneType)
- 任何一列出现新值 = 走对应层的扩展点；A 列要新增 = 必须文档评审

## Flow 双层命名（避免混淆）
- **类名 / 文件名**：`FlowEngine`（与 `AudioService / MusicPlayer / McpServer` 同风格的"X+职能词"）
- **enum 值**：`ActivityType::Flow`（短名）
- **业务概念**：Flow（流程）
- 改名约束：① 保留 `live_companion.start` 等远程命令字段 ② Settings 持久化字段不动 ③ 仅类名+文件名+注释

## 三个心智 enum（已就位 · main/）
- `main/activity_type.h` — ActivityType（kNone / kChat / kFlow / kMusic）
- `main/scene_type.h` — SceneType（8 种 · Clock / Emoji / Player / ConfigQr / ControlCenter / Pomodoro / Todo / RoleSwitcher）
- `main/audio_source.h` — AudioSource（kNone / kTts / kMp3 / kAlarmBell / kPomodoroBell / kReminder）

查询 API：
- `Application::GetCurrentActivity()` / `GetCurrentAudioSource()`
- `UiDisplay::GetCurrentScene()`

新增散落判断 → 必须用 enum 查询，禁止加新 IsXxx flag。

## 硬件事实速查（避免再犯错）
- **三板都有 AXS5106L 触摸**（不要标"仅 P31"）
- WiFi CSI 已驱动 + 默认禁用（NVS enabled=0，远程命令 `{"type":"csi","enabled":true}` 启用）
- iBeacon 仅 P31 实例化
- GNSS 仅 P30-4G/P31（依赖 ML307）
- NFC 仅 P31
- ControlCenter **已完整实现**（main/display/control_center.cc 6 宫格）
- RemoteCmd 14 个 on_*；McpServer 20+ 注册工具
- ESP32-S3：16MB Flash + 8MB PSRAM；内部 RAM 红线 60KB

## FreeRTOS 任务纪律（2026-04-29 双核重平衡后）
- **main/ 下 0 裸 `xTaskCreate`**：grep `xTaskCreate\b` 必须 0 命中（每个 task 必须 Pin Core 或 Static Pin）
- **PSRAM 栈红线**：禁止 PSRAM 栈 + Core 0 组合（cache 共享 SPI · NVS/OTA flash op 触发 Double exception）
- 新增任务必须在 § 四.1 落位表登记（栈/优先级/Core/生命周期）

### 双核分配（2026-04-29 重平衡 · 解决 Task WDT reboot）

**Core 0**（网络协议栈 + 主循环 + 编解码 + 实时 DAC · 占 ~50%）
- 强制（IDF 框架）：WiFi/LWIP/NimBLE/esp_timer/app_main
- 主循环：main_app(P10)
- 编解码：**opus_codec**(P7 · 24K 栈 · 必须留 Core 0 否则 Core 1 爆)
- 实时 DAC：**audio_output**(P10 · 与 codec 写同核)
- 网络/Modem：ml307_net(P5)、MQTT/WS
- 控制 IO：LedEvent(P2)、alarm_manager(P2)、headset_detect/typec_headset/nfc(P31)
- 临时（HTTP）：activation(P2)、flow_load(P1)、status_assets(P4)
- 配网期：dns_server(P5)

**Core 1**（实时音频 AEC + UI + 唤醒 · 占 ~63%）
- 实时音频输入：**audio_input**(P10 · AEC 计算密集 · 高于 LVGL P5)
- AFE 唤醒：audio_communication(P7)、audio_detection(P5 · WakeNet)
- 唤醒上报：encode_wake_word ×2(P2 · 24K INT 静态)
- UI：LVGL(P5)、gfx_core(P4)
- MP3 播放：mp3_dec(P7)、mp3_play/mp3_out(P10)、mp3_dl(P1 PSRAM)
- 临时（HTTP）：stt_post(P1 · INT 静态 · 红线已修)
- 配网期：blufi_wifi(P5)、config_done(P5)、config_switch ×2(P3)、wifi_ap ×4

### 关键设计原则
- **audio_input + AFE Feed 必须 Core 1**（AEC VOIP_HIGH_PERF 30-50ms/帧 · Core 0 装不下）
- **opus_codec 必须 Core 0**（同核会让 Core 1 总占用爆 88% · 跨核通过 mutex queue 0 开销）
- **优先级 P10 高于 LVGL P5**（实时音频抢占 UI · LVGL 帧率 30→25fps 用户无感）

## 量产前 P1 修复清单（review § 5.2）
- [x] `wake_encode` PSRAM 栈 + Core 0 红线 ✅（custom_wake_word + afe_wake_word 双修 · 2026-04-28/29）
- [x] 14 处任意 Core 漂移任务 ✅（grep 0 命中 · 2026-04-29）
- [ ] 单击响应 1.5s 阻塞（OTA 改异步）
- [ ] 欢迎音自动 ToggleChatState（产品评估后修）
- [ ] 临时任务静态栈（activation/flow_load/status_assets 改 xTaskCreateStatic · 减堆碎片）

## 架构文档双源同步
- markdown 是权威源：`docs/p30-architecture.md` · `docs/p30-architecture-review.md`
- HTML 是视觉版：`docs/p30-architecture.html`（128KB · 10 章 · 10 SVG）
- 修改顺序：先改 markdown，再同步 HTML 对应章节
- **不允许只改 HTML**——会导致双源漂移

详细的双源规则在路径作用域文件 `~/.claude/rules/architecture-doc.md`。

## 22 功能优先级（KPI 拉动）

| 优先级 | 项 | 状态 | 工作量 |
|---|---|---|---|
| P0 | 教育卡 + GIF 识字图 | ❌→🟢 | 4-6 周（依赖服务端 schema） |
| P0 | Flow 改名 + ScriptProvider 抽象 | 🟢→✅ | 2 周（依赖产品决策脚本来源）|
| P1 | 闹钟 + 待办语音提醒 | 🟡→✅ | 3-4 周 |
| P1 | 番茄钟 | 🟡→✅ | 2-3 周 |
| P2 | 主屏时间独立 Scene + 激活 QR 强化 | 🟡→✅ | 1-2 周 |
| P3 | 运动唤醒算法 / GPS 上报 / iBeacon 扫描 / NFC 业务 / CSI 感知 | 🟢/🟡 | 各 2-3 周 |
| 后期 | 切换 AI 角色 + 待办清单 UI | ❌ | 各 4-6 周 |

完整盘点见 `docs/p30-architecture.html` § 一.3。

## 上游 rebase 友好原则
- 现有差异 11 处（review § 6）
- 新增模块尽量挂在 RemoteCmd / MCP Tool / LiveCompanion / 新文件中，不动 Application / UiDisplay 主结构
- 每次 rebase 上游前看 § 十"上游 vs 自研"对照表


## 代码任务流程（覆盖全局规则）
量产期紧张，简化为：
1. **静态扫描**：grep + 类型检查
2. **实地修改**：保守原则，零改现有逻辑优先
3. **自我 review**：编译 + 影响面分析 + 给 diff 摘要
4. **必须给"diff 摘要 + 影响面 + 下一步建议"**，不要默认结束
