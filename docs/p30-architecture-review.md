# MyDazy P30 架构梳理与量产前评估

> 版本：v2.2.5  · 适用：mydazy-p30-4g / mydazy-p30-wifi（P31 同源，传感器迁移未完成不在本评估范围）  · IDF：5.5+

---

## 1. 项目定位

| 维度 | 内容 |
|---|---|
| 产品形态 | ESP32-S3 智能 AI 对话终端，**1.83" 圆角矩形屏 284×240** + 触摸 + 语音交互 |
| 阶段 | 1→量产，非 0→1（P30 项目曾因量产前激进改动产生 1 万台呆滞库存，铁律：保守 > 激进） |
| 上游 | xiaozhi-esp32（虾哥开源，本仓库属 V1 产品线衍生） |
| 当前 SKU | P30-4G（ML307 4G + WiFi 双网）、P30-WiFi（纯 WiFi）、P31（4G + NFC + GPS + Type-C 耳机） |
| 服务端 | MyDazy 自建（mydazy.cn）+ JoyAI（joyinside.jd.com）+ 第三方（百度 bcelive） |

---

## 2. 功能列表

### 2.1 核心交互
| 模块 | 说明 |
|---|---|
| AI 对话 | 唤醒词 / 触摸 / 按键三种触发；ASR + LLM + TTS 端到端走 WebSocket / MQTT / JoyAI 协议 |
| 唤醒词 | 乐鑫 esp_sr AFE，可 Kconfig 切 listening 中是否继续检测 |
| 触摸交互 | AXS5106L 单击唤醒/打断 TTS，下拉手势进控制中心（手势层禁区） |
| 主屏时钟 | UiDisplay 内联 88px 时间 + 30px 日期 + 星期，依赖 NTP 校时 |
| 全局状态栏 | 信号图标（4G 5 档 / WiFi 4 档）+ 电量 5 档（充电时染色） |
| 配网 | BluFi（默认）+ AP 热点（双击切换），均挂全屏 QR overlay；**禁区** |
| 激活 | 4 G 板内置激活码 / WiFi 板凭服务端下发；ShowActivationPage 全屏 QR + 6 位码 |
| OTA | 差分升级 + 回滚检测；Settings ota.url 可定制 |

### 2.2 业务功能
| 模块 | 文件 | 说明 |
|---|---|---|
| 直播伴侣 LiveCompanion | `main/live_companion.cc` | 自动播放脚本 + 断连自重连，OnAudioChannelClosed 回调时延迟 5s 自动重连 |
| 闹钟 AlarmManager | `main/alarm_manager.cc`（如存在） | Timer 唤醒后立即检查并触发到期闹钟（**P30 当前未读取，需上游同步**） |
| MP3 流播 MusicPlayer | `main/audio/music_player.cc` | 远程 music_play 命令触发，独立任务；触摸单击停止 |
| 资源下载 Assets | `main/assets.cc` | NTP 后从服务端拉资产到分区；UiImageManager 加载 PNG/字体 |
| 远程命令 RemoteCmd | `main/remote_cmd.cc` | STT 文本回传 / 自定义 message 处理 |
| MCP 工具 | `main/mcp_server.cc` + `Application::InitializeProtocol` | 通用工具自动注册；board 自定义工具：`self.audio.set_aec` |

### 2.3 硬件抽象
| 外设 | 驱动 | 接口 |
|---|---|---|
| 音频 codec | ES8311（DAC）+ ES7210（ADC，带回采） | `BoxAudioCodec` (P30-4G/WiFi)，`Es7111AudioCodec` (P31) |
| LCD | JD9853 **1.83" 圆角矩形屏（284×240，4 角圆角 RADIUS=25）** | `mydazy/esp_lcd_jd9853` ESP-Registry 组件 |
| 触摸 | AXS5106L | `mydazy/esp_lcd_touch_axs5106l`（C 风格句柄 API） |
| 加速度计 | SC7A20H（运动检测唤醒） | `mydazy/esp_sc7a20h`（C 风格句柄 API） |
| 4G modem | ML307R（仅 P30-4G/P31） | `78/esp-ml307` |
| 电源 | PowerManager（ADC 电池监控）+ PwmBacklight（背光 PWM） | 公共组件 |
| 按键 | Button（boot + vol_up + vol_down） | `espressif/button` |

### 2.4 板差异（仅业务关心）
| 维度 | P30-4G | P30-WiFi | P31 |
|---|---|---|---|
| 网络 | 4G + WiFi 双网（DualNetworkBoard） | 纯 WiFi（WifiBoard） | 4G + WiFi |
| codec | ES8311 + ES7210 | ES8311 + ES7210 | ES7111 + ES7210 |
| AEC | 支持 device-side（双击切换） | 支持 | 不支持（硬件无回采通道） |
| 三连击 | 4G/WiFi 切换 + 进配网 | 直接进配网 | 进配网 |
| 状态上报 | 电量 + cellular csq + carrier | 电量 + wifi rssi | 电量 + cellular |
| 特有 | — | — | NFC（WS1850S）+ GPS（GNSS）+ Type-C 耳机 |

---

## 3. 关键流程

### 3.1 开机流程（已优化）

```
T0   app_main() [main.cc]
     ├─ NVS init（损坏自动 erase 重建）
     └─ Application 构造 → event_group + clock_timer

T+0a Application::Initialize()
     ├─ SetDeviceState(Starting)
     ├─ display->SetupUI()              ← UiDisplay::SetupUI
     │    ├─ 父类 SpiLcdDisplay::SetupUI（创建 emoji_box / status_bar / container）
     │    ├─ emoji_image_ ← ui_img_start_logo_png  🔵 LOGO 上屏
     │    ├─ CreateGlobalStatusBar()
     │    └─ StartBootAnimation()       🔵 logo fade_in 800ms（持续显示，无 3s 倒计时）
     ├─ vTaskDelay(80ms)                ← 等 LVGL 首帧黑底刷到 GRAM
     ├─ board.GetBacklight()->RestoreBrightness()  ← 背光 PWM 渐入 ~175ms
     ├─ audio_service.Initialize/Start
     ├─ MusicPlayer::Initialize
     ├─ state_machine_.AddStateChangeListener
     ├─ esp_timer_start(clock_timer, 1s)
     ├─ McpServer.AddCommonTools / UserOnlyTools
     ├─ board.SetNetworkEventCallback   ← 4G modem 错误细分 + Alert
     └─ board.StartNetwork()            ← 异步立即返回

T+1  Application::Run()
     ├─ vTaskPrioritySet(self, 10)
     └─ while xEventGroupWaitBits(...)  ← 主事件循环

[网络异步连接...]

T+?  NetworkEvent::Connected → MAIN_EVENT_NETWORK_CONNECTED
     └─ HandleNetworkConnectedEvent
          ├─ Starting → SetDeviceState(Activating)
          └─ xTaskCreate(ActivationTask, 8KB, prio=2)

T+?  ActivationTask（独立任务）
     ├─ CheckAssetsVersion             ← 资产下载 / UiImageManager.LoadAll
     ├─ CheckNewVersion                ← OTA 检查 + 激活码校验
     │    └─ ShowActivationPage(...)   ← 激活态 overlay 覆盖 logo
     ├─ InitializeProtocol             ← MQTT / WebSocket / JoyAI 三选一
     └─ setBits(ACTIVATION_DONE)

T+?  HandleActivationDoneEvent
     ├─ SetDeviceState(Idle)
     │    └─ HandleStateChangedEvent::Idle
     │         └─ FinishBootAndShowClock        ✅ logo fade_out 400ms → 时钟主屏
     ├─ display->ShowNotification("v"+ver)
     ├─ board.SetPowerSaveLevel(LOW_POWER)
     └─ PlaySound(OGG_SUCCESS)
```

**特性**：
- 开机黑屏（无白闪）→ logo 渐入 → logo 持续到激活完成 → 平滑切到时钟主屏
- 网络慢/无网时 logo 不会消失（避免 `--:--` 假死感）
- 配网/激活时全屏 QR overlay 覆盖 logo（视觉无缝）

### 3.2 按键流程

| 触发 | 行为 | 备注 |
|---|---|---|
| 单击 boot | Idle → 唤醒对话；Listening → 退出；Speaking → 打断 | 单击回调中阻塞 1.5s（OGG_WAKEUP 等播完）— P2 优化项 |
| 双击 boot | 恢复出厂确认中 → 执行擦除；否则 → AEC 切换（仅 P30-4G/WiFi，P31 跳过） | 状态分支靠 `waiting_factory_reset_confirm_` atomic |
| 3 连击 boot | P30-4G：4G/WiFi 切换 + 进配网；P30-WiFi：直接进配网 | 进配网调 `EnterWifiConfigMode` |
| 4 连击 boot | 关机（与"长按 5 秒关机"重复入口） | 量产保留 |
| 6 连击 boot | 进入音频测试模式 | 工厂用 |
| 9 连击 boot | 发起恢复出厂请求（10 秒内双击确认） | atomic + timestamp 双段保护 |
| 长按 700ms boot | 录音；Idle/Speaking/Listening → 录音并发送；其他态 → 测试录音 | — |
| 长按 3s boot | 关机倒计时提醒（"再按 2 秒关机"） | 可松开取消 |
| 长按 5s boot | 真正关机（EnterDeepSleep） | — |
| PressUp boot | 取消关机倒计时；停止录音 | — |
| 单击 vol+/- | 音量 ±10 | — |
| 长按 vol+/- | 持续 ±5（独立 vol_adjust 任务，每 200ms） | atomic + 任务句柄保护 |
| 触摸单击 | Idle → 唤醒；Speaking → 打断；MusicPlayer 播放中 → 停 | 见 3.6 |

### 3.3 配网流程（禁区）

```
入口：3 连击 / 首次开机无 WiFi 凭证 / Settings.wifi.blufi=1
   ↓
WifiBoard::EnterWifiConfigMode → SetDeviceState(WifiConfiguring)
   ↓
UiDisplay::ShowWifiQrCode(content, hint, "BluFi", "AP", active=BluFi)
   ↓
[用户手机端配网] → BluFi/AP 收到 SSID+PWD
   ↓
WiFi 连接成功 → NetworkEvent::Connected → HandleNetworkConnectedEvent
   ↓
HideWifiQrCode → SetDeviceState(Activating) → ActivationTask
```

### 3.4 激活流程

```
ActivationTask::CheckNewVersion
  ↓ ota.HasActivationCode/Challenge
ShowActivationCode(code, message)
  ↓ ShowActivationPage(mac, code) - 全屏 QR overlay + 6 位激活码 + 数字逐个播报
[等待用户输入激活码到服务端]
  ↓ ota.Activate() 重试 10 次（每次 3-10s 退避）
HideActivationPage（隐式 — Idle 态 FinishBootAndShowClock 触发清理）
```

### 3.5 深睡流程（4 步拆分）

```
EnterDeepSleep(enable_gyro_wakeup)
  ├─ ShutdownTouchAndAudioForSleep
  │    ├─ axs5106l_touch_del              ← 触摸释放
  │    └─ AUDIO_PWR_EN_GPIO=0 + rtc_hold  ← 音频电源切断（穿越 deep sleep）
  ├─ ConfigureDeepSleepWakeupSources
  │    ├─ EXT0 wakeup: BOOT_BUTTON_GPIO（低电平唤醒）
  │    └─ EXT1 wakeup: SC7A20H_GPIO_INT1（陀螺仪运动唤醒，Settings.pickupWake=1 启用）
  ├─ WifiManager.StopStation                ← P30-WiFi；P30-4G 多一个 if NetworkType==WIFI
  ├─ power_save_timer.SetEnabled(false)
  ├─ DISPLAY_BACKLIGHT=0
  ├─ ResetAllGpiosForSleep                  ← 16 个 GPIO 重置 + 输入模式（降功耗）
  └─ esp_deep_sleep_start()
```

### 3.6 唤醒流程

```
HandleWakeupCause（在 InitializeGpio 末尾调用）
  ├─ EXT0（boot 键）: first_boot=true，CheckBootHoldOnWakeup 验证 2 秒长按
  │                   不满足 → 立即回深睡（防口袋误触）
  ├─ EXT1（陀螺仪）: first_boot=true（运动唤醒）
  ├─ TIMER（闹钟）  : is_alarm_clock=true
  └─ POWERON       : first_boot=true（首次上电/复位）
```

### 3.7 恢复出厂流程

```
9 连击 boot → HandleBootMultiClick9_FactoryReset
  ├─ AbortIfSpeaking
  ├─ waiting_factory_reset_confirm_ = true (atomic)
  ├─ factory_reset_request_time_ = esp_timer_get_time()
  └─ Alert("恢复出厂设置", "10秒内双击确认", "logo", OGG_FACTORY_RESET)

[10 秒内] 双击 boot → HandleBootDoubleClick → 命中 waiting_factory_reset_confirm_ 分支
  ├─ AbortIfSpeaking
  ├─ Alert("确认恢复", "开始执行")
  ├─ vTaskDelay(3000ms)
  ├─ RequestServerUnbind() ← HTTP /reset，5s 超时，失败不阻塞
  └─ SystemReset::DoFactoryReset()
       ├─ ResetNvsFlash       ← nvs_flash_erase
       ├─ ResetToFactory      ← otadata 分区擦除
       └─ RestartInSeconds(3) ← esp_restart
                              ↓
                       ShutdownHandler 自动断 LDO（AUDIO_PWR_EN_GPIO=0 + rtc_hold + 500ms 放电）
                              ↓
                       LCD + 音频 + 4G 完整电源复位（避免 GRAM 残留）
```

### 3.8 状态机

11 个状态（`main/device_state.h`）：

```
Unknown ──→ Starting ──→ Activating ──→ Idle ⇄ Connecting ⇄ Listening ⇄ Speaking
              │              │              ↑
              ↓              ↓              │
           WifiConfiguring  Upgrading ──────┘
                                          AudioTesting / FatalError（终态/错误态）
```

转换由 `DeviceStateMachine::TransitionTo` 校验合法性（`device_state_machine.cc::IsValidTransition`），`AddStateChangeListener` 广播给订阅方（UI 切换 / GPS 自启 / LiveCompanion）。

### 3.9 状态上报

90s 周期 `esp_timer`，仅 idle/listening 态上报：
```json
{
  "battery": 78,
  "charging": false,
  "network": { "type": "wifi" | "cellular", "rssi" | "csq" | "carrier": ... },
  "free_heap": 65432
}
```
通过 `Ota::ReportStatus(json)` 走当前协议（MQTT / WS）上报。

---

## 4. 架构

### 4.1 类层次

```
Board (基类)
 ├── WifiBoard  ─────── MyDazyP30_WifiBoard
 ├── Ml307Board
 └── DualNetworkBoard ── MyDazyP30_4GBoard
                      ── MyDazyP31Board

Display (基类)
 └── LvglDisplay ── LcdDisplay ── SpiLcdDisplay ── UiDisplay  ← P30 三板共用
                                                 ← OledDisplay（P30 不用）
                                                 ← EmoteDisplay（P30 不用，CONFIG 切）

Application（单例）
 ├── DeviceStateMachine     ← 状态机 + Listener 广播
 ├── AudioService           ← AFE / 编解码 / I2S / 唤醒词回调
 ├── Protocol* protocol_    ← MqttProtocol / WebsocketProtocol / WebsocketJoeaiProtocol
 ├── RemoteCmd              ← STT 回传 / custom 消息
 ├── LiveCompanion          ← 直播伴侣
 └── std::unique_ptr<Ota>   ← 仅激活期持有，激活完成后 reset
```

### 4.2 模块边界

```
main/
├── application.cc/h      ← Application 主循环 + 状态分发
├── main.cc               ← app_main 入口（NVS init + Application::Initialize/Run）
├── device_state*         ← 状态机 + 事件广播
├── ota.cc/h              ← OTA 检查 / 激活 / 状态上报
├── mcp_server.cc/h       ← MCP 工具注册中心
├── live_companion.cc/h   ← 直播伴侣业务
├── remote_cmd.cc/h       ← 远程命令
├── assets.cc/h           ← 资产下载 / 校验
├── audio/                ← 音频核心（service / codec / processor / wake_word）
├── display/              ← 显示抽象 + UiDisplay（时钟 + 配网 QR + 激活页）
├── boards/
│   ├── common/           ← Board 基类、WifiBoard、DualNetworkBoard、Backlight、Button、PowerManager、SystemReset
│   ├── mydazy-p30-4g/    ← P30-4G 板级（999 行）
│   ├── mydazy-p30-wifi/  ← P30-WiFi 板级（与 P30-4G 仅网络部分差异）
│   └── mydazy-p31/       ← P31 板级（含 NFC / GPS / Type-C 耳机）
├── protocols/            ← MQTT / WebSocket / JoyAI 协议层（禁区）
└── blufi/                ← BluFi ibeacon（禁区）
```

### 4.3 任务/优先级（FreeRTOS）

| 优先级 | 任务 | 核心 | 栈 | 用途 |
|---|---|---|---|---|
| P12 | audio_input | Core1 | 4096 (internal) | I2S ADC |
| P10 | audio_output | Core1 | 4096 (internal) | I2S DAC |
| P8 | afe | Core1 | 8192 (internal) | AFE 降噪 + 唤醒词检测 |
| P7 | opus_codec | Core0 | 8192 (internal) | 编解码 |
| P6 | wake_word_post / modem | Core0 | 6144 (internal) | 唤醒回调 / 4G AT |
| P5 | LVGL | Core1 | 8192 (internal) | UI 刷新 |
| P3 | main_loop | Core0 | 8192 (internal) | Application::Run（vTaskPrioritySet 升 10） |
| P2 | network/websocket | Core0 | 6144 (PSRAM) | WS/MQTT I/O |
| P1 | activation / vol_adjust / welcome / lc_reconn | 任意 | 2-4KB | 一次性 / 后台 |

### 4.4 内存占用（实测 v2.2.5，P30-WiFi 编译）

| 区 | 占用 | 备注 |
|---|---|---|
| Flash app | 3.4 MB（占 app 分区 87%） | 分区剩 15%（约 600 KB） |
| 内部 RAM 起始 | ~310 KB 可用 | 启动后保留 > 60 KB |
| PSRAM | 8 MB | LVGL 缓冲 / 大字体 / 网络任务栈 |

---

## 5. 量产前评估

### 5.1 优势

- **开机引导完整**：黑屏 → logo → 配网/激活 QR → 时钟主屏，全程无白闪、无 `--:--` 假死
- **状态机解耦**：`DeviceStateMachine` + `Listener` 模式，UI / GPS / LiveCompanion 各自订阅
- **深睡 4 步拆分**：`ShutdownTouchAndAudioForSleep` / `ConfigureWakeupSources` / `ResetAllGpios` / `esp_deep_sleep_start`，每段 ≤ 50 行可测可读
- **关机/重启电源复位**：`ShutdownHandler` 用 ESP-IDF 标准 hook，无需改 base Board，OTA / 出厂复位 / 网络切换全路径覆盖
- **双重保护的恢复出厂**：9 连击发起 + 10s 内双击确认 + atomic + timestamp 防误触
- **logo 资源公共化**：`display/ui_img_start_logo_png.c` 三板共用，CMakeLists 集中管理
- **system_reset 接口分层**：上游默认 `SystemReset(pins)+CheckButtons` 不破坏；新增 `static DoFactoryReset()` 给无专用 GPIO 的板用

### 5.2 风险与建议（按优先级）

#### 🟢 量产已就绪
- **开机白闪** — 已修复（背光延迟到 LVGL 首帧后开启）
- **logo 持续显示** — 已修复（Idle 触发 fade_out，不再固定 3s）
- **死代码** — 已清理（`boot_long_press_confirmed_` / `CleanupDisplay`）
- **恢复出厂确认窗口文案 vs 代码不一致** — 已修复（统一 10 秒）
- **system_reset 调用** — 已重构（`DoFactoryReset` 一行替代两行）

#### 🟡 量产可放行，建议 OTA 后端跟进

| 项 | 现象 | 建议 |
|---|---|---|
| `HandleBootClick` 阻塞 1.5s | 单击回调内 `vTaskDelay(1500ms)`，期间错过其他按键 | 改用 `Schedule + WaitForPlaybackQueueEmpty + ToggleChat`，让回调立即返回 |
| 欢迎音自动 `ToggleChatState` | 首次开机 4.5s 后强制连云端开始对话，吃 4G 流量 | 改为播完 OGG_WELCOME 即结束，由用户主动单击触发对话 |
| `idf_component.yml` 公共依赖污染 | P30-WiFi 拉了 `78/esp-ml307` ~80KB 死代码 | Flash 充裕（剩 15%），不优化也可 |

#### 🔴 后续重构（P2，量产后大版本做）

| 项 | 现象 | 建议 |
|---|---|---|
| P30-4G / P30-WiFi 代码重复 ~85% | 850 行近似副本（按键 / 深睡 / 默认设置 / 欢迎音 / 音量任务） | 抽公共基类 `MydazyP30Common`，唯一差异：网络类型 + ReportStatus 网络部分 |
| 6/9 连击工厂模式入口 | 普通用户记不住，但量产已发货依赖 | Kconfig 工厂模式开关，量产固件关闭普通用户入口 |
| P31 传感器迁移未完成 | 工作树残留旧 C++ 类引用导致编译失败 | 参照 P30-WiFi 已对齐方式补完 |

#### ⛔ 禁区（绝对不动）

- 蓝牙配网 BluFi（`main/boards/common/blufi.cpp`）
- WiFi 热点配网（`main/boards/common/wifi_board.cc::EnterWifiConfigMode`）
- 下拉手势切换控制中心（`UiDisplay` 手势层）
- 基础绑定 / OTA / MQTT 首次握手
- 分区表 `partitions/v2/`
- 协议兼容层 `BinaryProtocol2/3`

### 5.3 量产 checklist（建议烧录前跑）

- [ ] 多 SKU 编译：P30-4G ✅ / P30-WiFi ✅ / P31（待迁移）
- [ ] 内部 RAM 启动后 > 60 KB（heap_caps_get_free_size 验证）
- [ ] 5 台设备开机 → 配网 → 激活 → 时钟 端到端跑通
- [ ] 弱信号区 4G 激活降级路径（10 次重试 + 10s 窗口）
- [ ] 触摸 + 按键 + 触屏单击 + 9连击 + 双击 端到端测试
- [ ] 深睡 → EXT0 / EXT1 / TIMER 三种唤醒源
- [ ] 恢复出厂：9 连击 + 10 秒内双击 → 服务器解绑 → NVS 擦 → 重启
- [ ] OTA：v2.2.4 → v2.2.5 升级 + 回滚检测
- [ ] LCD GRAM 残留检查：连续重启 50 次，无白屏 / 雪花

### 5.4 已知 P1 问题（按 P30 教训框，量产保留 + 下版本 OTA 修复）

| 编号 | 问题 | 触发场景 | 影响 | 处置 |
|---|---|---|---|---|
| P1-1 | 单击响应延迟 1.5s | Idle 单击对话 | 用户感知慢 | OTA 改异步 |
| P1-2 | 欢迎音自动连云端 | 首次开机 4.5s 后 | 4G 流量浪费 | OTA 评估产品需求 |
| P1-3 | P30-4G/WiFi 代码重复 | — | 维护成本 | 大版本重构 |

---

## 6. 与上游 xiaozhi-esp32-189 的关键差异

| 维度 | 上游 v1.9.60 | 本仓库 v2.2.5 |
|---|---|---|
| 入口 | `Application::Start()` 单一调用，event_loop_create 显式 | `Application::Initialize() + Run()` 两段式，自升 prio=10 |
| 网络等待 | 同步阻塞在 `StartNetwork` | 异步 + `MAIN_EVENT_NETWORK_CONNECTED` 事件驱动 |
| 状态机 | 直接 `SetDeviceState` 改成员 | `DeviceStateMachine.TransitionTo` + Listener 广播 |
| 中间状态 | Starting → Idle | Starting → **Activating** → **Upgrading** → Idle |
| OTA + 协议初始化 | 主流程同步串行 | 独立 `ActivationTask`（8KB / prio=2） |
| 资产管理 | 无 | `CheckAssetsVersion + UiImageManager.LoadAll` |
| MP3 流播 | 无 | `MusicPlayer`（远程 music_play 命令） |
| 4G modem 错误 | 简单 | `NetworkEvent` 细分 5 种（Detecting/NoSim/RegDenied/InitFailed/Timeout）+ Alert |
| 健康监控 | 60s 周期 PrintHeapStats | 10s 周期 + clock_tick 同源 |
| 启动日志 | `BOOT_OK v...` ERROR 级 | 暂未补回（建议下版本加） |
| `PostAudioInit` 声学校准 | 有 | 暂未补回（建议下版本评估） |
| 欢迎音 | OGG_WELCOME | OGG_WELCOME + 自动 `ToggleChatState`（待评估） |

---

## 附录：关键路径行号索引

| 入口 | 文件:行 |
|---|---|
| `app_main` | `main/main.cc:14` |
| `Application::Initialize` | `main/application.cc:69` |
| `Application::Run`（主事件循环） | `main/application.cc:178` |
| `Application::HandleStateChangedEvent` | `main/application.cc:906` |
| `Application::ActivationTask` | `main/application.cc:336` |
| `UiDisplay::SetupUI` | `main/display/ui_display.cc:85` |
| `UiDisplay::FinishBootAndShowClock` | `main/display/ui_display.cc:404`（约） |
| `UiDisplay::SwitchToClockMode` | `main/display/ui_display.cc:295` |
| `MyDazyP30_4GBoard::HandleBootDoubleClick`（恢复出厂确认 + AEC 切换） | `main/boards/mydazy-p30-4g/mydazy_p30_4g_board.cc:510` |
| `MyDazyP30_4GBoard::EnterDeepSleep` | `main/boards/mydazy-p30-4g/mydazy_p30_4g_board.cc:442` |
| `SystemReset::DoFactoryReset` | `main/boards/common/system_reset.cc:39` |
| `WifiBoard::EnterWifiConfigMode`（禁区） | `main/boards/common/wifi_board.cc:319` |
