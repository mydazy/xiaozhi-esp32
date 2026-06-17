# 模块 05 · power 电源域（红线，默认保留）

> 模式：A 对比。基线 v2.2.4。
> 范围：`main/boards/common/power_manager.h`（自研 357 行）、`power_save_timer.cc/.h`（官方小改）。
> **红线**：电源域误配置会烧硬件/砖机，头号风险区。默认保留，只标不动。本阶段只分析不改。
> **关联 memory**：`post-prod-stability-2026-06`（过放保护失效 P0）、`realtime-aec-weaknet-ram-2026-06`（已校准白片复现不了 08-P0-A）、`gyro-sleep-wake-2026-06`。

---

## 一、核心结论：红线必要模块，无过度优化

power_manager 是**自研**（v2.2.4 无），实现 08-P0-A 过放保护——本项目头号红线。357 行复杂度**全部对应"不烧电芯"的真实安全需求**，无识别出过度优化项。

---

## 二、过放保护审查（08-P0-A 红线）✅ 正确

| 机制 | 实现（power_manager.h）| 判定 |
|------|----------------------|------|
| 关机阈值 | `voltage < 3400 && !is_charging_` → off_battery（:215）| 🟢 红线 |
| **3 次连续确认** | `off_battery_streak_ >= 3` 才触发关机（:223）| 🟢 防瞬时压降误关机 |
| **ready_ 门控** | 板级注入回调后 `Start()` 才 `ready_=true`，构造期检测不触发关机（:223/350）| 🟢 防构造期回调未就绪 UB（注释明确）|
| **充电复位守卫** | `ResetOffBatteryGuard` 充电时清 streak + shutdown_requested（:343）| 🟢 防充放电循环后保护永久哑火 |
| 恢复滞回 | recover 3450mV > shutdown 3400mV（:43）| 🟢 防关机阈值附近抖动 |
| 开机即检测 | 构造期若 off_battery 连测 3 次（:277）| 🟢 防开机已亏电 |
| **未校准回退 + 产线登记** | 无 eFuse 校准 → `3300*adc/4096*2` 线性估算 + `LOGE "08-P0-A 产线需登记此机"`（:181/273）| 🟢 红线配套（见第四节观察）|
| 充/放双档分档 | levels_fd / levels_cd 两套表 + 线性插值（:150/159）| 🟢 充电电压虚高需分档 |
| vector 上限 | adc_values_ size ≤ kBatteryAdcDataCount=3（:140 erase）| 🟢 符合内存规范 |

**全部默认保留，不动。**

## power_save_timer（官方小改）

| 改动 | 判定 |
|------|------|
| enabled_/in_sleep_mode_/is_wake_word_running_/ticks_ 裸 bool/int → `std::atomic` | 🟢 并发（官方 data race，符合 esp32-memory 规范）|
| `IsInSleepMode()` 接口 | 🟢 触摸 swallow 首触判据配套（模块03）|
| 降频条件 `cpu_max_freq_ != -1` → `< 120` + WakeUp 加 `codec->EnableInput(true)` | ⚠️ 见第四节（实际禁用降频，技术债非过度）|

---

## 三、🔴 过度优化：0 项

power 模块每一处复杂度都对应过放保护红线，无"治标无效/堆复杂度/收益不明"。单例 `instance_` + `GetSharedAdcHandle` 静态接口是为共享 ADC handle 给其它模块（避免重复初始化），合理。**本模块无 🔴 追加。**

---

## 四、⚠️ 非过度的质量观察（电源域红线，建议留意）

> 以下均**不是过度优化**，但属电源域红线区的质量瑕疵 / 技术债，列出供红线评审参考（处置需双确认 + 实测，本任务不改）。

| # | 观察 | 性质 | 建议 |
|---|------|------|------|
| 1 | **省电一级降频未生效**：板级 `PowerSaveTimer(120,...)` → `120 < 120`=false → 不降频不关音频，只息屏降亮 | 技术债（文档已记）| 确认产品是否需要"60s 后降功耗"——若需要则改条件，若有意禁用降频（防 4G/音频实时性风险）则补注释说明 |
| 2 | **battery_level_ / is_off_battery_ / is_low_battery_ / off_battery_streak_ / shutdown_requested_ 是裸 bool/int** | 潜在 data race：timer 任务写、UI/主线程读，无同步（is_charging_/ready_ 已 atomic 但这些没有）| 电源域红线，建议这些跨线程状态补 `std::atomic`（撕裂读理论上影响电量显示/过放判据）|
| 3 | 未校准 Vref 回退用 `3300mV/4096` 线性估算 | 08-P0-A 已知风险：未校准机电压估算偏差 → 可能该关机不关/早关 | 已有产线登记 LOGE 配套；根治靠出厂 eFuse 校准流程（属产线，非代码）|
| 4 | `IsCharging()` 内注释掉的 `battery_level_==100` 死代码（:313-316）| 微小源码噪音 | 可清理（非红线）|

---

## 五、判定汇总

- **🔴 过度优化：0 项。** power 是红线必要模块，过放保护实现正确，改动全部服务电池安全。
- 🟢 主导：过放保护全套（streak 确认/ready 门控/充电复位/未校准回退）、power_save_timer atomic 防 race。
- ⚠️ 4 个非过度观察（降频未生效 / 裸标志 race / 未校准估算 / 死注释），供电源域红线评审，处置需双确认实测。

> 本阶段只分析不改代码。本模块无 🔴 追加。
