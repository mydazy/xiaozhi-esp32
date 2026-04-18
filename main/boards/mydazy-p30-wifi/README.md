# MyDazy P30 WiFi 开发板（纯 WiFi 版，不含 4G）

## 硬件配置

- **主控**: ESP32-S3 (8MB PSRAM + 16MB Flash)
- **显示**: 284x240 LCD (JD9853) + AXS5106L 电容触摸
- **音频**: ES8311 (DAC) + ES7210 (ADC)，BoxAudioCodec 驱动
- **网络**: WiFi + BLE（无 4G 模组）
- **传感器**: SC7A20H 三轴加速度计
- **电源**: 1000mAh 锂电池，LDO 总开关 (GPIO9)

## 与 4G 版本（mydazy-p30-4g）的差异

| 特性 | P30-4G | **P30-WiFi（本板）** |
|------|--------|---------------------|
| 基类 | `DualNetworkBoard` | `WifiBoard` |
| 4G 模组 | ML307R (GPIO1/2/6) | ❌ 无 |
| 双网络切换 | ✅ WiFi ↔ 4G | ❌ 仅 WiFi |
| 连按 3 次按键 | 切换 WiFi/4G 网络 | 进入配网模式 |
| 状态上报 | cellular (CSQ/Carrier) | wifi (RSSI) |
| GPIO1/2/6 | ML307 UART/DTR | 预留可扩展 |
| 其他硬件 | 完全一致（音频/显示/触摸/传感器/电源） | — |

## 与 P31 的差异

| 特性 | P30-WiFi | P31 |
|------|----------|-----|
| 音频 DAC | ES8311 (I2C+I2S) | ES7111 (纯 I2S) |
| 音频驱动 | BoxAudioCodec | Es7111AudioCodec |
| 音频电源 | GPIO_9 | GPIO_15 |
| 充电检测 | GPIO_21 | GPIO_44 |
| TE 引脚 | GPIO_40 | NC |
| Type-C 耳机 | 无 | 有 |
| NFC | 无 | WS1850S |
| 4G | 无 | ML307R + GNSS |

## 功能特性

- ✅ WiFi + Blufi（BLE）配网
- ✅ 284×240 彩屏 + AXS5106L 触摸
- ✅ 离线 AFE 唤醒词 + 云端 ASR/LLM/TTS
- ✅ SC7A20H 晃动/拿起唤醒
- ✅ 深度睡眠 + BOOT/EXT1/Timer 多唤醒源
- ✅ 1000mAh 电池 + ADC 电量 + 充电检测
- ✅ Emote 动画表情显示
- ❌ 不支持 4G 上行
- ❌ 不支持 GNSS 定位
- ❌ 不支持 Type-C 耳机、NFC

## 按键交互

| 动作 | 功能 |
|------|------|
| BOOT 单击 | 开始/结束对话 |
| BOOT 双击 | 切换 AEC 开关 |
| BOOT 长按 | 录音（按住讲话） |
| BOOT 连按3次 | 进入 WiFi 配网模式 |
| BOOT 连按4次 | 深度睡眠关机 |
| BOOT 连按6次 | 进入音频测试模式 |
| BOOT 连按9次 | 恢复出厂设置（10秒内双击确认） |
| VOL+ / VOL- | 音量 +10 / -10，长按连续调节 |
| 触摸单击 | 唤醒/打断 TTS |

## 编译与烧录

**方式一：release.py（推荐）**
```bash
python scripts/release.py mydazy-p30-wifi
```

**方式二：idf.py 手动**
```bash
source idf54   # 此仓库基于上游 v2.2.4，使用 idf54
idf.py set-target esp32s3
idf.py fullclean
idf.py menuconfig   # Xiaozhi Assistant → Board Type → MyDazy P30 WiFi
idf.py build
idf.py -p /dev/cu.usbmodem2101 -b 2000000 flash monitor
```

## GPIO 映射

完整映射见 `mydazy-p30-4g/HARDWARE.md`（本板同 PCB，仅 GPIO1/2/6 未使用）。核心映射：

| GPIO | 功能 |
|------|------|
| 0 | BOOT 按键（多功能） |
| 3 | SC7A20H 中断 |
| 4/5 | 触摸屏 RST / INT |
| 8 | 电池电压 ADC（CH7） |
| 9 | LDO 总开关（LCD + 音频） |
| 10 | 功放 PA_EN |
| 11/12 | I2C1 SDA/SCL（音频+触摸+传感器共线） |
| 13/14/16/17/18 | I2S DOUT/WS/BCLK/MCLK/DIN |
| 21 | 充电状态检测 |
| 38/39/40/47/48 | LCD SPI MOSI/CS/TE/SCLK/DC |
| 41 | 背光 PWM |
| 42/45 | VOL+ / VOL- |

## 调试注意

- 此仓库使用 **idf54**（基于上游 v2.2.4），不是 idf55
- I2C 总线挂 4 个 slave（ES8311/ES7210/AXS5106L/SC7A20H），外部 10kΩ 上拉
- GPIO9 LDO 断电会同时关闭 LCD + 音频，500ms 电容放电后方可重启
