# MyDazy-P30-4G 硬件设计文档

> 版本：基于固件 v1.9.66 硬件映射提取
> MCU：ESP32-S3（8MB PSRAM，16MB Flash）
> 更新日期：2026-04-17

---

## 1. 系统硬件概览

```
                    ┌────────────────────────┐
                    │      ESP32-S3 MCU      │
                    │  8MB PSRAM + 16MB Flash│
                    └────────────────────────┘
                              │
       ┌───────────┬──────────┼──────────┬──────────┐
       │           │          │          │          │
   ┌───▼──┐   ┌───▼────┐ ┌───▼────┐ ┌──▼────┐ ┌────▼────┐
   │ 音频 │   │  显示  │ │ 触摸屏 │ │  4G   │ │  传感器  │
   │I2S+I2C│  │SPI+PWM │ │ I2C+INT│ │ UART  │ │ I2C+INT │
   └──────┘   └────────┘ └────────┘ └───────┘ └─────────┘
       │           │          │          │          │
   ES8311     JD9853     AXS5106L    ML307R    SC7A20H
  +ES7210   284×240 LCD  触控芯片   4G 模组    三轴加速度
  +PA 功放
                              │
                         ┌────┴────┐
                         │ 按键×3  │
                         │ BOOT    │
                         │ VOL+/-  │
                         └─────────┘
                              │
                         ┌────┴────┐
                         │ 电源系统 │
                         │ BAT ADC │
                         │ 充电检测 │
                         │ LDO 控制 │
                         └─────────┘
```

### 核心产品定位
- 面向 3-8 岁儿童的 AI 语音陪伴设备
- 284×240 彩色 LCD + 电容触控
- WiFi + 4G 双网络（室内 WiFi，室外 4G 自动切换）
- 离线唤醒（ESP-SR）+ 云端 ASR/LLM/TTS
- 电池供电（1000mAh）+ USB 充电
- 晃动/拿起/按键唤醒 + 定时器唤醒（闹钟）

---

## 2. 完整 GPIO 映射

### ESP32-S3 GPIO 总体使用率

| 类别 | 数量 |
|------|------|
| 已使用 | **25 个** |
| 未使用可扩展 | ~8 个 |
| 内部占用（Flash/PSRAM/USB/UART0） | ~15 个 |

### 详细 GPIO 分配表

| GPIO | 功能 | 方向 | 说明 | 备注 |
|------|------|------|------|------|
| **0** | BOOT 按键 | IN | 多功能按键（单击/双击/长按/连按） | ⚠️ Strap pin，烧录触发 |
| **1** | ML307_RX | UART TX | ESP32 → ML307 数据 | 4G 模块 UART |
| **2** | ML307_TX | UART RX | ML307 → ESP32 数据 | 4G 模块 UART |
| **3** | SC7A20H_INT1 | IN+INT | 三轴加速度中断（运动检测） | ⚠️ JTAG TDI，调试冲突 |
| **4** | TOUCH_RST | OUT | 触摸屏复位 | |
| **5** | TOUCH_INT | IN+INT | 触摸事件中断 | 硬件确认机制 |
| **6** | **MODEM_DTR** | OUT | ML307R `MAIN_DTR` 控制（睡眠/唤醒握手） | ✅ 已启用（v1.9.66+） |
| **7** | **USB_DET**（预留） | IN | USB 数据口检测 | ⚠️ 原分压电路 100K:10K 不适合电平检测，**充电检测改用 GPIO21 CHRG**；GPIO7 保留给其他业务 |
| **8** | BATTERY_ADC | ADC1_CH7 | 电池电压检测 | ADC1 可与 WiFi 共存 |
| **9** | **AUDPWR-EN 总电源开关** | OUT | 🔴 **同时控制 LCD+音频+4G VDD_EXT 的 LDO 总开关**（VT3+VT4 级联） | 深睡前断电 / 软重启前必须拉低 |
| **10** | AUDIO_PA | OUT | 功放使能 | 播放时拉高 |
| **11** | I2C_SDA | I/O | I2C1 数据（共用总线） | 外部 10kΩ 上拉 |
| **12** | I2C_SCL | I/O | I2C1 时钟（共用总线） | 外部 10kΩ 上拉 |
| **13** | I2S_DOUT | OUT | I2S 播放数据（→ES8311） | |
| **14** | I2S_WS | OUT | I2S 字选择（LRCLK） | |
| **15** | **ES8311_ASDOUT** | IN | ES8311 ADC 数据输出（麦克风→I2S）| ⚠️ config.h 漏定义，P30 用 ES7210 录音，此线闲置 |
| **16** | I2S_BCLK | OUT | I2S 位时钟 | |
| **17** | I2S_MCLK | OUT | I2S 主时钟 | |
| **18** | I2S_DIN | IN | I2S 录音数据（ES7210→） | |
| 19 | — | USB D- | 内部 USB | ESP32-S3 USB-OTG |
| 20 | — | USB D+ | 内部 USB | ESP32-S3 USB-OTG |
| **21** | CHARGE_DET | IN | 充电状态检测（**低=充电中**，开漏输出 + 内部上拉） | |
| 26-32 | — | SPI Flash/PSRAM | ESP32-S3 内部占用 | 不可用 |
| 33-37 | — | Octal PSRAM | 若使用 Octal PSRAM | 部分可能占用 |
| **38** | LCD_MOSI | SPI OUT | LCD SPI 数据 | |
| **39** | LCD_CS | SPI CS | LCD 片选 | |
| **40** | LCD_TE | IN (预留) | 撕裂效应信号 | ⚠️ **预留未用** |
| **41** | BACKLIGHT | PWM | 背光亮度控制 | LEDC 通道 |
| **42** | VOL_UP | IN | 音量+按键 | |
| 43 | — | UART0 TX | 内部 UART0（串口日志） | 系统保留 |
| 44 | — | UART0 RX | 内部 UART0（串口日志） | 系统保留 |
| **45** | VOL_DOWN | IN | 音量-按键 | |
| 46 | — | — | 未使用 | 可扩展 |
| **47** | LCD_SCLK | SPI CLK | LCD SPI 时钟 | |
| **48** | LCD_DC | OUT | LCD 数据/命令选择 | |

### 特殊/保留信号
| 信号 | 状态 | 说明 |
|------|------|------|
| LCD_RESET | `GPIO_NUM_NC` | 未连接，使用软件复位命令 |
| LCD_MISO | `GPIO_NUM_NC` | 单向传输，省 1 个 GPIO |
| BUILTIN_LED | `GPIO_NUM_NC` | 屏幕上电时 D3/D1 LED 间接可视（功放/充电指示）|
| ML307/ML307R PWR_EN 独立 GPIO | `GPIO_NUM_NC` | **通过 GPIO9 AUDPWR-EN 级联控制（隐式电源开关）** |
| ML307/ML307R RESET 独立 GPIO | `GPIO_NUM_NC` | 🔴 无独立复位，只能靠 GPIO9 断电或 AT+CFUN |

---

## 3. 子系统详细

### 3.1 音频系统
- **编解码**：ES8311（播放）+ ES7210（4 麦克风阵列录音，实际只用 MIC1/MIC2 双麦）
- **I2C 地址**：ES8311 = `0x30` (7位 `0x18`)，ES7210 = `0x80` (7位 `0x40`)
- **I2S 配置**：采样率 24kHz（输入输出一致），16bit，立体声
- **AEC 回声消除**：TDM 4-slot（`AUDIO_INPUT_REFERENCE = true`），参考信号通过 ES7210 REFP34/REFQ34 回环
- **电源管理**：🔴 **GPIO9（AUDPWR-EN）是全局电源总开关，通过 VT3（AO3401 PMOS）+ VT4（SK2302AAT NMOS）级联开关 LCD+音频+4G VDD_EXT。断电=三者同时失效！**
- **功放**：NS4150B D 类功放（8.5W/4Ω），独立 PA 使能引脚（GPIO10）
- **音频输出路径（两路并行）**：
  - 内置喇叭（PCB 主喇叭接口）
  - **PJ-237 3.5mm 音频输出插座**（外接耳机/有源音箱，带 AO-EN 信号）
- **ES8311 ADC 闲置**：ES8311 ASDOUT → GPIO15 悬空未启用

### 3.2 显示系统
- **屏幕型号**：**HQR180009BH**（湖南宏祺瑞科技有限公司），1.80" TFT
- **原生分辨率**：**240×284**（portrait 竖屏）
- **软件可视**：**284×240**（JD9853 通过 `MADCTL` 做 SWAP_XY + MIRROR_X 逆时针 90° 旋转）
- **LCD 驱动 IC**：JD9853（本项目自研驱动 `components/esp_lcd_jd9853`）
- **总线**：SPI2_HOST，单向 MOSI
- **帧缓冲**：LVGL PSRAM 双缓冲，全屏 136KB
- **DMA**：SPI DMA 自动通道（`SPI_DMA_CH_AUTO`）
- **单次传输块**：284×48×2 = 27.3KB（4 行分片，提升 LVGL 刷新并行度）
- **背光**：PWM 单通道（LEDC + GPIO41）
- **TE 信号**：硬件连接到 GPIO40，软件**未启用 VSYNC 同步**

### 3.3 触摸系统
- **驱动 IC**：**AXS5106L**（深圳芯之源 ChipSourceTek，焊在屏 FPC 上）
- **TP 固件**：**V2905**（十六进制写法，=`V10501` 十进制，0x400 偏移双字节 BE）
  - 由屏厂烧写 MTP，**active area 硬编码在固件内**，host 侧无法通过 I2C 配置
  - 内嵌固件 `axs5106l_firmware.h`（15071 字节 8051 二进制），每次启动校验版本，不同则升级
- **I2C 地址**：`0x63`（7位）
- **总线**：I2C1（与音频编解码共用），速率 400kHz
- **中断**：硬件 INT 信号（GPIO5）确认触摸事件，避免 4G RF 干扰
- **手势支持**：host 软件识别——单击/双击/长按/长按释放/上滑/下滑/左滑/右滑
- **软件复位命令**：`{0xB3, 0x55, 0xAA, 0x34, 0x01}` 写入 `0xF0`
- **坐标映射**：chip 已内部做 rotation，直接输出 landscape 坐标 [0..283]×[0..239]，驱动 1:1 直出（和 v1/udour 一致）
- ⚠️ **已知 edge suppression**（V2905 固件限制）：
  - 实测 raw 范围 X∈[21..273]、Y∈[1..236]，**屏幕左 ~21px / 右 ~10px / 下 ~3px 触不到**
  - 根因烧在 chip MTP 固件里，AXS5106L 无 runtime 配置寄存器（调研确认）
  - **唯一解法**：联系屏厂（湖南宏祺瑞）出一版 edge suppression=0 的新固件
  - 量产 UI 设计**按键避开屏幕边缘 ≥25px**

#### AXS5106L 可用 I2C 寄存器（穷举）

| 寄存器 | 作用 |
|-------|------|
| `0x01` | 读触摸数据（6 字节：gesture + num + XH/XL/YH/YL） |
| `0x05` | 读固件版本（2 字节 BE） |
| `0x08` | 读 chip ID（3 字节） |
| `0x19` | Sleep 命令（写 `0x03`） |
| `0xF0` | 软复位（写 `{0xB3,0x55,0xAA,0x34,0x01}`） |
| `0x80/0x90/0xA0/0xAA` | 固件升级专用（调试模式/解锁/擦写 flash） |

**没有**分辨率 / 工作区 / edge suppression / mirror 配置寄存器。

### 3.4 网络系统（双网络）
- **WiFi/BLE 天线**：A1 2.4G 贴片天线（ESP32-S3 内置）
- **蓝牙能力**：⚠️ ESP32-S3 **仅支持 BLE 5.0**，不支持 Classic BT，因此**无 A2DP/HFP 等音频协议**
- **4G 模组**：**ML307R（合宙 Cat.1 模组）** — ⚠️ 注意：原理图 PDF 第 3 页错误标注为 `Air780EP`，实际物料是 ML307R（代码 `components/78__esp-ml307/` 与硬件一致，原理图需修正）
- **4G 天线**：A2 IPEX 主天线 + A3 IPEX 辅天线（MIMO 预留）
- **SIM 卡**：Nano SIM 卡座（USIM1）+ USIM2 预留（双卡硬件，单卡实装）
- **UART**：ESP32 GPIO1/2 ↔ ML307R MAIN_RXD/TXD (17/18)
- **DTR**：ESP32 GPIO6 ↔ ML307R MAIN_DTR (19) — **硬件连接但软件未用**
- **电源控制**：ML307R VDD_EXT 由 GPIO9（AUDPWR-EN）通过 VT3/VT4 级联控制（与 LCD/音频共用电源轨）
- **切换逻辑**：`DualNetworkBoard` 基类实现，优先 WiFi，失败回退 4G
- **BLE 配网**：Blufi 协议（ESP32 内置 BLE），用于首次配网

### 3.5 传感器
- **SC7A20H**：I2C 三轴加速度计（0x19）
- **功能**：运动检测唤醒、晃动停止闹钟、拿起唤醒
- **防抖**：500ms 软件防抖
- **初始化**：I2C 重试 3 次，失败则弹窗提示

### 3.6 电源系统
- **电池**：1000mAh（小容量，续航约 2-4 小时对话）
- **检测**：
  - 电压：ADC1_CH7（GPIO8）读取分压后的电池电压
  - 充电：GPIO21 电平检测（**低=充电中**，充电芯片开漏输出，ESP32 内部上拉拉高表示未充电）
- **深睡**：ESP32 Deep Sleep（原生 wake source，未启用 ULP 协处理器）
- **唤醒源**：
  1. EXT0/EXT1：BOOT 键 + SC7A20H 中断
  2. TIMER：定时器唤醒（闹钟/定时上报）

---

## 4. 硬件资源占用分析

| 资源 | 已用 | 总量 | 余量 | 备注 |
|------|------|------|------|------|
| **GPIO** | 25 | ~40 可用 | ~8 扩展 | 去除内部占用后 |
| **I2C** | 1 (I2C_NUM_1) | 2 | 1 | I2C_NUM_0 空闲 |
| **SPI** | 1 (SPI2) | 3 | 2 | SPI3 + SPI_LCD 可用 |
| **I2S** | 1 (I2S_NUM_0) | 2 | 1 | 双工播放+录音 |
| **UART** | 2 (UART0 日志 + UART1 4G) | 3 | 1 | UART2 可用 |
| **ADC1** | 1 通道 (CH7) | 10 | 9 | 电池检测 |
| **LEDC** | 1 通道 (背光) | 8 | 7 | 充裕 |
| **Timer** | 多个 esp_timer | 软件定时器 | ✅ | |

**I2C 总线设备**（I2C_NUM_1 @ 400kHz）：
| 设备 | 8位地址 | 7位地址 | 特性 |
|------|---------|---------|------|
| ES8311 音频编解码 | `0x30` | `0x18` | 初始化后访问少 |
| ES7210 麦克风阵列 | `0x80` | `0x40` | 初始化后访问少 |
| AXS5106L 触摸屏 | — | `0x63` | INT 驱动，非轮询 |
| SC7A20H 三轴加速度 | — | `0x19` | INT 驱动，非轮询 |
| ~~MS85163M 外部 RTC~~ | `0xA2/0xA3` | `0x51` | ⚠️ **原理图画了但实际 BOM 未贴片**（无需考虑） |

- **实际有效 slave = 4 个**（原理图画了 MS85163M RTC 但 BOM 未贴片，实际不上总线）
- 多数设备为中断/初始化后低频访问
- 实际冲突热点在 **4G 活跃窗口 vs 音频 AEC 参考写寄存器**

---

## 5. ESP32-S3 特殊引脚风险

| GPIO | 用途 | 风险 | 评估 |
|------|------|------|------|
| **GPIO 0** | BOOT 按键 | Strap pin，上电时电平影响启动模式 | ⚠️ 长按 BOOT 重启会进入下载模式，需硬件避免上电时长按 |
| **GPIO 3** | SC7A20H 中断 | JTAG TDI | ⚠️ 量产阶段 JTAG 调试不可用（一般量产不调试，OK） |
| GPIO 43-44 | UART0 | 系统日志 | ✅ 未作业务使用，OK |
| GPIO 19-20 | USB | OTG | ✅ 未作业务使用，OK |
| GPIO 26-32 | Flash/PSRAM | 系统保留 | ✅ 未占用 |

---

## 6. 硬件优点 ✅

1. **I2C 总线复用设计精良** — 4 个 I2C 设备共总线，节省 6 个 GPIO
2. **ADC1 选型正确** — WiFi 运行时 ADC2 不可用，选 ADC1 避免冲突
3. **外部上拉 10kΩ** — 不依赖 ESP32 内部上拉，信号完整性更好
4. **TE 信号已引出** — 硬件预留 VSYNC，未来可解决撕裂
5. **音频独立电源控制** — LDO 软件开关，深睡完全断电降低待机功耗
6. **PA 独立使能** — 不播放时关闭 PA，降低底噪和电流
7. **SPI 单向传输** — 省掉 MISO 引脚（LCD 不需要读）
8. **BOOT 键多功能复用** — 一个物理按键通过软件区分单击/双击/长按
9. **多唤醒源** — EXT0/EXT1/TIMER 组合，应对多种使用场景
10. **触摸硬件 INT 确认** — 避免 4G RF 干扰导致的 I2C 伪触发

---

## 7. 硬件缺点 / 风险 ⚠️

### 🔴 严重（影响量产/稳定性）

**1. 4G 模组（ML307R）电源与 LCD/音频耦合 — 无法独立控制**

原理图揭示：ML307R 的 `VDD_EXT` 通过 **VT3(AO3401 PMOS) + VT4(SK2302AAT NMOS) 级联开关**，与 LCD/音频的 `AUD_VDD-3.3V` **共用 GPIO9（AUDPWR-EN）** 控制。

- **GPIO6 = MAIN_DTR 被浪费**：硬件连接但软件完全未使用
  - 正确用法：
    - `DTR 拉低` → 唤醒 ML307R
    - `DTR 拉高` → 允许 ML307R 进入睡眠（省电）
- **ML307R 独立 RESET-N 引脚（模块 15 脚）未引出** — 原理图没有主控 GPIO 控制该引脚
- **AT 命令软重启**：`AT+CFUN=4` → `AT+CFUN=1`（飞行模式切换），协议栈层有效，硬件异常失效
- **极端补救**：`esp_restart()` + GPIO9 断电 500ms 间接硬复位 ML307R

**下版硬件 P0 改进建议**：
1. 🔴 **分离 4G 电源**：ML307R 独立 LDO（如 GPIO46 控制）
2. 🔴 **引出 ML307R RESET-N**：独立 GPIO（如 GPIO15 或 GPIO21）控制复位
3. 🟡 **启用 GPIO6 DTR 控制**：软件补丁即可利用现有硬件资源省电

**2. LCD 复位引脚未连接（已通过电源控制补救）**
- 软件已实现"电源级硬件复位"方案（`mydazy_p30_board.cc:1235-1244`, `1295-1299`）
  - LCD 和音频共用 LDO（GPIO9 AUDIO_PWR_EN）
- **残余问题**：
  - LCD 复位 = 音频同时断电，只能在 `esp_restart()` 前做，**运行期间不能做**
  - 如果 LCD 运行中异常（花屏/撕裂/总线挂死），只能整机重启才能恢复
- **下版硬件建议**：独立 LCD_RESET 引脚，运行中即可单独复位 LCD 不影响音频

**3. I2C 总线挂载设备较多（4 个 slave）**
- ES8311 + ES7210 + AXS5106L + SC7A20H = 4 个 slave（原理图 MS85163M RTC 未贴片）
- 4G RF 干扰会造成 I2C 传输错误（memory 明确记录）
- 音频 I2C 写寄存器较频繁，与触摸扫描可能竞争总线
- **下版硬件建议**：触摸屏或传感器迁移到 I2C_NUM_0（独立总线）

**软件补救方案（分级落地）**：

| 级别 | 方案 | 文件 / 做法 | 风险 |
|------|------|-------------|------|
| ⭐ P0（10 min） | **I2cDevice 基类加 3 次重试+退避** | `common/i2c_device.cc` WriteReg/ReadReg 循环重试 5/10/20ms | 低 |
| ⭐ P0（已做） | 触摸屏 INT 硬件确认 + 3 次重试 | `axs5106l_touch.cc:237-262` ✅ 已实现 | 低 |
| P1（1-2h） | **I2C 总线级恢复** | 连续失败 N 次调用 `i2c_master_bus_reset()` 手动时钟 9 次释放 SDA | 中 |
| P1（30min） | **触摸屏 ESD 硬件复位** | I2C 挂起时 `gpio_set_level(TOUCH_RST, 0); delay; set_level(1)` | 低 |
| P2（半天） | **设备健康状态机** | 每个 slave 错误计数，超阈值标记不可用，业务降级 | 中 |
| — | ~~启用 MS85163M~~ | BOM 未贴片，无需考虑 | — |
| P3（1-2 天） | **4G 活跃窗口规避** | 音频初始化避开 4G 传输爆发期 | 高（复杂度大） |

**推荐立即做 P0 I2cDevice 基类重试**，单文件 20 行改动覆盖 ES8311/ES7210/SC7A20H 全部。

### 🟡 中等（可运维规避）

**4. BOOT 按键作为业务多功能键**
- 系统上电时如用户长按 BOOT，会进入下载模式而非正常启动
- 用户"开机键"体验可能与烧录模式冲突
- 可通过硬件延时电路规避（上电先拉高再释放）

**5. 无状态 LED 指示**
- 屏幕关闭/休眠时用户无法通过指示灯判断设备状态（充电/在线/故障）
- 儿童产品家长需要外部指示
- **下版硬件建议增加 RGB 或单色 LED**

**6. 背光 PWM 单路**
- 无法实现 LCD 和状态灯独立亮度曲线
- 深睡部分唤醒场景无法「屏幕关但背光弱提示」

**7. 电池容量小（1000mAh）**
- 对话时功耗 ~200mW，续航约 2-4 小时
- 儿童使用场景（半天-全天）可能不够
- **下版硬件建议增加到 1500-2000mAh**

**8. 音量键占用 2 个 GPIO**
- VOL+（42）/VOL-（45）物理分离
- 可替换为单键旋钮（编码器）或 I2C 触摸滑动
- 但 2 键更符合儿童直觉操作

### 🟢 次要（可接受）

**9. GPIO 3 占用 JTAG TDI**
- 量产后无需 JTAG 调试，无实际影响
- 若需刷写固件 JTAG 仍可用 USB

**10. SPI_MISO 未连接**
- 无法读取 LCD 状态寄存器（调试困难）
- 量产后一般不需要读 LCD，OK

**11. TE 信号硬件接线但软件未使用**
- 没有利用 VSYNC 同步，可能有屏幕撕裂
- 软件层改造成本低，未来优化点

**12. ML307 无 DTR 引脚**
- 深睡后无法通过 UART 自动唤醒 4G
- 依赖 4G 模块自身电源管理状态机
- 增加唤醒延迟和不确定性

---

## 8. 下版硬件改进建议（P30-V2.x）

### 🔴 P0 必改
1. **ML307 控制引脚完整连接**
   - GPIO6 → ML307 PWR_EN（电源使能，软件可控断电）
   - GPIO7 → ML307 RESET_N（复位，软件可控重启）
   - 预留 GPIO15 → ML307 DTR（深睡唤醒信号）

2. **LCD 硬件复位引脚**
   - GPIO46（当前未用）→ LCD_RESET
   - 异常状态可硬件复位恢复

### 🟡 P1 建议
3. **I2C 总线拆分**
   - I2C_NUM_0：触摸屏 + 传感器（低频）
   - I2C_NUM_1：音频 ES8311/ES7210（高频 + AEC）
   - 物理隔离 4G RF 干扰敏感总线

4. **状态指示 LED**
   - 增加 1 个 RGB LED（或 3 色）作为充电/在线/故障指示
   - 使用 RMT 驱动 WS2812，只占 1 个 GPIO

5. **电池容量提升**
   - 1000mAh → 1500mAh 或 2000mAh
   - 改善续航至 6-10 小时

### 🟢 P2 可选
6. **启用 TE 同步**
   - 软件层启用 GPIO40 TE 中断
   - 实现 VSYNC，消除屏幕撕裂

7. **BOOT 键硬件保护**
   - 上电 RC 延时电路，避免启动时误入下载模式

---

## 9. 关键配置参数

| 参数 | 值 | 出处 |
|------|-----|------|
| 音频采样率 | 24000 Hz | `config.h` |
| 显示刷新 | LVGL 33ms 周期 | `config.json` `CONFIG_LV_DEF_REFR_PERIOD=33` |
| 触摸 I2C 速率 | 400 kHz | `config.h` |
| SPI DMA 分片 | 284×48×2 = 27.3KB | `InitializeSpi()` |
| SPIFFS 挂载 | /storage | `CONFIG_SPIFFS_BASE_PATH` |
| OTA URL | https://www.mydazy.cn/v1/ota/ | `config.json` |
| 闹钟最大循环 | 10 次 (~80s) | `ALARM_MAX_RINGS` |
| 晃动停止阈值 | 3 次 × 800mg | `ALARM_SHAKE_NEED` + `SHAKE_THRESHOLD_MG` |
| 电池容量 | 1000 mAh | `config.h` |

---

## 10. 参考资料

- [ESP32-S3 技术规格书](https://www.espressif.com/sites/default/files/documentation/esp32-s3_datasheet_en.pdf)
- [ML307R 4G 模组规格](https://wiki.luatos.com/chips/air780ep/)（合宙 Cat.1 模组）
- [ES8311 Datasheet](http://www.everest-semi.com)
- AXS5106L 触摸屏规格（私有协议）
- SC7A20H 三轴加速度规格（硅传感器）
- JD9853 LCD 驱动规格（京东方定制）
- MS85163M RTC 时钟芯片规格（未使用）

---

## 11. 原理图验证补充（MYDAZY-P30.pdf 确认）

> 本章基于原始原理图 `MYDAZY-P30.pdf`（3 页 A4）逐 net 核对，修正前文根据 config.h 的猜测。

### 11.1 config.h 与原理图的差异

| 项 | config.h 记载 | 原理图实际 | 影响 |
|----|---------------|------------|------|
| `GPIO6` | 未定义（NC） | `UART0_DTR` → ML307R MAIN_DTR | 可启用 4G 睡眠控制（软件补丁即可） |
| `GPIO7` | 未定义（NC） | `USB_DET`（USB 插拔检测） | 可启用充电器接入事件（软件补丁即可） |
| `GPIO15` | 未定义 | `ES8311_ASDOUT` | ES8311 附加数据通路，需定义 |
| `ML307_PWR_GPIO = NC` | 标注「未连接」 | 实际由 GPIO9 级联控制 | 文档误导，实际有控制路径 |
| `ML307_RST_GPIO = NC` | 标注「未连接」 | 无独立 RST 控制 | 确认无独立复位路径 |
| 4G 模组型号 | `ML307R` | 原理图错标 `Air780EP` | ⚠️ **实物是 ML307R，原理图需修正** |
| I2C 设备数 | 4（音频×2 + 触摸 + 传感器） | **5**（多 MS85163M RTC） | 未使用但占 I2C 总线 |
| Flash 型号 | — | 原理图符号 `BY25Q64ASSIG`（8MB）**BOM 实际贴片 BY25Q128（16MB）** | 原理图符号库未更新，软件 16MB 配置正确 |

### 11.2 电源拓扑（原理图验证）

```
           VBUS_5V (USB Type-C)
                │
           ┌────┴────┐
           │ TP4054  │ (锂电充电 IC)
           └────┬────┘
                │
             VBAT ──────────────── ML307R VBAT1/VBAT2 (常供电)
                │
                ├── VIN (ME6211C33M5G LDO 1) ──> VCC-3.3V (ESP32/主电)
                │
                └── VT3/VT4 级联开关 (GPIO9 控制)
                         │
                         ├── AUD_VDD-3.3V ──> ES8311/ES7210/NS4150B
                         ├── AUD_VDD-3.3V ──> LCD (JD9853)
                         └── VDD_EXT ──────> ML307R 外设供电
                             (非主供电，主电路仍靠 VBAT)
```

**含义**：
- ML307R 的 **VBAT 常供电** — 即使 GPIO9 断电，模组主电路仍通电
- GPIO9 只切断 ML307R 的 **VDD_EXT**（外设/IO 供电），模组核心 RF/Baseband 仍在工作
- 所以「断 GPIO9」实际上不能完全关闭 4G，只是断了模组与主控的 IO 通信
- **真正关闭 ML307R 需要 AT+CPWROFF 指令**（软件层），硬件无路可关

### 11.3 音频链路（原理图第 2 页）

```
                  ┌──── MIC1P/N (主麦)
                  ├──── MIC2P/N (辅麦)
                  ↓
               ES7210 (4 麦 ADC, I2C 0x40)
                  ↓ I2S (GPIO17/16/14/18)
               ESP32-S3 (AFE + AEC)
                  ↓ I2S (GPIO17/16/14/13)
               ES8311 (DAC, I2C 0x18)
                  ↓ (GPIO15 ASDOUT 辅通路)
                  ↓ SPKP/SPKN
               NS4150B (D 类功放 8.5W/4Ω, GPIO10 使能)
                  ↓
                Audio PJ-237 (喇叭接口)
```

### 11.4 按键/唤醒源（原理图第 1 页）

| 按键 | GPIO | 标注 | 电气 |
|------|------|------|------|
| KEY1 (BOOT) | GPIO0 | 2×4mm 轻触 | 串 510Ω, 1MΩ 上拉 |
| KEY2 (?) | GPIO45 | 2×4mm 轻触 | 串 1MΩ |
| KEY3 (?) | GPIO42 | 2×4mm 轻触 | 串 1MΩ |
| MCU-RESET | RST pin | 2×4mm（原理图有，但一般不对外） | 10KΩ 上拉 |

⚠️ config.h 定义：
- `VOLUME_UP = GPIO_NUM_42`
- `VOLUME_DOWN = GPIO_NUM_45`

但原理图上 KEY2 和 KEY3 **位置与 config.h 命名相反**？需用实际硬件核对方向（按下 KEY2 打印 VOL+ 还是 VOL-）。

### 11.5 未使用的硬件资源（软件可挖掘）

| 资源 | 硬件存在 | 软件使用？ | 建议 |
|------|----------|-----------|------|
| GPIO6 (ML307R DTR) | ✅ 已布线 | ❌ 未使用 | 启用 4G 睡眠控制，降功耗 |
| GPIO7 (USB_DET) | ✅ 已布线 | ❌ 未使用 | 充电器插拔事件、调试模式触发 |
| GPIO15 (ES8311 ASDOUT) | ✅ 已布线 | ❌ 未使用 | ES8311 ADC 录音通路，被 ES7210 替代，无需启用 |
| ~~MS85163M RTC (I2C 0x51)~~ | ❌ **BOM 未贴片** | — | 原理图有但实物无 |
| USIM2 预留 | ✅ 卡座未焊 | ❌ 未使用 | 双 SIM 卡（出货时单卡） |
| ML307R AGPIO5/WAKEUP0/1 | ✅ 外接 | ❌ 未使用 | 唤醒外部事件（如来电唤醒）|

### 11.6 更新后的下版硬件改进建议（基于原理图）

| 优先级 | 改进 | 对应原理图问题 |
|--------|------|---------------|
| 🔴 P0 | 分离 4G 供电（ML307R 独立 LDO） | 当前 GPIO9 一锅端 LCD/音频/4G |
| 🔴 P0 | 引出 ML307R RESET-N 到主控 GPIO | 当前 RESET 仅能靠电源反复 |
| 🟡 P1 | 独立 LCD_RESET 引脚 | 当前靠 GPIO9 间接复位 |
| 🟡 P1 | I2C 总线拆分（触摸独立） | 当前 5 设备共线，4G RF 易干扰 |
| 🟢 P2 | 原理图 Flash 符号名从 `BY25Q64` 改为 `BY25Q128` | BOM 实际是 16MB，文档需一致 |
| 🟢 P2 | 原理图删除 MS85163M（BOM 已不贴） | 文档一致性 |
| 🟢 P2 | 状态指示 LED（RGB 或单色） | D1/D3 只是功放/充电指示，不够 |
| 🟢 P2 | LCD SPI MISO 接出用于诊断 | 当前无读回能力 |

### 11.7 可立即落地的软件改进（不改硬件）

**代码验证状态（2026-04-17 核对）**：
- `components/78__esp-ml307/src/at_uart.cc:178, 986` — ML307 驱动库**已支持 DTR 引脚**（`SetDtrPin()` + 初始化）
- `main/boards/mydazy-p30-4g/mydazy_p30_board.cc:1065` — 构造时传 `GPIO_NUM_NC`，**未启用**
- `config.h:92` 注释「ML307模块无DTR引脚」**错误**（硬件实际连了 GPIO6）

**最小修复（1 行改动）**：
```cpp
// config.h 添加
#define MODEM_DTR_GPIO GPIO_NUM_6

// mydazy_p30_board.cc:1065 改为
DualNetworkBoard(ML307_TX_PIN, ML307_RX_PIN, MODEM_DTR_GPIO, ML307_DEFAULT),
//                                           ^^^^^^^^^^^^^^^ 原为 GPIO_NUM_NC
```

启用后立即获得：
- 深睡时 DTR 拉高允许 ML307R 自主进入低功耗
- 唤醒时 DTR 拉低强制唤醒，省去 `AT+CFUN=1` 的延迟

**其他可补定义**：
```cpp
// config.h 补
#define USB_DETECT_GPIO      GPIO_NUM_7   // USB 插拔检测
#define AUDIO_ES8311_ASDOUT  GPIO_NUM_15  // ES8311 ADC 录音通路（P30 用 ES7210 代替，此线闲置）
```

---

## 12. PIR 人体感应 Dongle 方案（Type-C SBU 外挂）⭐

> **方案确认日期**：2026-04-17
> **背景**：P30 主板 GPIO46 无法飞线（芯片工艺/空间限制），但 Type-C 母座的 SBU1/SBU2 焊盘**可以内部飞线**。此方案不改主板布线约束，通过 Type-C 外挂 Dongle 实现 PIR 人体感应 + 保持充电功能。

### 12.1 硬件设计

#### 12.1.1 P30 主板飞线（2 根）

```
Type-C 母座焊盘                  →   ESP32-S3 GPIO
────────────────────────────────   ───────────────────────
A8  (SBU1)     ──飞 0.1mm 漆包线── GPIO33  [TVS ESD]  PIR 信号
B8  (SBU2)     ──飞 0.1mm 漆包线── GPIO34  [TVS ESD]  Dongle 识别

保持不动：
  VBUS (2/11)  → TP4054 充电 IC        （充电功能正常）
  GND (1/12)   → 公共地
  CC1/CC2      → 5.1KΩ 下拉             （USB Sink 角色，不动）
  DP/DN        → ESP32 USB-OTG          （TinyUSB CDC 数据）
```

**ESD 保护**（必须加）：
- SBU 焊盘外露 → 每次插拔放电 → 不加 TVS 会打死 ESP32 GPIO
- 选型：`PRTR5V0U2X` 双路 TVS（¥0.5），放在 GPIO 端附近
- 也可用 `WE 82400102` 等同规格

#### 12.1.2 Dongle PCB 设计

```
┌─────────────────────────────────────┐
│   Dongle（独立小板，10mm×20mm）        │
│                                      │
│  [Type-C 公头 12pin]                  │
│   ├─ VBUS (5V)  ──┐                  │
│   ├─ GND         │                  │
│   ├─ SBU1        │←── PIR OUT 信号   │
│   └─ SBU2        │←── 3.3V（Dongle ID）│
│                  │                   │
│  [ME6211C33 LDO] │                   │
│   5V → 3.3V ─────┘                   │
│         │                             │
│         ↓                             │
│  [AM312 PIR 模块]                     │
│   VCC ← 3.3V                          │
│   GND                                 │
│   OUT → SBU1                          │
│                                       │
│  3.3V 直接 → SBU2（Dongle 识别）       │
└─────────────────────────────────────┘

Dongle BOM：
  - Type-C 公头 12pin    ¥2.0
  - AM312 PIR 模块       ¥2.0
  - ME6211C33 LDO        ¥0.5
  - 滤波电容 ×3          ¥0.2
  - PCB + SMT            ¥1.0
  - 塑料外壳 3D 打印      ¥2.0
  ─────────────────────
  单价 BOM              ¥7.7（量产 1000 片）
```

#### 12.1.3 电平与信号时序

```
普通 Type-C 充电器插入：
  SBU1 → 悬空（软件下拉为 0）
  SBU2 → 悬空（软件下拉为 0）
  → 软件判定：未接 Dongle，PIR 检测禁用

PIR Dongle 插入 + 无人：
  SBU1 → 0        （PIR 默认低电平）
  SBU2 → 1        （3.3V 常高 = Dongle 在位）
  → 软件判定：Dongle 在位，等待触发

PIR Dongle + 检测到人（持续 2-3s）：
  SBU1 → 1        （PIR 上升沿）
  SBU2 → 1
  CHRG → 0        （充电中）
  → 软件判定：充电+有人 → 触发主动讲解
```

### 12.2 软件方案（三路并行）

代码同时支持两套 PIR 硬件：

| 方案 | GPIO | 状态 |
|------|------|------|
| A. 内置 PIR | GPIO46 | 预埋（若未来飞线可启用） |
| B. SBU Dongle PIR | GPIO33 (SBU1) | **当前主方案** |
| B. SBU Dongle ID | GPIO34 (SBU2) | 配对用 |

**自动识别活跃源**：
```cpp
// 优先 Dongle（在位时），否则回退板载
bool dongle_online = gpio_get_level(PIR_DONGLE_ID_GPIO);
int pir_level = dongle_online
    ? gpio_get_level(PIR_DONGLE_GPIO)      // 外挂 Dongle
    : gpio_get_level(PIR_SENSOR_GPIO);     // 板载备用
```

### 12.3 Dongle 握手协议（防误触发）

**问题**：用户可能插入非 PIR 的 Type-C 设备（如带 SBU 信号的显示器线），SBU2 偶然高电平会被误识别为 Dongle。

**解决**：Dongle 上电后发送**识别序列**：
```
SBU2 电平随时间变化：
  t=0ms    上电 → 0
  t=100ms  → 1
  t=200ms  → 0
  t=300ms  → 1
  t=400ms+ 保持 1（正常模式）

P30 软件在上电后 500ms 内检测 SBU2 是否符合此序列 → 合法 Dongle
不符合 → 忽略 SBU 信号
```

Dongle 实现：
- 用 MCU 时：MCU GPIO 输出序列
- 不用 MCU 时：**RC 延时电路 + 555 定时器**模拟（BOM +¥0.3）
- 最简方案：不做握手，直接 SBU2=3.3V 常高（接受误识别风险）

### 12.4 量产工艺

#### P30 主板飞线工序
```
1. SMT 贴完 Type-C 母座
2. 贴片工手工飞线（工序单列）：
   - SBU1 焊盘 → GPIO33 过孔（距离 5-10mm）
   - SBU2 焊盘 → GPIO34 过孔
3. 焊接 TVS 管（靠近 GPIO 端）
4. UV 胶点胶固定飞线防震动
5. ICT 测试：
   - 导通测试（SBU → GPIO）
   - 对地短路测试
6. 烧录 + CDC PIR 命令验证

工时：+30s/台（熟练工）
良品率预期：>99%
```

#### Dongle 量产工序
```
1. PCB 打样 → 贴片 → 测试
2. 外壳 3D 打印 / 注塑
3. 组装（公头 + PCB + 外壳）
4. 单件测试：
   - 插入标准 Type-C 母座测试夹具
   - 验证 SBU1 有 PIR 信号（对 PIR 挥手触发）
   - 验证 SBU2 常高电平
   - 验证 VBUS 透传正常（可充电）
5. 贴标签 + 包装

可销售形式：
  - 标配套装（P30 + Dongle，家长家里充电用）
  - 选配配件（家长按需购买，¥15-20 零售）
```

### 12.5 软件命令（当前已实现）

```
> PIR STATUS
SerialCmd: SOURCE: 外挂 Dongle (SBU) / 内置 GPIO46  ← 自动选择
SerialCmd: PIR_ONBOARD: GPIO46=x
SerialCmd: PIR_DONGLE:  GPIO33(SBU1)=x
SerialCmd: DONGLE_ID:   GPIO34(SBU2)=x (在位/悬空)
SerialCmd: CHRG:        GPIO21=x (充电中/电池供电)
SerialCmd: DETECTED:    有人/无人
SerialCmd: MODE:        充电+有人 (触发讲解) / 待命
SerialCmd: OK:PIR
```

### 12.6 未来升级：PIR → 毫米波雷达

Dongle 可无痛升级为雷达版，P30 主板飞线保持不变：

```
雷达 Dongle（LD2410 版本）：
  Type-C 公头
    VBUS → LDO → 3.3V
    GND
    SBU1 ← LD2410 UART TX（P30 GPIO33 = UART1 RX）
    SBU2 → LD2410 UART RX（P30 GPIO34 = UART1 TX，由 P30 发配置命令）
    
BOM：¥22（比 PIR Dongle 多 ¥15）

能力提升：
  - 检测静止人体
  - 测距 0.5-5m 精度 0.2m
  - 穿透窗帘/纸板
  - 可编程检测区域
```

P30 软件层改动：
- `PIR_DONGLE_ID_GPIO (GPIO34)` 重用为 UART TX
- 新增 UART1 @ GPIO33/34，115200 baudrate
- 解析 LD2410 帧协议（Modbus-like）

### 12.7 限制与注意事项

| 项 | 说明 |
|----|------|
| Dongle 在位时 USB CDC | ⚠️ Dongle 只接 SBU + VBUS/GND，不接 DP/DN，所以 USB CDC 不可用（P30 会认为是普通充电器） |
| 工厂烧录 | ✅ 不受影响，Dongle 拔掉走 ROM USB-JTAG 烧录 |
| ESD 可靠性 | 必加 TVS，否则量产 ~1% 机器会 ESD 损坏 GPIO |
| Dongle 误插识别 | 简单方案：SBU2=3.3V 常高；严谨方案：RC 握手序列 |
| VBUS 透传充电 | ✅ Dongle 不占用 VBUS，TP4054 正常充电 |

### 🚨 12.8 关键发现（2026-04-17 实测）：GPIO33/34 不可用

**实际芯片是 ESP32-S3R8V（Octal PSRAM），GPIO 33-37 被 PSRAM 数据线占用！**

sdkconfig 确认：
```
CONFIG_SPIRAM_MODE_OCT=y   ← Octal PSRAM 模式
# CONFIG_SPIRAM_MODE_QUAD is not set
```

Octal PSRAM 数据线占用：
- GPIO 33: SPIIO4 (PSRAM 数据 4)
- GPIO 34: SPIIO5
- GPIO 35: SPIIO6
- GPIO 36: SPIIO7
- GPIO 37: SPIDQS

**把 GPIO33/34 配为普通 GPIO = 破坏 PSRAM 访问 = TG1WDT_SYS_RST**

### 🔧 12.9 修订：SBU Dongle 方案需重新选 GPIO

**原方案**：SBU1 → GPIO33, SBU2 → GPIO34（**不可用**）

**修订方案**：
| 选项 | GPIO | 可用性 | 说明 |
|------|------|--------|------|
| A. 只用 SBU1 一根线 | GPIO15 (ES8311 ASDOUT 悬空) | ✅ | 仅单路 PIR 信号，无 Dongle 识别 |
| B. 换 Quad PSRAM 芯片 (R8) | GPIO33/34 恢复可用 | 🔴 要求换料 | 可做完整方案 |
| C. 用板载 PIR + 飞 GPIO46 | GPIO46 | ✅ | 最简单，无需 Dongle |

**当前代码实现**（已禁用 GPIO33/34 初始化）：
- 仅保留 GPIO46 内置 PIR 初始化
- SBU Dongle 方案代码保留，但 `gpio_config()` 仅配置 GPIO46
- CDC `PIR STATUS` 命令同步简化

**推荐实施路径**：
1. **立刻做**：内置 PIR + GPIO46 飞线（最简单，¥2 BOM）
2. **如果仍需 Dongle**：PCB 改版时换 R8（Quad）芯片，或用 GPIO15 做单线方案

