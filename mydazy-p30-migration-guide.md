# MyDazy P30-V32 升级迁移指南

> **基准对比**: upstream `78/xiaozhi-esp32` tag `v2.0.5` vs 本仓库 `origin/main` (commit 34a3304)
> **目标版本**: upstream `v2.2.4`（最新 tag，90 个提交差距）
> **文档生成**: 2026-04-09

---

## 一、当前修改总览（基于 v2.0.5 的定制）

总计 212 文件变更（排除 main/boards/），+12827 行，-7121 行。

---

## 二、逐模块修改清单

### 2.1 核心应用层

| 文件 | 变更 | 改动摘要 | 定制类型 |
|------|------|---------|---------|
| `main/application.cc` | 修改 +918/-49 | Type-C 耳机插拔检测（ADC CC线+MIC通道切换+PA开关）；NFC扫描任务；60秒周期POST状态到mydazy.cn；AEC模式NVS持久化；JoyAI协议选择；远程自定义消息处理(reboot/update/reconnect/wakeup)；启动预加载Assets显示logo；重启前清理显示防假死 | P30定制 |
| `main/application.h` | 修改 +7 | 新增 status_timer_handle_、StartStatusReportTimer()、extern headset_present | P30定制 |
| `main/main.cc` | 修改 +41 | 启动时设置编译时间为系统默认时间（避免1970-01-01） | P30定制 |

### 2.2 音频模块

| 文件 | 变更 | 改动摘要 | 定制类型 |
|------|------|---------|---------|
| `main/audio/audio_codec.cc` | 修改 | 输入增益保存到NVS | P30定制 |
| `main/audio/audio_codec.h` | 修改 | 默认音量70->80，默认增益0->24.0dB | P30定制 |
| `main/audio/audio_service.cc` | 修改 +183 | 4通道TDM MIC：耳机时切MIC4，否则MIC1；录音前关PA减少4G干扰；输出超时缩到2秒；硬编码启用AFE | P30定制 |
| `main/audio/audio_service.h` | 修改 | OUTPUT_AUDIO_POWER_TIMEOUT_MS = 2000 | P30定制 |
| `main/audio/codecs/box_audio_codec.cc` | 修改 +87 | 4通道MIC全开(ES7210)；增益从NVS读取；音量上限92防爆音；新增SetInputGain() | P30定制 |
| `main/audio/codecs/box_audio_codec.h` | 修改 | SetInputGain() override | P30定制 |
| `main/audio/processors/afe_audio_processor.cc` | 修改 | 启用AGC(WebRTC,6dB压缩)；启用VAD；AFE任务绑核Core0 | 混合 |
| `main/audio/wake_word.h` | 修改 | 新增 IsAfeType() 虚函数 | 通用改进 |
| `main/audio/wake_words/afe_wake_word.cc` | 修改 | 唤醒词任务绑核Core0 | P30定制 |
| `main/audio/wake_words/afe_wake_word.h` | 修改 | override IsAfeType() | 通用改进 |
| 删除: es8311/es8374/es8388/es8389/dummy/no_audio_codec, no_audio_processor, esp_wake_word | 删除 -1780行 | 精简不用的codec驱动 | P30裁剪 |

### 2.3 显示/UI

| 文件 | 变更 | 改动摘要 | 定制类型 |
|------|------|---------|---------|
| `main/display/lcd_display.cc` | 修改 +83/-478 | SPI LCD缓冲改PSRAM双缓冲(20行->48行)；刷新率50ms->33ms(30FPS)；析构重写(lv_display_delete统一回收)修复假死bug；删除CONFIG_USE_WECHAT_MESSAGE_STYLE代码块 | 混合 |
| `main/display/lvgl_display/lvgl_display.cc` | 修改 +29 | 析构不再手动删LVGL控件，防重复释放假死 | 通用改进 |
| `main/display/lvgl_display/emoji_collection.cc` | 修改 +8 | 新增logo/network/qrcode表情图标 | P30定制 |
| `main/display/lvgl_display/jpg/image_to_jpeg.cpp` | 修改 | printf格式修复 %08x->%08lx | 通用改进 |
| `main/assets/twemoji_64/*.png` (28个) | 新增 | 自定义表情包PNG资源 | P30定制 |
| `main/assets/image/*.gif` (5个) | 新增 | GIF动画(chat/listening/logo/network/testing) | P30定制 |
| `main/assets/common/clock.ogg` | 新增 | 时钟音效 | P30定制 |
| `main/assets/locales/zh-CN/*.ogg` (17个) | 新增/修改 | 大量中文语音提示音 | P30定制 |

### 2.4 网络/协议

| 文件 | 变更 | 改动摘要 | 定制类型 |
|------|------|---------|---------|
| `main/blufi/` (5个文件, ~1216行) | 新增 | BluFi BLE配网，SSID前缀"MyDazy"，NimBLE，DH密钥+AES加密 | P30定制 |
| `main/protocols/websocket_joyai_protocol.cc/h` (484行) | 新增 | JoyAI WebSocket协议适配 | P30定制 |
| `main/protocols/websocket_protocol.cc` | 修改 +9 | 析构时优雅关闭(发送关闭帧+等500ms) | 通用改进 |
| `main/protocols/mqtt_protocol.cc` | 修改 +8 | 析构时优雅断开(DISCONNECT+等500ms) | 通用改进 |

### 2.5 硬件驱动

| 文件 | 变更 | 改动摘要 | 定制类型 |
|------|------|---------|---------|
| `components/ckv__esp_lcd_jd9853/` (全目录, ~901行) | 新增 | JD9853 LCD驱动(1.83寸240x284 SPI屏) | P30定制 |
| `components/lcd_driver/` (全目录, ~399行) | 新增 | LCD驱动工厂(6种屏幕统一接口) | P30定制 |
| `main/nfc/` (4个文件, ~3138行) | 新增 | NFC驱动(WS1850S I2C) + ISO14443A/B | P30定制 |
| `main/ulp/main.c` (70行) | 新增 | ULP协处理器(低功耗唤醒) | P30定制 |

### 2.6 OTA/系统

| 文件 | 变更 | 改动摘要 | 定制类型 |
|------|------|---------|---------|
| `main/ota.cc` | 修改 +236 | HTTP头加BindingCode；ReportStatus()POST到mydazy.cn(时间同步+远程设置)；ProcessCustomContent()下载自定义素材到SPIFFS | P30定制 |
| `main/ota.h` | 修改 | 新增ReportStatus()/ProcessCustomContent() | P30定制 |
| `main/ota_http_download.cc/h` (501行) | 新增 | HTTP下载器(单例,1MB缓冲,MD5校验,进度回调) | P30定制 |
| `main/system_info.cc` | 修改 +127 | GetMacAddressRaw/Last4()；PrintHeapStats增强(PSRAM/CPU频率/芯片温度)；C接口包装 | 混合 |
| `main/system_info.h` | 修改 | 新增函数声明 | 混合 |

### 2.7 其他

| 文件 | 变更 | 改动摘要 | 定制类型 |
|------|------|---------|---------|
| `main/led/gpio_led.cc` | 修改 | Listening独立处理；Speaking改500ms闪烁 | 混合 |
| `main/led/single_led.cc` | 修改 | 同上 | 混合 |
| `main/mcp_server.cc` | 修改 | 新增self.get_mac_address和self.audio.set_aec工具 | P30定制 |

### 2.8 构建配置

| 文件 | 变更 | 改动摘要 |
|------|------|---------|
| `main/CMakeLists.txt` | 大幅修改 | 删除不用codec源文件；新增blufi/nfc/ota_http_download/joyai源文件和include路径 |
| `main/Kconfig.projbuild` | 大幅修改 -268 | 默认板型改MYDAZY_P30；删除~50种上游开发板定义 |
| `sdkconfig.defaults` | 修改 | 日志NONE->INFO；波特率2000000；启用SC7A20H/GIF/LODEPNG |
| `sdkconfig.defaults.esp32s3` | 修改 | 启用BT NimBLE+BluFi；禁用ULP；启用RTTI |
| `sdkconfig` | 新增 +3681 | 完整编译配置 |
| `main/idf_component.yml` | 修改 | 大量依赖裁剪 |
| `partitions/v2/16m.csv` | 修改 | 分区表调整 |
| `partitions/v2/8m.csv` | 修改 | 分区表调整 |
| 删除: sdkconfig.defaults.esp32/c3/c5/c6/p4 | 删除 | 其他平台配置 |
| 删除: partitions/v1/全部 + v2/c3/4m | 删除 | 不需要的分区表 |
| `.gitignore` | 修改 | 不忽略components/和sdkconfig；新增.idea/.github忽略 |

### 2.9 删除的脚本/文档

- `.github/` (CI/Issue模板)、`README.md`、`README_ja.md`、`docs/v0/`、`docs/v1/`
- `scripts/Image_Converter/`、`scripts/ogg_converter/`、`scripts/p3_tools/`
- `scripts/sonic_wifi_config.html`、`scripts/mp3_to_ogg.sh`

---

## 三、上游 v2.0.5 → v2.2.4 变更摘要（90 个提交）

### 3.1 版本里程碑

#### v2.0.5 → v2.1.0（25 个提交）— 架构重构

| 变更 | 说明 |
|------|------|
| **设备状态机** | `device_state_event.cc/h` 被删除，新增 `device_state_machine.cc/h`（std::atomic + 状态转换校验 + 观察者模式）|
| **Application 大重构** | `Start()` → `Initialize()` + `Run()`；`MainEventLoop()` 拆分为独立事件处理函数 |
| **SetDeviceState()** | 返回值 void → bool（通过状态机校验合法性）|
| **Schedule()** | 参数改为右值引用 `std::function<void()>&&` |
| **电源管理** | `SetPowerSaveMode(bool)` → `SetPowerSaveLevel(PowerSaveLevel)` |
| **OTA** | `CheckNewVersion(Ota&)` → `CheckNewVersion()`，OTA改为成员变量 |
| **BluFi** | 上游也新增了 BluFi 支持 |
| **esp-wifi-connect** | 升级到 3.0 |
| **新状态** | kDeviceStateWifiConfiguring / kDeviceStateAudioTesting |

#### v2.1.0 → v2.2.2（30+ 提交）— 音频/显示增强

| 变更 | 说明 |
|------|------|
| **Opus 编解码器替换** | 自研 78opus → ESP 官方 `esp_audio_codec` + `esp_audio_effects` |
| **OGG 解封装器** | 新增 `ogg_demuxer.cc/h`（311行），替换内联解析 |
| **重采样器替换** | 自研 Resampler → `esp_ae_rate_cvt` |
| **Noto 字体 + Emoji** | 替换部分 puhui 为 noto |
| **Display 基类** | 新增 `SetupUI()` 虚函数 + `ClearChatMessages()` |
| **emote_display 重构** | 使用 `esp_emote_expression` 组件 |
| **LVGL** | 9.3 → 9.4 |
| **esp_lvgl_port** | 2.6 → 2.7 |
| **USB RNDIS 网络** | 新增支持 |
| **clang-format** | 引入代码格式化 |

#### v2.2.2 → v2.2.4（30 提交）— 稳定性修复

| 变更 | 说明 |
|------|------|
| **AudioService** | 输入任务不因读超时终止（break → delay重试）|
| **SetupUI** | 防重复调用机制 |
| **LVGL 线程安全** | SetChatMessage 增加 lv_obj_is_valid 校验 |
| **流式 OGG** | 解封装支持 |
| **AEC 记忆修正** | 云聊蓝牙功能 |
| **内存泄漏修复** | EchoEar 等 |

### 3.2 上游关键依赖变化

| 组件 | v2.0.5 | v2.2.4 | 影响 |
|------|--------|--------|------|
| **IDF 最低版本** | >=5.4.0 | **>=5.5.2** | 必须升级 IDF |
| esp-wifi-connect | ~2.6.2 | ~3.1.1 | WiFi API 变化 |
| **opus 编解码** | 78/esp-opus-encoder | **esp_audio_codec ~2.4.1** | 完全替换 |
| **新增 esp_audio_effects** | 无 | ~1.2.1 | 音效库 |
| esp-ml307 | ~3.5.1 | ~3.6.4 | 4G 模块 |
| LVGL | ~9.3.0 | **~9.4.0** | UI 组件 |
| esp_lvgl_port | ~2.6.0 | ~2.7.0 | LVGL 端口 |
| xiaozhi-fonts | ~1.5.5 | ~1.6.0 | 字体资源 |
| esp-sr | ~2.2.0 | ~2.3.0 | 语音识别 |
| esp_emote_gfx | ^1.1.2 | **esp_emote_expression ^0.1.0** | 表情组件替换 |

---

## 四、冲突分析与迁移方案

### 4.1 冲突风险矩阵

| 文件 | 风险 | 原因 |
|------|------|------|
| **application.cc/h** | 极高 | 上游架构重构(Start→Initialize+Run)，本地+918行定制，无法自动合并 |
| **audio_service.cc/h** | 极高 | 上游替换opus编解码器+重采样器，API完全不同 |
| **CMakeLists.txt** | 高 | 上游重写board列表+显式文件列表，本地大幅裁剪 |
| **Kconfig.projbuild** | 高 | 双方都大幅修改board选项 |
| **lcd_display.cc** | 中高 | 上游新增SetupUI/防重复/lv_obj_is_valid，本地改缓冲+析构 |
| **ota.cc/h** | 中 | 上游Upgrade()改static+callback+4KB对齐 |
| **idf_component.yml** | 高 | 组件大量替换(opus→esp_audio_codec等) |
| **protocols/** | 中 | CloseAudioChannel 新增 send_goodbye 参数 |
| **system_info.cc** | 低 | 上游仅新增PrintPmLocks()，可干净合并 |
| **device_state 全替换** | 高 | device_state_event→device_state_machine，本地如有引用需全面替换 |

### 4.2 迁移方案

#### 策略：以上游 v2.2.4 为基础，逐模块移植 P30 定制

**不建议 git merge**，因为：
1. application.cc 架构完全重构，merge 冲突无法自动解决
2. 音频编解码器整体替换，旧 API 不存在了
3. 删除了大量上游文件，merge 会产生大量无意义冲突

#### Phase 1: 准备新基线

```bash
# 在新目录基于上游 v2.2.4 创建工作分支
cd ~/GitHub
git clone https://github.com/78/xiaozhi-esp32.git mydazy-p30-v32-upgrade
cd mydazy-p30-v32-upgrade
git checkout v2.2.4
git checkout -b mydazy-p30-upgrade
```

#### Phase 2: 移植纯新增文件（无冲突）

直接复制，不需要 merge：

```
# 硬件驱动
components/ckv__esp_lcd_jd9853/     → 直接复制
components/lcd_driver/              → 直接复制
main/nfc/                           → 直接复制
main/ulp/main.c                     → 直接复制

# 功能模块
main/blufi/                         → 对比上游BluFi实现，选择保留哪个版本
main/protocols/websocket_joyai_protocol.cc/h → 直接复制，适配CloseAudioChannel新签名
main/ota_http_download.cc/h         → 直接复制

# 资源文件
main/assets/twemoji_64/             → 直接复制
main/assets/image/                  → 直接复制
main/assets/common/clock.ogg        → 直接复制
main/assets/locales/zh-CN/*.ogg     → 直接复制
```

#### Phase 3: 合并 application.cc（最复杂）

**上游新架构**：
- `Start()` → `Initialize()` + `Run()`
- `MainEventLoop()` → 独立事件处理函数
- 状态机替换 volatile
- OTA 改为成员变量

**P30 定制功能需迁移到新架构**：

| P30功能 | 迁移位置 |
|---------|---------|
| Type-C 耳机检测(ADC+GPIO) | `Initialize()` 中初始化，新增 `HandleHeadsetEvent()` |
| NFC扫描任务 | `Initialize()` 中创建任务，通过 Schedule() 回调到主线程 |
| 状态上报定时器 | `ActivationTask()` 完成后启动定时器 |
| AEC模式NVS持久化 | `GetDefaultListeningMode()` 中读取NVS |
| JoyAI协议选择 | `ResetProtocol()` 中添加选择逻辑 |
| 远程消息处理 | 协议层回调中处理 |
| 启动logo预加载 | `Initialize()` 中早期调用 |
| 重启前显示清理 | 调用 Board::CleanupDisplay() |

> 建议：将 GPIO 硬编码（PA_PIN、CC_ADC等）全部移到 boards/mydazy-p30/config.h，通过 Board 虚函数暴露

#### Phase 4: 合并音频层

**关键变化**：opus 编解码器完全替换

| 任务 | 说明 |
|------|------|
| 4通道TDM MIC | 在新 audio_service.cc 中重新实现，使用新的 esp_audio_codec API |
| 耳机MIC切换 | 保持逻辑，适配新的 EnableWakeWordDetection/EnableVoiceProcessing |
| 录音前关PA | 保持逻辑 |
| AGC/VAD | 检查上游是否已默认启用，避免重复配置 |
| box_audio_codec | 在新版 box_audio_codec 基础上添加 4通道+增益NVS+音量上限 |

#### Phase 5: 合并显示层

| 任务 | 说明 |
|------|------|
| PSRAM双缓冲 | 在新版 lcd_display.cc 中添加（上游可能已支持类似功能）|
| 刷新率调整 | 33ms 参数，在新版中设置 |
| 析构修复 | 上游 v2.2.4 已有部分修复(lv_obj_is_valid)，对比后决定是否需要额外修改 |

#### Phase 6: 合并 OTA

| 任务 | 说明 |
|------|------|
| Upgrade() 签名 | 适配新的 static + callback 接口 |
| BindingCode HTTP头 | 在新版 Upgrade() 或 CheckNewVersion() 中添加 |
| ReportStatus() | 直接复制函数，适配新架构调用点 |
| ProcessCustomContent() | 直接复制 |

#### Phase 7: 合并构建配置

| 任务 | 说明 |
|------|------|
| CMakeLists.txt | 在上游新版基础上添加 P30 board + nfc/blufi/joyai 源文件 |
| Kconfig.projbuild | 在上游新版基础上添加 MYDAZY_P30 板型选项 |
| idf_component.yml | 使用上游新版依赖，额外添加 P30 特有的组件 |
| sdkconfig.defaults | 在上游基础上添加 P30 定制配置 |
| 分区表 | 使用上游新版，按需修改 |

#### Phase 8: 验证

```bash
# 1. 编译检查
source idf55 && idf.py set-target esp32s3
idf.py menuconfig  # 选择 MYDAZY_P30
idf.py build

# 2. 内存水位检查
# 编译后检查 .map 文件确认内部 RAM 使用

# 3. 功能测试清单
- [ ] WiFi 连接
- [ ] 4G 连接
- [ ] 语音唤醒
- [ ] 对话功能
- [ ] 耳机插拔检测
- [ ] NFC 读写
- [ ] BluFi 配网
- [ ] OTA 升级
- [ ] 状态上报
- [ ] 显示正常（无假死）
- [ ] 深度睡眠/唤醒
```

---

## 五、通用改进清单（可回馈上游）

以下修改是通用改进，不依赖 P30 硬件：

1. **显示析构修复** — lcd_display.cc/lvgl_display.cc 防假死（上游可能已部分修复）
2. **WebSocket/MQTT 优雅关闭** — 发送关闭帧后等待
3. **WakeWord::IsAfeType()** — 替代 dynamic_cast
4. **printf 格式修复** — `%08lx`
5. **芯片温度/CPU频率监控** — system_info.cc

---

## 六、BluFi 特别说明

上游 v2.1.0 也新增了 BluFi 支持。迁移时需要对比：
- 上游 BluFi 实现 vs P30 BluFi 实现
- P30 版本有 "MyDazy" SSID 前缀和 BindingCode 功能
- 建议：**以上游实现为基础**，在其上添加 MyDazy 定制（前缀+绑定码）

---

## 七、工作量估算

| 阶段 | 预计工作量 | 风险 |
|------|----------|------|
| Phase 2: 纯新增文件 | 1小时 | 低 |
| Phase 3: application.cc | 4-6小时 | 高（架构完全不同）|
| Phase 4: 音频层 | 3-4小时 | 高（API完全替换）|
| Phase 5: 显示层 | 1-2小时 | 中 |
| Phase 6: OTA | 1-2小时 | 中 |
| Phase 7: 构建配置 | 2-3小时 | 中 |
| Phase 8: 验证调试 | 4-8小时 | 取决于编译和运行问题 |
| **总计** | **16-26小时** | |

---

*文档生成时间：2026-04-09*
*基准版本：v2.0.5 (upstream commit 860d12a) ↔ commit 34a3304 (mydazy fork)*
*目标版本：v2.2.4 (upstream commit e77dedb)*
