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

## 量产前 P1 修复清单（review § 5.2）
- [ ] `wake_encode` PSRAM 栈 + Core 0 红线（必修）
- [ ] 单击响应 1.5s 阻塞（OTA 改异步）
- [ ] 欢迎音自动 ToggleChatState（产品评估后修）

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

## 禁区（绝对不动 · review § 5.2）
- 蓝牙配网 BluFi（`main/boards/common/blufi.cpp`）
- WiFi 热点配网（`wifi_board.cc::EnterWifiConfigMode`）
- 下拉手势切换控制中心（`UiDisplay` 手势层）
- 基础绑定 / OTA / MQTT 首次握手
- 分区表 `partitions/v2/`
- 协议兼容层 `BinaryProtocol2/3`

## 代码任务流程（覆盖全局规则）
量产期紧张，简化为：
1. **静态扫描**：grep + 类型检查
2. **实地修改**：保守原则，零改现有逻辑优先
3. **自我 review**：编译 + 影响面分析 + 给 diff 摘要
4. **必须给"diff 摘要 + 影响面 + 下一步建议"**，不要默认结束
