# MyDazy P30 架构剖析

> **版本**：v2.2.7
> **适用**：mydazy-p30-v32 仓库（含 mydazy-p30-4g / mydazy-p30-wifi / mydazy-p31 三块板）
> **ESP-IDF**：5.5
> **生成日期**：2026-04-28
> **v2.2.7 变更**：新增 § 10.5「远期扩展候选：好友组网互动」可行性评估（不动状态机、不抽基类的扩展路径）
> **目的**：从启动到稳态、从硬件到云端，画出整张架构地图。每张图先描述，后落到具体 `file:line`。

---

## 目录

1. [四层架构鸟瞰](#一四层架构鸟瞰)
2. [启动链路（冷启动 → 稳态）](#二启动链路冷启动--稳态)
3. [12 态设备状态机](#三12-态设备状态机)
4. [FreeRTOS 任务拓扑（按核心+优先级）](#四freertos-任务拓扑按核心优先级)
5. [音频数据流（双向，端到端）](#五音频数据流双向端到端)
6. [控制流（JSON 消息流）](#六控制流json-消息流)
7. [Board 抽象 — 三块板差异](#七board-抽象--三块板差异)
8. [第三方依赖与协议层](#八第三方依赖与协议层)
9. [内存分布画像](#九内存分布画像)
10. [耦合度评估](#十耦合度评估)

---

## 一、四层架构鸟瞰

P30 采用经典的「业务 → 服务 → 硬件抽象 → 平台」四层结构。Application 是单例编排中枢，Board 工厂在编译期决定具体硬件实现。

```
┌──────────────────────────────────────────────────────────────────┐
│ L4 业务编排层  Application (singleton)  main/application.cc      │
│  ├─ Schedule() 主事件循环 (P10/Core0)                             │
│  ├─ Activation/OTA 异步任务                                       │
│  └─ DeviceStateMachine 状态机 (12 态)                             │
├──────────────────────────────────────────────────────────────────┤
│ L3 服务抽象层  AudioService │ Protocol │ RemoteCmd │ FlowEngine   │
│  ├─ AudioService     5 任务 (input/output/enc/dec/AFE)            │
│  ├─ Protocol         WebSocket / MQTT+UDP / JoyAI                 │
│  ├─ RemoteCmd        JSON 命令分发 (15 个 on_*)                   │
│  ├─ McpServer        MCP Tool 框架 (20+ 注册工具)                 │
│  ├─ FlowEngine       Flow 流程脚本（原 LiveCompanion，已重命名）  │
│  └─ Ota              CheckVersion / Activate / Upgrade            │
├──────────────────────────────────────────────────────────────────┤
│ L2 硬件抽象层  Board (factory)  main/boards/                      │
│  ├─ Display (LcdDisplay / OledDisplay / UiDisplay)                │
│  ├─ AudioCodec (ES8311 / ES7111 + ES7210)                         │
│  ├─ NetworkInterface (WifiBoard / Ml307Board / DualNetworkBoard) │
│  ├─ Led / Backlight / Camera (可选)                               │
│  └─ Settings (NVS Wrapper)                                        │
├──────────────────────────────────────────────────────────────────┤
│ L1 IDF 与第三方  ESP-IDF 5.5 / FreeRTOS / LVGL                    │
│  ├─ components/espressif__esp-sr  (WakeNet / AFE)                 │
│  ├─ components/78__xiaozhi-fonts  (字体打包)                      │
│  ├─ components/mydazy__esp_mp3_player                             │
│  └─ components/esp_nfc_ws1850s    (P31 专属)                      │
└──────────────────────────────────────────────────────────────────┘
```

**分层原则**：
- L4 只与 L3 接口对话，不直接操作硬件
- L3 通过 L2 的基类接口屏蔽板间差异
- L2 基类定义合同，子类落地（Board / AudioCodec / Display 都是工厂模式 + 虚函数）
- L1 是 IDF 框架与第三方组件，固定不动

---

## 二、启动链路（冷启动 → 稳态）

从上电到 `kDeviceStateIdle`（可对话稳态），共经历 7 个初始化步骤 + 1 次异步激活。

```
app_main()  main/main.cc:14
  │
  ├─► esp_event_loop_create_default()
  ├─► nvs_flash_init()
  └─► Application::GetInstance().Initialize()    application.cc:69
        │
        ├─[1]─► Board::GetInstance()  (DECLARE_BOARD 工厂)
        │       └─► p30-4g / p30-wifi / p31 之一被实例化
        │
        ├─[2]─► display_->SetupUI()              ← 80ms 背光延迟
        ├─[3]─► audio_codec_->Initialize()       ← I2C 配寄存器
        ├─[4]─► audio_service_.Start()           ← 5 个任务起飞
        ├─[5]─► state_machine 注册 callback
        ├─[6]─► Board::SetNetworkEventCallback()
        └─[7]─► Board::StartNetwork()            ← 异步：WiFi or 4G
                                │
                                ▼ 网络上线后
                Application::HandleNetworkConnectedEvent()
                                │
                                ▼ 状态切到 Activating
                ActivationTask (P2, 8KB stack)   application.cc:297
                  ├─► Ota::CheckVersion()   ─── HTTP /v1/ota/check
                  ├─► Ota::Activate()       ─── HTTP /v1/ota/activate
                  └─► InitializeProtocol()  ─── 选 MQTT 或 WS
                                │
                                ▼
                      kDeviceStateIdle  (稳态)
```

**关键时序点**：
- 第 [7] 步是异步的，第 [2-6] 步在 `Initialize()` 里同步顺序执行
- 如果 board 的 `StartNetwork()` 阻塞，主流程会被卡住——这就是 4G 模组检测最坏 90s 浪费的来源（ML307 detect 30 次 × 1s + register 6 次 × 10s）
- 背光 80ms 延迟用于跳过 LVGL 首帧黑底刷新，避免开机黑屏闪烁
- ActivationTask 是临时任务，激活完成后自删
- **ML307R 上电稳定 1500ms**（已迁移到 `ml307_board.cc::NetworkTask`，2026-04-28）：GPIO9 级联开关同时上电 LCD/音频/4G 三负载，ML307R boot 完成（800-1200ms）后才能响应 AT；不等会触发 detect 重试 30 次（最坏 30s 浪费）。WiFi 板（mydazy-p30-wifi）不走此路径，启动同步段省 1.5s
- **架构债登记**：Initialize 是 monolithic God Function（100+ 行 / 6 种关注点），量产前 PR-A 拟做 Phase 化重构（详见 HTML 视觉版 § 二.3 评审决策记录）

---

## 三、12 态设备状态机

设备状态机定义在 `device_state.h:4`，转换规则在 `device_state_machine.cc`，事件分发在 `device_state_event.cc`。

```
                    ┌──────────────┐
                    │   Unknown    │（构造默认）
                    └──────┬───────┘
                           ▼
                    ┌──────────────┐
                    │   Starting   │
                    └──┬─────────┬─┘
                       │         │
              ┌────────▼──┐   ┌──▼──────────────┐
              │ WifiConfig │   │   AudioTesting   │
              └─────┬──────┘   └──────┬───────────┘
                    │                 │
                    ▼                 ▼
                ┌────────────────────────┐
                │      Activating        │◄──── 网络重连
                └──────────┬─────────────┘
                           │
            ┌──────────────┼──────────────┐
            ▼              ▼              ▼
        ┌──────┐      ┌──────────┐   ┌─────────┐
        │ Idle │◄────►│Connecting│   │Upgrading│
        └─┬──┬─┘      └────┬─────┘   └─────────┘
          │  │             │
          ▼  ▼             ▼
    ┌────────┐      ┌───────────┐
    │ Listen │◄────►│ Speaking  │
    └────────┘      └───────────┘

                    ┌──────────────┐
                    │ FatalError   │ (终止态)
                    └──────────────┘
```

**12 态枚举**（`device_state.h`）：

| 编号 | 状态 | 含义 |
|------|------|------|
| 0 | `kDeviceStateUnknown` | 初始态，构造默认值 |
| 1 | `kDeviceStateStarting` | 启动中 |
| 2 | `kDeviceStateWifiConfiguring` | WiFi 配网模式（AP+DNS） |
| 3 | `kDeviceStateIdle` | 空闲，可监听/被唤醒 |
| 4 | `kDeviceStateConnecting` | 正在连接服务器 |
| 5 | `kDeviceStateListening` | 监听音频上传中 |
| 6 | `kDeviceStateSpeaking` | TTS 播放中 |
| 7 | `kDeviceStateUpgrading` | 固件升级中 |
| 8 | `kDeviceStateActivating` | 激活/绑定中 |
| 9 | `kDeviceStateAudioTesting` | 音频测试模式 |
| 10 | `kDeviceStateFatalError` | 致命错误，单向终止态 |

**关键转换规则**（`IsValidTransition`）：

| 当前状态 | 可转移到 | 触发事件 | 处理函数 |
|---------|---------|---------|---------|
| Starting | WifiConfiguring / Activating | WiFi 扫描 / 网络已连接 | `HandleNetworkConnectedEvent()` |
| WifiConfiguring | Activating / AudioTesting | 网络连接 / 用户测音 | 同上 |
| Activating | Idle / Upgrading | OTA 检查完成 | `HandleActivationDoneEvent()` |
| Idle | Connecting | 手动/唤醒触发 | `HandleStartListeningEvent()` |
| Connecting | Listening | WS/MQTT 连接成功 | `Protocol::on_audio_channel_opened_` |
| Listening | Speaking | 服务器推流 | `AudioService::on_incoming_audio_` |
| Speaking | Listening / Idle | TTS 播放完成 | `protocol_->on_audio_channel_closed_` |
| Idle | Upgrading | OTA 检查发现新版本 | `CheckNewVersion()` |
| 任意 | FatalError | 不可恢复错误 | 单向，需重启才能脱出 |

**广播机制**：`DeviceStateMachine::TransitionTo()` 同步遍历 `observers` 列表，调用所有已注册的 `StateCallback`。订阅方包括 Application（更新 UI）、FlowEngine（脚本同步）、RemoteCmd（响应远程查询）。

---

## 四、FreeRTOS 任务拓扑（按核心+优先级）

任务按 CLAUDE.md 规范分配：网络与 OTA 上 Core 0，音频实时与 LVGL 上 Core 1。优先级从 P12（最高实时）到 P1（后台）。

```
Core 0（网络 / modem / OTA / main loop 主战场）        Core 1（音频实时 / LVGL）
┌────────────────────────────────────────┐    ┌─────────────────────────────┐
│ P10  main_app          (8KB INT)       │    │ P12  audio_input    INT     │
│ P8   audio_input  (当前在 Core0,见下注) │    │ P10  audio_output   INT     │
│ P7   opus_codec   (当前在 Core0)       │    │ P8   AFE processor          │
│ P6   wake_word    (✅ 已改 INTERNAL)   │    │ P5   LVGL                   │
│ P4   wifi/modem/lwip                   │    │ P3   audio_communication    │
│ P3   tcpip                             │    │ P2   headset_detect         │
│ P2   activation (临时, PSRAM 可)       │    │ P1   后台                   │
│ P1   stt_post (临时, PSRAM 可)         │    └─────────────────────────────┘
└────────────────────────────────────────┘
```

**任务清单**（关键 17 个）：

| 任务名 | 文件 | 栈 (字节) | 优先级 | 核心 | 生命周期 | 功能 |
|-------|------|----------|--------|------|---------|------|
| `main_app` | `main.cc:14` | 8192 | 10 | Core 0 | 永久 | `Application::Run()` 主事件循环 |
| `audio_input` | `audio_service.cc:133` | 6144 (P_USE_PROC) / 4096 | 8 | Core 0 | Start→Stop | I2S 采集 + 唤醒词 |
| `audio_output` | `audio_service.cc:140` | 4096 | 4 | 任意 | Start→Stop | PCM → DAC DMA |
| `opus_codec` | `audio_service.cc:147` | 24576 | 7 | Core 0 | Start→Stop | Opus 编/解码 |
| `audio_communication` | `processors/afe_audio_processor.cc` | 4096 | 3 | Core 1 | Start→Stop | AFE 回声消除 |
| `encode_wake_word` | `audio/wake_words/afe_wake_word.cc:179` | 24KB, **INTERNAL 栈** | 2 | 任意 | 一次性（栈复用） | 唤醒词 Opus 编码上报（✅ 2026-04-28 修：栈 PSRAM→INTERNAL，避开 cache 禁用窗口崩溃） |
| `activation` | `application.cc:297` | 8192 | 2 | 任意 | 网络上线→完成 | OTA 激活 + 协议初始化 |
| `stt_post` | `remote_cmd.cc` | 4096 | 1 | Core 0 | 触发→完成 | STT 文本回调 HTTP POST |
| `flow_load` | `flow_engine.cc` | 6144 | 1 | Core 0 | 触发→完成 | FlowEngine HTTP 脚本加载（一次性任务，加载完成自删） |
| `lvgl_task` | (esp_idf 内置) | 配置项 | 5 | Core 1 | 永久 | LVGL 图形引擎 |
| `wifi_task` | (esp_idf 内置) | 配置项 | 23 | Core 0 | 网络启动→停止 | WiFi 驱动 |
| `tcpip_thread` | (esp_idf 内置) | 配置项 | 18 | Core 0 | 永久 | LWIP TCP/IP |
| `modem_task` | `boards/common/ml307_board.cc` | 4096 | 5 | Core 0 | 网络启动→停止 | 4G AT 命令 |
| `LedEvent` | `led/gpio_led.cc:85` | 2048 | 5 | 任意 | 永久 | LED 状态机 |
| `dns_server` | `wifi/dns_server.cc` | 2048 | 5 | Core 1 | AP 模式中 | 配网 DNS |
| `headset_detect` | `boards/common/typec_headset.cc` | 3072 | 3 | Core 0 | Init→Destroy | Type-C 耳机 |
| `clock_timer` | esp_timer 服务 | 3584 | 22 | Core 0 | 永久 | 周期 tick |

**任务间通信**：
- 跨任务事件：`xEventGroup`（`event_group_` 在 Application 与 AudioService 各一份）
- 队列：`xQueue` 用于音频包；`std::deque` + `std::mutex` 用于 `Application::Schedule()`
- 异步回调：`Schedule(lambda)` 把闭包压队 + 触发 `MAIN_EVENT_SCHEDULE`
- 协议 → 应用：`Protocol::on_incoming_audio_` / `on_incoming_json_` 直接回调

**主事件循环优先级**（`application.cc:Run`）按事件 bit 顺序处理：

```
1. MAIN_EVENT_ERROR              → 显示错误告警
2. MAIN_EVENT_NETWORK_*          → 更新 UI / 重启协议
3. MAIN_EVENT_ACTIVATION_DONE    → 启动监听或 OTA
4. MAIN_EVENT_STATE_CHANGED      → 通知 FlowEngine / UI
5. MAIN_EVENT_TOGGLE_CHAT        → 手动模式切换
6. MAIN_EVENT_START/STOP_LISTENING → 音频通道控制
7. MAIN_EVENT_SEND_AUDIO         → Protocol::SendAudio() (批量)
8. MAIN_EVENT_WAKE_WORD_DETECTED → 自动进入 Listening
9. MAIN_EVENT_VAD_CHANGE         → LED 状态更新
10. MAIN_EVENT_SCHEDULE          → 回调队列执行
11. MAIN_EVENT_CLOCK_TICK        → 状态栏 + 堆栈统计 (10s)
```

---

## 五、音频数据流（双向，端到端）

录制和播放是两条独立的链路，通过 `AudioService` 内部队列串接 5 个任务。

```
录制路径                                   播放路径
─────────                                 ─────────
🎙 MIC → ES7210 ADC                       MQTT/WS 入帧
   │                                       │
   ▼ I2S DMA (INT RAM)                     ▼ on_incoming_audio_ → decode_queue
audio_input_task                           │
   ├─► WakeWord (AFE WakeNet)             audio_opus_dec_task
   ├─► AFE (AEC/NS/VAD)                    ├─► Opus_Decoder::Decode()
   └─► encode_queue                        └─► playback_queue
       │                                       │
       ▼                                       ▼
audio_opus_enc_task                       audio_output_task
   ├─► Opus_Encoder::Encode (60/80/100ms) ├─► 重采样
   └─► send_queue                         └─► I2S Write → DAC DMA
       │                                       │
       ▼                                       ▼
Application::Run() 拉取                    🔊 ES8311 → SPK
   ├─► MAIN_EVENT_SEND_AUDIO
   └─► protocol_->SendAudio()
       │
       ▼
   ☁ MQTT/WS → 云端
```

**关键队列上限**（`audio_service.h:40-45`）：

```cpp
#define MAX_ENCODE_TASKS_IN_QUEUE  2
#define MAX_PLAYBACK_TASKS_IN_QUEUE 2
#define MAX_DECODE_PACKETS_IN_QUEUE (2400 / OPUS_FRAME_DURATION_MS)
#define MAX_SEND_PACKETS_IN_QUEUE   (2400 / OPUS_FRAME_DURATION_MS)
#define MAX_TIMESTAMPS_IN_QUEUE     3
```

**编码参数**：
- 采样率：24 kHz mono 16 bit
- 帧长：60 / 80 / 100 ms（Opus）
- 自适应：根据 `MAIN_EVENT_SEND_AUDIO` 拉取节奏决定

**硬件资源共享**：
- I2S Master 总线（ESP32 ↔ ES8311+ES7210）：MCLK / BCLK / WS / DIN / DOUT
- I2C_NUM_1 共用：AudioCodec（ES8311/ES7210/ES7111）+ Touch（AXS5106L，三板均有）
- DMA 缓冲：必须内部 RAM（PSRAM 不可 DMA 直接访问）

---

## 六、控制流（JSON 消息流）

控制流（区别于音频流）走 JSON 消息，由 Protocol 解析后分发给 RemoteCmd / McpServer / FlowEngine。

```
☁ Server                                  📱 远程命令/激活码
   │                                          │
   ▼ MQTT Topic / WS Frame                    ▼
Protocol::on_incoming_json_(cJSON*)        Ota::CheckVersion 响应
   │                                          │
   ▼                                          ▼
RemoteCmd::Handle(json)                    Settings 持久化
   ├── on_reboot                              │
   ├── on_ota                                 ▼
   ├── on_volume                          NVS partition (Flash)
   ├── on_tts             ←─────  也可能由 ────┤
   ├── on_music_play              MCP Server 触发
   ├── on_set_url                            │
   └── on_*                                   ▼
       │                                  McpServer::HandleRequest
       ▼                                      ├── press_to_talk
   立即执行 / Schedule()                      ├── set_volume
                                              └── ...
```

**消息分类**：

| 来源 | 入口 | 处理者 | 典型消息 |
|------|------|--------|---------|
| 服务器主动推送 | `Protocol::on_incoming_json_` | `RemoteCmd::Handle` | 重启 / OTA / TTS / 音量 / URL 切换 |
| 服务器音频流 | `Protocol::on_incoming_audio_` | `AudioService::EnqueueDecodeTask` | Opus 帧 |
| OTA 检查响应 | `Ota::CheckVersion` 回调 | `Settings::Set*` 持久化 | MQTT/WS 配置 / 自定义资源 URL |
| MCP Tool 调用 | `McpServer::HandleRequest` | 注册的 Tool 函数 | press_to_talk / 音量 / 设备信息 |
| Flow 脚本 | `FlowEngine::StartWithScript` (`flow_engine.cc`) | 内部调度循环 | TTS 序列 / 状态机变更 |
| NFC 卡读取（P31） | NFC ISR → `Ota::RequestSwitch("nfc", data)` | 服务端 | 激活/切换 |

**分发规则**：`RemoteCmd::Handle` 根据 JSON `type` 字段分派；耗时操作（HTTP / Flash 写）通过 `Application::Schedule()` 异步执行，避免阻塞协议接收任务。

---

## 七、Board 抽象 — 三块板差异

三块板共享 80% 公共代码（`boards/common/`），差异主要集中在外设选型和电源时序。

| 维度 | mydazy-p30-4g | mydazy-p30-wifi | mydazy-p31 |
|------|---------------|-----------------|-----------|
| 网络 | 4G+WiFi 双网（DualNetworkBoard） | 仅 WiFi（WifiBoard） | 4G+WiFi 双网 |
| 4G 模组 | ML307R | — | ML307R |
| 音频 DAC | ES8311 | ES8311 | **ES7111**（更新型号） |
| 音频 ADC | ES7210 | ES7210 | ES7210 |
| 触摸屏 | **AXS5106L (I2C)** | **AXS5106L (I2C)** | **AXS5106L (I2C)** |
| NFC | 否 | 否 | **WS1850S** (esp_nfc_ws1850s) |
| 加速度计 | SC7A20H | — | SC7A20H |
| 唤醒源 | 按键 | 按键 | 按键 + 触摸 + 加速度 |
| 板专属代码量 | ~1000 行 | ~1015 行 | ~1300+ 行 |

**工厂模式**：每块板用 `DECLARE_BOARD(MyDazyP31Board)` 宏注册。编译期根据 Kconfig 的 `BOARD_TYPE_*` 选择唯一 `create_board()` 实现，运行期由 `Board::GetInstance()` 返回单例。

**公共抽象层**（`boards/common/`）：

```
boards/common/
├─ board.h / board.cc           Board 基类（虚函数定义）
├─ wifi_board.cc                WiFi 模式实现（AP + STA）
├─ ml307_board.cc               4G 模式实现（ML307R 驱动）
├─ dual_network_board.cc        WiFi+4G 切换调度
├─ ml307_gnss.h                 ML307 内置 GNSS（P32 用）
├─ lamp_controller.h            灯效控制
├─ typec_headset.cc             Type-C 耳机检测
├─ wifi_ap.cc                   配网热点 + DNS
└─ blufi/                       BluFi 配网（备选方案）
```

**板专属代码**（每板一个 `*_board.cc`）覆盖：
- GPIO 引脚映射（在 `config.h`）
- 电源时序（开机/深睡/唤醒）
- 外设初始化（触摸 / NFC / 加速度计）
- 自定义 LED 灯效

---

## 八、第三方依赖与协议层

### 8.1 components/ 第三方组件

```
components/
├─ espressif__esp-sr           ESP 官方音频前端
│   ├─ WakeNet (唤醒词模型 ~1.5MB PSRAM)
│   ├─ MultiNet (自定义唤醒词 ~1MB PSRAM)
│   └─ AFE (AEC + NS + VAD)
├─ 78__xiaozhi-fonts           虾哥字体打包
│   └─ FontAwesome + CJK + Emoji  (~2MB Flash)
├─ mydazy__esp_mp3_player      自研 MP3 流式播放
│   └─ libmad 解码 → playback_queue
└─ esp_nfc_ws1850s             P31 专用 NFC
```

**字体加载**（`assets.cc`）：
- `FLASH_DEFAULT_ASSETS` → `/assets/default_assets.bin`（预打包）
- `FLASH_EXPRESSION_ASSETS` → `/assets/expression_assets.bin`（emote 风格）
- `FLASH_CUSTOM_ASSETS` → HTTP 下载自定义 .bin

### 8.2 Protocol 三选一

由 `Ota::CheckVersion` 返回的服务端配置决定使用哪个协议（互斥，运行期一次决定）。

| 协议 | 实现 | 特点 |
|------|------|------|
| **WebsocketProtocol** | `protocols/websocket_protocol.cc` | esp_websocket_client + TLS；JSON 控制 + Opus 二进制混合；自适应帧率 |
| **MqttProtocol** | `protocols/mqtt_protocol.cc` | esp_mqtt_client；订阅 `<topic>/audio` 推 Opus、发布 `<topic>/response` 推 JSON；保持长连接 |
| **WebsocketJoyaiProtocol** | `protocols/websocket_joyai_protocol.cc` | Joyai 兼容层（京东系服务端） |

**Protocol 基类合同**（`protocols/protocol.h:44`）：

```cpp
class Protocol {
    virtual bool Start() = 0;
    virtual bool OpenAudioChannel() = 0;
    virtual void CloseAudioChannel(bool send_goodbye) = 0;
    virtual bool SendAudio(AudioStreamPacket) = 0;
    virtual bool IsAudioChannelOpened() const = 0;
    void SendWakeWordDetected(const std::string& word);
    void SendStartListening(ListeningMode mode);
    void SendStopListening();

    // 注册回调
    void OnIncomingAudio(...);
    void OnIncomingJson(...);
    void OnAudioChannelOpened(...);
    void OnAudioChannelClosed(...);
    void OnNetworkError(...);
};
```

**二进制协议帧**：

```
BinaryProtocol2  : version(2B) + type(2B) + timestamp(4B) + payload_size(4B) + payload[]
BinaryProtocol3  : type(2B) + payload_size(2B) + payload[]    （简化版本）
```

解析入口在 `websocket_protocol.cc:112-147`，按 `version_` 字段路由到 v2/v3。

---

## 九、内存分布画像

ESP32-S3 配置：16MB Flash + 8MB PSRAM。`SPIRAM_MALLOC_RESERVE_INTERNAL=98304`（96KB），高于 CLAUDE.md 红线 64KB。

### 9.1 内部 RAM（约 320KB 可用）

| 区域 | 估算 | 大头 |
|------|------|------|
| IDF 系统 | ~80KB | WiFi 协议栈 RX (20KB) + LWIP TCP/IP (30KB) + RTOS 内核 (30KB) |
| 音频系统 | ~60KB | Opus 编/解码状态 (各 10KB) + I2S DMA ping-pong (~30KB) + PCM 队列 (10KB) |
| 任务栈分配 | ~100KB | audio_input 8KB + audio_output 4KB + main_app 8KB + activation 8KB + 其他 |
| 堆分配 | ~80KB | Protocol 对象 (10KB) + 接收缓冲 (20KB) + JSON 临时 (5KB) + 字符串 (20KB) + 杂项 |
| 余量预留 | ~30KB | 启动新任务的安全水位 |

**红线**：`free_internal < 60KB` 时风险显著（CLAUDE.md 规范）。

### 9.2 PSRAM（8MB）

```
┌─────────────────────────────────────────────────────────┐
│           PSRAM (8MB)                                    │
├─────────────────────────────────────────────────────────┤
│ LVGL Framebuffer                 ~400KB                  │
│  ├─ 284×240 RGB565 单帧          ~140KB                 │
│  ├─ 双缓冲                       ~280KB                 │
│  └─ UI 分层缓存                  ~20KB                  │
├─────────────────────────────────────────────────────────┤
│ 字体资源                          ~2MB                    │
│  ├─ FontAwesome (icon font)      ~200KB                 │
│  ├─ CJK 字体 (多语言)            ~1.5MB                 │
│  └─ Emoji 图集                   ~300KB                 │
├─────────────────────────────────────────────────────────┤
│ AI 模型                           ~3MB                    │
│  ├─ WakeNet 模型                 ~1.5MB                 │
│  ├─ MultiNet (Custom Wake)       ~1MB                   │
│  ├─ AFE 参数                     ~200KB                 │
│  └─ 智能体配置 (下载)            ~300KB                 │
├─────────────────────────────────────────────────────────┤
│ 音频编解码缓冲                    ~500KB                  │
│  ├─ Opus 帧缓冲队列              ~100KB                 │
│  ├─ 解码 PCM 缓冲                ~200KB                 │
│  └─ 临时处理缓冲 (AFE)           ~200KB                 │
├─────────────────────────────────────────────────────────┤
│ 协议栈缓冲                        ~1.5MB                 │
│  ├─ WebSocket 接收缓冲           ~500KB                 │
│  ├─ MQTT Rx/Tx 队列              ~500KB                 │
│  └─ TLS/DTLS 秘钥交换            ~500KB                 │
├─────────────────────────────────────────────────────────┤
│ 运行时堆 (任务栈+动态分配)        ~1.5MB                 │
│  └─ 任务栈选择 INTERNAL/SPIRAM 视场景而定（持续循环或被 flash op 影响的任务 → INTERNAL）│
└─────────────────────────────────────────────────────────┘
```

### 9.3 Flash 分区（16MB）

```
├─ bootloader         (0x0)        64KB
├─ partition table    (0x10000)    8KB
├─ phy_init           (0x12000)    4KB
├─ nvs                (0x13000)    4KB    设备配置 / UUID / 激活码
├─ otadata            (0x14000)    8KB    OTA 标记
├─ firmware v1        (0x50000)    6MB    当前运行的 app partition
├─ firmware v2        (0x650000)   6MB    OTA 升级目标
├─ storage            (0xC50000)   2MB    备用
└─ FATFS/SPIFFS       (0xE50000)   可选
```

### 9.4 关键内存瓶颈

| 瓶颈 | 占用 | 优化方向 |
|------|------|---------|
| AFE 处理 | 1.5MB PSRAM (常驻) + 100KB 内部 RAM | 仅 Idle 状态保持 WakeWord 运行 |
| LVGL 渲染 | 双缓冲 280KB PSRAM | 启用 DMA 增量片段渲染 |
| 网络对话 | 接收缓冲 500KB + JSON 临时峰值 50KB | 流式 JSON 解析，及时释放 |
| 字体 | 2MB Flash + 加载到 PSRAM | 按需加载，分包压缩 |

---

## 十、耦合度评估

模块按耦合紧密程度划分：紧耦合的改造成本高，松耦合的可作为扩展点。

### 10.1 紧耦合（重构难度高）

| 耦合对 | 耦合方式 | 影响范围 | 解耦成本 |
|-------|---------|---------|---------|
| Application ↔ AudioService | 直接对象持有 + EventGroup | 初始化顺序、事件时序 | 高（需接口重设计） |
| AudioService ↔ Protocol | 回调 onIncomingAudio / onIncomingJson | 编解码格式、时序 | 中（抽象通用包格式） |
| Board ↔ Display | 工厂创建、GPIO 硬编码（config.h） | 屏幕初始化、DMA pin | 高（每板单独写） |
| Board ↔ AudioCodec | I2C/I2S pin 映射 | 不同板差异大 | 中（板级 config 隔离） |
| StateMachine ↔ Application | 状态转移验证 + 观察者 | 状态转移规则 | 中（FSM 独立可行） |

### 10.2 松耦合（易于扩展）

| 模块 | 扩展点 | 迁移成本 | 已有实现样本 |
|-----|--------|---------|------------|
| Protocol 实现 | 继承 `Protocol` 基类 | 低 | MqttProtocol / WebsocketProtocol / WebsocketJoyaiProtocol |
| WakeWord 引擎 | 继承 `WakeWord` 基类 | 低 | AfeWakeWord / CustomWakeWord |
| Display 实现 | Display → LcdDisplay / OledDisplay 分层 | 低 | LcdDisplay / OledDisplay / UiDisplay |
| Board 实现 | 继承 Board 基类 + config.h 定制 | 低 | mydazy-p30-4g / mydazy-p30-wifi / mydazy-p31 |
| AudioCodec 实现 | I2C 寄存器配置隔离 | 低 | ES8311 / ES7111 + ES7210 |
| RemoteCmd 命令 | 新增 `on_*()` 方法 | 低 | 15 已实现命令（建议远期收拢业务调度类到 ActivityCoordinator）|
| FlowEngine 脚本 | JSON 格式扩展 | 低 | HTTP 下载脚本（`flow_engine.cc`） |
| McpServer Tool | 注册新 Tool 函数 | 低 | press_to_talk / set_volume / 设备信息 |

### 10.3 上游 vs 自研

| 来自虾哥 xiaozhi-esp32 上游 | mydazy 自研增量 |
|----------------------------|----------------|
| Application 主框架 | 4G/WiFi 双网卡（DualNetworkBoard + ML307） |
| Board / Display / Protocol 抽象 | MQTT 协议实现（默认） |
| WiFi 配网（AP+DNS / BluFi） | RemoteCmd 远程命令体系 |
| LVGL UI + 多语言资源 | FlowEngine 流程脚本（原 LiveCompanion） |
| Opus 编解码集成 | Ota 激活流程 + RequestSwitch |
| esp-sr 集成 | MCP Tool 框架 |
| 设备状态机基础 | NFC（P31） |
| WakeWord 抽象 | 加速度计唤醒 |

### 10.4 扩展建议（基于耦合度）

- **新增协议**：直接继承 `Protocol`，在 `Application::InitializeProtocol` 选择逻辑加分支即可
- **新增唤醒词引擎**：继承 `WakeWord`，加 Kconfig 开关
- **新增 board 变种**：复制现有 board 目录，改 `config.h` + `*_board.cc`，加 Kconfig
- **新增远程命令**：在 `RemoteCmd` 加 `on_xxx()` 方法 + JSON schema 文档
- **新增 MCP 工具**：用 `McpServer::AddCommonTools` 或 `AddUserOnlyTools` 注册
- **避免**改 Application / AudioService / Board 基类合同，影响半径太大

### 10.5 远期扩展候选：好友组网互动

> 状态：路线图候选（非 v2.x 量产范围）。本节用于评估"是否在不破坏现有架构的前提下可落地"，结论是**可以**，记录设计取舍以备未来决策参考。

**结论先说**：现有架构对"设备间好友互动"天然友好——身份、协议、扩展点都已就绪，**全套扩展可在不动 12 态状态机、不抽新基类、不破坏上游 rebase 友好性的前提下完成**。

#### 10.5.1 协议支持评估

| 协议 | 多设备路由能力 | 扩展难度 | 推荐用途 |
|---|---|---|---|
| **MqttProtocol** | ★★★★★ broker 天然 pub/sub | 低 | 好友推送 / 在线状态 / 文字气泡 |
| **WebsocketProtocol** | ★★ 单连接，需 server 中转 | 中 | 实时语音互通（复用 Opus 通道） |
| **WebsocketJoyaiProtocol** | 同上 | 中 | 同 ws |

证据：`mqtt_protocol.cc:71` 已按 `publish_topic_` + server 下发 `client_id` 做路由——server 给好友 A 设备的 publish_topic 设成 B subscribe 的地址（或单独建 `friends/<uuid>/inbox` 主题），即可让 A→B 消息走同一 broker，**协议代码 0 改动**。

`BinaryProtocol2/3`（§ 八.2）支持 `type+payload`，新增 `type=friend_audio / friend_text` 即可承载好友内容，**帧格式不动**。

`Ota::RequestSwitch(type, data)`（`ota.cc:315`）已是"设备→server，请求语境切换"的成熟通道（iBeacon/NFC 都在用），好友邀请/接受可直接复用：`RequestSwitch("friend_invite", {target_uuid:...})`。

**不需要做**：自研 P2P / NAT 穿透 / WebRTC——server 中继成本可控，安全模型清晰，符合"保守 > 激进"。

#### 10.5.2 设备识别与关联

身份基础已齐：

| 来源 | 字段 | 持久化 | 用途 |
|---|---|---|---|
| `SystemInfo::GetMacAddress()` | WiFi STA MAC | 烧录芯片 | 全局唯一 ID |
| OTA 激活 | UUID / 激活码 | NVS `nvs` | 设备身份令牌 |
| MQTT 配置 | `client_id` | NVS `mqtt` namespace | broker 路由键 |

server 侧只需新增 `friends(owner_uuid, friend_uuid, alias, status, created_at)` 关系表。

**三种配对入口**（按推荐度）：
1. **服务端短码配对（主路径）**：A 喊"添加好友"→ `RequestSwitch("friend_invite_code")` → server 返 6 位码并 TTS 播报 → B 输入确认。**纯软件，三块板通用**。
2. **iBeacon 近场（P31 加分项）**：复用 `mydazy_p31_board.cc:1259` iBeacon 扫描器，定义"好友设备 iBeacon 广播包"——同一 owner 名下 P31 互相靠近自动建议配对。
3. **NFC 碰一碰（P31 加分项）**：把对方 UUID 编码进自定义 NDEF，复用 `Ota::RequestSwitch("nfc", data)` 链路。

近场入口（2、3）做成可选锦上添花，主路径必须是入口 1，否则 P30-WiFi/4G 用户被排除。

#### 10.5.3 三维取值登记（CLAUDE.md § 一.5.2 规范）

| 功能 | A: DeviceState | B: ActivityType | C: SceneType | 是否需新值 |
|---|---|---|---|---|
| 好友列表查看 | Idle（不变） | kNone | **kFriends（新增）** | C 列加 1 |
| 好友语音消息（异步） | Idle/Speaking（不变） | **kFriendVoiceMail（新增）** | Emoji/Player | B 列加 1 |
| 好友实时通话（同步） | Listening↔Speaking（复用） | **kFriendCall（新增）** | Player | B 列加 1 |
| 好友状态推送 | 任意（不变） | 不变 | 不变 | 全 0，仅 RemoteCmd |

**A 列保持 12 态不动**，符合"不动状态机"铁律。B、C 列加值不破坏既有判断（§ 一.5 三个心智 enum）。

#### 10.5.4 落地点清单（按耦合从低到高）

| # | 改动点 | 文件 | 工作量 | 风险 |
|---|---|---|---|---|
| 1 | `RemoteCmd::OnFriendInvite/OnFriendMessage/OnFriendCallRing` 三个 `on_*` | `main/remote_cmd.cc/h` | 0.5 周 | 极低（已有 14 个先例） |
| 2 | MCP Tool：`find_friend` / `send_voice_to_friend` / `call_friend` | `main/mcp_server.cc` | 0.5 周 | 极低 |
| 3 | `ActivityType` 加 `kFriendVoiceMail` / `kFriendCall` | `main/activity_type.h` | 0.2 周 | 低（enum 加值） |
| 4 | `SceneType` 加 `kFriends`，新建好友列表 LVGL screen | `main/scene_type.h` + `main/display/` | 1 周 | 中（UI 工作量） |
| 5 | 好友通话期复用现有 audio channel（server 透传 Opus 帧） | 0 行代码 | — | 关键依赖：server 实现 |
| 6 | FlowEngine 加好友剧本（来电铃声 + TTS + 接听确认） | JSON schema 扩展 | 0.5 周 | 极低（脚本驱动） |
| 7 | server 侧好友关系 + 路由 + 短码配对 | server 仓库 | 2-3 周 | 高（独立工程） |

设备侧累计 **2.7 周**，server 侧 **2-3 周**。

#### 10.5.5 关键设计取舍

- **同步通话 vs 异步语音消息**：建议 v1 先做异步语音消息（pub/sub 即可），先验证用户需求；同步通话留 v2，因为它会触发"两个设备同时占用 Listening/Speaking"的全新时序问题，需要更严谨的回声/打断处理。
- **全双工 vs 半双工通话**：硬件支持全双工（已有 AEC），但 server 侧实现成本高 1 个数量级。建议先半双工（按住说话），与 P30 现有交互范式一致。
- **不要碰的禁区**（§ 5.2 review）：BluFi、wifi 配网、OTA 握手、分区表——好友功能完全无需触碰。

#### 10.5.6 优先级与排期约束

- **当前优先级表（CLAUDE.md）未占位**：好友互动不抢 v2.x 量产窗口
- 量产前剩余 P1 修复（单击 1.5s 阻塞）和 P0（教育卡 + GIF）优先级更高（wake_encode PSRAM 栈已于 2026-04-28 修复）
- **建议排进 v3.0 路线图**，量产稳定 + KPI 验证后再推进

---

## 附录 A：关键文件索引

| 模块 | 关键文件 |
|------|---------|
| 入口 | `main/main.cc` |
| 业务编排 | `main/application.cc/h`, `main/device_state*.cc/h` |
| 音频服务 | `main/audio/audio_service.cc/h`, `main/audio/processors/`, `main/audio/wake_words/` |
| 协议 | `main/protocols/protocol.cc/h`, `main/protocols/websocket_protocol.cc`, `main/protocols/mqtt_protocol.cc` |
| 远程命令 | `main/remote_cmd.cc/h`, `main/mcp_server.cc/h` |
| OTA / 激活 | `main/ota.cc/h`, `main/ota_http_download.cc/h` |
| FlowEngine（流程脚本） | `main/flow_engine.cc/h`（原 `live_companion.cc/h`，已重命名为 FlowEngine；JSON type `"flow"`） |
| Board 基础 | `main/boards/common/board.h`, `wifi_board.cc`, `ml307_board.cc`, `dual_network_board.cc` |
| Board 实现 | `main/boards/mydazy-p30-4g/`, `mydazy-p30-wifi/`, `mydazy-p31/` |
| 显示 | `main/display/lcd_display.cc`, `oled_display.cc`, `ui_display.cc`, `lvgl_display/`, `ui/` |
| WiFi/配网 | `main/wifi/`, `main/blufi/` |
| LED | `main/led/circular_strip.cc`, `gpio_led.cc` |
| 设置 | `main/settings.cc/h` |
| 资源 | `main/assets.cc/h`, `components/78__xiaozhi-fonts/` |

## 附录 B：本文档与其他文档的关系

| 文档 | 视角 | 互补关系 |
|------|------|---------|
| `docs/p30-architecture-review.md` (v2.2.5) | 量产前评估、风险对账 | 与本文互补：本文重「是什么」，那篇重「能不能上量产」 |
| `docs/mqtt-udp.md`, `websocket.md` | 协议帧格式细节 | 第八节的展开 |
| `docs/mcp-protocol.md`, `mcp-usage.md` | MCP Tool 接口规范 | 第六节的展开 |
| `docs/blufi.md` | 配网协议 | 第七节 boards/common/blufi 的展开 |
| `docs/code_style.md` | 编码规范 | 跨架构约束 |
| `docs/custom-board.md` | 新增 board 教程 | 第七节、第十节的实操指南 |
