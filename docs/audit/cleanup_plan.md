# 量产前代码精简规划

> **状态**：规划稿，未执行。  
> **范围**：仅输出删除/保留决策，不动代码。  
> **评审通过后**，按"批次"章节分批实施，每批末尾闸门验证。  
> **依据版本**：`dev` 分支，`HEAD=76d152ee`（2026-04-23）。  
> **目标产品**：`mydazy-p30-4g` / `mydazy-p30-wifi` / `mydazy-p31`（三块 ESP32-S3 板）。

---

## Part 0：产品经理摘要（大白话）

1. **预计删除体量**：约 **2100 行代码 + 2 个 ESP-IDF 组件依赖 + 1 份芯片配置 + 约 200 行 Kconfig 僵尸选项**。不包括 Kconfig 下翻出来的待删 LCD/OLED 子菜单（如果连那些都裁，能再多 100–150 行）。
2. **最大的 3 个删除动作**：
   - **Camera / 视频整套**（`esp32_camera.cc` + `esp_video.cc` + `esp32_camera.h` + `esp_video.h`，约 1460 行）+ 两个依赖组件（`espressif/esp32-camera`、`espressif/esp_video`）。你已确认"不会上摄像头"。
   - **Kconfig 僵尸分支**：文件里 42 处 `depends on BOARD_TYPE_XXX`，引用的板（LILYGO、BREAD_COMPACT、TAIJI_PI_S3、ESP_S3_LCD_EV_Board、ESP_BOX、ECHOEAR、WAVESHARE 等）早就不在本仓库，留着只是噪音。整段带走。
   - **RNDIS 网卡板支持**（`rndis_board.cc/h`，约 320 行）——是上游某块"USB 网卡桥接"参考板专用代码，三块 MyDazy 板都不用。
3. **最大的 3 个删除风险**：
   - **上游合并冲突**：删 `main/` 和 `main/boards/common/` 下的文件 = 改动主干，未来跟 xiaozhi-esp32 上游拉新功能时冲突会变大。**建议**：把删除动作集中到一两个独立 commit，commit message 里写清楚"精简 P30/P31 用不到的部分"，合并时整段取舍。
   - **OLED 隐性依赖**：`main/mcp_server.cc:264` 和 `main/boards/common/board.cc:162` 都在跑 `dynamic_cast<OledDisplay*>` 去适配 OLED 屏。三块板都是彩色 LCD，不是 OLED，但**这些代码不能删**——删了会编不过。规划里会把 `oled_display.cc/h` 列为"保留"。
   - **idf_component.yml 里一大坨 LCD/触摸驱动**：能删得很爽（`sh8601`、`co5300`、`ili9341`、`gc9a01`、`st77916`、`axs15231b`、`st7701`、`st7796`、`spd2010`、`cst9217`、`cst816s`、`gt911`……），**但** ESP-IDF 组件管理器是"lazy pull"，yml 里写的依赖只有被 CMake `PRIV_REQUIRES` 引用到的才会进固件。这意味着**删掉对固件大小没影响**（只影响 `managed_components` 下载量 + CI 缓存大小），但也意味着**删不干净不会报错**——所以这一批要特别小心，先跑一次 build 确认 `managed_components` 里真的没这些组件的目录，再动手。
4. **整个精简预估耗时**：3 人日。批次 1 零风险（1 人时），批次 2（2 人时），批次 3（1 人日，Kconfig 裁剪调试最费时间），批次 4 默认不做（见下）。
5. **现在就能看到的「建议不要删」的特殊情况**：
   - **`oled_display.cc/h`**：如上，有硬引用。留。
   - **`no_audio_processor.cc`**：当 `CONFIG_USE_AUDIO_PROCESSOR=n` 时 CMake 会选它。我们当前全都 `=y`，但这是**运行时降级通道**，留着上游兼容，删了不大。
   - **`blufi/ibeacon.cc`**：前一次 cleanup 已明确 Jack 指令保留，这次照旧。
   - **`mqtt_protocol.cc/h`（389 + 65 行）**：你确认是**默认协议**，必留。
   - **`websocket_baidu_protocol.cc/h`（1380 行）**：你确认作为未来切换平台的占位，**暂留**——但这 1380 行是当前仓库最大的一块"未接线死代码"，规划里会单列一节提醒：**建议在 3～6 个月内要么接入要么删掉**，长期留着会腐化。
   - **参考板**：不建议额外保留。upstream `78/xiaozhi-esp32` 本身公开、完整，作为"怎么写一块新板"的参考用它就够了，本仓库不必为示范目的多背一块板。

---

## Part 1：保留清单（白名单）

### 1.1 目标硬件

| 维度 | 保留 | 依据 |
|---|---|---|
| 开发板 | `mydazy-p30-4g` / `mydazy-p30-wifi` / `mydazy-p31` | 三块独立 board（已分好，不合并） |
| 主控 | ESP32-S3（唯一） | 三块 `config.json` 都写 `target:"esp32s3"`；Kconfig choice 三条都 `depends on IDF_TARGET_ESP32S3` |
| Flash 大小 | 16 MB | `sdkconfig.defaults.esp32s3`: `CONFIG_ESPTOOLPY_FLASHSIZE_16MB=y` |
| PSRAM | Octal 80MHz | `CONFIG_SPIRAM_MODE_OCT=y` + `CONFIG_SPIRAM_SPEED_80M=y` |
| 分区表 | `partitions/v2/16m.csv`（双 OTA 各 3.9MB + 8MB assets） | `sdkconfig.defaults`: `CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions/v2/16m.csv"` |

### 1.2 音频 codec（按板拆）

| 板 | TX（播放）codec | RX（录音）codec | 驱动文件 |
|---|---|---|---|
| `mydazy-p30-4g` | ES8311 | ES7210 | `main/audio/codecs/box_audio_codec.{cc,h}` |
| `mydazy-p30-wifi` | ES8311 | ES7210 | `main/audio/codecs/box_audio_codec.{cc,h}` |
| `mydazy-p31` | ES7111 | ES7210 | `main/audio/codecs/es7111_audio_codec.{cc,h}` |

> **依据**：`board.cc` 的 `#include "codecs/box_audio_codec.h"` / `es7111_audio_codec.h`；Kconfig 条目注释"(ES8311+ES7210)" / "(ES7111+ES7210)"。  
> **结论**：仓库**只需要** `box_audio_codec` 和 `es7111_audio_codec`。其他 codec 驱动不在 `main/audio/codecs/` 下（已经被先前 cleanup 删干净了，确认 `ls` 只剩这两套）。

### 1.3 显示与触摸（三块板完全一致）

| 功能 | 型号 | 组件 |
|---|---|---|
| LCD 驱动 IC | JD9853（284×240，SPI） | `components/esp_lcd_jd9853` |
| 电容触摸 IC | AXS5106L | `components/esp_lcd_touch_axs5106l` |
| UI 框架 | LVGL 9.5 + esp_lvgl_port | `lvgl/lvgl ~9.5.0` + `esp_lvgl_port ~2.7.2` |
| 表情动画 | esp_emote_expression + image_player | `espressif2022/esp_emote_expression`、`espressif2022/image_player` |
| 字体 | xiaozhi-fonts（puhui + awesome） | `78/xiaozhi-fonts ~1.6.0` |

### 1.4 通信协议

| 协议 | 保留 | 状态 |
|---|---|---|
| MQTT + UDP | ✅ **默认协议** | `application.cc:497-514` 服务端 OTA 下发决定；`HasMqttConfig()` 为真时走 MQTT |
| WebSocket | ✅ 主力备选 | `WebsocketProtocol` |
| WebSocket (JoyAI) | ✅ | URL 含 `joyinside` 自动切换 |
| WebSocket (百度) | ✅ **占位** | 文件存在，**未接线**，作为未来切换平台的占位 |

> **百度协议的长期风险**：`websocket_baidu_protocol.cc` 1380 行未接线，是**当前仓库最大的一块死代码**。建议标记 owner + deadline，3～6 个月内决定接入或删除。

### 1.5 必保留的核心功能模块

| 模块 | 路径 | 作用 |
|---|---|---|
| Wakenet 唤醒词 | `audio/wake_words/afe_wake_word.{cc,h}` | 在线唤醒词，S3 走 AFE |
| 自定义唤醒词 | `audio/wake_words/custom_wake_word.{cc,h}` | Multinet 模型，支持自定义词 |
| AFE 音频处理 | `audio/processors/afe_audio_processor.{cc,h}` | 降噪 + AEC |
| Opus 编码 | esp_audio_codec 组件（managed） | 语音传输压缩 |
| OTA 升级 | `ota.cc`、`ota_http_download.cc` | 固件升级 |
| NVS 设置 | `settings.cc` | 非易失配置 |
| MCP server | `mcp_server.cc` | IoT 控制协议 |
| Application 主循环 | `application.cc` + `device_state_machine.cc` | 状态机 + 事件循环 |
| LiveCompanion | `live_companion.cc` | 直播伴侣扩展能力 |
| RemoteCmd | `remote_cmd.cc` | 远程命令推送 |
| 蓝牙 BluFi | `boards/common/blufi.cpp` + `blufi/ibeacon.cc` | 蓝牙配网 + iBeacon |
| 热点配网 | `boards/common/wifi_board.cc` 内嵌 HTTP server | P30-4G 蓝牙 + 热点双配网 |
| 音乐播放 | `audio/music_player.cc` + `audio/demuxer/ogg_demuxer.cc` | MP3/OGG 播放（P30-4G 已用） |

### 1.6 必保留的板级公共驱动（`main/boards/common/`）

| 文件 | 谁用 | 能删吗 |
|---|---|---|
| `board.{cc,h}` | 所有 | ❌ 基类 |
| `wifi_board.{cc,h}` | p30-wifi 直接用；p30-4g/p31 通过 DualNetworkBoard 用 | ❌ |
| `ml307_board.{cc,h}` | p30-4g/p31 通过 DualNetworkBoard 用 | ❌ |
| `dual_network_board.{cc,h}` | p30-4g/p31 | ❌ |
| `button.{cc,h}` | 所有 | ❌ |
| `i2c_device.{cc,h}` | 所有（触摸、传感器、codec） | ❌ |
| `backlight.{cc,h}` | 所有 | ❌ |
| `power_save_timer.{cc,h}` | 所有 | ❌ |
| `power_manager.h` | 所有（electricity） | ❌ |
| `system_reset.{cc,h}` | 所有 | ❌ |
| `sc7a20h.{cc,h}` | 三板都在，P31 启用加速度唤醒 | ❌（保留给 P31；P30 侧编入不激活） |
| `axp2101.{cc,h}` | PMIC 基类，被某些板用 | ❓ **待确认**（grep 未命中，但电源链路偶有隐式依赖）— 批次 4 再动，默认保留 |
| `adc_battery_monitor.{cc,h}` | 电量检测 | ❌ |
| `press_to_talk_mcp_tool.{cc,h}` | MCP 工具 | ❌ |
| `blufi.h` | 配网声明 | ❌ |
| `ml307_gnss.{cc,h}` | **仅 P31** 用 | ❌（P31 必留） |
| `typec_headset.{cc,h}` | **仅 P31** 用（Type-C 耳机检测） | ❌（P31 必留） |
| `camera.h` + `esp32_camera.{cc,h}` + `esp_video.{cc,h}` | **无人使用** | ✅ **可删**（见 Part 2） |
| `rndis_board.{cc,h}` | **无人使用**（是上游 ESP_KORVO2_V3_RNDIS 专用） | ✅ **可删**（见 Part 2） |
| `lamp_controller.h` | grep 主干代码未发现引用，疑似僵尸 | ❓ **需人工确认** |

### 1.7 必保留的 `main/display/` 层

| 文件 | 作用 | 说明 |
|---|---|---|
| `display.{cc,h}` | 抽象基类 | ❌ 必留 |
| `lcd_display.{cc,h}` | 通用 LCD 显示 | ❌ 三块板都用 |
| `ui_display.{cc,h}` | P30/P31 的定制 UI | ❌ 三块板都用 |
| `emote_display.{cc,h}` | 表情动画显示 | ❌ 三块板都用 |
| `oled_display.{cc,h}` | OLED 抽象 | ⚠️ **虽三块板都不是 OLED，但 `mcp_server.cc:264` 和 `board.cc:162` 有 `dynamic_cast<OledDisplay*>` 引用，删了编不过**。留。 |
| `lvgl_display/*`（9 文件） | LVGL 封装 | ❌ 必留 |
| `ui/core/` `ui/theme/` `ui/resources/` `ui/widgets/` | UI 组件 | ❌ 必留 |

### 1.8 未来会加、现在没有的模块（占位保护）

| 未来功能 | 现状 | 精简时**不能删**的东西 |
|---|---|---|
| **4G Air780EP**（新模组） | 现用 ML307R；Air780 系列 AT 指令与 ML307 兼容，理论上 `ml307_board.cc` 改几行注册即可 | `ml307_board.cc`、`dual_network_board.cc`、`78/esp-ml307` 组件 — 全留 |
| **P31 GPS** | ML307R 内置 GNSS，`ml307_gnss.cc` 已经接 | `ml307_gnss.cc`、`power_manager.h` — 全留 |
| **P31 NFC 读卡器** | WS1850S 已经在 `components/esp_nfc_ws1850s` | 组件留；CMake `PRIV_REQUIRES esp_nfc_ws1850s` 留 |
| **平台切换（百度）** | `websocket_baidu_protocol.cc/h` 已写未接 | 文件留（按你确认） |

---

## Part 2：删除候选清单（黑名单）

每条都给出：**删什么** / **为什么能删** / **风险等级** / **删完多少行**。

> **风险图例**：🟢 零风险（纯孤立）｜🟡 低风险（需看引用但基本无依赖）｜🟠 中风险（要动 Kconfig/CMake）｜🔴 高风险（碰主干，跟上游合并冲突）

---

### A. 多余的板级目录

**现状**：`main/boards/` 只剩 4 个目录 → `common/`、`mydazy-p30-4g/`、`mydazy-p30-wifi/`、`mydazy-p31/`。**非 common 的只有三块目标板，没有多余 board 目录**。

**结论**：本维度**没有可删项**。前一次 cleanup（`76d152ee`）已彻底。

---

### B. 多余的芯片支持（聚焦 ESP32-S3）

| 删除项 | 位置 | 风险 | 行数估 |
|---|---|---|---|
| `sdkconfig.defaults.esp32p4` | 根目录 | 🟢 S3-only 永不用 | ~25 |
| `main/CMakeLists.txt`: `if(CONFIG_IDF_TARGET_ESP32)` 的 REMOVE_ITEM 块 | main/CMakeLists.txt:346-354 | 🟠 改 CMake，需跑 build | ~10 |
| `main/CMakeLists.txt`: `OR CONFIG_IDF_TARGET_ESP32P4` 分支收紧成 `IF(S3)` | main/CMakeLists.txt 3 处 | 🟠 同上 | ~5 |
| `main/audio/audio_service.cc:33-37` 的 `#else` 分支 + `#include "wake_words/esp_wake_word.h"` + `esp_wake_word.cc/h` 调用（line 717） | `audio_service.cc` | 🟡 头文件已删，当前靠 `#if S3/P4` 短路；S3-only 后彻底删分支 | ~10 |

**小计**：~50 行 + 1 文件

---

### C. 多余的音频 codec 驱动

**现状（`ls main/audio/codecs/`）**：`box_audio_codec.{cc,h}` + `es7111_audio_codec.{cc,h}`，**只剩在用的两套**。

**结论**：本维度**没有可删项**。前一次 cleanup 已彻底（`no_audio_codec` 已删）。

---

### D. 多余的显示驱动 / LCD

#### D.1 `main/display/` 目录

**没有可删项**（见 Part 1.7，`oled_display` 因为硬引用必须留）。

#### D.2 `main/idf_component.yml` 中的 LCD/触摸/音频 IC 依赖清单

下面这些组件在 `yml` 中声明，但在 `main/CMakeLists.txt` 的 `PRIV_REQUIRES` 中**没有列出**，且 `main/boards/` 下**没有 `#include`**。ESP-IDF 组件管理器不会编进固件，但留着 = 每次 CI/本地构建拉 managed_components 多下载几百 MB、dependencies.lock 噪音大。

| yml 条目 | 是否被项目代码引用 | 可删 |
|---|---|---|
| `waveshare/esp_lcd_sh8601` | 否 | ✅ |
| `espressif/esp_lcd_co5300` | 否 | ✅ |
| `espressif/esp_lcd_ili9341` | 否 | ✅ |
| `espressif/esp_lcd_gc9a01` | 否 | ✅ |
| `espressif/esp_lcd_st77916` | 否 | ✅ |
| `espressif/esp_lcd_axs15231b` | 否 | ✅ |
| `espressif/esp_lcd_st7701` | 否 | ✅ |
| `espressif/esp_lcd_st7796` | 否 | ✅ |
| `espressif/esp_lcd_spd2010` | 否 | ✅ |
| `espressif/esp_io_expander_tca9554` | 否 | ✅ |
| `waveshare/custom_io_expander_ch32v003` | 否 | ✅ |
| `espressif/esp_lcd_panel_io_additions` | 否 | ✅ |
| `78/esp_lcd_nv3023` | 否 | ✅ |
| `espressif/esp_lcd_touch_ft5x06` | 否 | ✅ |
| `espressif/esp_lcd_touch_gt911` | 否 | ✅ |
| `espressif/esp_lcd_touch_gt1151` | 否 | ✅ |
| `waveshare/esp_lcd_touch_cst9217` | 否 | ✅ |
| `espressif/esp_lcd_touch_cst816s` | 否 | ✅ |
| `espressif/esp_lcd_touch_st7123` | 否 | ✅ |
| `espressif/esp32-camera` | **camera 要删** | ✅ |
| `espressif/esp_video` | **video 要删** | ✅ |
| `espressif/esp_image_effects` | 否 | ✅ |
| `espressif2022/image_player`（当前 CMakeLists 用过一次但依赖在 `esp_emote_expression`） | 需复核 | ❓ **需跑 build 验证** |
| `espressif/led_strip` | 否（LED 文件已删） | ✅ |
| `espressif/knob` | 否（knob 文件已删） | ✅ |
| `espressif/adc_battery_estimation` | 需复核（`adc_battery_monitor.cc` 是否依赖） | ❓ **需 grep 验证** |
| `espressif/esp_new_jpeg` | 需复核（`display/lvgl_display/jpg/` 下有 JPG 代码） | ❓ **需 grep 验证** |
| `wvirgil123/sscma_client` | 否 | ✅ |
| `tny-robotics/sh1106-esp-idf` | 否 | ✅ |
| `espfriends/servo_dog_ctrl` | 否（`esp32c3` only） | ✅ |
| `llgok/cpp_bus_driver` | 否（`esp32p4` only） | ✅ |
| `espressif/bmi270_sensor` | 否 | ✅ |
| `espressif/iot_usbh_rndis` | 否（rndis_board 要删） | ✅ |
| `txp666/otto-emoji-gif-component` | 否 | ✅ |
| `espressif/esp_io_expander_tca95xx_16bit` | 否 | ✅ |
| `78/uart-eth-modem` | 否（非 esp32，但 S3 条件下仍拉；无源调用） | ✅ |
| `espressif/esp_lcd_jd9365` (p4 only) | 否 | ✅ |
| `waveshare/esp_lcd_st7703` (p4 only) | 否 | ✅ |
| `espressif/esp32_p4_function_ev_board` | 否 | ✅ |
| `espressif/esp_lcd_ili9881c` | 否 | ✅ |
| `espressif/esp_lcd_ek79007` | 否 | ✅ |
| `espressif/esp_hosted` | 否 | ✅ |
| `espressif/esp_wifi_remote` | 否 | ✅ |

**保留**：`78/esp-wifi-connect`、`78/esp-ml307`、`78/xiaozhi-fonts`、`espressif/esp_audio_effects`、`espressif/esp_audio_codec`、`espressif/esp-sr`、`espressif/esp_codec_dev`、`espressif/button`、`espressif2022/esp_emote_expression`、`espressif/esp_mmap_assets`、`lvgl/lvgl`、`esp_lvgl_port`。

**小计**：~35 条依赖移除 ≈ 80 行 yml

> ⚠️ **批次 3 验证脚本**：删完后 `rm -rf managed_components/ dependencies.lock && idf.py reconfigure` 重跑一次，对比保留下的 `managed_components` 目录；如果意外出现某个已删依赖 → 那个依赖其实被 transitively 需要，要回退。

---

### E. 多余的依赖组件

见 D.2 表格。**另外需要动的**：

| 动作 | 风险 |
|---|---|
| `main/idf_component.yml` 删 `espressif/knob: ^1.0.0`（已经没人调用） | 🟢 |
| 若 Camera 删：同时删 `espressif/esp32-camera` + `espressif/esp_video` | 🟡 |

---

### F. 多余的配置示例（`sdkconfig.defaults.*`）

| 文件 | 保留 | 原因 |
|---|---|---|
| `sdkconfig.defaults` | ✅ | 通用基线 |
| `sdkconfig.defaults.esp32s3` | ✅ | **唯一在用** 的芯片特化 |
| `sdkconfig.defaults.esp32p4` | ❌ 可删 | 三块板都不是 P4 |

**小计**：1 文件 ≈ 25 行

---

### G. 文档与示例

| 文件 | 建议 | 原因 |
|---|---|---|
| `README.md` / `README_zh.md` | ❌ **重写**（非本规划范围） | 当前仍是 upstream xiaozhi 的宣传稿，不反映已精简到三板的现状 |
| `README_ja.md`（已 `git rm`） | ⏳ 已删除未提交 | 确认并提交 |
| `docs/known-issues.md`（已 `git rm`） | ⏳ 已删除未提交 | 确认并提交 |
| `docs/sdk-analysis.md`（已 `git rm`） | ⏳ 已删除未提交 | 确认并提交 |
| `docs/websocket.md`、`docs/mqtt-udp.md`、`docs/mcp-*` | ✅ 保留 | 协议规范，三块板都需要 |
| `docs/blufi.md` | ✅ | 配网流程文档 |
| `docs/custom-board.md` | ✅ | 未来扩展板时要查 |
| `docs/code_style.md` | ✅ | C++ 风格规范 |
| `docs/audit_pass1_architecture.md`（未跟踪） | ⏳ 你的新文档，待整理 | 不动 |
| `main/boards/mydazy-p30-4g/HARDWARE.md` + `MYDAZY-P30.pdf` | ✅ | 硬件参考 |
| `main/boards/mydazy-p30-4g/ui_img_start_logo_png.c` | ❓ **待确认** | grep 其他板没这个，是否 P30-4G 专用开机图？看名字像 |

**小计**：本批次仅清理已删的文件状态，不再删新的。

---

### H. CI / 脚本

| 项 | 动作 |
|---|---|
| `.github/workflows/build.yml` | ✅ **不改**。是按 `scripts/release.py --list-boards` 动态 matrix，board 从 `main/boards/*/config.json` 枚举，三板自动生效 |
| `scripts/release.py` | ✅ 保留 |
| `scripts/build_default_assets.py`、`gen_lang.py`、`versions.py` | ✅ 保留 |
| `scripts/p3_tools/`、`scripts/Image_Converter/`、`scripts/ogg_converter/`、`scripts/spiffs_assets/` | ✅ 资产/多语言辅助，保留 |
| `scripts/audio_debug_server.py` | ✅ 保留（调试用） |
| `scripts/download_github_runs.py` | ❓ **待确认**（是否自用？）— 默认保留 |
| `scripts/mp3_to_ogg.sh` | ✅ 保留 |
| `scripts/sonic_wifi_config.html` | ❓ **待确认**（声波配网 HTML，当前配网是蓝牙+热点，未见引用）— 建议删，但需确认 |

---

### I. Kconfig 僵尸选项（独立一块，重要）

`main/Kconfig.projbuild` 515 行里**至少 200 行**是已删板的遗留配置菜单。所有 `depends on BOARD_TYPE_XXX`（其中 XXX 不在当前 choice 里的）对应的 choice/config 都是不可达代码。

**僵尸引用总数**：42 条 `depends on BOARD_TYPE_(LILYGO_T_DISPLAY_P4|BREAD_COMPACT_*|TAIJI_PI_S3|ESP_S3_LCD_EV_Board|ESP_KORVO2_V3*|ESP_BOX*|ECHOEAR|LICHUANG_DEV_S3|ESP_SENSAIRSHUTTLE|WAVESHARE_*|HU_087|CGC|ESP_HI)`。

**删除块**：
| 行区间（大致） | 内容 | 可删 |
|---|---|---|
| `LILYGO_T_DISPLAY_P4` screen type / pixel format | ~20 行 | ✅ |
| `ESP_S3_LCD_EV_Board_Version_TYPE` | ~10 行 | ✅ |
| `DISPLAY_OLED_TYPE` 整个 choice（`BREAD_COMPACT_*` / `HU_087`） | ~20 行 | ✅（注意：OLED 菜单本身可删，但不动 `oled_display.cc`，见 Part 1.7） |
| `DISPLAY_LCD_TYPE` 整个 choice（`BREAD_COMPACT_*_LCD` / `CGC` / `BREAD_COMPACT_WIFI_CAM`） | ~60 行 | ✅ |
| `DISPLAY_ESP32S3_KORVO2_V3` | ~15 行 | ✅ |
| `DISPLAY_ESP32S3_AUDIO_BOARD` | ~15 行 | ✅ |
| `DISPLAY_ESP32S3_TOUCH_LCD_1_85C` | ~10 行 | ✅ |
| `USE_EMOTE_MESSAGE_STYLE` 的 `depends on` 列表里僵尸引用（`ESP_BOX / ESP_BOX_3 / ECHOEAR / LICHUANG_DEV_S3 / ESP_SENSAIRSHUTTLE`） | depends 子句简化 | ✅（但 `USE_EMOTE_MESSAGE_STYLE` 本身**必留**，三块板都用） |
| `BOARD_TYPE_ESP_HI` 相关下载逻辑（`CMakeLists.txt:CONFIG_BOARD_TYPE_ESP_HI` 那块） | ~35 行 | ✅ |
| `menu "TAIJIPAI_S3_CONFIG"` 整块 | ~20 行 | ✅ |
| `USE_ESP_WAKE_WORD` choice 分支（非 S3/P4 走的） | 保留？ | ❓ **保留菜单结构，删不可达 choice 分支**（代码里 `esp_wake_word.cc` 已删） |

**小计**：~200 行 Kconfig + `CMakeLists.txt` 中的 `BOARD_TYPE_ESP_HI` 下载块 ~35 行 = **~235 行**

---

### J. 大件代码文件（camera + rndis + 死代码分支）

| 文件 | 行数 | 删除依据 |
|---|---|---|
| `main/boards/common/esp32_camera.cc` | 322 | 三块板 config.h 零 camera GPIO；主干代码 `grep Esp32Camera` 零命中 |
| `main/boards/common/esp32_camera.h` | 44 | 同上 |
| `main/boards/common/esp_video.cc` | 1041 | 同 camera；`grep EspVideo` 仅自身文件命中 |
| `main/boards/common/esp_video.h` | 53 | 同上 |
| `main/boards/common/camera.h` | 16 | camera 抽象基类，删 esp32_camera 后无实现，可一并删 |
| `main/boards/common/rndis_board.cc` | 246 | 专用于 `ESP_KORVO2_V3_RNDIS`，本仓库无此板；`grep RndisBoard` 零命中 |
| `main/boards/common/rndis_board.h` | 74 | 同上 |
| **依赖组件同步删** | — | `espressif/esp32-camera`、`espressif/esp_video`、`espressif/iot_usbh_rndis` |

**小计**：~1796 行 + 3 个依赖组件

---

## Part 3：风险与依赖分析

### R1. 反向依赖风险（已 grep 核实）

| 删除目标 | 反向引用点 | 处理 |
|---|---|---|
| `esp32_camera.*` / `esp_video.*` / `camera.h` | 零（除自身） | 安全 |
| `rndis_board.*` | 零 | 安全 |
| `esp_wake_word.h` 的 include（`audio_service.cc:35`） | 仅在 `#else` 分支（S3 永不命中） | 删 `#else` 整块 |
| `espressif/knob` 依赖 | 零（`knob.cc/h` 已被 `76d152ee` 删） | 安全 |
| `oled_display.cc/h` | `mcp_server.cc:264` + `board.cc:162` 硬引用 | ❌ **不删** |
| `no_audio_processor.cc` | CMakeLists 条件包含（`!CONFIG_USE_AUDIO_PROCESSOR` 时加入） | ❌ **不删**（上游兼容） |
| `sdkconfig.defaults.esp32p4` | 零 | 安全 |
| Kconfig 僵尸 choice（`BOARD_TYPE_LILYGO_*` 等） | 零（choice 里已无此 BOARD_TYPE） | 安全 |

### R2. Kconfig 级联风险

| 可能的级联 | 说明 |
|---|---|
| 删 `DISPLAY_LCD_TYPE` choice | `depends on` 的父 board 都不存在，删除安全；**不会**影响 P30/P31（它们不走此 choice，LCD 是 board 内部硬编码 JD9853） |
| 删 `USE_EMOTE_MESSAGE_STYLE` 的 `depends on` 列表僵尸项 | **只改 `depends on` 子句**，不删 config 本身。三块板 `config.json` 都设 `CONFIG_USE_EMOTE_MESSAGE_STYLE=y`，需要保留它可被选中 → depends on 改成 `(BOARD_TYPE_MYDAZY_P31 \|\| BOARD_TYPE_MYDAZY_P30_4G \|\| BOARD_TYPE_MYDAZY_P30_WIFI)` 或直接 `default y` 无 depends |
| `WAKE_WORD_TYPE` choice 保留结构 | `USE_ESP_WAKE_WORD` 分支（非 S3/P4）的 choice 表项可保留（反正不可达）或删掉分支本身——**建议保留结构**，与上游合并时少冲突 |
| `FLASH_ASSETS` choice | 三块 board 都 `CONFIG_FLASH_DEFAULT_ASSETS=y`，结构照旧 |

### R3. 历史参考价值

- **结论**：不另留参考板。
- 理由：upstream `78/xiaozhi-esp32` 是公开完整仓库，任何时候都能看到"如何写一块新板"。
- 加一块参考板 = 多一份 Kconfig 分支 + CMake 分支 + `config.json` + 板级目录要维护。跟你"精简"目标冲突。
- **如果你坚持留参考**：推荐从 upstream 拉 **`xiao_esp32s3`**（最小 S3 板，约 200 行）放回 `main/boards/xiao_esp32s3/`，Kconfig 加回一条 choice。但**我不建议**。

### R4. 上游合并风险（按严重度排序）

| 删除动作 | 冲突风险 | 说明 |
|---|---|---|
| 删 `main/boards/common/esp32_camera.*` + `esp_video.*` + `rndis_board.*` | 🔴 **高** | 这是 upstream 的主干文件。未来 upstream 更新 camera/rndis 逻辑，你的分支会持续遇到"文件不存在"的合并提示。建议删的时候把**删除决策**写进 commit message，合并时明确取舍 |
| 删 `main/CMakeLists.txt` 的 IDF_TARGET 分支 | 🟠 中 | 改 CMake，upstream 格式变化会冲突 |
| 删 `main/Kconfig.projbuild` 大量僵尸 choice | 🟠 中 | upstream 会持续新增 board，冲突主要在 choice 主列表 |
| 删 `main/idf_component.yml` 大量依赖 | 🟢 低 | yml 格式简单，冲突容易 resolve |
| 删 `sdkconfig.defaults.esp32p4` | 🟢 低 | 独立文件 |
| 删 `audio_service.cc` 的 `#else` 分支 | 🟠 中 | 修改 main/ 下文件，upstream 改 wake_word 逻辑时要合 |

**缓解建议**：
1. 所有 `main/` 下和 `main/boards/common/` 下的删除集中到 **一个 commit**，commit message 明确标注 "[Cleanup-Batch-N] 精简 P30/P31 不用的 XXX"。
2. 保留一份 `docs/audit/removed_from_upstream.md`（本规划执行时补）记录 upstream 里有、本仓库删掉了哪些，未来合并时 bot / 人工都能快速对照。

---

## Part 4：执行顺序与批次

### 批次 1 — 零风险（预计 1 人时）

- [ ] 确认并 commit 已 `git rm` 但未提交的文件（`README_ja.md`、`docs/known-issues.md`、`docs/sdk-analysis.md`、`partitions/v1/*`、`sdkconfig.defaults.esp32/c3/c5/c6`、`scripts/acoustic_check/*`）
- [ ] 删 `sdkconfig.defaults.esp32p4`
- [ ] 删 `main/idf_component.yml` 里 `espressif/knob: ^1.0.0`
- [ ] 删 `audio_service.cc:33-37` 的 `#else` 分支 + `line 717` 的 `EspWakeWord` 实例化（都是 S3 永不命中的死代码）

**🚪 闸门 1**：
```bash
idf55
rm -rf build managed_components dependencies.lock
python scripts/release.py mydazy-p30-4g  # 三板逐一
python scripts/release.py mydazy-p30-wifi
python scripts/release.py mydazy-p31
```
三个固件 zip 都生成即通过。

---

### 批次 2 — 低风险大件（预计 2 人时）

- [ ] 删 `main/boards/common/esp32_camera.{cc,h}`、`esp_video.{cc,h}`、`camera.h`
- [ ] 删 `main/boards/common/rndis_board.{cc,h}`
- [ ] `main/CMakeLists.txt` 同步：
  - 删 `esp32_camera.cc` / `esp_video.cc` / `rndis_board.cc` 的 `list(APPEND SOURCES ...)`
  - 删 `if(CONFIG_IDF_TARGET_ESP32)` 的 REMOVE_ITEM 整块
  - `CONFIG_IDF_TARGET_ESP32S3 OR CONFIG_IDF_TARGET_ESP32P4` → `CONFIG_IDF_TARGET_ESP32S3`（3 处）
  - 删 `CONFIG_BOARD_TYPE_ESP_HI` 的整块 `file(DOWNLOAD ...)` 下载逻辑（~35 行）
- [ ] 删 `main/idf_component.yml` 里 `espressif/esp32-camera`、`espressif/esp_video`、`espressif/iot_usbh_rndis`、`espressif/esp_image_effects`（仅 camera 用）
- [ ] 删 `main/idf_component.yml` 里的 **LCD/触摸未用依赖组**（见 D.2 表格打 ✅ 的那一批，除 `adc_battery_estimation` / `esp_new_jpeg` / `image_player` 三个待验证项）

**🚪 闸门 2**：
```bash
idf55
rm -rf build managed_components dependencies.lock
idf.py reconfigure  # 重新拉组件
# 对比 managed_components/ 目录，确认删的组件真没被 transitive 引入
ls managed_components/ | grep -E "esp32-camera|esp_video|iot_usbh_rndis|sh8601|co5300|ili9341|gc9a01|st77916|axs15231b|st7701|st7796|spd2010|cst9217|cst816s|gt911|ft5x06|gt1151|image_player|led_strip|knob|sscma|sh1106"
# 应全部无返回
python scripts/release.py mydazy-p30-4g
python scripts/release.py mydazy-p30-wifi
python scripts/release.py mydazy-p31
# 对比固件大小：三板 .bin 体积应显著下降（预计 -5% ~ -10%）
```

---

### 批次 3 — 中风险（Kconfig/CMake 裁剪，预计 1 人日）

- [ ] `main/Kconfig.projbuild`：按 Part 2.I 表格分块删除僵尸 choice 和 config
  - 策略：**每删一块，`idf.py menuconfig && idf.py build`** 验证菜单能进、config 能选、build 能过
  - 顺序建议：`LILYGO_T_DISPLAY_P4 → ESP_S3_LCD_EV_Board → BREAD_COMPACT_* → DISPLAY_LCD_TYPE/OLED_TYPE → ESP_KORVO2_V3 → WAVESHARE → TAIJIPAI → ESP_BOX/ECHOEAR/LICHUANG 的 depends 子句简化 → WAKE_WORD_TYPE 收紧`
- [ ] `main/idf_component.yml`：处理 D.2 表格里 ❓ 的三项（`adc_battery_estimation`、`esp_new_jpeg`、`image_player`），逐一尝试删 → build → 不通过就回退
- [ ] 复核并处理 Part 1 中的 ❓ 项：
  - `main/boards/common/axp2101.{cc,h}` grep 是否有板引用，否则删
  - `main/boards/common/lamp_controller.h` 同上
  - `scripts/sonic_wifi_config.html` 是否还用
  - `main/boards/mydazy-p30-4g/ui_img_start_logo_png.c` 是 P30-4G 专用资产还是僵尸（grep build 产物）

**🚪 闸门 3**：
```bash
idf55
rm -rf build managed_components dependencies.lock
python scripts/release.py all       # 三板全编
# 烧录真机（Jack 物理动作）：
#   idf.py -p /dev/cu.usbmodem2101 -b 2000000 flash monitor
#   验证：启动动画、配网（蓝牙+热点）、唤醒词、对话、OTA 下发配置切协议
```

---

### 批次 4 — 高风险（默认不执行）

**默认不做**。只有前三批稳定运行 1–2 周后、确实有强需求时再考虑：

- `main/` 主干代码裁剪（如移除 `no_audio_processor.cc` 等上游兼容通道）
- 按 board 条件编 `ml307_board.cc`（P30-WiFi 不需要 4G 代码）→ 能省点 flash 但会引入 `#ifdef` 污染
- 合并百度协议 1380 行（前提：确认删除或接入）
- 重写 `README.md` 反映精简后的现状

---

## 附录 A：证据索引（每条删除决策的原始依据）

| 决策 | 验证命令 | 结果 |
|---|---|---|
| Camera 零引用 | `grep -rE "Esp32Camera\|esp32_camera\|EspVideo\|esp_video\.h" main/ --include='*.cc' --include='*.h'`（排除自身） | 空 |
| RNDIS 零引用 | `grep -rE "RndisBoard\|rndis_board\.h" main/ --include='*.cc' --include='*.h'`（排除自身） | 空 |
| OLED 不能删 | `grep -rE "OledDisplay\|oled_display\.h" main/`（排除 display/） | `mcp_server.cc:264`、`board.cc:162` |
| Camera GPIO 缺失 | `grep -iE "CAMERA" main/boards/{mydazy-p30-4g,mydazy-p30-wifi,mydazy-p31}/config.h` | 空 |
| 三板都 S3 | `cat main/boards/*/config.json | grep target` | 三处 `"esp32s3"` |
| P30 codec | `grep "box_audio_codec" main/boards/{mydazy-p30-4g,mydazy-p30-wifi}/*.cc` | ES8311+ES7210 |
| P31 codec | `grep "es7111_audio_codec" main/boards/mydazy-p31/*.cc` | ES7111+ES7210 |
| MQTT 是默认 | `sed -n '497,514p' main/application.cc` | 见 Part 1.4 |
| 百度协议未接线 | `grep -rE "WebsocketBaidu\|websocket_baidu" main/application.cc main/ota.cc main/settings.cc` | 空（只存在于 `main/protocols/websocket_baidu_protocol.cc/h`） |
| Kconfig 僵尸 BOARD_TYPE 数 | `grep -cE "BOARD_TYPE_\|LCD_ST\|BREAD_COMPACT\|TAIJIPAI" main/Kconfig.projbuild` | 42 |
| CI 是动态 matrix | `cat .github/workflows/build.yml` | `release.py --list-boards --json` 动态生成 |

---

## 附录 B：预估清理后的体量

| 维度 | 当前 | 预估清理后 | 减少 |
|---|---|---|---|
| `main/` + `main/boards/common/` 代码行 | ~19000 | ~17200 | -1800 |
| `main/Kconfig.projbuild` | 515 行 | ~280 行 | -235 |
| `main/CMakeLists.txt` | 526 行 | ~470 行 | -56 |
| `main/idf_component.yml` | 120 行 | ~50 行 | -70 |
| `sdkconfig.defaults.*` | 3 个 | 2 个 | -1 文件 |
| `managed_components/` 下载量 | ~待测 | 显著下降 | |
| 三板固件 `.bin` 体积 | 当前 | 预计 -5%～-10% | |

---

## 下一步

1. 你 review 本文档，确认或修改删除范围
2. 我把「需要人工确认」的 ❓ 项收齐一次再问你
3. 确认后按批次执行，每批闸门通过再进下一批
4. 每批结束生成变更记录（`docs/audit/cleanup_batch_N.md`），commit message 标 `[Cleanup-Batch-N]` 前缀
