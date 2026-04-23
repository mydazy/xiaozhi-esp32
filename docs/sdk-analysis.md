# xiaozhi-esp32 SDK 全面分析报告

> **分析对象**: `mydazy-p30-v32`（V1 产品线 · P31-GPS 试验田）
> **版本**: v2.2.6（上游 xiaozhi-esp32-189 v1.9.60 衍生）
> **框架**: ESP-IDF 5.5 · FreeRTOS · C++17 · LVGL 9.2.2
> **SKU 覆盖**: P30-4G / P30-WiFi / P31-GPS
> **分析日期**: 2026-04-23
> **方法**: 三视角多 agent 并行深度扫描
>   - 第一遍·架构视角 — `feature-dev:code-explorer`
>   - 第二遍·功能视角 — `Explore (very thorough)`
>   - 第三遍·扩展方向 — `researcher (含 WebSearch)`

---

## 导读

本文从三个独立视角对当前 SDK 做完整扫描，各自回答一个问题：

| 视角 | 回答的问题 | 主要产出 |
|---|---|---|
| 架构 | 模块长什么样？数据怎么流？ | 分层图 / 类层次 / 任务模型 / 启动时序 |
| 功能 | 现在能做什么？各 SKU 差别？ | 8 大类功能矩阵 × SKU / 15 条 RemoteCmd / 禁用清单 |
| 扩展 | 下一步该做什么？ | 上游 gap / 硬件 / AI / 工程化 / 优先级矩阵 |

三视角互为补充：**架构告诉你"改动边界"，功能告诉你"你已拥有什么"，扩展告诉你"下一步投哪"**。建议：产品同学从第二遍读，研发从第一遍读，决策者从第三遍读。

---

---

# 第一遍 · 架构视角

## 1. 分层架构总览

```
┌─────────────────────────────────────────────────────────────────────┐
│  Layer 5: UI 层                                                      │
│  LvglDisplay → LcdDisplay → SpiLcdDisplay → UiDisplay                │
│  ControlCenter / ClockPage / WifiQrPage / ActivationPage / EmotePage │
├─────────────────────────────────────────────────────────────────────┤
│  Layer 4: Application 单例（主事件循环 + 状态机）                      │
│  Application::Run()    EventGroup(13 bit)    DeviceStateMachine      │
│  LiveCompanion / RemoteCmd / Ota                                     │
├─────────────────────────────────────────────────────────────────────┤
│  Layer 3: 核心服务层                                                  │
│  AudioService   Protocol(WebSocket/MQTT)   McpServer                 │
│  WakeWord       AudioProcessor(AFE)        Settings(NVS)             │
│  MusicPlayer                                                         │
├─────────────────────────────────────────────────────────────────────┤
│  Layer 2: Board 抽象层                                                │
│  Board(虚基类) → WifiBoard / Ml307Board → DualNetworkBoard           │
│  AudioCodec(虚基类) → BoxAudioCodec / Es7111AudioCodec               │
│  Display(虚基类) → LvglDisplay → LcdDisplay → UiDisplay              │
├─────────────────────────────────────────────────────────────────────┤
│  Layer 1: HAL / managed_components                                   │
│  ESP-IDF (I2S / I2C / SPI / GPIO / NVS / OTA / WiFi / BLE)           │
│  esp-sr(AFE/Wakenet)  esp_codec_dev  lvgl  esp_websocket_client      │
│  mqtt  cJSON  mbedtls  esp_lcd_touch  esp_lvgl_port                  │
└─────────────────────────────────────────────────────────────────────┘
```

**各层职责边界**

- **Layer 1 (HAL)**：驱动寄存器级操作，完全由 ESP-IDF 和 managed_components 承载，业务代码不直接触碰。
- **Layer 2 (Board 抽象)**：隔离硬件差异（DAC、网络、引脚），向上提供统一的 `AudioCodec*` / `Display*` / `NetworkInterface*`。`Board::GetInstance()` 通过 `create_board()` 工厂在链接期确定具体实现。
- **Layer 3 (核心服务)**：无状态或有限状态单元，不依赖具体硬件型号。`Protocol` 只持有回调，不知道 Application 的状态机。
- **Layer 4 (Application)**：全局唯一单例，持有所有服务对象生命周期。FreeRTOS EventGroup 驱动 13 位事件循环；状态流转由 `DeviceStateMachine` 强制校验。
- **Layer 5 (UI)**：所有 `lv_*` 调用必须在 LVGL 任务上下文；跨任务 UI 更新必须经 `Application::Schedule()` 投递。

## 2. 核心类继承关系

```
Board (virtual base, board.h)
├── WifiBoard              ← WiFi 配网 + BluFi，热点/蓝牙双模
├── Ml307Board             ← Cat.1 4G 模组，AT 命令 + PPP
└── DualNetworkBoard       ← 运行时委托 WifiBoard 或 Ml307Board
    ├── MyDazyP30_4GBoard  ← P30-4G / P30-WiFi，ES8311+ES7210
    └── MyDazyP31Board     ← P31，ES7111+ES7210 + NFC + GPS

AudioCodec (virtual base, audio_codec.h)
├── BoxAudioCodec          ← ES8311(DAC/I2C)+ES7210(ADC/I2C)，走 esp_codec_dev
└── Es7111AudioCodec       ← ES7111(纯 I2S DAC)+ES7210，绕过 esp_codec_dev，直接 I2S

Protocol (virtual base, protocol.h)
├── WebsocketProtocol      ← BinaryProtocol2/3，JSON+二进制混合
├── WebsocketJoyaiProtocol ← JoyAI 平台变体
└── MqttProtocol           ← MQTT(控制) + UDP(音频流)，AES 加密

Display (virtual base, display.h)
└── LvglDisplay
    └── LcdDisplay
        ├── SpiLcdDisplay → UiDisplay   ← P30/P31 三页（时钟/配网/激活）+ 控制中心
        ├── RgbLcdDisplay
        └── MipiLcdDisplay

AudioProcessor (virtual base)
├── AfeAudioProcessor  ← ESP-SR AFE：降噪 + VAD + AEC
└── NoAudioProcessor   ← 直通，无降噪

WakeWord (virtual base)
├── AfeWakeWord        ← Wakenet + AFE (S3 + PSRAM)
├── CustomWakeWord     ← Multinet 自定义唤醒词（"搭子精灵"注入）
└── EspWakeWord        ← 轻量实现（C3 等无 AFE 芯片）
```

## 3. 三条关键数据流

### 3.1 音频上行（麦克风 → 服务器）

```
I2S DMA(RX)
  → AudioCodec::InputData()        [audio_input: Core1/P12 规范, 当前 Core0/P8 ⚠]
  → 重采样 (esp_ae_rate_cvt)        若硬件采样率 ≠ 16kHz
  → AudioProcessor::Feed()         AFE：降噪 / AEC / VAD
  → VAD 回调 → MAIN_EVENT_VAD_CHANGE
  → audio_encode_queue_            std::deque, 上限 2
  → OpusCodecTask                  [opus_codec: Core0/P7 规范, 当前无绑核/P2 ⚠]
  → esp_opus_enc_process()         60ms OPUS 帧
  → audio_send_queue_              std::deque, 上限 40
  → on_send_queue_available()      → MAIN_EVENT_SEND_AUDIO
  → Protocol::SendAudio()          WebSocket/UDP 发送
```

### 3.2 音频下行（服务器 → 扬声器）

```
WebSocket/UDP 接收
  → Protocol::on_incoming_audio_
  → audio_decode_queue_            std::deque, 上限 40
  → OpusCodecTask                  OPUS → PCM
  → audio_playback_queue_          std::deque, 上限 2
  → AudioOutputTask                [audio_output: Core1/P10 规范, 当前 P4 ⚠]
  → 重采样 (output_resampler_)
  → AudioCodec::OutputData()
  → I2S DMA(TX) → 扬声器
```

> OGG 音效走独立 `OggDemuxer` 路径，直接投入 `audio_playback_queue_`，与 OPUS 解码共用 `audio_output` 消费。

### 3.3 协议控制流（JSON 通道）

```
WebSocket/MQTT 网络线程
  → Protocol::on_incoming_json_
  → Application 分发 (按 type 字段)
  → Application::Schedule(lambda)  投入 main_tasks_ 队列（mutex, 上限 32）
  → MAIN_EVENT_SCHEDULE
  → Application::Run() 消费
  → DeviceStateMachine::TransitionTo()
  → MAIN_EVENT_STATE_CHANGED
  → HandleStateChangedEvent()
  → DisplayLockGuard → lv_obj_* 更新
```

`RemoteCmd` 处理 WebSocket `custom` 消息（重启/OTA/音量/TTS/直播伴侣等），同样经 `Application::Schedule()` 回到主循环执行。

## 4. 任务/并发模型

| 任务 | 规范优先级 | 实际优先级 | 核绑定 | 栈类型 | 栈大小 |
|---|---|---|---|---|---|
| main_loop (app_main) | P3 | P10 ⚠ | Core0 | internal | 8KB |
| audio_input (有 AFE) | P12/Core1 | P8/Core0 ⚠ | Core0 ⚠ | internal | 6KB |
| audio_output (有 AFE) | P10 | P4 ⚠ | 无 | internal | 4KB |
| audio_output (无 AFE) | P10 | P4 ⚠ | 无 | internal | 2KB ⚠ |
| opus_codec | P7/Core0 | P2/无绑核 ⚠ | 无 ⚠ | internal | 24KB |
| AFE | P8/Core1 | P3 | 由 esp-sr 管 | internal | 8KB |
| wake_word | P6 | P3 | 无 | internal | 4KB |
| lvgl | P5/Core1 | P5/Core1 | Core1 | internal | 8KB |
| ml307_net | P6/Core0 | P5/无 | 无 | internal | 4KB |
| nfc_detect (P31) | P1 | P1 | Core0 ⚠ | PSRAM ⚠ | 4KB |
| lc_load | P1 | P1 | Core0 | internal | 6KB |
| mp3_dl / mp3_dec | P1 / P7 | 符合 | Core0 | PSRAM / internal | 6KB / 10KB |

> **PSRAM 栈铁律**：持续循环 + Core0 的任务必须用内部 RAM 栈，否则 flash op（NVS/OTA）期间被调度 → double exception（SP=0x60100000）。带 ⚠ 的项目详见 `docs/known-issues.md` 或同路径下的 bug 审计报告。

## 5. 启动流程时序

```
app_main()
│
├─ nvs_flash_init()                         NVS 初始化（含自愈）
│
├─ Application::GetInstance()               懒初始化，创建 EventGroup + clock_timer
│
├─ Application::Initialize()
│   ├─ Board::GetInstance()                create_board() 工厂，构造 P30-4G/P31 板子
│   ├─ Board::Initialize()                 (ctor 内)
│   │   ├─ InitializeI2c()                 I2C 总线 (音频/触摸共用)
│   │   ├─ InitializeSpi()                 SPI 总线 (LCD)
│   │   ├─ InitializeLcd()                 esp_lcd_panel_init + LVGL display
│   │   ├─ InitializeAudioCodec()          BoxAudioCodec / Es7111AudioCodec
│   │   └─ HandleWakeupCause()             深睡唤醒原因判断
│   │
│   ├─ Display::SetupUI()                  创建 LVGL 页面树，启动开机动画
│   │
│   ├─ AudioService::Initialize()          OPUS enc/dec + AFE + 重采样器
│   ├─ AudioService::Start()               启动 audio_input/output/opus 任务
│   ├─ MusicPlayer::Initialize()           MP3 流式播放器
│   │
│   ├─ McpServer::AddCommonTools()         音量/重启/截图 等通用工具
│   ├─ McpServer::AddUserOnlyTools()       用户专属工具
│   │
│   ├─ Board::SetNetworkEventCallback()    绑定 UI 更新回调
│   └─ Board::StartNetwork()               异步启动网络（非阻塞）
│
└─ Application::Run()                      提升优先级到 P10，进入事件循环
    │
    ├─ [NETWORK_CONNECTED] → InitializeProtocol() → Protocol::Start() → Ota::Activate()
    ├─ [WAKE_WORD_DETECTED] → ContinueWakeWordInvoke()
    ├─ [VAD_CHANGE] → 按 ListeningMode 决定停/续监听
    ├─ [STATE_CHANGED] → HandleStateChangedEvent() → Display/LED/音效
    └─ [SCHEDULE] → 消费 main_tasks_ 队列
```

## 6. 配置/编译矩阵

**关键 Kconfig 选项**

| 选项 | 类型 | 作用 |
|---|---|---|
| `BOARD_TYPE_MYDAZY_P31/P30_4G/P30_WIFI` | choice | 决定链接哪个 board 实现 |
| `USE_AUDIO_PROCESSOR` | bool | 启用 AFE 降噪（影响 audio_input 栈/核） |
| `USE_DEVICE_AEC` / `USE_SERVER_AEC` | bool | AEC 模式二选一 |
| `USE_AFE_WAKE_WORD` / `USE_CUSTOM_WAKE_WORD` | choice | 唤醒词引擎 |
| `CUSTOM_WAKE_WORD` | string | 自定义词拼音（当前 `wn9_da1zijing1ling2` = 搭子精灵） |
| `DISPLAY_STYLE` | choice | 默认 / 微信气泡 / 表情动画 |
| `SEND_WAKE_WORD_DATA` | bool | 首包音频是否捎带唤醒词数据 |

**SKU 矩阵（阶梯式差异，从小到大）**

| SKU | 定义 |
|---|---|
| **P30-WiFi**（最小基线） | WiFi + LCD JD9853 + 触摸 AXS5106L + ES8311+ES7210 + SC7A20H 加速度计 + AXP2101 电池管理 |
| **P30-4G** | = P30-WiFi **+ 4G 模组 ML307R**（仅增一项） |
| **P31** | = P30-4G **+ NFC(WS1850S) + GPS(ML307 GNSS)**，且 **DAC 换为 ES7111**（纯 I2S，绕过 esp_codec_dev） |

> NFC 和 GPS 是 **P31 独有**的扩展硬件；P30-4G 和 P30-WiFi 均未贴片。

## 7. 关键外部依赖

| managed_component | 角色 |
|---|---|
| `espressif/esp-sr` | AFE 降噪 + Wakenet 唤醒词 + Multinet 自定义词 |
| `espressif/esp_codec_dev` | BoxAudioCodec 使用；Es7111 绕过其 paired 机制 |
| `lvgl/lvgl` 9.2.2 | UI 渲染，经 `esp_lvgl_port` 集成 |
| `esp-mqtt` / `esp_websocket_client` | 协议层传输底座 |
| `esp-audio-codec` | `esp_opus_enc/dec` + `esp_ae_rate_cvt` 重采样 |
| `esp_lcd_jd9853` | P30/P31 LCD 面板驱动 |
| `esp_lcd_touch_axs5106l`（自定义） | 触摸屏驱动 |
| `esp_nfc_ws1850s` | P31 NFC（I2C） |
| `at_modem` / `ml307_gnss` | ML307 AT 层 + GNSS |

> 架构视角分析完成 —— by code-explorer

---

---

# 第二遍 · 功能视角

## 一、AI 对话核心

| 功能 | 实现 | P30-4G | P30-WiFi | P31 | 状态 | 开关 |
|---|---|---|---|---|---|---|
| 唤醒词（AFE+Wakenet） | `audio/wake_words/afe_wake_word.*` | ✅ | ✅ | ✅ | ✅ | `USE_AFE_WAKE_WORD` |
| 自定义唤醒词（Multinet） | `audio/wake_words/custom_wake_word.*` | ✅ | ✅ | ✅ | ✅ | `USE_CUSTOM_WAKE_WORD` + `CUSTOM_WAKE_WORD` |
| VAD 语音活动检测 | `audio/processors/afe_audio_processor.*` | ✅ | ✅ | ✅ | ✅ | `USE_AUDIO_PROCESSOR` |
| AFE 降噪 / AEC | 同上 | ✅ | ✅ | ✅ | ✅ | `USE_DEVICE_AEC` / `USE_SERVER_AEC` |
| 流式 STT 上传 | `protocols/*.cc` | ✅ | ✅ | ✅ | ✅ | 默认 |
| 流式 TTS 播放 | `audio/audio_service.*` + protocols | ✅ | ✅ | ✅ | ✅ | 默认 |
| OPUS 编解码 | `esp_audio_codec` | ✅ | ✅ | ✅ | ✅ | 编译时 |
| 对话打断 | `application.cc` + `device_state_machine.*` | ✅ | ✅ | ✅ | ✅ | `WAKE_WORD_DETECTION_IN_LISTENING` |
| 唤醒词首包捎带 | `audio/audio_service.cc` | ✅ | ✅ | ✅ | ✅ | `SEND_WAKE_WORD_DATA` |

## 二、网络连接

| 功能 | 实现 | P30-4G | P30-WiFi | P31 | 状态 |
|---|---|---|---|---|---|
| WiFi 连接/重连 | `boards/common/wifi_board.*` | ✅ | ✅ | ✅ | ✅ |
| WiFi 热点配网 | 同上 | ✅ | ✅ | ✅ | ✅ |
| BluFi 蓝牙配网 | `boards/common/blufi.cpp` | ✅ | ✅ | ✅ | ✅ |
| **配网热切换（BluFi ↔ 热点）** | `wifi_board.cc` | ✅ | ✅ | ✅ | ✅ (v2.2.6 新增) |
| 4G (ML307) | `boards/common/ml307_board.*` | ✅ | ❌ | ✅ | ✅ |
| 双网络切换 | `boards/common/dual_network_board.*` | ✅ | ❌ | ✅ | ✅ |
| MQTT 协议 | `protocols/mqtt_protocol.*` | ✅ | ✅ | ✅ | ✅ |
| WebSocket 协议 | `protocols/websocket_protocol.*` | ✅ | ✅ | ✅ | ✅ |
| WebSocket-JoyAI 协议 | `protocols/websocket_joyai_protocol.*` | ✅ | ✅ | ✅ | ✅ |
| OTA 检查/下载/验证 | `ota.*` + `ota_http_download.*` | ✅ | ✅ | ✅ | ✅ |
| 激活/绑定握手 | `ota.cc::CheckVersion` | ✅ | ✅ | ✅ | ✅ |
| ReportStatus 上报（含过滤） | `ota.cc::PostToOta` | ✅ | ✅ | ✅ | ✅ (v2.2.5+ 过滤) |

## 三、音频媒体

| 功能 | 实现 | 状态 | 备注 |
|---|---|---|---|
| 内置提示音（OGG） | `assets/lang_config.h` | ✅ | 多语言切换 |
| MP3 流式下载/播放 | `audio/music_player.*` | ✅ | 两任务：dl(PSRAM) + dec(internal) |
| 音频测试模式（UDP） | `audio/audio_debugger.*` | 🚧 | `USE_AUDIO_DEBUGGER` |
| 音量控制 | MCP `self.audio_speaker.set_volume` | ✅ | — |
| 输入增益（MIC） | `audio/audio_codec.*` + `remote_cmd.cc::OnGain` | ✅ | v2.2.6 加 NVS 持久化（存在类型 bug，见综合结论） |
| 输出增益（DAC） | `audio/audio_codec.*` | ✅ | — |
| OGG/Opus 解码 | `audio/demuxer/ogg_demuxer.*` | ✅ | — |

## 四、UI / 交互

| 功能 | 实现 | P30-4G | P30-WiFi | P31 | 状态 |
|---|---|---|---|---|---|
| 时钟页 | `display/ui_display.cc` | ✅ | ✅ | ✅ | ✅ |
| 聊天页（消息气泡） | 同上 | ✅ | ✅ | ✅ | ✅ |
| 表情动画样式 | `display/emote_display.cc` | ✅ | ✅ | ✅ | ✅ (`USE_EMOTE_MESSAGE_STYLE`) |
| 控制中心（下拉） | `display/ui/widgets/control_center.*` | ✅ | ✅ | ✅ | ✅ |
| 配网 QR 页 | `display/ui_display.cc` | ✅ | ✅ | ✅ | ✅ |
| 双击 QR 切换配网模式 | 同上 | ✅ | ✅ | ✅ | ✅ (v2.2.6) |
| 开机动画 | 同上 | ✅ | ✅ | ✅ | ❌ v2.2.6 已删除 |
| 状态栏（网络/电池/时间） | 同上 | ✅ | ✅ | ✅ | ✅ |
| 表情符号集（TwEmoji 64/128/240） | `display/lvgl_display/emoji_collection.*` | ✅ | ✅ | ✅ | ✅ |
| 触屏（AXS5106L） | 自定义 component | ✅ | ✅ | ✅ | ✅ |
| 按键（Boot/Vol/长按/双击/连击） | `boards/common/button.*` | ✅ | ✅ | ✅ | ✅ |
| LCD 驱动 JD9853 | managed_components | ✅ | ✅ | ✅ | ✅ |

## 五、IoT / 智能家居

### MCP Server

| 方面 | 实现 |
|---|---|
| 核心 | `mcp_server.*` |
| 工具注册 | `McpServer::AddCommonTools()` + `AddUserOnlyTools()` + Board 特定 |
| 当前工具数 | **6 个**（对比 189 有 13 个，存在 gap） |

### RemoteCmd 远程命令（经 WebSocket `custom` 通道）

| 命令 | 处理函数 | 功能 |
|---|---|---|
| `reboot` | `OnReboot` | 重启设备 |
| `ota` | `OnOta` | 触发 OTA 检查并升级 |
| `sleep` | `OnSleep` | 进入低功耗（可选深睡） |
| `reconnect` | `OnReconnect` | 重新连接云端 |
| `wakeup` | `OnWakeup` | 远程模拟唤醒 |
| `tts` | `OnTts` | 播放文本语音 |
| `ttai` | `OnTtai` | 发送文本到 AI |
| `volume` | `OnVolume` | 调整音量 0-100 |
| `gain` | `OnGain` | 调整输入增益 |
| `download` | `OnDownload` | 下载资源/表情包 |
| `vad_config` | `OnVadConfig` | 配置 VAD 参数 |
| `live_companion` | `OnLiveCompanion` | 控制直播伴侣 |
| `stt_url` | `OnSttUrl` | 动态切换 STT 后端（NVS 持久化） |
| `music_play` / `music_stop` | `OnMusicPlay` / `OnMusicStop` | MP3 URL 播放 |

### 其他 IoT

| 功能 | 实现 | 状态 |
|---|---|---|
| 直播伴侣 LiveCompanion | `live_companion.*` | ✅ HTTP 脚本驱动（所有 SKU） |
| NFC (WS1850S) | `components/esp_nfc_ws1850s` | ✅ **仅 P31**（P30-4G / P30-WiFi 未贴片） |
| GPS/GNSS | `boards/common/ml307_gnss.*` | ✅ **仅 P31**（P30-4G 虽有 ML307，但 GNSS 未启用） |
| iBeacon 广播/扫描 | `blufi/ibeacon.*` | 🛑 **已禁用**（注释 TODO: 等 BLE host sync 完成） |
| Press-To-Talk | `boards/common/press_to_talk_mcp_tool.*` | ✅ |

## 六、板级硬件

| 硬件 | P30-WiFi | P30-4G | P31 |
|---|---|---|---|
| 处理器 | ESP32-S3 双核 | ESP32-S3 双核 | ESP32-S3 双核 |
| PSRAM | 8MB (OCT) | 8MB (OCT) | 8MB (OCT) |
| DAC | ES8311 | ES8311 | **ES7111** |
| ADC | ES7210 | ES7210 | ES7210 |
| LCD | JD9853 240×284 | JD9853 240×284 | JD9853 240×284 |
| 触摸屏 | AXS5106L | AXS5106L | AXS5106L |
| 4G | ❌ | **ML307R** | ML307R |
| NFC | ❌ | ❌ | **WS1850S** |
| GPS | ❌ | ❌ | **ML307 GNSS** |
| 加速度计 | SC7A20H | SC7A20H | SC7A20H |
| 电池管理 | AXP2101 | AXP2101 | AXP2101 |

> **实质差异阶梯**：
> - **P30-WiFi → P30-4G**：唯一差异是贴片 ML307R 4G 模组
> - **P30-4G → P31**：加贴 NFC（WS1850S）+ 启用 GPS（ML307 GNSS），DAC 从 ES8311 换成 ES7111
>
> **UI / 音频 / 触摸三大基础能力在所有 SKU 上完全一致**。双网络切换（`dual_network_board`）在 P30-WiFi 上自然无意义；NFC / GPS 相关 MCP tool 和 UI 在 P30-4G / P30-WiFi 上应通过 Kconfig 禁用。

Board 切换通过 `python scripts/release.py <board-name>` 完成。

## 七、工程体系

| 工具 | 路径 | 状态 |
|---|---|---|
| 编译脚本 | `scripts/release.py` | ✅ |
| Kconfig | `main/Kconfig.projbuild` | ✅ |
| 多层 sdkconfig | 根 `sdkconfig.defaults` + board `config.json` | ✅ |
| 分区表 | `partitions/v2/16m.csv`（双 OTA） | ✅ 冻结 |
| CI（GitHub Actions） | `.github/workflows/build.yml` | ✅ 仅编译验证 |
| 字体工具 | `components/78__xiaozhi-fonts/` | ✅ |
| GIF 转换 | 同上 `generate_gifs.py` | ✅ |
| 图像→C 资源 | `scripts/Image_Converter/LVGLImage.py` | ✅ |
| **SDL 模拟器** | — | 🛑 未实现（V2 已有，本仓库 V1 没移植） |
| **单元测试** | — | 🛑 未实现（V2 已有 Catch2） |
| **cppcheck 集成** | — | 🛑 未实现 |
| **CrashReporter / 遥测** | — | 🛑 未实现（UDOUR 已有，本仓库缺失） |

## 八、禁用 / 未完成清单

| 功能 | 现状 | 位置 | 说明 |
|---|---|---|---|
| iBeacon 广播/扫描 | 🛑 已禁用 | `boards/mydazy-p31/mydazy_p31_board.cc:44` | `// InitializeIBeacon();` 等 BLE host sync |
| 自定义唤醒词动态更新 | 📝 占位 | `audio/wake_words/custom_wake_word.cc` | Multinet 支持，运行时更新未完全恢复 |
| 深睡 Board 特定实现 | 📝 占位 | `remote_cmd.cc:54` | `TODO: V2 Board 特定深睡方法` |
| OTA `ProcessCustomContent` | 📝 占位 | `remote_cmd.cc:193` | `TODO: 从 v1.89 移植` |
| LED 指示灯 | 🛑 硬件未设计 | — | 框架预留接口 |
| MCP GPIO 控制 tool | ⚠️ 预留 | `boards/common/board.h` | Board 可扩展，未预装 |

> 功能视角分析完成 —— by explore

---

---

# 第三遍 · 扩展方向视角

## 一、上游对齐差距（Gap Analysis）

### 1.1 xiaozhi-esp32-189 有而本仓库没有

| 模块 | 189 有 | 回移优先级 | 价值理由 |
|---|---|---|---|
| `alarm_manager`（闹钟，699 行） | ✅ | **🔴 高** | 儿童/家庭场景核心需求 |
| `acoustic_calibration`（MIC/REF 双通道自动校准） | ✅ | **🔴 高** | 不同批次麦克风灵敏度差异直接影响 AEC 质量，量产良率关键 |
| MCP tool 扩展（13 vs 6） | ✅ | **🔴 高** | tool 多寡直接决定 AI 能做什么，照搬即可 |
| `wifi/` 子模块（ssid_manager + csi + dns_server） | ✅ | 🟡 中 | CSI 人体检测是差异化卖点 |
| `emotion_gif_cache` | ✅ | 🟡 中 | 表情动画流畅度 |
| `assets_manager`（结构化资源） | ✅ | 🟡 中 | 影响多主题扩展 |
| `lv_mem_psram.c`（LVGL PSRAM 内存池） | ✅ | 🟡 中 | 降低 LVGL 内部 RAM 压力 |
| `websocket_baidu_protocol` | ✅ | 🟢 低 | 按商务需求评估 |

### 1.2 mydazy-p30-v2 有而本仓库没有

| 模块 | V2 有 | 回移可行性 |
|---|---|---|
| `assets_dynamic`（动态资源分区，自由区热更新） | ✅ | 需同步改分区表，2-3 周 |
| `edu_protocol`（LLM 驱动儿童教育 + SM-2 复习算法） | ✅ | 高价值，前置需后端 java_mydazy edu 模块就绪 |
| `media_player`（升级版） | ✅ | API 对齐，工作量低 |
| 独立 `wifi/` 模块目录 | ✅ | 建议随 189 对齐同步 |

### 1.3 mydazy-p30-udour 的 UI 架构

UDOUR 已经走得很深：**14 页面 + `UiPageBase` 基类 + theme/ + widgets/ + `ui_strings` 国际化 + `ui_config` 统一配置**。对比本仓库扁平 UI 结构，差距是架构级。

**正确做法**：不直接 fork 代码（UDOUR 是 V1 独立变体），而是抽离 `UiPageBase` 架构决策移植到本仓库。工作量约 1 个月，消除后续每次加页面都复制生命周期代码的技术债。

## 二、硬件扩展方向

| 方向 | 可行性 | 工作量 | 价值 | 判断 |
|---|---|---|---|---|
| **WiFi CSI 人体接近检测** | ✅ 高（纯软件，189 已实现） | 1 周 | 🔴 高 | 桌宠场景走近自动迎接，无额外 BOM |
| **摄像头（OV2640/GC0308）** | ✅ 框架已就绪（Camera 抽象 + MCP tool） | 2-3 周 + PCB 改板 | 🟡 中 | 儿童场景价值"看到用户"，非视频通话 |
| **超低功耗 ULP** | ✅ UDOUR 已验证 | 2-3 周 | 🟡 中 | 便携版电池续航关键 |
| **BLE Mesh** | ✅ 硬件支持 | 3 个月+ | 🟢 低 | 需云端联动配合，不独立做 |
| **新一代音频（ES8323 / 双麦）** | ✅ 189 已支持 | 换料 + 1 月 | 🟡 中 | 等下一版 PCB 迭代 |

## 三、协议 / AI 能力

### 端侧 LLM 可行性（2025-2026 调研）

**当前结论：暂不做生成式端侧 LLM**

- ESP32-S3 能跑的只有 ~1MB TinyStories 级模型（llama2.c 类，19 token/s），**语义理解不足**
- Phi-3 Mini（3.8B INT4）需 ~2GB 内存，ESP32-S3 只有 8MB PSRAM，差两个量级
- **可行方向**：端侧推理用于分类（情绪识别、命令意图），生成式保持云端

**建议接入 `esp-tflite-micro` 做情绪分类**，工作量 1-2 周。

### MCP 生态（2026 年已成 IoT 事实标准）

按商业价值排序的 tool 扩展：

1. `self.pomodoro.*` + `self.alarm.*` — 从 189 回移，**🔴 最优先**
2. `self.education.show_stroke` / `show_english` — 儿童差异化
3. `self.screen.set_theme` — 对话驱动换主题
4. `self.csi.set_enabled` — 配合 CSI 移植
5. 家居控制（米家/HA） — 家庭助理场景

### WebRTC

2026-01 乐鑫发布 Amazon KVS WebRTC SDK ESP-IDF 移植（Split Mode 针对低功耗）。**对直播伴侣场景有价值**（低延迟双向语音）。当前 WebSocket 已够用，WebRTC 是进阶选项。工作量 1-2 个月。

### 百度协议

189 和 V2 均有 `websocket_baidu_protocol`，本仓库缺失。国内覆盖面大，作为备用协议接入 3-5 天。**风险极低，建议顺带补齐**。

## 四、产品化扩展

| 方向 | 工作量 | 价值 | 前置条件 |
|---|---|---|---|
| 多语言唤醒词 | 2 周 + 商务 | 🟡 中 | 唤醒词商务授权（讯飞/百度 0.5-1 元/台） |
| 主题/皮肤系统 | 3 个月+ | 🔴 高 | 先回移 `assets_dynamic` + 后端资源服务 |
| 儿童/老年/职场模式 | 2-3 月 | 🔴 高 | UDOUR `UiPageBase` + `ui_strings` + V2 `edu_protocol` |
| 直播伴侣 SDK 化（抽为 component） | 1 周 | 🟡 中 | 消除三仓库重复维护技术债 |
| 离线应急模式 | 2-4 周 | 🟡 中 | 需 TFLite 意图分类接入 |

## 五、量产工程化（**最高 ROI 方向**）

| 方向 | 工作量 | 价值 | 判断 |
|---|---|---|---|
| **CrashReporter 移植**（UDOUR→本仓库） | 3-5 天 | 🔴🔴 极高 | 量产设备无遥测等于裸奔，长期稳定维度**强制要求** |
| **差分 OTA**（`esp32_compressed_delta_ota`） | 3-4 周 + 后端 | 🔴 高（仅 4G 设备） | 升级包 ~3.9MB → 200-600KB，4G 流量成本真实存在 |
| 灰度发布（UUID 抽样） | 1 周（后端为主） | 🔴 高 | 量产发版风险控制必需 |
| 配置热更新（经 MQTT） | 1 周 | 🟡 中 | 已有 remote_cmd 框架 |

## 六、开发者生态

| 方向 | 可行性 | 工作量 | 判断 |
|---|---|---|---|
| Web 调试面板（设备本地 HTTP） | ✅ 189 的 `wifi_ap.cc` 可参考 | 2 周 | 🟡 中（现场售后友好） |
| Lua 脚本系统 | ⚠️ 内存挤压（eLua 60-80KB） | 1 月 | 🟢 低（等 ESP32-P4） |
| QuickJS / JS 引擎 | ❌ 约 200KB，不可行 | — | 🛑 当前硬件不做 |
| MCP 工具市场（社区贡献） | ✅ | 3 月+ | 🟡 中（生态建设） |

## 七、优先级矩阵（价值 × 工作量）

|  | **≤ 1 周** | **1 个月内** | **3 个月+** |
|---|---|---|---|
| **🔴 高价值** | • CrashReporter 移植 (UDOUR→本仓库)<br>• MCP tools 补齐 6→13<br>• 百度协议补齐<br>• CSI 人体检测移植<br>• 灰度发布后端对齐 | • `acoustic_calibration` 回移<br>• `alarm_manager` 回移<br>• 差分 OTA (4G 设备)<br>• 配置热更新 | • 儿童模式完整体系 (edu_protocol + 多龄 UI)<br>• 主题动态下发系统 |
| **🟡 中价值** | • `live_companion` 抽独立 component | • WebRTC 音频接入<br>• `UiPageBase` 架构移植<br>• `assets_dynamic` 回移 | • 多语言唤醒词（含商务）<br>• 离线应急模式完整实现 |
| **🟢 低价值** | — | • Web 调试面板<br>• CSI+摄像头联动 | • TFLite 端侧情绪分类<br>• BLE Mesh 联动 |
| **🛑 当前不可行** | — | — | • 端侧生成式 LLM（内存不足）<br>• Lua 脚本系统（内存挤压） |

### 三个最优先项（按四维决策判据排序）

1. **CrashReporter 移植**（3-5 天）
   - 长期稳定维度强制要求，UDOUR 已有现成实现
   - 不加遥测的量产 = 闭眼飞
   - ROI 极高

2. **MCP tools 补齐 + `alarm_manager` 回移**（1 周）
   - 用户角度：闹钟/番茄钟是儿童/家庭必选
   - 行业标准：AI tool 多寡直接影响产品天花板
   - 从 189 照搬成本低

3. **`acoustic_calibration` 回移**（2 周）
   - 行业标准：麦克风自动校准是量产音频设备基础
   - 不同批次麦克风灵敏度差异真实存在，不校准 = 靠出厂调参赌运气
   - 与 P30 量产呆滞教训直接相关

> 扩展方向分析完成 —— by researcher

---

---

# 综合结论（主线交叉验证）

## 三视角交叉发现

### 1. 架构与功能的对齐验证

- 架构描述的"Layer 3 核心服务 = AudioService / Protocol / McpServer"与功能清单的功能数量一致（三大能力各对应一个服务）
- 架构的"DualNetworkBoard 委托模式"准确对应功能清单"双网络切换仅 P30-4G / P31 可用"
- 架构的"UiDisplay 仅 SPI LCD 子类"对应所有三个 SKU（P30-4G/P30-WiFi/P31 都是 JD9853 LCD），UI 功能完全一致

### 2. 功能与扩展的差距交叉

- 功能清单标记"MCP 当前 6 个工具" ↔ 扩展方向指出"189 有 13 个，可照搬"
- 功能清单标记"iBeacon 已禁用 / ProcessCustomContent 占位 / 深睡 Board 未实现" ↔ 扩展方向的"上游对齐差距"中完全吻合
- 功能清单标记"🛑 单元测试 / CrashReporter / SDL 模拟器 未实现" ↔ 扩展方向"工程化 ROI 最高"指向的正是这里

### 3. 架构约束反向验证扩展方向

- 架构的"内部 RAM > 60KB 红线" ↔ 扩展方向"端侧生成式 LLM 不可行"的硬约束
- 架构的"PSRAM 栈陷阱" ↔ 扩展方向"Lua 脚本 60-80KB 挤压"的具体原因
- 架构的"LVGL 任务 + DisplayLockGuard" ↔ 扩展方向"UiPageBase 移植"的改造边界

## 下一步建议（按紧急度 × 重要度）

### 🔴 立刻做（本周 / 下个 commit）

1. **修已知 P0 bug**（见同路径的 bug 审计报告）
   - `audio_service.cc:141` audio_input Core 0→1
   - `audio_service.cc:148,162,170` audio_output/opus_codec 优先级
   - `audio_codec.cc:53` `SetInt(float)` → `SetFloat`
   - `box_audio_codec.cc:191` `SetInputGain` 加硬件实时更新

### 🟡 本 sprint（2-3 周）

2. **CrashReporter 从 UDOUR 移植** — 量产遥测前提
3. **MCP tool 从 189 补齐到 13** — AI 对话天花板提升
4. **禁区改动回归测试**（BluFi/WiFi 配网热切换，v2.2.5→v2.2.6）

### 🟢 下个版本（1-2 月）

5. **`acoustic_calibration` 回移** — 量产一致性兜底
6. **`alarm_manager` + `wifi_csi` 回移** — 产品差异化
7. **`UiPageBase` 架构抽离** — 消除 UI 技术债
8. **差分 OTA 评估**（4G 设备专项）

### ⏸️ 战略评估（Jack 决策）

9. **儿童模式 / 主题动态下发 / WebRTC 直播伴侣** — 3 个月级投入，需对齐商业节奏

## 本文档的局限

- 本分析基于 v2.2.6 **未提交**的工作树状态，落入 release 分支前需重跑
- 优先级矩阵的"工作量"估计基于单人开发，并行可压缩但不线性
- 扩展方向的商业价值评估假设当前产品定位（儿童桌宠 + 家庭助理 + 直播伴侣），重大定位调整需重排
- `memory/` 里 2026-04-17 PSRAM 栈实测记录是本分析关键依据之一，该机制在未来 IDF 版本可能演变

## 相关文档

- `CLAUDE.md` — 项目宪法（代码规范 + 任务规则 + 禁区清单）
- `docs/known-issues.md` — 已知问题追踪
- `docs/blufi.md` / `docs/mqtt-udp.md` / `docs/websocket.md` / `docs/mcp-protocol.md` — 各协议细节
- `docs/custom-board.md` — 新 board 接入指南
- `mydazy-p30-migration-guide.md` — 从上游迁移的历史笔记

---

*本报告由三个独立 agent 并行扫描 + 主线交叉验证产出。Agent transcript 保存在 `/tmp/claude-501/` 下对应 task id 目录。*
