# 08 电源域子系统审计报告

> 审计范围（仅电源相关切片）：
> - `main/boards/common/power_manager.h`
> - `main/boards/common/power_save_timer.cc` / `.h`
> - `main/boards/mydazy-p30-wifi/mydazy_p30_board.cc` + `config.h`（电源切片）
> - `main/boards/mydazy-p30-4g/mydazy_p30_board.cc` + `config.h`（电源切片）
>
> 外设/显示/音频部分不在本报告范围。审计方法：三遍递进（广度遍历 → 红线深挖 → 反审自检）。
> 重点：电池电压 ADC 分压换算、ADC 校准、充电检测、过放保护、深睡/唤醒源配置。
>
> 注：`backlight.*` / `system_reset.*` 在本仓库 `main/boards/common/` 下不存在（Glob 无结果），
> 背光降亮逻辑实际在板级 `GetBacklight()` / PowerSaveTimer 回调内，已并入相关条目审计。

---

## 第一遍 · 广度遍历（显性缺陷）

锚点命中的电源相关位置（计数）：
- `power_manager.h`：ADC 初始化、校准、电压换算、充放电分级表、过放判定、充电检测引脚 —— 1 处核心实现，约 12 个电源关键点。
- `power_save_timer.cc/.h`：light sleep / esp_pm 降频 / 关机请求 —— 1 处。
- WiFi `board.cc`：InitializePowerManager / InitializePowerSaveTimer / EnterDeepSleep / HandleWakeupCause / ShutdownHandler —— 5 处。
- 4G `board.cc`：同上 5 处（结构镜像 WiFi）。
- 两块 `config.h`：BATTERY_ADC / POWER_MANAGER_GPIO / 容量 —— 各 4 个宏。

### 08-P0-A　非校准回退路径 Vref 取 3600mV 与 ESP32-S3 12dB 实际满量程不符，电压系统性高估 → 整批未烧 eFuse 样机过放
- 严重等级：**P0**
- 判级理由：ESP32-S3 在 `ADC_ATTEN_DB_12` 下 ADC 有效输入约 0~3100mV（满量程参考 ~3.1V 量级，并非 3.6V）。代码在 eFuse 未烧录（`do_calibration1_chan0_ == false`）时用 `3600` 作为满量程参考做线性换算。**未做出厂 ADC 校准的整批样机会走这条回退路径**，把真实电压系统性放大约 (3600/3100−1)≈16%。结果：电池实际 3.4V（应关机）时被算成 ~3.9V，设备不触发过放关机，持续放电直至硬件欠压保护或电芯损伤。这正是"整批未校准机器"级别的电源红线。
- 文件：`main/boards/common/power_manager.h:174-177`
- 问题代码：
```cpp
else {
    voltage = 3600 * 1000 / 4096 * average_adc / 1000;
    voltage = voltage * 2;
}
```
- 根因：① 用 3600mV 当 12dB 满量程参考（应为实测的 ESP32-S3 12dB 上限，典型 ~3100mV，且 12bit 满码 4095 非 4096）。② 整数运算 `3600*1000/4096 = 879`（截断丢 0.4），叠加误差。③ 校准路径（`adc_cali_raw_to_voltage`）才是准确的，但出厂未烧 eFuse 时 `AdcCalibrationInit` 直接返回 false，全批走这条错误回退。
- 触发条件/影响面：**两块板共用同一 `power_manager.h`，整批未做 eFuse 校准的设备**全部受影响。读数偏高 → 过放风险（伤电芯/砖机），并非个例。
- 修复建议：
  1. 回退系数按 12dB 实测满量程改：`voltage = (int)((float)average_adc * 3100.0f / 4095.0f) * 2;`（3100 用本批 ADC 实测标定值替换；4095 非 4096）。
  2. 更稳妥：**强制要求出厂烧录 ADC eFuse 校准**，并在 `AdcCalibrationInit` 失败时把电压判定保守化（宁可偏低早关机，不可偏高过放），例如未校准时整体下调 5% 余量。
  3. 长期：把"是否走回退路径"上报到出厂测试门禁，未校准设备不予出货。
- [发现于第一遍]

### 08-P1-B　ADC 量程被硬编码为 ADC_CHANNEL_7，无视 config.h 的 BATTERY_ADC_CHANNEL，跨板改通道会静默读错脚
- 严重等级：**P1**
- 判级理由：`config.h` 明确定义了 `BATTERY_ADC_CHANNEL ADC_CHANNEL_7`，但 `power_manager.h` 顶部用 `#define ADC_CHANNEL ADC_CHANNEL_7` 硬编码，初始化与读取全用这个宏，**完全不引用 config 的定义**。当前两板恰好都是 channel 7 不会立刻出事，但任何新板或改 BOM 改了 ADC 脚，config 改了通道而固件仍读 7 → 读到悬空/错误引脚电压 → 电量乱跳、过放判定失效。这是电源域不该存在的"配置看似生效实则没接线"陷阱。
- 文件：`main/boards/common/power_manager.h:12`；对照 `main/boards/mydazy-p30-wifi/config.h:95`、`mydazy-p30-4g/config.h:105`
- 问题代码：
```cpp
#define ADC_CHANNEL    ADC_CHANNEL_7   // power_manager.h，硬编码，未用 BATTERY_ADC_CHANNEL
```
- 根因：PowerManager 抽象层未参数化 ADC 通道，靠"巧合一致"维持正确。
- 触发条件/影响面：当前两板不触发（个例=0）；改板/改通道时整批读错。属潜在批量风险，故判 P1 而非 P3。
- 修复建议：删掉硬编码宏，改为构造参数：`PowerManager(gpio_num_t charging_pin, adc_channel_t adc_channel)`，板级传入 `BATTERY_ADC_CHANNEL`；或在 `power_manager.h` 顶部 `#include "config.h"` 后 `#define ADC_CHANNEL BATTERY_ADC_CHANNEL`。
- [发现于第一遍]

---

## 第二遍 · 红线深挖（逐行核公式/阈值/唤醒源）

### 08-P1-C　开机过放强制关机被 `battery_level > 0` 条件挡掉：电量算到 0% 的极低电芯反而被放行开机
- 严重等级：**P1**
- 判级理由：开机自检在 `IsOffBatteryLevel()`（电压 < 3400mV 且未充电）为真时强制关机，但额外加了 `&& battery_level > 0` 条件。`battery_level` 在电压低于 `levels[0].adc(3400)` 时被算成 **0**（`power_manager.h:180-181`）。于是最危险的工况——电压已掉到 3.4V 以下、电量计算为 0%——`battery_level > 0` 为假，**强制关机被跳过**，深度过放的电芯被放行继续开机运行，直接对着电芯过放。这是过放红线被自身保护条件反向打穿。
- 文件：`main/boards/mydazy-p30-wifi/mydazy_p30_board.cc:490-493`；`mydazy-p30-4g/mydazy_p30_board.cc:480-483`；联动 `power_manager.h:180-181`、`209-214`
- 问题代码：
```cpp
if (power_manager_->IsOffBatteryLevel() && battery_level > 0 && !is_charging) {
    ESP_LOGE(TAG, "电量过低，强制关机");
    ShutdownOrSleep("电量过低", "强制关机", Lang::Sounds::OGG_LOW_BATTERY, 3000, false);
}
```
- 根因：`battery_level > 0` 本意大概是"过滤 ADC 还没采到数（level 默认 0）的误关机"，但与"真过放→level 也=0"语义撞车，导致最该关机时不关机。
- 触发条件/影响面：电池接近耗尽（resting < 3400mV）冷开机时触发，整批共性逻辑，非个例。叠加 08-P0-A（电压被高估）时此条更隐蔽——但只要真实电压够低使 level 落到 0 即漏关。
- 修复建议：去掉 `battery_level > 0`，改用"ADC 数据是否就绪"做防误判：
  - PowerManager 增加 `bool IsBatteryReady()`（`adc_values_.size() >= kBatteryAdcDataCount`）；
  - 板级条件改为 `if (power_manager_->IsBatteryReady() && power_manager_->IsOffBatteryLevel() && !is_charging)`。
- [发现于第二遍]

### 08-P2-D　过放/关机阈值 3400mV 取 resting 值，未计放电内阻压降，重载瞬间可能跌破电芯保护下限
- 严重等级：**P2**
- 判级理由：`kBatteryShutdownMv = 3400` 与放电分级表 `levels_fd[0]={3400,0}` 同值。3.4V 对单节锂电是偏保守的空载关机点，本身不烧硬件；但本设备在 4G 发射 / 喇叭播放峰值电流大，1000mAh 小电芯内阻下瞬时压降可达 200~400mV，关机判定用的是 3 次平均的"准空载"值，可能在重载瞬间真实电芯电压已跌到 3.0V 附近（接近多数电芯 2.8~3.0V 保护点）。边缘场景，判 P2。
- 文件：`main/boards/common/power_manager.h:37`、`144-151`、`209-214`
- 问题代码：
```cpp
const int kBatteryShutdownMv = 3400;
...
const bat_level_t levels_fd[] = { {3400, 0}, ... };
...
if(voltage < kBatteryShutdownMv && is_charging_ == false){ is_off_battery_ = true; }
```
- 根因：阈值按空载电压设定，未对负载工况留余量；ADC 平均化也滞后于瞬时压降。
- 触发条件/影响面：低电量 + 4G 发射/大音量峰值的边缘组合，偶发。
- 修复建议：把关机阈值上调留余量（如 3500mV 关机、3400 仍可作 0% 显示），或在 4G 发射/播放高负载场景动态抬高关机阈值；并依赖硬件电芯保护板作为最终兜底（确认 BOM 含保护板）。
- [发现于第二遍]

### 08-P2-E　充电检测仅靠 GPIO 电平、无充电截止/满充判定，"充电中"分级表上限 4150mV 长期顶格无 CV 阶段保护语义
- 严重等级：**P2**
- 判级理由：`is_charging_` 仅由 `gpio_get_level(charging_pin_)==0` 决定（开漏低=充电中），固件**不参与任何充电截止/恒压控制**——这本应由充电 IC 硬件完成，固件只读状态，方向正确。风险点在于：充电分级表 `levels_cd` 上限到 4150mV 才算 100%，而固件无"满充已停"信号，UI 与省电逻辑长期把顶格电压当充电中，`OnShutdownRequest` 里"充电中跳过深睡"会让设备在满充后仍保持唤醒词常开 + 降亮屏常亮（桌钟场景），增加满电涓流/发热时长。不烧硬件（充电 IC 兜底），判 P2。
- 文件：`main/boards/common/power_manager.h:96-107`（充电状态读取）、`153-160`（充电分级表）、`mydazy_p30_board.cc:511-516`（充电跳过深睡）
- 问题代码：
```cpp
bool new_charging_status = gpio_get_level(charging_pin_) == 0;
...
const bat_level_t levels_cd[] = { ... {4150, 100} };
...
if (PowerManager::IsChargingGlobal()) { /* 跳过深睡，常亮常开 */ return; }
```
- 根因：未引入充电 IC 的 "CHG_DONE/STDBY" 信号，单引脚无法区分"正在充"与"已充满涓流"。
- 触发条件/影响面：长时间座充/夜间充电场景，发热与功耗，偶发体验问题。依赖充电 IC 截止，无烧硬件风险。
- 修复建议：若充电 IC 提供 STDBY/DONE 引脚，增加第二路检测区分满充；满充后允许进入降耗（关唤醒词或熄屏），缓解发热。无引脚则在 100% 持续 N 分钟后主动降级常亮策略。
- [发现于第二遍]

### 08-P3-F　非校准回退路径整数运算截断（3600*1000/4096=879）叠加在 P0-A 上，额外丢约 0.05% 精度
- 严重等级：**P3**
- 判级理由：`3600 * 1000 / 4096` 在 C++ 整数运算中先算 `3600000/4096 = 878`（实际 878.9，截断），相对真值丢约 0.1%。相对 08-P0-A 的 16% 系统误差这点很小，但属同一行的代码质量问题，单列以便随 P0-A 一并修。
- 文件：`main/boards/common/power_manager.h:175`
- 问题代码：
```cpp
voltage = 3600 * 1000 / 4096 * average_adc / 1000;
```
- 根因：整数除法先于乘法、且分母用 4096（应 4095）。
- 触发条件/影响面：所有走回退路径设备，误差极小。
- 修复建议：随 08-P0-A 一并改为浮点：`voltage = (int)((float)average_adc * VREF_MV / 4095.0f) * 2;`。
- [发现于第二遍]

### 第二遍·两板配置 / 实现差异核对（结论）
- WiFi 与 4G 两板 `config.h` 电源切片**完全一致**：`POWER_MANAGER_GPIO=GPIO_NUM_21`、`BATTERY_ADC_GPIO=GPIO_NUM_8`、`BATTERY_ADC_CHANNEL=ADC_CHANNEL_7`、`BATTERY_CAPACITY_MAH=1000`。无分压系数/阈值不一致。
- 两板共用同一份 `power_manager.h` / `power_save_timer.*`，电压换算、过放、充电检测实现一致 → P0-A/P1-B/P1-C/P2-D/P2-E 同时命中两板。
- 唤醒源配置一致：EXT0=BOOT 键（`esp_sleep_enable_ext0_wakeup(BOOT_BUTTON_GPIO, 0)` 低电平唤醒）、EXT1=陀螺仪 INT1、TIMER=闹钟，三源并存，深睡前 `rtc_gpio_pullup_en(BOOT_BUTTON_GPIO)` 保证高电平 sleep、关机分支等松手兜底，逻辑正确。
- **唯一差异（正向）**：4G 板"死按短路 sleep"分支（`board.cc:217-218`）多了 `gpio_set_level(AUDIO_PWR_EN_GPIO,0)+rtc_gpio_hold_en` 先断 LDO 再回睡；WiFi 板该分支（`board.cc:221-223`）**未先断 AUDIO_PWR_EN 就直接 deep_sleep**。见下条 08-P3-G。

### 08-P3-G　WiFi 板"死按短路回睡"分支未先断 AUDIO_PWR_EN，与 4G 板不对称（潜在多耗电/外设带电入睡）
- 严重等级：**P3**
- 判级理由：4G 板在 EXT0 死按短路 sleep 前显式 `AUDIO_PWR_EN=0 + hold_en` 断 LDO 再回睡；WiFi 板同分支直接 `esp_deep_sleep_start()`，未断 LDO。该分支仅在"距上次 sleep<500ms 且仍按住"极短窗口触发且随即回睡，影响小；但跨板不对称是潜在隐患（外设带电入睡多耗电、且与 4G 行为不一致难维护）。
- 文件：`main/boards/mydazy-p30-wifi/mydazy_p30_board.cc:216-224`；对照 `mydazy-p30-4g/mydazy_p30_board.cc:215-223`
- 问题代码（WiFi 缺断电）：
```cpp
if (since_us < kDeadHoldWindowUs && gpio_get_level(BOOT_BUTTON_GPIO) == 0) {
    ESP_LOGW(...);
    s_last_sleep_us = NowRtcUs();
    esp_sleep_enable_ext0_wakeup(BOOT_BUTTON_GPIO, 0);   // ← 缺 AUDIO_PWR_EN=0 + hold_en
    rtc_gpio_pullup_en(BOOT_BUTTON_GPIO);
    esp_deep_sleep_start();
}
```
- 根因：两板该分支代码未同步对齐（4G 后补了断 LDO，WiFi 未跟进）。
- 触发条件/影响面：仅死按延续极短窗口，偶发，WiFi 板。
- 修复建议：WiFi 板在该分支 `esp_sleep_enable_ext0_wakeup` 前补 `gpio_set_level(AUDIO_PWR_EN_GPIO,0); rtc_gpio_hold_en(AUDIO_PWR_EN_GPIO);`，与 4G 对齐。
- [发现于第二遍]

---

## 第三遍 · 反审自检（复验 + 对抗视角查漏报/删误报）

### 复验已发现条目
- 08-P0-A：复看 `power_manager.h:170-177` —— **校准分支（170-173）的 `*2` 与回退分支（175-176）的 `*2` 均存在，分压系数没漏乘**。最初"漏乘分压"嫌疑排除；真正问题是回退分支 Vref 取值 3600mV 偏高（系统性高估）。判级维持 P0：误判方向是"偏高→过放"，电源问题不压低。行号、代码片段核对无误。
- 08-P1-C：复看 `power_manager.h:180-181` 确认 `voltage < levels[0].adc(3400)` 时 `battery_level_=0`；板级 `battery_level > 0` 确实会挡掉此工况。逻辑链成立，维持 P1。
- 08-P1-B：确认 `config.h` 定义了通道宏但 `power_manager.h` 未引用，硬编码 7。两板巧合一致，故判 P1（潜在批量）不拔到 P0。
- 08-P2-D / P2-E：均依赖硬件（电芯保护板 / 充电 IC）兜底，不直接烧硬件，维持 P2，不拔高。

### 对抗视角："什么配置会让整批机器开机即关机 / 烧充电管理？"
- "开机即关机"经典成因——分压漏乘导致电压算成一半、阈值反了：本仓库**未命中**（`*2` 在；阈值方向 `voltage < shutdown` 正确）。08-P0-A 是反向（电压偏高、不该不关机），不会"开机即关机"，但会"该关不关→过放"，同属电源头号红线。
- "烧充电管理"：固件不直接控制充电（无 PWM/无截止寄存器写），充电由 IC 硬件完成，固件只读 GPIO 状态 → **无固件烧充电 IC 路径**，未发现此类 P0。
- 过放保护链：唯一软过放保护 = 开机自检 `IsOffBatteryLevel` 强制关机 + 运行中 `is_off_battery_` 标志。已被 08-P1-C（条件挡掉）+ 08-P0-A（电压高估）双重削弱；运行中 `is_off_battery_=true` 后**是否真的触发关机**需确认调用方（板级只在 init 时查一次，运行中似无周期性检查 `IsOffBatteryLevel()` 并关机的路径）。见下 08-P1-H。

### 08-P1-H　运行期无周期性过放关机：`is_off_battery_` 仅开机自检读一次，开机后持续放电至硬件保护
- 严重等级：**P1**
- 判级理由：`IsOffBatteryLevel()` 仅在 `InitializePowerManager()`（开机一次）被消费。PowerManager 定时器每秒更新 `is_off_battery_`，但**板级运行期没有任何地方再查它并触发关机**。即开机时电量尚可、运行中逐渐放电跌破 3400mV 时，固件不会主动过放关机，只能等电芯保护板硬切。低电量提示（`is_low_battery_`，5%）会响"请充电"，但用户不充电时无软关机兜底。这把过放保护实际退化成"只在开机那一刻有效"。
- 文件：消费点仅 `mydazy_p30_board.cc:490`（WiFi）/`480`（4G）；`power_manager.h:209-214` 每秒更新但无运行期消费者。
- 根因：过放强制关机只接在 init，未接到运行期回调（类似 `OnLowBatteryStatusChanged` 那样的 `OnOffBatteryStatusChanged`）。
- 触发条件/影响面：开机后长时间使用至耗尽、用户忽略低电提示，整批共性，非个例。依赖硬件保护板兜底但软层缺失，判 P1。
- 修复建议：PowerManager 增加 `OnOffBatteryStatusChanged` 回调（仿 `OnLowBatteryStatusChanged`），在 `is_off_battery_` 由 false→true 时触发；板级注册后调用 `ShutdownOrSleep("电量过低","强制关机",...)`。注意复用 08-P1-C 的修复（用 ready 标志防误判）。
- [发现于第三遍]

### 误报排查（删除/不计）
- 关于"`average_adc`(uint32_t) 传入 `adc_cali_raw_to_voltage`(int) 截断"：`average_adc` 最大为 12bit 平均（≤4095），远在 int 范围内，**非缺陷，不计**。
- 关于"light sleep + esp_pm 降频烧硬件"：`power_save_timer.cc:93-98` 配置 `max_freq=120/min_freq=40/light_sleep`，为标准动态调频，无危险参数，**不计**。
- 关于"充电检测 GPIO 内部上拉冲突"：`power_manager.h:224-230` 开漏+内部上拉，与"低=充电"语义一致，**正确，不计**。

---

## 统计

| 等级 | 数量 | 编号 |
|------|------|------|
| P0 | 1 | 08-P0-A |
| P1 | 3 | 08-P1-B、08-P1-C、08-P1-H |
| P2 | 2 | 08-P2-D、08-P2-E |
| P3 | 2 | 08-P3-F、08-P3-G |
| **合计** | **8** | |

三遍新增分布（a + b + c）：
- 第一遍：2（08-P0-A、08-P1-B）
- 第二遍：4（08-P1-C、08-P2-D、08-P2-E、08-P3-F、08-P3-G） —— 实为 5 条
- 第三遍：1（08-P1-H）

> 修正：第一遍 2 + 第二遍 5 + 第三遍 1 = 8。（08-P3-G 为第二遍跨板差异核对时发现，归入第二遍。）
> 即 **2 + 5 + 1 = 8**。

### 最高优先级修复建议（离量产最近）
1. **08-P0-A**：未校准回退路径电压高估 → 强制出厂烧 ADC eFuse 校准，且回退系数改实测 12dB 满量程（~3100mV）。这是整批未校准样机的过放红线，出货前必须清零。
2. **08-P1-C + 08-P1-H**：过放保护被开机条件挡掉 + 运行期无周期过放关机 —— 一并补 `IsBatteryReady()` 与 `OnOffBatteryStatusChanged` 回调，恢复完整软过放兜底。
