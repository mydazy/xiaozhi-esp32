# MyDazy P30 全面测试清单（v2.2.7）

> **适用板型**：P30-4G（主） / P30-WiFi（同步验证）
> **测试目标**：本次精简优化后回归全功能 + 边界 / 异常场景
> **执行方式**：按维度顺序逐项验证，每项打勾 ✅ / 不通过 ❌（附 log）
> **烧录命令**：`idf.py -p /dev/cu.usbmodem2101 -b 2000000 flash monitor`
> ⭐ 标记 = 本次改动直接影响项，**必须验证**

---

## 一. 上电 / 启动流程

| # | 步骤 | 预期 | 关注 |
|---|------|------|------|
| 1.1 | 冷启动（拔插电池或先长按 5s 关机后再按 BOOT 启动）| 系统初始化日志、LCD 显示 logo 渐入、状态栏渐入、3-5s 内进时钟主屏 | 无白屏 / 无花屏 / 无残留旧画面 |
| 1.2 | 启动日志检查 | 串口能看到 `MyDazy P30 4G 初始化完成 (ES8311+ES7210, 支持4G、电源管理、触摸屏)` | TAG 全程为 `MyDazyP30_4GBoard` |
| 1.3 | 内部 RAM 检查 | 启动稳定后串口打印 free heap > **60KB** 内部 RAM | ⭐（量产红线） |
| 1.4 | 字体加载日志 | 看到 `Fallback text font loaded: font_maru_common_20_4.bin` + 时钟字体加载成功 | ⭐ 教育卡前置 |
| 1.5 | 首次开机欢迎语 | 1.5s 后听到欢迎音 + 自动进入对话 | 仅 `first_boot_` 触发 · 已删 NVS 开关 |
| 1.6 | 二次开机不再播欢迎语 | 仅冷启动播一次（`first_boot_` 控制）| 防扰民 |

---

## 二. BOOT 按键交互矩阵 ⭐（本次改动核心）

| # | 操作 | 状态前提 | 预期行为 | 关注 |
|---|------|---------|---------|------|
| 2.1 | 单击 | Idle | 唤醒音 + 1.5s 后进 Listening | |
| 2.2 | 单击 | Listening | 退出对话音 + 退到 Idle | |
| 2.3 | 单击 | Speaking | 立即打断 TTS 进 Idle | |
| 2.4 | 单击 | MP3 播放中 | **先停 MP3 + 退 Player UI**，再走唤醒流程 | ⭐ D4 |
| 2.5 | 双击 | Idle/Listening/Speaking（AEC 启用编译时） | AEC 开/关切换 + popup 提示 | |
| 2.6 | 双击 | 配网态（kDeviceStateWifiConfiguring）| 切换 BLUFI ↔ AP 模式 + 对应提示音 | |
| 2.7 | 3 连击 | 任意非配网态 | WiFi/4G/进配网 三态轮换 + 对应提示音 → 重启 | 需观察重启不抖动 |
| 2.8 | 4 连击 | 任意 | 提示"再见" + 2.5s 后真关机（`enable_gyro_wakeup=false`，仅按键能唤醒）| ⭐ inline |
| 2.9 | **9 连击** | 任意 | 进恢复出厂确认态，提示"10秒内双击确认" | ⭐ inline |
| 2.10 | 9 连击后 10s 内**双击** | waiting_factory_reset_confirm_ | 停 TTS + Alert"确认恢复" + 服务器解绑 + NVS 全擦 + 重启 | |
| 2.11 | 9 连击后 **超 10s** 双击 | 超时窗口 | 自动取消，无副作用 | |
| 2.12 | 9 连击后**单击**（任意状态）| | 应清掉 waiting_factory_reset_confirm_ 标记，恢复正常单击行为 | |
| 2.13 | **长按 0.7s** | 任意 | **应无任何反应**（PTT 已删除）| ⭐ PTT 移除核心验证 |
| 2.14 | 长按 3s | 任意 | Alert"长按 5 秒关机 / 继续按住..." + OGG_REBOOT 提示音 | ⭐ inline |
| 2.15 | 长按 3-5s 之间松开 | shutdown_armed_=true | popup 提示音 + 取消关机 | ⭐ inline（取消） |
| 2.16 | 长按到 5s 不松开 | shutdown_armed_=true | 进入"关机中" + 2s 后真关机 | ⭐ inline |
| 2.17 | 持续长按 ≥5s | | OGG_REBOOT 自然延续到关机，**不应**重复播提示音 | WiFi 板特殊注释 |
| 2.18 | 关机后按 BOOT 单按一下立刻松开 | 深睡中 | 立即回深睡（`CheckBootHoldOnWakeup` 失败，不开机）| |
| 2.19 | 关机后按 BOOT 长按 2s 不松 | 深睡中 | 真正开机 | ⭐ 长按 2s 开机 |
| 2.20 | 关机后按 BOOT 长按 1s 后松 | 深睡中 | 不开机，回深睡 | 防口袋误触 |

---

## 三. 触摸交互 ⭐

| # | 操作 | 状态前提 | 预期行为 | 关注 |
|---|------|---------|---------|------|
| 3.1 | 屏幕**非顶部 36px** 单击 | Idle | 唤醒音 + ToggleChatState | 与 BOOT 单击同义 |
| 3.2 | 屏幕单击 | Speaking | 打断 TTS | |
| 3.3 | 屏幕单击 | MP3 播放中 | **忽略**（让 LVGL Player Pause 按钮独占处理）| ⭐ |
| 3.4 | 屏幕**顶部 36px**（状态栏）单击 | 任意 | driver 路径忽略，LVGL CLICKED 接管 | |
| 3.5 | 屏幕双击 | Listening/Speaking | 退出对话音 + 退到 Idle | |
| 3.6 | 屏幕双击 | Idle / MP3 | **忽略**（避免误触干扰）| |
| 3.7 | RF 风暴模拟（4G 高发期连续触发 ≥3 次 gesture in 1.2s）| | 静默 3s + ESP_LOGW 日志 | ⭐ D5 节流 |
| 3.8 | RF 风暴静默期内点击 | 静默期内 | 全部丢弃 + ESP_LOGD 日志 | |
| 3.9 | 滑动手势（上/下/左/右） | | **全部丢弃**（ControlCenter 已 stub）| |
| 3.10 | 长按 / Release 手势 | | **全部丢弃**（PTT 已下线）| 与 D5 一致 |
| 3.11 | 教育卡显示中触屏任意位置 | edu_card_overlay_ active | 立即关卡 + 恢复底层 emoji_box / clock | |
| 3.12 | QR 码页双击 | 配网 / 激活 QR overlay | 触发 on_double_click 回调（如 BLUFI/AP 切换）| 时间窗 500ms |

---

## 四. 音量按键

| # | 操作 | 预期 | 关注 |
|---|------|------|------|
| 4.1 | 单击 VOL+ / VOL- | 当前音量 ±10，状态栏 1.5s notification | clamp [0,100] |
| 4.2 | 长按 VOL+ | 每 200ms +5 步进，最高 100 停 | atomic running 控制 |
| 4.3 | 长按 VOL- | 每 200ms -5 步进，最低 0 停 | |
| 4.4 | 长按中松开 | 任务退出，状态栏不再变化 | task_handle 重置 nullptr |
| 4.5 | 重启后音量持久化 | NVS audio.output_volume 保留上次值 | |
| 4.6 | 首次启动或音量 < 50 | 自动修正为 80 + log "检测到音量X小于50" | ApplyDefaultSettings |

---

## 五. 配网（关键 - 4G 板）

| # | 步骤 | 预期 | 关注 |
|---|------|------|------|
| 5.1 | 首次启动无 WiFi 配置 | 进入配网态（蓝牙/AP 二选一）| 默认 BLUFI（NVS wifi.blufi=1）|
| 5.2 | 配网态显示 QR 码页 | 设备名 / 二维码 / 左右色条 | |
| 5.3 | 配网态双击切换 BLUFI ↔ AP | QR 内容更新 + 提示音 + 色条 active 翻转 | task pin Core 1 + atomic_flag 防重入 |
| 5.4 | 配网完成自动重启 | 进入正常对话流程 | |
| 5.5 | 已配网设备 3 连击切到 4G | "切换到4G" Alert + 1.5s 重启 | |
| 5.6 | 4G 模式 3 连击切回 WiFi | "切换到WiFi" Alert + 1.5s 重启 | |
| 5.7 | WiFi 模式 3 连击触发清配置 | 重启 + 进配网态 | wifi_board.ResetWifiConfiguration |
| 5.8 | 切网前停 MP3 + 打断对话 | 切换前听不到残音 | ⭐ D4 PauseAudioAndChatBeforeSwitch |

---

## 六. 对话流程

| # | 步骤 | 预期 | 关注 |
|---|------|------|------|
| 6.1 | 单击唤醒 → 说话 → 等回复 | Listening → Speaking → 回 Idle | 状态栏文字同步 |
| 6.2 | 唤醒词唤醒（"小智"等）| 自动进 Listening 录音 | AFE wake word |
| 6.3 | TTS 期间触摸双击 | 立即停 + 退 Idle + popup | |
| 6.4 | 网络断 → 自动重连 | 指数退避 1s→60s | 重连成功后能继续对话 |
| 6.5 | 长时间静音不说话 | 服务端超时回 Idle | |
| 6.6 | 4G 弱信号场景对话 | 卡顿但不死锁 | 串口 `csq` 数据 |

---

## 七. MP3 播放器

| # | 步骤 | 预期 | 关注 |
|---|------|------|------|
| 7.1 | LLM 触发播 MP3 | 自动切到 Player UI（曲名 + Play/Pause + 进度条）| |
| 7.2 | 进度条 200ms 更新 | 时间显示从 0:00 滚动 | player_tick_ |
| 7.3 | 屏幕中央按钮单击 | Play/Pause toggle + 图标切换 | LVGL CLICKED 独占 |
| 7.4 | BOOT 单击 | **停 MP3 + 退 Player + 唤醒** | ⭐ D4 |
| 7.5 | 触屏单击 | **保持 MP3，gesture 忽略** | ⭐ |
| 7.6 | BOOT 3 连击切网 | **先停 MP3** 再切（防异响）| ⭐ D4 PauseAudio |
| 7.7 | MP3 播放中 ReportStatus | **跳过**（防 OSS Range / TLS 抢带宽）| ⭐ |
| 7.8 | 自然播放完毕 | 自动退 Player UI 回 Idle | |

---

## 八. 教育卡 ⭐（本次新增）

> MCP 工具：`self.education.show_card(category, main, top, bottom)` + `self.education.show_stroke(character)`

| # | 触发 | 预期 | 关注 |
|---|------|------|------|
| 8.1 | LLM 触发 word 卡："学单词 cat" | 黑屏 / 上拼读 / **48px 金黄** "cat" / 分隔线 / 30px 绿色释义 | ⭐ word/pinyin main = 48px |
| 8.2 | LLM 触发 hanzi 卡："教我写鸟字" | 黑屏 / 上 30px 拼音 / **田字格 100×100 红边白底** / 黑字汉字居中 / 灰虚线十字 / 30px 绿色组词 | ⭐ 田字格 |
| 8.3 | LLM 触发 pinyin 卡："学整体认读 ying" | 黑屏 / 上类别"整体认读" / **48px 橙红 ying** / 无分隔线 / 30px 例字 | ⭐ pinyin |
| 8.4 | 长英文单词（>9 字母）"elephant" | 主体自动降到 30px（防溢出 270px 屏宽）| 字号自适应 |
| 8.5 | 教育卡触屏退出 | 卡片消失 + 底层 emoji_box / clock 自动暴露 | |
| 8.6 | 教育卡显示中 BOOT 单击 | （当前未联动，gesture 路径才退出）| 待补 / 设计意图 |
| 8.7 | LLM 触发 show_stroke "鸟" | 后台异步下载笔画 GIF（< 512KB），完成后替换 emoji "font" 槽 + SetEmotion | PSRAM 任务 |
| 8.8 | show_stroke 缺字（无 GIF）| 校验失败 + heap_caps_free + log "GIF 校验失败" | 防解码崩溃 |
| 8.9 | 显示生僻字（不在 GB2312 一级 3755）| 主体 fallback 30px → 20px g_text_font，可显示但字号变小 | 字库覆盖 |
| 8.10 | 副字行任意中文 | 30px 字库覆盖（无白方块）| ⭐ 字库验证 |

---

## 九. 省电 / 唤醒

| # | 步骤 | 预期 | 关注 |
|---|------|------|------|
| 9.1 | 静止 2 分钟（默认 PowerSaveTimer） | 进省电 → 背光降到 15 + ReportStatus 触发一次 | ⭐ D1 唤醒事件上报 |
| 9.2 | 静止 5 分钟 | OnShutdownRequest → 进深睡（提示"休眠中 / 拿起唤醒"）| `deepSleep` 设置 |
| 9.3 | 深睡中拿起摇晃 | SC7A20H ext1 唤醒 + 进对话 | `pickupWake=1` |
| 9.4 | 深睡中按 BOOT（长按 2s）| 唤醒进对话 | ⭐ D9 长按 2s 开机 |
| 9.5 | 深睡中按 BOOT（短按）| 立即回深睡 | |
| 9.6 | 深睡中定时器到（如闹钟）| TIMER 唤醒 → `AlarmManager::MarkTimerWakeup()` → CheckAndTrigger 触发回调 | 闹钟已接入 |
| 9.7 | EnterDeepSleep 流程日志 | 见 7 步：停 AudioService → ResetProtocol → GracefulShutdownModem → arm_wakeup → 关触摸/音频 → 配置唤醒源 → 复位 GPIO | |
| 9.8 | 关机时按住 BOOT | 等 BOOT 释放后再进深睡（最多 5s） | 防立即被唤醒 |
| 9.9 | EnableAutoSleep(false) | 禁用自动休眠，5 分钟仍亮屏 | NVS deepSleep=0 |

---

## 十. 摇一摇

| # | 步骤 | 预期 | 关注 |
|---|------|------|------|
| 10.1 | Idle 状态拿起设备甩 ≥3 次（600ms 内） | popup 音 + LLM 触发"启蒙场景" | EduScenePool 随机 |
| 10.2 | 对话中甩 | **忽略**（只 Idle 接管） | ESP_LOGD "Shake ignored" |
| 10.3 | MP3 播放中甩 | **忽略**（同上） | |
| 10.4 | 1.5s 内连甩两次 | 第二次冷却拒绝 | kCooldownUs |
| 10.5 | 摇一摇日志 | `Shake detected! peak_dev_sq=X strong=Y/6` | |

---

## 十一. 状态上报

| # | 步骤 | 预期 | 关注 |
|---|------|------|------|
| 11.1 | 进省电模式触发 ReportStatus | POST /status 含 battery / volume / brightness / theme / network.csq / network.carrier | ⭐ D1 |
| 11.2 | Idle 之外触发 | **跳过** + log "skip status report, state=X" | ⭐ |
| 11.3 | MP3 播放中触发 | **跳过** + log "skip status report, music playing" | ⭐ |
| 11.4 | 周期定时器是否还在 | **不应有** `status_timer_` 任务（已删 D1） | ⭐ |

---

## 十二. OTA / 恢复出厂

| # | 步骤 | 预期 | 关注 |
|---|------|------|------|
| 12.1 | 通过 MCP `self.ota.upgrade` 触发 | 下载固件 + 校验 + 重启升级 | |
| 12.2 | OTA 升级前 | ShutdownHandler 自动断 LDO（音频 PA 静音 → AUDIO_PWR_EN 拉低 → hold_en）| 防 LCD 闪 / 音圈 POP |
| 12.3 | 9 连击 + 双击恢复出厂 | NVS 全擦 + 服务器解绑（mydazy-ota）+ 3s 倒计时 esp_restart | |
| 12.4 | 服务器解绑失败 | 不阻塞本地擦除 | http timeout 5s |
| 12.5 | 网络切换重启 | 走 ShutdownHandler → 不会闪屏 | 跨 esp_restart 保持 LDO 低 |

---

## 十三. 字库 / 显示

| # | 步骤 | 预期 | 关注 |
|---|------|------|------|
| 13.1 | 时钟字体加载 | font_maru_88_4.bin（大字）+ font_maru_30_4.bin（副）| |
| 13.2 | 教育卡 48px 字体加载 | font_maru_48_4.bin 加载成功 + fallback 链 48→30→g_text_font | ⭐ |
| 13.3 | assets 分区使用率 | `df` 类似（generated_assets.bin ≈ 4.87 MB / 8 MB）| 留 3.13 MB 余量 |
| 13.4 | 状态栏 1Hz 时钟刷新 | 时间稳定走秒，不掉帧 | LvglDisplay UpdateStatusBar |
| 13.5 | 表情切换 (chat) | emoji_box 显 emotion，font 模式按 220×220 缩放 | |
| 13.6 | 通知 / Alert 文字 | recolor 支持 + 多行不溢 | |

---

## 十四. 边界 / 异常

| # | 场景 | 预期 | 关注 |
|---|------|------|------|
| 14.1 | 电池电量 < 阈值 | Alert "电量不足请充电" + OGG_CHARGE | |
| 14.2 | 电量极低（IsOffBatteryLevel）| ShutdownOrSleep "电量过低 / 强制关机" | |
| 14.3 | I2C 总线在 4G RF 干扰下 | glitch_ignore_cnt=15 滤毛刺，无 I2C error | |
| 14.4 | 长按 BOOT 关机时按住不放 | ConfigureDeepSleepWakeupSources 等释放最多 5s 再 enable_ext0_wakeup | 防立即唤醒 |
| 14.5 | 极端：开机瞬间触屏 | LVGL 起来前不响应 | 触摸驱动初始化顺序 |
| 14.6 | 编译两板一致性 | P30-4G + P30-WiFi 都能 release.py 通过 | 同源代码差异最小 |

---

## 验证矩阵汇总

| 维度 | 项数 | ⭐ 必测项 |
|------|------|---------|
| 一. 启动 | 6 | 1.3, 1.4 |
| 二. BOOT 按键 | 20 | **2.4 / 2.8 / 2.9 / 2.13-2.20**（全部本次改动相关） |
| 三. 触摸 | 12 | 3.3, 3.7-3.10 |
| 四. 音量 | 6 | - |
| 五. 配网 | 8 | 5.8 |
| 六. 对话 | 6 | - |
| 七. MP3 | 8 | 7.4-7.7 |
| 八. 教育卡 | 10 | 全部 |
| 九. 省电 | 9 | 9.1, 9.4 |
| 十. 摇一摇 | 5 | - |
| 十一. 状态上报 | 4 | 全部 |
| 十二. OTA | 5 | - |
| 十三. 字库 | 6 | 13.2 |
| 十四. 边界 | 6 | 14.6 |
| **合计** | **111** | **35 项必测** |

---

## 测试报告模板

```text
板号: ____________   测试人: ____________   日期: ____________
固件版本: 2.2.7   板型: P30-4G / P30-WiFi
环境: WiFi 信号强度 ____  4G 运营商 / CSQ ____  电池 ____%

总览：✅ ___ / 111   ⭐必测：✅ ___ / 35   ❌阻塞：___ 项
阻塞详情:
  - 项号 X.Y: 现象描述 + 串口日志摘要
回归状态: PASS / 部分 PASS（带条件） / FAIL（不能发版）
```

## 异常上报规范

每条 ❌ 项需附：
1. **项号 + 标题**（例 `2.13 长按 0.7s 应无反应`）
2. **实际现象**（一句话）
3. **串口 log 关键 5-10 行**（用 ```code``` 包裹）
4. **复现步骤**（用户操作序列）
5. **影响范围**：阻塞发版 / 体验降级 / 偶发

---

## 本次改动专项关注点（速查）

| 改动 | 验证项 | 失败影响 |
|------|--------|---------|
| D1 删周期上报 | 11.4 / 11.1 | 状态上报机制错乱 |
| D2/D3 删死字段 | 编译通过 = OK | 无运行时影响 |
| D4 抽 StopMp3AndExitPlayerUi | 2.4 / 5.8 / 7.4 / 7.6 | MP3 残音 / 切网卡顿 |
| D5 触摸节流展平 | 3.7 / 3.8 | 4G RF 干扰下误触发风暴 |
| D6 注释更新 | 无运行时影响 | - |
| D7 GetAudioCodec 注释 | 无运行时影响 | - |
| D8 DEFAULT_VOLUME constexpr | 4.6 | 首次启动音量修正 |
| D9 HandleWakeupCause 移出 | 1.1 / 9.4 | 唤醒原因解析时序问题 |
| **PTT 移除** | **2.13**（核心）/ 3.10 | 长按 0.7s 仍录音 = 失败 |
| **5 短 handler 内联** | 2.8 / 2.9 / 2.14-2.16 | 关机/出厂确认行为偏差 |
| **48px 教育字库** | 8.1-8.10 / 13.2 | 字体缺失 / 字号自适应失效 |

---

## 最小回归集（10 分钟急测，发版前）

> 必跑，任一失败禁止发版：

1. **2.13** 长按 BOOT 0.7s **无反应** ⭐
2. **2.14-2.16** 长按 3s/5s 关机 + 中途松开取消 ⭐
3. **2.19** 关机后长按 2s 开机 ⭐
4. **2.4** MP3 播放中按 BOOT → 停 MP3 + 唤醒 ⭐
5. **8.1-8.3** 教育卡 word/hanzi/pinyin 三类显示 ⭐
6. **9.3** 摇一摇唤醒
7. **5.5** 3 连击切 WiFi/4G
8. **9.1** 进省电模式触发 ReportStatus

剩余项发布后 24 小时内补完整测试。
