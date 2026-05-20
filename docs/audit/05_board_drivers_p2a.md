# 05 · 板级驱动 第二轮补充 - 电源/充电/休眠

> 审计范围（逐行精读）：`main/boards/common/` 下 power_manager.h / power_save_timer.{cc,h} /
> sleep_timer.{cc,h} / backlight.{cc,h} / typec_headset.{cc,h} / button.{cc,h} / system_reset.{cc,h}
> 审计日期：2026-05-20 · 目标：ESP32-S3 / ESP-IDF 5.5
> 本轮原则：**只补第一轮 `05_board_drivers.md` 未报告的新问题**，已报项（ADC abort、P31 充电极性、
> 共享 ADC 无锁、背光 300/1023、SleepTimer 唤醒词时序、TypecHeadset 校准未判返回、共享 ADC 析构悬空）不重复。

---

## P0（必崩 / 烧硬件 / 砖机 / 安全）

### P0-A1 `is_off_battery_` 关机门限被 `kLowBatteryLevel=0` 与电压双重失效，过放保护形同虚设（电源安全）
- **判级理由**：低电关机保护是锂电过放安全底线。`kLowBatteryLevel = 0`（:36）意味着 `is_low_battery_`
  只有在 `battery_level_ <= 0`（即电量已归零）才会置位；而 `is_off_battery_` 的强制关机门限是
  `voltage < levels_fd[0].adc`（3400mV，:203），但 `levels_fd[0]` 已经是放电曲线最低点 0%。也就是说
  设备会在电压跌破 3.4V 之后才触发关机，对 3.7V 单节锂电而言 3.4V 已接近过放拐点，关机阈值过低且
  与 BMS 截止电压无安全裕量。配合 voltage 换算偏差（见 P0-A2），实际可能在更低电压才关机 → 长期过放
  鼓包/损伤电芯，属硬件安全，定 P0。
- **文件**：`main/boards/common/power_manager.h:36`、:174-175、:192-194、:203-208
- **代码**：
  ```cpp
  const int kLowBatteryLevel = 0;                         // :36 低电门限=0%
  ...
  bool new_low_battery_status = battery_level_ <= kLowBatteryLevel;  // :193 只有 0% 才算低电
  if(is_charging_) new_low_battery_status = false;
  ...
  if(voltage < levels_fd[0].adc && is_charging_ == false){  // :203 3400mV 才强制关机
      is_off_battery_ = true;
  }
  ```
- **根因**：低电告警门限与过放关机门限都钉在曲线最低点（0% / 3400mV），没有为安全关机预留电压裕量
  （通常应在 3.45~3.5V 即告警、3.3~3.4V 关机），且 `kLowBatteryLevel=0` 让低电回调几乎永不触发。
- **触发条件与影响面**：全板型。用户在低电下持续使用 → 电压一路掉到 3.4V 才关机，长期反复触发会
  加速电芯老化/鼓包。若 ADC 换算偏高（P0-A2），真实电压更低，安全隐患放大。
- **修复建议**：把告警门限提到 5~10%（`kLowBatteryLevel=5`），过放关机门限独立成显式电压常量
  （如 `kCutoffMv = 3300`）并与电池规格书截止电压对齐；关机前应给用户提示与保存状态时间。需硬件
  确认所用电芯的厂商截止电压。

---

## P1（高频崩溃 / 体验严重退化）

### P1-A1 light sleep 唤醒回路中 `lvgl_port_stop()` 失败/触摸唤醒后 LVGL 不恢复，UI 卡死（休眠/显示）
- **判级理由**：`SleepTimer::CheckTimer` 的 Schedule lambda 里 `lvgl_port_stop()` → `esp_light_sleep_start()`
  → `lvgl_port_resume()`。若唤醒原因不是 timer（`break` 退出循环，:104-106）后才走到 `WakeUp()`，但
  循环体内每轮都 `lvgl_port_stop()` 一次却只在循环内 `lvgl_port_resume()`，当 `break` 发生在
  `esp_light_sleep_start()` 之后、`lvgl_port_resume()` 之前的语义边界没有问题；真正的问题是：整个
  while 跑在 `app.Schedule` 投递的主循环上下文里，期间 `in_light_sleep_mode_` 由其它线程的 `WakeUp()`
  清零来退出，但 `WakeUp()`（:125）只清标志不唤醒正阻塞在 `esp_light_sleep_start()` 的 CPU，导致最长
  要等满 30s timer 才退出一轮，按键/触摸唤醒后 UI 有最长 30s 黑屏无响应窗口。高频体验退化，定 P1。
- **文件**：`main/boards/common/sleep_timer.cc:88-109`、:125-133
- **代码**：
  ```cpp
  app.Schedule([this, &app]() {
      while (in_light_sleep_mode_) {
          ... lvgl_port_stop();
          esp_sleep_enable_timer_wakeup(30 * 1000000);
          esp_light_sleep_start();          // 阻塞最长 30s
          lvgl_port_resume();
          if (wakeup_reason != ESP_SLEEP_WAKEUP_TIMER) break;  // 仅非timer才退出
      }
      WakeUp();
  });
  ```
- **根因**：用 30s timer 作为唯一可控唤醒源，未把按键/触摸 GPIO 注册为 light sleep 唤醒源；
  `WakeUp()` 改标志不能打断已进入的 `esp_light_sleep_start()`。
- **触发条件与影响面**：启用 SleepTimer 的板型。每次进 light sleep 后，外部交互最长 30s 才被响应。
- **修复建议**：进 light sleep 前 `esp_sleep_enable_gpio_wakeup()`/`gpio_wakeup_enable()` 把按键/触摸
  中断脚设为唤醒源，缩短 timer 周期（如 1~2s）并在每轮唤醒后检查交互标志；或改用事件驱动而非轮询。

### P1-A2 `Backlight` 渐变定时器与 `SetBrightness` 跨线程竞争 `brightness_/step_`，可能死循环或越界（显示/电源）
- **判级理由**：`SetBrightness`（主线程/事件线程调用）写 `target_brightness_`、`step_` 并 start 5ms
  周期定时器；`OnTransitionTimer`（esp_timer 任务上下文）读改 `brightness_` 并与 `target_brightness_`
  比较。两者无任何互斥，`brightness_/target_brightness_/step_` 都是裸 `uint8_t`。若在定时器回调正读改
  时主线程改了 `target_brightness_`/`step_` 方向，可能出现 `brightness_` 永远跨过目标（步进方向与差值
  反号）→ `brightness_ += step_` 在 0/255 处回绕 → 定时器永不停且亮度乱跳。高频（每次亮度变更）且体验
  退化，定 P1。
- **文件**：`main/boards/common/backlight.cc:46-74`、:76-88；`backlight.h:24-26`
- **代码**：
  ```cpp
  // SetBrightness（线程 A）
  target_brightness_ = brightness;
  step_ = (target_brightness_ > brightness_) ? 1 : -1;   // step_ 是 uint8_t，-1 变 255
  esp_timer_start_periodic(transition_timer_, 5*1000);
  // OnTransitionTimer（定时器任务 B）
  brightness_ += step_;        // step_=255 时实为 +255 → uint8_t 回绕
  ```
- **根因**：(1) `step_` 声明为 `uint8_t`（:26）却赋 `-1`，递减路径 `brightness_ += step_` 依赖
  无符号回绕"碰巧"等价于 -1，但配合并发改向会偏离预期；(2) 三个共享字段无 atomic/锁，违反
  ESP32 多核共享变量规范。
- **触发条件与影响面**：全板型，亮度调节频繁（自动调光/进出省电）时撞车。表现为亮度跳变、渐变不停。
- **修复建议**：`step_` 改 `int8_t`；`brightness_/target_brightness_` 改 `std::atomic` 或在
  `SetBrightness` 与 `OnTransitionTimer` 间加轻量锁；并在 `OnTransitionTimer` 用"是否越过目标"判停
  （`(step_>0 && brightness_>=target) || (step_<0 && brightness_<=target)`）而非 `==`，防错过相等点死循环。

### P1-A3 PowerSaveTimer 进 light sleep 时未关 modem/4G 与外设电源域，省电不达标且 4G 掉线（电源/休眠）
- **判级理由**：`PowerSaveCheck` 进省电仅 `EnableWakeWordDetection(false)` + `codec->EnableInput(false)`
  + `esp_pm_configure(light_sleep_enable=true)`（:78-98）。开了自动 light sleep 但 4G modem（P31 主用）
  的 UART/电源没有协调，IDF 自动 light sleep 在有活跃 UART/外设时会被频繁打断或导致 modem 通信丢字节，
  既省不了电又可能掉线重连。P31 实际以 4G 为主链路，定 P1。
- **文件**：`main/boards/common/power_save_timer.cc:78-99`
- **代码**：
  ```cpp
  if (cpu_max_freq_ < 120) {
      ... codec->EnableInput(false);
      esp_pm_config_t pm_config = { .max_freq_mhz=cpu_max_freq_, .min_freq_mhz=40,
                                    .light_sleep_enable = true };
      esp_pm_configure(&pm_config);     // 未处理 modem/UART power lock
  }
  ```
- **根因**：自动 light sleep 与 modem UART 未做 power-management lock 协调（IDF 需 `esp_pm_lock`
  或 UART 配 `ESP_PM_NO_LIGHT_SLEEP` ref），也未在进出省电时通知 modem 进低功耗。
- **触发条件与影响面**：P31 4G 板型进省电模式后。表现：4G 偶发掉线/重连、省电收益低于预期。
- **修复建议**：modem UART 持有 `esp_pm_lock`（`ESP_PM_NO_LIGHT_SLEEP` 在有数据时 acquire），或在进
  深度省电前主动让 modem 进 PSM/eDRX；确认 `CONFIG_PM_ENABLE`/`CONFIG_FREERTOS_USE_TICKLESS_IDLE`
  与 UART 唤醒配置一致。[待确认 modem 驱动是否已加 pm_lock]

---

## P2（偶发 / 边缘场景）

### P2-A1 充电状态翻转时 `ReadBatteryAdcData` 用旧的 `is_charging_` 选曲线，临界点电量跳变（电源）
- **判级理由**：`CheckBatteryStatus` 检测到充电态变化后 `adc_values_.clear()` 并立即 `ReadBatteryAdcData()`
  return（:98-106）。此时 `is_charging_` 已更新，但仅用单次新采样（clear 后只 1 个值）做均值，且充/放
  曲线（levels_cd/levels_fd）门限不同（充电曲线整体偏高 ~150mV），插拔瞬间电量百分比会阶跃跳变（如插上
  充电器瞬间从 60% 掉到 40%）。偶发、非崩溃，定 P2。
- **文件**：`main/boards/common/power_manager.h:98-106`、:148-162、:182-189
- **代码**：
  ```cpp
  if (new_charging_status != is_charging_) {
      is_charging_ = new_charging_status;
      ... adc_values_.clear(); ReadBatteryAdcData();  // 清空后单点采样 → 抖动
      return;
  }
  ```
- **根因**：充放电用两套曲线但切换瞬间未做迟滞/平滑，且清空采样队列后立即出值。
- **修复建议**：插拔瞬间保留历史均值或加迟滞带，待采满 `kBatteryAdcDataCount` 再更新百分比显示。

### P2-A2 ADC 电压换算与分压系数硬编码，未校准回退路径漏乘 2 倍分压（电源/换算）
- **判级理由**：校准路径 `voltage = voltage * 2`（:167，注释"2 个 1M 电阻分压"）；但未校准回退路径
  `voltage = 3600*1000/4096*average_adc/1000`（:170）**没有乘 2**。两条路径量纲不一致：未校准时算出的
  电压只有真实电池电压的一半，会被判成永久低电/过放关机。早期未烧 eFuse 样机会直接误关机。仅影响未校准
  设备，定 P2（在受影响设备上其实接近 P0，但量产已烧 eFuse 概率高，故折中 P2）。
- **文件**：`main/boards/common/power_manager.h:165-171`
- **代码**：
  ```cpp
  if (do_calibration1_chan0_) {
      ESP_ERROR_CHECK(adc_cali_raw_to_voltage(..., &voltage));
      voltage = voltage * 2;        // :167 校准路径 ×2
  } else {
      voltage = 3600 * 1000 / 4096 * average_adc / 1000;   // :170 回退路径未 ×2
  }
  ```
- **根因**：回退公式忘记乘分压系数 2，且 `3600*1000/4096` 整数运算先算得 878，精度也丢失。
- **修复建议**：回退路径同样 `* 2`：`voltage = (3300 * average_adc / 4095) * 2;`（注意满量程
  ADC_ATTEN_DB_12 约 3.1~3.3V，常数 3600 偏高需复核），统一两条路径量纲。

### P2-A3 充电检测引脚无去抖/迟滞，每 1s 单次 `gpio_get_level` 抖动触发回调风暴（电源）
- **判级理由**：`CheckBatteryStatus` 每秒读一次充电脚电平直接比较，无去抖。充电器接触不良/CC 抖动时
  `is_charging_` 反复翻转，每次翻转都触发 `on_charging_status_changed_` 回调 + 清空 ADC 队列重采样
  + UI 刷新。偶发，定 P2。
- **文件**：`main/boards/common/power_manager.h:97-106`
- **代码**：
  ```cpp
  bool new_charging_status = gpio_get_level(charging_pin_) == 0;  // 单次采样无去抖
  if (new_charging_status != is_charging_) { ... 触发回调 + clear + 重采样 }
  ```
- **根因**：充电状态判定无连续 N 次确认（对比 TypecHeadset 有 insert_cnt/remove_cnt 去抖）。
- **修复建议**：加去抖计数（连续 2~3 次同态才确认翻转）。

### P2-A4 `Button::OnLongPress`/`OnMultipleClick` 每次注册 `new CallbackContext` 永不释放，重复注册即泄漏（按键/内存）
- **判级理由**：`OnLongPress`（:81）与 `OnMultipleClick`（:146）各 `new CallbackContext`，指针交给
  iot_button 作 usr_data，但 `~Button`（:38）只 `iot_button_delete`，从不 `delete context`。一次性
  注册仅泄漏几十字节；但若运行期对同一按键多次调用同一注册函数（换绑回调），旧 context 全部泄漏。
  违反 ESP32 内存规范"所有 new 有对应释放"。偶发，定 P2。
- **文件**：`main/boards/common/button.cc:81`、:146、:38-42
- **代码**：
  ```cpp
  auto* context = new CallbackContext{this, press_time_ms};   // :81 无对应 delete
  ...
  auto* context = new CallbackContext{this, click_count};     // :146 无对应 delete
  ```
- **根因**：context 生命周期未纳管理；`~Button` 未注销/释放这些 context。
- **修复建议**：用成员容器（如 `std::vector<std::unique_ptr<CallbackContext>>`）持有，析构时随对象释放；
  或不额外 new，把 press_time/click_count 编码进 map key 后用 `this` 作 usr_data 在回调里查。

### P2-A5 `SystemReset::ResetToFactory` 在 `esp_partition_erase_range` 失败时仍重启，可能擦半 otadata 变砖（板级/安全）
- **判级理由**：`esp_partition_erase_range`（:68）未判返回值，紧接着倒计时重启（:72）。若擦除中断电或
  返回错误，otadata 处于半擦状态，引导可能选错 slot 或进入不可引导态。仅在工厂复位路径触发（低频），
  定 P2，但后果接近砖机。
- **文件**：`main/boards/common/system_reset.cc:60-73`
- **代码**：
  ```cpp
  esp_partition_erase_range(partition, 0, partition->size);   // :68 未判返回
  ESP_LOGI(TAG, "Erased otadata partition");
  RestartInSeconds(3);                                        // 失败也重启
  ```
- **根因**：擦除操作未检查 esp_err_t 返回。
- **修复建议**：判返回值，失败则 LOGE 并不重启（或重试），同时 `ResetNvsFlash` 的擦除失败也应阻断后续。

### P2-A6 TypecHeadset `DetectLoop` 用函数内 `static` 计数器，多实例/重启共享状态（按键/驱动）
- **判级理由**：`insert_cnt/remove_cnt/dbg_cnt` 是 `DetectLoop` 内 `static`（:115、:117），属进程级
  而非实例级。若存在两个 TypecHeadset 实例（多板/测试）或 Stop 后再 Start 新实例，去抖计数会被串扰，
  导致插拔判定错乱。当前 P31 单实例，定 P2。
- **文件**：`main/boards/common/typec_headset.cc:115-129`
- **代码**：
  ```cpp
  static int insert_cnt = 0, remove_cnt = 0;
  static int dbg_cnt = 0;
  ```
- **根因**：本应是实例状态的去抖计数被写成函数 static。
- **修复建议**：移为类成员变量，随实例隔离与 Start 重置。

---

## P3（潜在远期风险）

### P3-A1 `Stop()` 用 `vTaskDelay(100ms)` 软等待检测任务退出，非可靠 join（休眠/任务）
- **判级理由**：`TypecHeadset::Stop`（:200-209）置 `running_=false` 后只 `vTaskDelay(100)` 就把
  `task_=nullptr` 并删 ADC unit。DetectLoop 单轮最长含多个 `vTaskDelay(50)`，100ms 不保证任务已退出
  循环，存在删 ADC unit 后任务仍调用 `adc_oneshot_read`(UAF) 的窗口。当前仅析构路径触发（极少），定 P3。
- **文件**：`main/boards/common/typec_headset.cc:200-210`
- **代码**：
  ```cpp
  running_ = false;
  if (task_) { vTaskDelay(pdMS_TO_TICKS(100)); task_ = nullptr; }
  if (adc_owned_ && adc_handle_) { adc_oneshot_del_unit(adc_handle_); }
  ```
- **修复建议**：用任务退出信号量/`eTaskGetState` 轮询确认 Deleted 后再删资源，或 DetectLoop 退出前
  主动给信号。

### P3-A2 PowerSaveTimer/SleepTimer 多个 `ESP_ERROR_CHECK(esp_timer_*)` 在运行期调用会 abort（电源/休眠）
- **判级理由**：`SetEnabled` 内 `esp_timer_start_periodic`/`esp_timer_stop` 用 `ESP_ERROR_CHECK`
  （power_save_timer.cc:40/43、sleep_timer.cc:44/47）。这些在运行期被业务多次调用（进出配网/会话），
  若 timer 已是目标态返回 `ESP_ERR_INVALID_STATE` 会 abort 重启。比第一轮报的 ADC abort 概率低（state
  错误较少见），但同属"可恢复错误用断言"，定 P3 补充。
- **文件**：`power_save_timer.cc:40`、:43；`sleep_timer.cc:44`、:47
- **代码**：
  ```cpp
  ESP_ERROR_CHECK(esp_timer_start_periodic(power_save_timer_, 1000000));
  ...
  ESP_ERROR_CHECK(esp_timer_stop(power_save_timer_));
  ```
- **修复建议**：运行期 start/stop 改为判返回值并容忍 `ESP_ERR_INVALID_STATE`（已停/已启不报错）。

### P3-A3 light sleep 期间未做 GPIO 保持，输出脚（PA/USB_SW 等）电平在睡眠中漂移（休眠/漏电）
- **判级理由**：`SleepTimer` 进 light sleep 前未对 TypecHeadset 的输出脚（pa_pin/usb_sw_pin/cc_vdd_pin）
  或背光脚做 `gpio_hold_en`。light sleep 一般保持数字 IO，但若叠加 modem/pm 配置变更或某些域掉电，输出
  态可能漂移导致 PA 误开（耗电）或漏电。P31 深睡路径已对 AUDIO_PWR_EN 做 hold（board.cc:516），但
  light sleep 路径与这些外设脚未覆盖。远期/边缘，定 P3。
- **文件**：`main/boards/common/sleep_timer.cc:88-107`（无 gpio_hold 处理）；对照
  `main/boards/mydazy-p31/mydazy_p31_board.cc:516` 深睡 hold
- **修复建议**：进 light sleep 前对关键输出脚 `gpio_hold_en`，恢复后 `gpio_hold_dis`；明确各电源域在
  light sleep 的保持策略。

---

## 统计

| 等级 | 数量 | 条目 |
|------|------|------|
| P0   | 1    | P0-A1 过放/低电关机门限失效（kLowBatteryLevel=0 + 3400mV 关机无裕量） |
| P1   | 3    | P1-A1 light sleep 唤醒最长 30s 黑屏 · P1-A2 背光渐变跨线程竞争/step_ 无符号 · P1-A3 省电未协调 modem 致 4G 掉线 |
| P2   | 6    | P2-A1 充电翻转电量跳变 · P2-A2 未校准回退漏 ×2 分压 · P2-A3 充电脚无去抖回调风暴 · P2-A4 Button context 泄漏 · P2-A5 otadata 擦除未判返回近砖 · P2-A6 DetectLoop static 计数串扰 |
| P3   | 3    | P3-A1 Stop 非可靠 join 致 UAF 窗口 · P3-A2 timer start/stop ESP_ERROR_CHECK abort · P3-A3 light sleep 未 gpio_hold 输出脚 |
| **合计** | **13** | |

> 协同提示：
> - P0-A1 与 P2-A2 叠加最危险——未校准设备电压被算成一半 → `is_off_battery_` 在真实电压远未到截止时即触发
>   误关机，量产前务必先修 P2-A2 的 ×2 分压再调 P0-A1 的门限裕量，并以实测电压标定 levels_fd/cd 曲线。
> - P1-A1 与第一轮 P2-2（唤醒词恢复时序）同处 sleep_timer.cc 的 Schedule lambda，建议一并重构该段。
