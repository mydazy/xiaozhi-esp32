# 05 · 板级驱动 / 电源 / I2C 子系统 缺陷审计报告

> 审计范围：`main/boards/`（38 文件）、`components/mydazy__i2c_bus_worker/`、
> `components/mydazy__esp_sc7a20h/`、`components/esp_nfc_ws1850s/`、
> `components/esp_lcd_touch_axs5106l/`
> 审计日期：2026-05-20 · 目标：ESP32-S3 / ESP-IDF 5.5

## 子系统概述

本子系统覆盖三个板型（mydazy-p30-4g / mydazy-p30-wifi / mydazy-p31）的板级初始化、
电源管理（电池 ADC 采样 + 充电检测 + 低电关机）、深度睡眠/省电定时器、按键、背光、
Type-C 耳机检测，以及四个 I2C 外设驱动（SC7A20H 加速度计、WS1850S NFC、AXS5106L 触摸、
音频 codec）。

总体结构良好：`i2c_bus_worker` 是一个设计精良的单线程串行调度器，超时/节流/诊断都到位；
SC7A20H 和 AXS5106L 驱动都正确走 worker。但存在几处真实缺陷，集中在三个方向：

1. **电源域**：P31 充电检测引脚极性与 `PowerManager` 硬编码逻辑相反；电池 ADC 读失败直接
   `ESP_ERROR_CHECK` 触发 abort/重启；多任务共享同一 ADC oneshot unit 无锁并发。
2. **I2C 串行化失效**：NFC 与音频 codec 绕开 worker 直接访问共享总线，`lock_session`
   会话锁对单条 op 不生效，SC7A20H 初始化的"原子序列"承诺无法兑现。
3. **显示/背光**：背光占空比上限被错误地钉死在 ~29%（10-bit 分辨率却只用 300/1023）。

---

## P0（必崩 / 烧硬件 / 砖机 / 安全）

### P0-1 电池 ADC 读取失败直接 `ESP_ERROR_CHECK` → abort 重启（电源域）
- **判级理由**：定时器回调中对 `adc_oneshot_read` 用 `ESP_ERROR_CHECK`，一旦 ADC 返回非
  ESP_OK（总线/校准/瞬态错误）立即 abort，触发 panic 重启。低电/充电插拔等高频路径上
  任何一次失败都使设备无规律重启，符合"必然崩溃"定义，故 P0。
- **文件**：`main/boards/common/power_manager.h:125`（同款写法散落在 :166、:237、:238、
  :245、:251）
- **代码**：
  ```cpp
  ESP_ERROR_CHECK(adc_oneshot_read(adc_handle_, ADC_CHANNEL, &adc_value));
  ...
  ESP_ERROR_CHECK(adc_cali_raw_to_voltage(adc1_cali_chan0_handle_, average_adc, &voltage));
  ```
- **根因**：把"可恢复的运行时错误"当成"启动期断言"处理。`adc_oneshot_read` 在并发访问
  或瞬态时可能返回 `ESP_ERR_TIMEOUT`/`ESP_ERR_INVALID_STATE`。
- **触发条件与影响面**：每 1 秒触发的电池检查定时器回调中执行；与 TypecHeadset 任务共享
  ADC unit 时（见 P0-3）冲突概率显著升高 → 整机随机重启。影响全部三个板型（共用
  power_manager.h）。
- **修复建议**：回调路径里改成检查返回值并 `return`/重试，不要 abort：
  ```cpp
  if (adc_oneshot_read(adc_handle_, ADC_CHANNEL, &adc_value) != ESP_OK) {
      ESP_LOGW(POWER_MANAGER_TAG, "adc read failed, skip this tick");
      return;
  }
  ```

### P0-2 P31 充电检测引脚极性与驱动逻辑相反（电源域）
- **判级理由**：充电状态被反读，导致"充电不休眠""低电关机""充电图标"全部判断颠倒。其中
  低电强制关机依赖 `is_charging_` 判定（`InitializePowerManager` 中 `!is_charging` 才关机），
  极性反了会在正充电时误判为放电、或放电时误判充电而拒绝关机保护，属电源安全逻辑失效，定 P0。
- **文件**：判定逻辑 `main/boards/common/power_manager.h:97`；引脚定义
  `main/boards/mydazy-p31/config.h:98`
- **代码**：
  ```cpp
  // power_manager.h:97  — 硬编码 低=充电
  bool new_charging_status = gpio_get_level(charging_pin_) == 0;
  // power_manager.h:223  — 构造函数固定开内部上拉
  io_conf.pull_up_en = GPIO_PULLUP_ENABLE;
  ```
  ```c
  // p31/config.h:98
  #define POWER_MANAGER_GPIO  GPIO_NUM_44  // 充电状态检测（高电平=充电中）ADD YZT
  ```
- **根因**：P30（4g/wifi）的引脚注释是"低电平=充电中"，与 `==0` 逻辑一致；P31 换成 GPIO44
  且注释明确写"高电平=充电中"，但 `PowerManager` 的判定逻辑没有随板型参数化，仍硬编码 LOW=充电，
  且强制内部上拉。
- **触发条件与影响面**：仅 P31 板型，开机即生效。充电/电量 UI 全错；"充电中跳过深睡"
  （`mydazy_p31_board.cc:451`）逻辑反转 → 该休眠时不休眠（耗电）或充电时强行深睡。
- **修复建议**：给 `PowerManager` 增加 `bool charging_active_high` 构造参数（或读 config 宏），
  P31 传 true：
  ```cpp
  bool lvl = gpio_get_level(charging_pin_);
  bool new_charging_status = charging_active_high_ ? (lvl != 0) : (lvl == 0);
  ```
  同时 P31 上拉应改为下拉（active-high 检测时空闲应被拉低）。需硬件确认 GPIO44 实际电平。

---

## P1（高频崩溃 / 体验严重退化）

### P1-1 PowerManager 与 TypecHeadset 共享同一 ADC oneshot unit 跨任务无锁并发（电源域）
- **判级理由**：两个独立 FreeRTOS 任务并发调用 `adc_oneshot_read`/`config_channel` 于同一
  unit。ESP-IDF 的 `adc_oneshot` 单元无内部互斥，并发读会返回错误（叠加 P0-1 的
  `ESP_ERROR_CHECK`→重启）或读到串扰电压。高频且必然发生，定 P1。
- **文件**：`main/boards/common/typec_headset.cc:41-59`、:79；
  `main/boards/common/power_manager.h:241-251`、:125；
  桥接处 `main/boards/mydazy-p31/mydazy_p31_board.cc:1118`
  （`headset_->Start(PowerManager::GetSharedAdcHandle())`）
- **代码**：
  ```cpp
  // typec_headset.cc:41 共享 PowerManager 的 unit，再在其上 config 两个新通道并由 DetectLoop 任务持续读
  void TypecHeadset::InitAdc(adc_oneshot_unit_handle_t shared_adc) {
      if (shared_adc) { adc_handle_ = shared_adc; adc_owned_ = false; }
      ...
      adc_oneshot_config_channel(adc_handle_, cfg_.cc_adc_channel, &chan_cfg);
  ```
- **根因**：共享 ADC unit handle 但未提供任何互斥；PowerManager 1Hz 定时器任务读 CH7（电池），
  Headset 任务以 ~20ms 周期读 CH5（CC）/CH8（MIC），三通道同 unit 并发。
- **触发条件与影响面**：P31 始终成立（TYPEC_HEADSET_ENABLED=1）。耳机插拔检测越频繁，
  与电池采样撞车概率越高。
- **修复建议**：为共享 unit 增加一个全局 `SemaphoreHandle_t adc_mutex`，所有
  `adc_oneshot_read` 前后加锁；或将电池采样也并入 TypecHeadset 的同一任务串行执行；
  或两个用途各用独立 ADC unit（ADC1 通道够用时）。最低限度先做 P0-1 的非 abort 化止血。

### P1-2 `i2c_worker_lock_session` 对单条 op 不生效，SC7A20H 初始化"原子序列"承诺落空（I2C）
- **判级理由**：会话锁只在"另一个 caller 也调 lock_session"时才互斥；而 `i2c_worker_write`
  等单 op 函数根本不获取 `session_mutex`，照常入队执行。因此被 `lock_session` 包裹的多寄存器
  序列仍可能被其它 driver 的 op 插队。SC7A20H 上电配置序列（power-down→量程→中断）若被打断，
  传感器进入半配置态，唤醒/摇一摇行为异常但不一定崩溃，定 P1。
- **文件**：实现 `components/mydazy__i2c_bus_worker/i2c_bus_worker.c:387-432`（write/read/
  write_read 均无 take session_mutex）与 :438-450（lock/unlock 只动 session_mutex）；
  使用方 `components/mydazy__esp_sc7a20h/sc7a20h.c:234-251`；
  头文件承诺 `components/mydazy__i2c_bus_worker/include/i2c_bus_worker.h:159-160`
- **代码**：
  ```c
  // i2c_bus_worker.c:387 单 op 直接 submit_and_wait，从不 take session_mutex
  esp_err_t i2c_worker_write(...) { op_t op = {...}; return submit_and_wait(dev->worker, &op, timeout_ms); }
  ```
  ```c
  // sc7a20h.c:235 期望此后 10 条 write 原子，实际无保护
  i2c_worker_lock_session(worker, 200);
  write_reg(d, REG_CTRL_REG1, 0x00); ... write_reg(d, REG_CTRL_REG1, 0x50 | 0x07);
  i2c_worker_unlock_session(worker);
  ```
- **根因**：session 锁与 op 队列是两套独立机制，op 路径没有"持锁才入队/执行"的检查。
- **触发条件与影响面**：仅当有第二个 caller 在同一 worker 上并发提交 op 时才暴露。当前 P31
  上 worker 的 caller 为 SC7A20H + AXS5106L 触摸（触摸 LVGL 读回调约 16ms 周期），初始化期内
  触摸尚未 attach，实际撞车窗口窄；但设计承诺与实现不符，属真实缺陷。
- **修复建议**：让 worker task 在执行每条非锁定 op 前 `xSemaphoreTakeRecursive(session_mutex)`、
  执行后 release；`lock_session` 期间该 caller 持锁，其它 op 在 worker 内被阻塞。或在
  `submit_and_wait` 内对非持锁 caller 做 `take/give` 包裹。需注意避免与 caller 持锁产生死锁
  （recursive mutex 的归属问题——worker task 与 caller 不同任务，不能用 caller 的递归计数，
  建议改为"worker 执行前 take 一把普通会话锁，lock_session 的 caller 用同一把"）。

### P1-3 NFC 与音频 codec 绕开 worker 直接访问共享 I2C 总线（I2C）
- **判级理由**：worker 的设计目标是"协议层 100% 串行"，但 WS1850S NFC 在 Core 0 后台任务里
  直接用 `i2c_master_transmit/receive`（自己的 dev handle），codec/I2cDevice 同样直连，全部
  与走 worker 的 touch/SC7A20H 共用同一物理总线（GPIO11/12）。IDF 底层 bus mutex 能防"物理
  事务交错"故一般不崩，但 `SetBitMask`/`ClearBitMask` 的读-改-写跨两次事务非原子，4G RF 干扰
  期 NFC 寄存器易被串扰错配。高频共存、体验退化，定 P1。
- **文件**：`components/esp_nfc_ws1850s/src/ws1850_iic.c:110-148`（直连）、:161-185
  （非原子 RMW）；后台任务 `components/esp_nfc_ws1850s/src/esp_nfc_ws1850s.cc:193`
  （`xTaskCreatePinnedToCore(detect_task_fn, ... core 0)`）；
  直连封装 `main/boards/common/i2c_device.cc:22-35`
- **代码**：
  ```c
  // ws1850_iic.c:161 读-改-写跨两条独立总线事务，期间可被其它直连访问插入
  void SetBitMask(unsigned char reg, unsigned char mask) {
      char tmp = ws_read_reg(reg);
      ws_write_reg(reg, tmp | mask);
  }
  ```
- **根因**：迁移到 worker 时只迁了 touch/sensor，NFC 与 codec 仍保留旧的直连路径，破坏了
  "单一访问者"前提（注释 `mydazy_p31_board.cc:170` 也承认"codec/NFC/touch 仍直接走 i2c_bus_"）。
- **触发条件与影响面**：P31 NFC 后台任务常驻（默认 300ms 轮询），与触摸/codec 长期共存。
  表现为 4G 场景下偶发 NFC 误读/漏读、RMW 寄存器值被污染。
- **修复建议**：把 NFC 与 codec 也接入同一 worker（`i2c_worker_add_device` + worker op），
  彻底单点串行；过渡方案：至少把 NFC 的 RMW 用 `lock_session` 包裹，并在修好 P1-2 后生效。

### P1-4 背光 PWM 占空比上限被钉死在约 29%（显示/电源）
- **判级理由**：10-bit LEDC（满量程 1023）却用 `300 * brightness / 100`，brightness=100 时
  duty 仅 300/1023≈29%。屏幕永远无法达到设计亮度，户外/景区演示严重偏暗，属体验严重退化，定 P1。
- **文件**：`main/boards/common/backlight.cc:121-126`
- **代码**：
  ```cpp
  void PwmBacklight::SetBrightnessImpl(uint8_t brightness) {
      // 注释自称 100% = 1023
      uint32_t duty_cycle = (300 * brightness) / 100;   // 实际 100→300
      ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, duty_cycle);
      ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
  }
  ```
- **根因**：常数应为 1023（与 `LEDC_TIMER_10_BIT` 一致），写成了 300。
- **触发条件与影响面**：全板型、所有亮屏场景。
- **修复建议**：`uint32_t duty_cycle = (1023 * brightness) / 100;`（若硬件确需限幅以降功耗/防过亮，
  应改成显式 `kMaxDuty` 命名常量并在注释说明，而非误用 300 当满量程）。[待确认硬件是否有意限亮]

---

## P2（偶发 / 边缘场景）

### P2-1 触摸坐标钳位用错维度上限，X 可被截断（驱动/显示）
- **判级理由**：屏 284(W)×240(H)，`DISPLAY_SWAP_XY=true`。`read_touch` 中 `swap_xy` 在驱动里被
  硬置 false（`axs5106l_touch.c:230`），而钳位用 `sx >= width(284)`、`sy >= height(240)`。固件
  V2907 已内部 rotation，但帧校验 `raw_x > TOUCH_MAX_X(284)+50` 用的是宏常量、钳位用的是传入
  width，两套基准。若某板型传入 width/height 与 284/240 不一致会错位。仅边缘坐标受影响，定 P2。
- **文件**：`components/esp_lcd_touch_axs5106l/axs5106l_touch.c:667`、:688-689、:36-37
- **代码**：
  ```c
  if (raw_x > TOUCH_MAX_X + 50 || raw_y > TOUCH_MAX_Y + 50) return false;  // 宏 284/240
  ...
  if (sx >= self->width)  sx = self->width  - 1;   // 运行时 width
  ```
- **根因**：宏常量与运行时尺寸两套来源未统一。
- **触发条件与影响面**：当前 P31 width=284/height=240 与宏一致，暂不触发；改板型/改宏易踩。
- **修复建议**：统一以运行时 `self->width/height` 为基准（含帧校验），删除 `TOUCH_MAX_X/Y` 宏
  或由 init 写入实例。

### P2-2 SleepTimer 在定时器回调内执行 `EnableWakeWordDetection` 后立即又恢复，逻辑悬空（电源）
- **判级理由**：`CheckTimer` 进 light sleep 分支里先关唤醒词、`Schedule` 一个会阻塞循环的
  lambda，然后在 lambda 还没执行前就同步 `EnableWakeWordDetection(true)` 恢复了。关/开成对错位，
  light sleep 期间唤醒词可能仍开着，省电打折。偶发、非崩溃，定 P2。
- **文件**：`main/boards/common/sleep_timer.cc:81-113`
- **代码**：
  ```cpp
  if (is_wake_word_running) { audio_service.EnableWakeWordDetection(false); vTaskDelay(100); }
  app.Schedule([this, &app]() { while (in_light_sleep_mode_) { ... esp_light_sleep_start(); ... } WakeUp(); });
  if (is_wake_word_running) { audio_service.EnableWakeWordDetection(true); }  // 在 Schedule 之前就恢复了
  ```
- **根因**：`Schedule` 异步，但恢复调用是同步紧跟，时序倒置。
- **触发条件与影响面**：启用 SleepTimer 的板型（P31 实际走 PowerSaveTimer/EnterDeepSleep，
  SleepTimer 使用面较窄）。
- **修复建议**：把"恢复唤醒词"移到 `Schedule` lambda 末尾（紧邻 `WakeUp()`）。

### P2-3 共享 ADC 缺校准时 TypecHeadset 仍创建 curve-fitting，可能拿到未初始化 cali handle（电源/驱动）
- **判级理由**：`InitAdc` 无条件 `adc_cali_create_scheme_curve_fitting`，不检查返回值即把
  `cc_cali_/mic_cali_` 用于 `ReadMv`；若 eFuse 未烧（早期样机）handle 为 NULL，`ReadMv` 内有
  `if (cali && ...)` 兜底回退 raw 换算，影响有限，故 P2 而非 P1。
- **文件**：`main/boards/common/typec_headset.cc:62-74`、:77-83
- **代码**：
  ```cpp
  adc_cali_create_scheme_curve_fitting(&cc_cal, &cc_cali_);   // 未判返回值
  ```
- **根因**：未检查校准创建结果。`ReadMv` 的 NULL 兜底使其不致崩溃。
- **修复建议**：判返回值，失败置 NULL 并打 WARN，与 `power_manager.h` 的 `AdcCalibrationInit` 一致。

### P2-4 AXS5106L 固件升级逐字节写且全程忽略写返回值（驱动）
- **判级理由**：`write_flash` 对每字节单独 `i2c_worker_write`（多 KB 固件=数千次同步往返，启动期
  阻塞）且无任何错误检查，`do_upgrade` 末尾默认跳过校验。烧录中途失败不会被发现，可能写出半截
  固件。仅在版本不一致触发升级时执行（非常态），定 P2。
- **文件**：`components/esp_lcd_touch_axs5106l/axs5106l_upgrade.c:227-266`、:268-279
- **代码**：
  ```c
  for (size_t i = 0; i < len; i++) { cmd[2] = data[i]; i2c_write_reg(h, 0x90, cmd, 3); }  // 不判返回
  ...
  /* Verification omitted by default */
  return true;  // write_flash 恒真
  ```
- **根因**：为兼容性走慢速逐字节路径，但牺牲了错误检测。
- **修复建议**：累计写失败计数，超阈值返回 false；升级后至少做一次版本回读校验（已有
  `read_chip_version_stable`，`axs5106l_upgrade_run:351` 实际有回读，但 `write_flash` 内部失败仍被吞）。

---

## P3（潜在远期风险）

### P3-1 `submit_and_wait` 超时兜底路径会泄漏 caller 栈上 semaphore（I2C）
- **判级理由**：作者已注释为"接受 leak 换 UAF 安全"——worker 仍可能持有 sem 时不能 delete。
  逻辑正确，但持续硬件卡死会缓慢泄漏（静态 sem 在栈上，实际泄漏的是内部 FreeRTOS 记账）。极低频，
  仅在 I2C 硬件长期挂死时发生，定 P3。
- **文件**：`components/mydazy__i2c_bus_worker/i2c_bus_worker.c:270-273`
- **代码**：
  ```c
  if (xSemaphoreTake(sem, pdMS_TO_TICKS(200)) != pdTRUE) {
      /* 极端情况下 sem 不能释放 — 接受 leak 换 UAF 安全 */
      return ESP_ERR_TIMEOUT;
  }
  ```
- **根因**：用栈上 `StaticSemaphore_t`，worker 持有引用时无法安全回收；权衡取舍。
- **修复建议**：改用堆分配 + 引用计数，或 op 携带"caller 已放弃"标志由 worker 完成后负责
  delete；当前不紧急。

### P3-2 `PowerManager` 单例 `instance_` 与 ADC handle 跨对象共享，析构早于 headset 会留悬空（电源）
- **判级理由**：`~PowerManager` 删除 ADC unit（:269-271），而 TypecHeadset 仍持有
  `adc_handle_`（`adc_owned_=false` 不会重复删）。若析构顺序为 PowerManager 先于 Headset，
  Headset 后续 `adc_oneshot_read` 用已删 handle → UAF。实际两者生命周期都贯穿程序，析构仅发生在
  极少的 Board 销毁路径（P31 析构 :1531 先删 power_manager 再——headset 已在 :1496 先 Stop），
  当前顺序安全，定 P3 作风险提示。
- **文件**：`main/boards/common/power_manager.h:264-272`、`:274-276`；
  `main/boards/mydazy-p31/mydazy_p31_board.cc:1496`（headset 先）、:1531（power 后）
- **根因**：共享 handle 但所有权与生命周期未用引用计数表达，靠析构顺序隐式保证。
- **修复建议**：用 shared_ptr/引用计数管理共享 ADC unit，或在文档明确析构顺序约束。

### P3-3 GPS 演示/状态等多个 esp_timer "复用不 delete" 模式依赖单次创建（板级）
- **判级理由**：`gps_demo_timer_`/`gnss_watchdog_timer_` 反复 start/stop 不 delete（注释自述
  防泄漏）。逻辑正确，但若未来加入"销毁后重建 Board"会重复创建。当前单例 Board 不触发，定 P3。
- **文件**：`main/boards/mydazy-p31/mydazy_p31_board.cc:954-957`、:1233
- **修复建议**：维持现状即可，加注释说明"Board 单例生命周期内复用"。

---

## 统计

| 等级 | 数量 | 条目 |
|------|------|------|
| P0   | 2    | P0-1 ADC 读失败 abort 重启 · P0-2 P31 充电检测极性反 |
| P1   | 4    | P1-1 共享 ADC 无锁并发 · P1-2 lock_session 对单 op 失效 · P1-3 NFC/codec 绕开 worker · P1-4 背光上限钉死 29% |
| P2   | 4    | P2-1 触摸坐标钳位维度 · P2-2 SleepTimer 唤醒词恢复时序 · P2-3 ADC 校准未判返回 · P2-4 触摸固件升级吞错 |
| P3   | 3    | P3-1 worker sem 泄漏兜底 · P3-2 共享 ADC handle 悬空风险 · P3-3 esp_timer 复用模式 |
| **合计** | **13** | |

> 说明：P0-1 与 P1-1 相互放大（共享 ADC 并发 → 读失败 → abort），建议优先一并修复：
> 先把 power_manager.h 的 `ESP_ERROR_CHECK(adc_oneshot_read...)` 改为软失败，再加 ADC 互斥锁。
> P0-2 需配合硬件确认 GPIO44 充电信号实际有效电平后再改。
