# 05 · 板级驱动 第二轮补充 — 板型实现

> 审计范围（逐行精读）：
> `main/boards/mydazy-p31/mydazy_p31_board.cc`（1620 行）、
> `main/boards/mydazy-p30-wifi/mydazy_p30_board.cc`（1062 行）、
> `main/boards/mydazy-p30-4g/mydazy_p30_board.cc`（1087 行）、
> `main/boards/common/wifi_board.cc`（602 行）、`ml307_board.cc`、`dual_network_board.cc`、
> `board.cc`、`power_manager.h`，以及三份 `config.h`（p31 / p30-wifi / p30-4g）。
> 审计日期：2026-05-20 · 目标：ESP32-S3 / ESP-IDF 5.5
>
> 已对照第一轮 `05_board_drivers.md`，本文件只列**新发现**，不重复 P0-1/P0-2/P1-1~4/P2-1~4/P3-1~3。

---

## P0（必崩 / 烧硬件 / 砖机 / 安全）

### P0-A 电池电压非校准回退路径漏乘 2（分压），eFuse 未烧的整批样机开机即误判"电量过低强制关机"（电源域）
- **判级理由**：`ReadBatteryAdcData()` 校准路径对 1M+1M 分压做了 `voltage *= 2`，而 **非校准回退路径没有乘 2**。
  ADC 实测的是电池电压的一半（满电 4.1V → ADC 端 ~2.05V），非校准时算出的"电压"只有真实值的一半（~2050mV），
  恒低于放电表最低阈值 3400mV → `battery_level_=0` 且 `is_off_battery_=true`。三块板的 `InitializePowerManager`
  都有 `if (IsOffBatteryLevel() && level>0 && !is_charging) 强制关机`——`level>0` 这层在满电时不成立，但一旦
  电量插值落到 0（必然，因为电压被腰斩）配合任意放电瞬间即触发**开机即关机**死循环。凡 eFuse 未烧录 ADC 校准
  参数的芯片（早期样机/部分批次 ESP32-S3 出厂未烧）整机变砖，定 P0。
- **文件**：`main/boards/common/power_manager.h:165-171`
- **代码**：
  ```cpp
  if (do_calibration1_chan0_) {
      ESP_ERROR_CHECK(adc_cali_raw_to_voltage(adc1_cali_chan0_handle_, average_adc, &voltage));
      voltage = voltage * 2;  // 2个1M电阻分压
  }
  else {
      voltage = 3600 * 1000 / 4096 * average_adc / 1000;   // ← 漏掉 *2，读数只有真实电压一半
  }
  ```
- **根因**：两条电压换算路径分别维护，分压系数只补在校准分支，回退分支忘记同步。
- **触发条件与影响面**：所有三个板型共用 `power_manager.h`；条件为运行芯片 ADC eFuse 未烧（`AdcCalibrationInit`
  打印 "eFuse not burnt, skip software calibration" 即命中）。命中后电量恒显 0%、低电告警常驻、开机阶段大概率
  `ShutdownOrSleep("电量过低")` 直接深睡，用户表现为"按不开机/开机即关机"。
- **修复建议**：回退路径同样乘 2，并与分压硬件一致：
  ```cpp
  else {
      voltage = (3600 * average_adc / 4096) * 2;   // 同样补分压系数
  }
  ```
  另建议把分压系数提成 `kBatteryDividerRatio` 常量，两路共用，杜绝再次漂移。

---

## P1（高频崩溃 / 体验严重退化）

### P1-A P31 深睡流程不停 NFC 后台 detection task，断 AUDIO_PWR_EN 后 NFC 仍在已掉电总线上轮询（板级/I2C）
- **判级理由**：`mydazy_nfc_init` 在 Core 0 起常驻 detection task（默认 300ms 轮询，直接走 `i2c_bus_`）。
  `EnterDeepSleep` 把 `AUDIO_PWR_EN_GPIO=0`（P31=GPIO15，注释明确"音频电源"，且 codec 失电后 ESD 二极管会
  把 SDA/SCL 钉住）并随后 `gpio_reset_pin(AUDIO_CODEC_I2C_SDA/SCL)`，但**全程没有 `mydazy_nfc_pause()`**
  （只在析构里有，而析构在正常运行期不发生）。深睡前 200~700ms 的窗口里 NFC task 持续在掉电/被复位的总线上发
  I2C 事务，可能与 worker 的最后清理、`ArmGyroWakeup`（SC7A20H 经同总线清 latch）相互干扰，导致拿起唤醒 latch
  清不掉（正是注释 §8.4 想避免的"秒醒失败"）。高频路径、与已知秒醒红线直接相关，定 P1。
- **文件**：`main/boards/mydazy-p31/mydazy_p31_board.cc:475-516`（EnterDeepSleep 未调 nfc pause）、
  :1080-1087（NFC task 起在 i2c_bus_）、:1513-1514（pause 只在析构）
- **代码**：
  ```cpp
  // EnterDeepSleep：停了 AudioService / 触摸 / 省电定时器，唯独没停 NFC task
  if (touch_driver_) { axs5106l_touch_sleep(touch_driver_); ... }
  if (enable_gyro_wakeup) ArmGyroWakeup();   // 经 SC7A20H 清 INT1 latch，同一条 I2C
  gpio_set_level(AUDIO_PWR_EN_GPIO, 0);      // codec 失电，SDA/SCL 被 ESD 钉死
  // …随后 gpio_reset_pin(SDA/SCL)，但 NFC detect task 仍在跑
  ```
- **根因**：深睡子步骤覆盖了 touch/audio/省电，漏掉 NFC；NFC 用独立后台任务且绕开 worker（见第一轮 P1-3），
  无统一"进睡前静默"汇聚点。
- **触发条件与影响面**：仅 P31（4G 板型有 NFC）。每次自动休眠/按键关机都经过该窗口；4G RF 干扰叠加时秒醒失败概率升高。
- **修复建议**：`EnterDeepSleep` 在 `ArmGyroWakeup()` 之前显式 `mydazy_nfc_pause();` 并 `vTaskDelay` 等其退出一次轮询，
  确保断电前 I2C 总线只剩 SC7A20H 一个访问者。

### P1-B `ml307_board.cc::GetDeviceStatusJson` / `GetBoardJson` 裸解引用 `modem_`，未注册成功时空指针崩溃（板级/网络）
- **判级理由**：`GetDeviceStatusJson()` 直接 `modem_->GetCarrierName()` / `GetCsq()`，`GetBoardJson()` 直接
  `modem_->GetModuleRevision()` 等，均**不判 `modem_==nullptr`**。而 `NetworkTask` 在 30 次 detect 全失败时
  会 `modem_` 保持 nullptr 并 return（:85-89）。状态上报（P31 周期/唤醒上报、控制中心读状态）在 4G 模组未检出
  时调用 `GetDeviceStatusJson` → 解引用空指针 → LoadProhibited 崩溃重启。无 SIM/天线松动/模组坏的设备每次状态
  上报必崩，定 P1。
- **文件**：`main/boards/common/ml307_board.cc:254`（`modem_->GetCarrierName()`）、:255、:173-182（GetBoardJson 全程裸用）
- **代码**：
  ```cpp
  cJSON_AddStringToObject(network, "carrier", modem_->GetCarrierName().c_str());  // modem_ 可能为 null
  int csq = modem_->GetCsq();
  ```
- **根因**：detect 失败把 `modem_` 留空但上层无防护；其它路径（`GetNetworkStateIcon`）有 `modem_==nullptr` 判断，
  说明此处属遗漏而非约定保证。
- **触发条件与影响面**：4G 板型（p30-4g / p31 走 ML307 模式）且模组检测失败。P31 默认网络类型=ML307（构造
  `DualNetworkBoard(...,1)`），无 SIM/模组异常时进入此分支。
- **修复建议**：两个函数开头 `if (!modem_) { 返回不含 network 段的最小 JSON; }`，与 `GetNetworkStateIcon` 的判空对齐。

---

## P2（偶发 / 边缘场景）

### P2-A P31 充电状态回调注册早于 power_save_timer_ 创建，电池定时器 1Hz 回调可能解引用空指针（电源域）
- **判级理由**：`InitializePowerManager`（构造序 8）里 `OnChargingStatusChanged` 的 lambda 体内访问
  `power_save_timer_->SetEnabled(...)`，但 `power_save_timer_` 要到 `InitializePowerSaveTimer`（序 9）才 `new`。
  PowerManager 构造时已 `esp_timer_start_periodic(...,1s)` 起了电池检查定时器（power_manager.h:238），其
  `CheckBatteryStatus` 一旦在序 8→9 之间检测到充电状态翻转就会回调 → `power_save_timer_`（nullptr）解引用。
  窗口约几十~几百 ms 且需恰好此刻插拔充电，偶发，定 P2。
- **文件**：`main/boards/mydazy-p31/mydazy_p31_board.cc:396-406`（注册回调引用 power_save_timer_）、
  :1393-1394（先 InitializePowerManager 后 InitializePowerSaveTimer）；定时器 `power_manager.h:227-238`
- **代码**：
  ```cpp
  power_manager_->OnChargingStatusChanged([this](bool is_charging) {
      ...
      power_save_timer_->SetEnabled(true);   // 此时 power_save_timer_ 可能仍为 nullptr
  });
  ...
  InitializePowerManager();   // 8
  InitializePowerSaveTimer(); // 9 ← power_save_timer_ 在这里才创建
  ```
- **根因**：回调对尚未初始化的成员有时序依赖，初始化顺序与依赖方向相反。
- **修复建议**：回调内加 `if (!power_save_timer_) return;`；或调换顺序先建 PowerSaveTimer 再建 PowerManager；
  或注册回调放到 `InitializePowerSaveTimer` 之后。

### P2-B P31 触摸缺 `on_double_click` 注册，双击在 P31 无任何业务响应（板级/交互一致性）
- **判级理由**：P30-wifi/4g 的 `PrepareTouchHardware` 都注册了 `.on_double_click = &OnTouchDoubleClick`
  （退出对话回 Idle），P31 的 cfg **只注册了 wake/click/swipe，没有 double_click**。P31 用户双击屏幕退出对话的
  心智失效，只能靠单击/按键。功能缺失非崩溃，定 P2。
- **文件**：`main/boards/mydazy-p31/mydazy_p31_board.cc:317-335`（cfg 无 on_double_click）
  对照 `mydazy-p30-wifi/mydazy_p30_board.cc:361`、`mydazy-p30-4g/mydazy_p30_board.cc:355`
- **代码**：
  ```cpp
  axs5106l_touch_config_t cfg = {
      .worker=..., .on_wake=&OnTouchWake, .on_click=&OnTouchClick,
      .on_swipe=&OnTouchSwipe,            // ← 无 .on_double_click
  };
  ```
- **根因**：P31 从 P30 派生时漏迁双击回调；P31 也无 `HandleTouchDoubleClick` 等价实现。
- **修复建议**：若产品要求三板一致，补 `on_double_click` 与对应处理；若 P31 有意只保留单击，应在注释明确，避免后续误判为 bug。

### P2-C P31 `EnterDeepSleep` 重复 `GetAudioService().Stop()` 两次 + 注释步骤号错乱（板级/可维护性）
- **判级理由**：函数开头（:481）已 `Stop()` 并 `vTaskDelay(100)`，到 :503 又写"[1/8] 停止音频服务"再 `Stop()` 一次。
  AudioService::Stop 幂等的话第二次无害，但白等 100ms 且步骤编号 [1/8]…[6/8] 出现重复（两个 [6/8]、缺 [4/8]/[5/8]/[7/8]），
  增加误读/误改风险。非崩溃，定 P2。
- **文件**：`main/boards/mydazy-p31/mydazy_p31_board.cc:480-503`、:543、:591
- **代码**：
  ```cpp
  Application::GetInstance().GetAudioService().Stop();   // :481 第一次
  ...
  ESP_LOGI(TAG, "[1/8] 停止音频服务 (I2S DMA)");
  Application::GetInstance().GetAudioService().Stop();   // :503 第二次（重复）
  ```
- **根因**：两版深睡代码合并时第一段 Stop 是后加的"提前停"，原 [1/8] 段未删。
- **修复建议**：删除 :503 重复 Stop（保留 :481 的提前停），并重排步骤注释为连续编号。

### P2-D P30-wifi/4g `ShutdownTouchAndAudioForSleep` 把 `touch_driver_=nullptr` 后无法二次进睡复用（板级）
- **判级理由**：进睡时 `axs5106l_touch_sleep(touch_driver_)` 后立刻 `touch_driver_ = nullptr`（与 P31 不同，P31
  保留指针）。正常路径深睡后会 `esp_deep_sleep_start` 不返回，置空无害；但 `WifiBoard::EnterWifiConfigMode` 配网
  超时分支会 `Board::GetInstance().EnterDeepSleep(true)`，若该路径之前已有人调过一次 sleep（如先关机长按再被打断），
  二次进睡 `touch_driver_` 已空则跳过 touch sleep，触摸 INT ISR 未关，深睡中 INT 抖动可能误唤醒。窗口极窄，定 P2。
- **文件**：`main/boards/mydazy-p30-wifi/mydazy_p30_board.cc:526-531`、
  `main/boards/mydazy-p30-4g/mydazy_p30_board.cc:515-520`
- **根因**：把"句柄置空"当成清理，但 deep sleep 不返回的前提下该置空只在异常二次路径才有意义且方向不对。
- **修复建议**：不要在 sleep 路径置空 touch_driver_（与 P31 对齐），或在置空前确保 INT ISR 已 detach。

---

## P3（潜在远期风险）

### P3-A `power_manager.h` 硬编码 `ADC_CHANNEL = ADC_CHANNEL_7`，无视各板 `BATTERY_ADC_CHANNEL`（电源/配置漂移）
- **判级理由**：`#define ADC_CHANNEL ADC_CHANNEL_7`（power_manager.h:12）写死，而三份 config.h 都另外定义了
  `BATTERY_ADC_CHANNEL ADC_CHANNEL_7`，PowerManager **不引用 config 宏**。当前三板都恰好是 CH7 所以未爆，但
  config 里的 `BATTERY_ADC_CHANNEL` 成了死定义；将来改板把电池 ADC 挪到别的通道、只改 config 不改 power_manager.h
  会读错通道（读到别的脚电压 → 电量乱跳/误关机）。当前不触发，定 P3。
- **文件**：`main/boards/common/power_manager.h:12`；config 死定义 `*/config.h: BATTERY_ADC_CHANNEL`
- **修复建议**：`#define ADC_CHANNEL BATTERY_ADC_CHANNEL`（或构造参数传入），消除两套来源。

### P3-B `BATTERY_CAPACITY_MAH` / `BRAND_NAME`（P31 缺）等 config 宏为死定义，易误导后续改板（配置一致性）
- **判级理由**：`BATTERY_CAPACITY_MAH 1000` 三板都定义但全仓无人引用（电量靠电压表插值，与容量无关）；
  `BRAND_NAME "MyDazy"` 在 p30-wifi/4g 定义、**P31 config.h 未定义**，但实际取值走 `Board::GetBrandName()` 硬编码
  返回 "MyDazy"（board.h:85），宏从未被读。死定义本身不致错，但"P31 少一个 BRAND_NAME"会让人误以为是缺陷、或
  日后真把 `GetDeviceShowName` 改成读 `BRAND_NAME` 时 P31 编译失败。定 P3 作清理提示。
- **文件**：`main/boards/mydazy-p31/config.h`（无 BRAND_NAME / 有未用 BATTERY_CAPACITY_MAH:101）；
  `main/boards/common/board.h:85`（GetBrandName 硬编码）
- **修复建议**：要么删除死定义，要么让 `GetBrandName()` 读 `BRAND_NAME` 宏并给 P31 补上；二选一，不要半挂。

### P3-C P31 SC7A20H 中断脚 `SC7A20H_GPIO_INT1 = GPIO_NUM_3` 为 ESP32-S3 strapping 脚（板级/硬件）
- **判级理由**：GPIO3 是 ESP32-S3 的 strapping/JTAG 源选择脚，复位瞬间被外部驱动会影响启动 strapping/调试。
  作为 SC7A20H INT1 输入，深睡 arm 后传感器在复位窗口若已拉低，理论上有改变 strapping 的风险。三板都用 GPIO3
  且现网量产稳定，说明硬件上电时序使其无害，但属"占用 strapping 脚"的远期隐患（改电路/换传感器默认电平时易踩）。定 P3。
- **文件**：三份 `config.h: #define SC7A20H_GPIO_INT1 GPIO_NUM_3`
- **修复建议**：维持现状但在 config 注释标明"GPIO3 为 strapping 脚，外围须保证复位期电平不冲突"；新板优先换非 strapping 脚。

### P3-D `StartVolumeTask` 任务句柄写回竞态 + 仅 P31 保留（多核/资源）
- **判级理由**：P31 `StartVolumeTask`（:1345-1372）后台任务退出时 `*task_handle = NULL; vTaskDelete(NULL);`，
  与 `OnPressUp` 的 `vol_*_running_=false` 及析构里的 `vTaskDelete(vol_*_task_)` 三方对同一 `vol_*_task_` 无锁读写
  （`volatile` 非原子，违反仓库 esp32 规范"多核共享用 std::atomic"）。极端时序下析构可能 `vTaskDelete` 一个刚被任务自己
  置空/已退出的句柄。P30 系列已删除该长按调音任务（注释 v2.2.10），只剩 P31 保留。低频（需长按音量+析构同时），定 P3。
- **文件**：`main/boards/mydazy-p31/mydazy_p31_board.cc:1345-1372`、:1516-1525
- **修复建议**：`vol_*_task_` / `vol_*_running_` 改 `std::atomic`；任务退出与析构删除二选一负责，避免双删；
  或与 P30 对齐直接删除长按连续调音（产品上音量长按价值低）。

---

## 统计

| 等级 | 数量 | 条目 |
|------|------|------|
| P0   | 1    | P0-A 电池非校准回退漏乘 2 → eFuse 未烧整批开机即关机 |
| P1   | 2    | P1-A P31 深睡不停 NFC task（秒醒红线） · P1-B ml307 状态 JSON 裸解引用 modem_ 空指针崩溃 |
| P2   | 4    | P2-A P31 充电回调早于省电定时器创建 · P2-B P31 缺触摸双击 · P2-C P31 深睡重复 Stop/步骤错乱 · P2-D P30 sleep 置空 touch 句柄 |
| P3   | 4    | P3-A ADC_CHANNEL 硬编码无视 config · P3-B 死定义/ P31 缺 BRAND_NAME · P3-C GPIO3 strapping 脚作 INT1 · P3-D P31 调音任务句柄竞态 |
| **合计** | **11** | |

> 关键提示：
> 1. **P0-A 是本轮最高优先级**——它解释了"部分整机按不开机/开机即关机"的现场反馈，且仅靠改 power_manager.h 一行
>    分压系数即可，建议立即修。可用 `heap`/串口确认日志是否出现 "eFuse not burnt" 来判断现网命中比例。
> 2. P1-A 与第一轮 P1-3（NFC 绕开 worker）同源，建议一并把 NFC 接入 worker + 进睡前统一静默。
> 3. P1-B 与第一轮无重叠，属 ml307_board.cc 的独立空指针面，无 SIM 现场必现。
