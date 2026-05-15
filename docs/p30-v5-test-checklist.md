# P30 触摸/传感器 v5.0 真机测试清单

> 2026-05-14 · 烧录验证 · 由工厂/硬件同事执行
> 改动范围：触摸驱动 v5.0 + sc7a20h + alarm_ringer + 三板 cb 迁移

---

## 0. 烧录前准备

### 0.1 环境

```bash
cd ~/GitHub/mydazy-p30-v32
source idf55
git log -1 --oneline    # 确认 commit
git status              # 应该是 clean
```

### 0.2 烧录命令

```bash
# P30-4G 板（默认 sdkconfig）
idf.py -p /dev/cu.usbmodem2101 -b 2000000 flash monitor

# P30-WiFi 板
sed -i.bak 's/^CONFIG_BOARD_TYPE_MYDAZY_P30_4G=y/# CONFIG_BOARD_TYPE_MYDAZY_P30_4G is not set/; s/^# CONFIG_BOARD_TYPE_MYDAZY_P30_WIFI is not set/CONFIG_BOARD_TYPE_MYDAZY_P30_WIFI=y/' sdkconfig
idf.py build && idf.py -p /dev/cu.usbmodem2101 -b 2000000 flash monitor

# P31 板
sed -i.bak 's/^CONFIG_BOARD_TYPE_MYDAZY_P30_4G=y/# CONFIG_BOARD_TYPE_MYDAZY_P30_4G is not set/; s/^# CONFIG_BOARD_TYPE_MYDAZY_P31 is not set/CONFIG_BOARD_TYPE_MYDAZY_P31=y/' sdkconfig
idf.py build && idf.py -p /dev/cu.usbmodem2101 -b 2000000 flash monitor
```

测试完恢复：`cp sdkconfig.bak sdkconfig && rm sdkconfig.bak`

---

## 1. P30-4G 板测试（核心 · 必过）

### 1.1 启动 log 验证（5 秒）

烧录后 boot log **必须**出现：

```
✓ axs5106l_touch: init RST=GPIO4 INT=GPIO5 284x240 · rf_mode=STRICT(4G)
✓ axs5106l_touch: chip ID: 0x?? 0x?? 0x??   (任意非零)
✓ axs5106l_touch: firmware version: 0x2907   (V2907)
✓ axs5106l_touch: INT edge ISR installed for RF storm detection
✓ axs5106l_touch: registered with LVGL
✓ sc7a20h: init ok (pickup=320mg/100ms · THS=0x0A DUR=0x0A)
✓ sc7a20h: shake on (dev=1500mg win=600ms tgt=2/6 cool=1500ms)
```

| 项 | 期望 | 通过 |
|---|---|---|
| `rf_mode=STRICT(4G)` | ✓ 这是 4G 板必须 | ☐ |
| `chip ID` 非全 0/0xFF | ✓ 焊接正常 | ☐ |
| `firmware version: 0x2907` | ✓ 固件正确 | ☐ |
| `INT edge ISR installed` | ✓ storm 检测启用 | ☐ |
| `sc7a20h init ok` | ✓ 加速计上线 | ☐ |
| `shake on ... tgt=2/6` | ✓ 摇一摇 target=2 | ☐ |

### 1.2 触摸基础功能（2 分钟）

| 操作 | 期望 log | 期望 UI | 通过 |
|---|---|---|---|
| 触屏任意位置 | `tap (x,y)` | 屏幕上**红点**跟随手指（DEBUG_OVERLAY=1） | ☐ |
| 单击空白处（y > 36） | `tap` + `单击唤醒对话` | 进入对话（800ms wakeup 音后） | ☐ |
| 进对话中再单击 | `tap` + `单击打断TTS` | 当前 TTS 立即停 | ☐ |
| 双击（间隔 < 600ms） | `tap` + `double-tap` + `双击退出对话：state=X → Idle` | **800ms 后短暂进对话 + 听到 exitchat 音 + 回时钟主屏**（已知体验略冗余 · 功能正确） | ☐ |
| 横滑左→右 | `swipe dx=XX dy=XX` | 无业务响应（on_swipe=NULL · 正常） | ☐ |
| 长按 1 秒 | `long-press (x,y)` + `long-press release` | 无业务响应（on_long_press=NULL · 正常） | ☐ |

### 1.3 加速计场景（3 分钟）

| 操作 | 期望 log | 期望行为 | 通过 |
|---|---|---|---|
| 设备静止 | 无 shake log | — | ☐ |
| 上下摇晃 **2 次** | `shake detected (strong=2/6)` + `shake → AI` | 播放 popup 音 + SendTextToAI "摇一摇随机互动" | ☐ |
| 上下摇晃 **1 次** | 无 shake log | 无反应（target=2 需要 2 次） | ☐ |
| 走路场景 1 分钟（设备装口袋） | 无 shake / 偶尔 1 次（容忍） | 不应大量误触 | ☐ |
| 桌面拍击 1 次 | 无 shake（瞬态不算 shake） | 无误触 | ☐ |

### 1.4 闹钟摇停（5 分钟）

**设置**：远程下发一个 1 分钟后的闹钟，到点响铃。

| 操作 | 期望 log | 期望行为 | 通过 |
|---|---|---|---|
| 响铃中 摇 1 次 | `AlarmRinger: shake stop pending (1/3)` | 闹钟继续响 | ☐ |
| 响铃中 摇 2 次 | `pending (2/3)` | 闹钟继续响 | ☐ |
| 响铃中 摇 3 次（5s 内累计） | `AlarmRinger: Stop by 'shake×N'` | 闹钟停 | ☐ |
| 5s 后只摇 1 次 | `pending (1/3)` 计数重置 | 闹钟继续响 | ☐ |

### 1.5 深睡 → 唤醒（10 分钟）

**进入深睡**：远程下发 sleep 命令 · 或长时间 idle 自动休眠。

| 阶段 | 期望 log | 通过 |
|---|---|---|
| sleep 前 | `axs5106l_touch_sleep` 调用 | ☐ |
| sleep 前 | `sc7a20h: wakeup armed on GPIO3` | ☐ |
| sleep 前 | `INT1_SRC stale latch cleared`（如有残留） | ☐ |
| sleep 前 | `ShutdownTouchAndAudioForSleep` + `AUDIO_PWR_EN=0` **在 wakeup armed 之后** | ☐ |
| 设备休眠时静止放置 | 不应自动唤醒 | ☐ |
| 用力摇设备 | EXT1 拉低 → 设备唤醒 → boot | ☐ |
| 唤醒后 boot log | `wakeup_cause=2`（EXT1）+ first_boot=true · 无欢迎音 | ☐ |

### 1.6 4G RF storm 防御（高级 · 可选）

**场景**：弱信号场景下 4G 上行突发（如 OTA 下载 / MQTT 推送）。

| 操作 | 期望 log | 通过 |
|---|---|---|
| RF burst 期间触屏 | `RF storm 12 edges/s → muted 2s` | ☐ |
| storm 后 5s 内 | `axs5106l_touch: RF storm ... hot` (HOT 模式阈值 6) | ☐ |
| storm muted 期间触屏 | 屏幕**无红点**（驱动屏蔽 · 正常） | ☐ |
| storm 退出后 | 触屏正常响应 | ☐ |

---

## 2. P30-WiFi 板测试（差异点）

### 2.1 启动 log

**核心差异**：`rf_mode=NORMAL`（WiFi 板无 4G 共线）

```
✓ axs5106l_touch: init RST=GPIO4 INT=GPIO5 284x240 · rf_mode=NORMAL
```

| 项 | 期望 | 通过 |
|---|---|---|
| `rf_mode=NORMAL` | ✓ WiFi 板正确档位 | ☐ |
| 其余项 | 同 P30-4G § 1.1 | ☐ |

### 2.2 触摸 / 加速计 / 闹钟 / sleep

**与 P30-4G 完全一致**（业务代码相同 · 仅 rf_mode 差异）· 复用 § 1.2 - § 1.5 清单。

### 2.3 WiFi 板特有：无 RF storm 期望

WiFi 板**不应**出现 `RF storm muted` log（无 4G 干扰源）。如出现说明 NORMAL 档阈值偏低 · 需调。

---

## 3. P31 板测试

### 3.1 启动 log（确认迁移成功）

```
✓ axs5106l_touch: init RST=GPIO4 INT=GPIO5 284x240 · rf_mode=NORMAL
✓ axs5106l_touch: chip ID: ...
✓ axs5106l_touch: firmware version: 0x2907
✓ sc7a20h: init ok (pickup=320mg/100ms)
✓ sc7a20h: shake on (dev=1500mg ... tgt=2/6 ...)
```

| 项 | 期望 | 通过 |
|---|---|---|
| 不再有 `class Axs5106lTouch` 相关 log | ✓ 已迁移到 C handle | ☐ |
| `rf_mode=NORMAL` | ✓ P31 默认档 | ☐ |

### 3.2 触摸 / 加速计基础

| 操作 | 期望 | 通过 |
|---|---|---|
| 单击 | `tap` + `单击唤醒对话` 或 `单击停止 MP3` | ☐ |
| 摇 2 次 | `shake detected` + AI 互动 | ☐ |
| 拿起唤醒 | EXT1 唤醒 → boot | ☐ |

### 3.3 P31 已知降级（暂不验证）

| 项 | 状态 |
|---|---|
| 单击→主菜单（ShowMenu） | ⏸ 当前走通用唤醒路径（UiDisplay 接口未暴露） |
| Swipe 控制中心 | ⏸ on_swipe = NULL · 不响应滑动 |
| 双击 | ⏸ 未接 on_double_click · 单击业务覆盖 |
| ibeacon.StartDeferred(30000) | ⏸ 改为 immediate Start · 见 board.cc TODO |

---

## 4. 共性回归项（24h 稳定测试）

### 4.1 误触率（核心量产指标）

| 板 | 场景 | 期望误触/24h |
|---|---|---|
| P30-4G | 桌面静置 + 4G 在线 | ≤ 1 次 shake / 0 次 click |
| P30-WiFi | 桌面静置 + WiFi 在线 | ≤ 1 次 shake / 0 次 click |
| P30-4G | 口袋移动 1h | 不进对话（pickupWake 启用时可拿起唤醒） |
| P30-4G | 充电中 4G 直播 | 触屏正常 · storm muted 偶发可接受 |

### 4.2 功能不退化

| 项 | 状态 |
|---|---|
| 闹钟到点响铃 | ☐ |
| 闹钟摇停 | ☐ |
| 摇一摇 AI 互动 | ☐ |
| BOOT 按键唤醒 | ☐ |
| 拿起唤醒（深睡场景） | ☐ |
| 4G 联网 / WiFi 联网 | ☐ |
| AI 对话（语音 + TTS） | ☐ |
| MP3 播放（含触屏暂停） | ☐ |
| 触摸唤醒亮屏 | ☐ |
| 深睡进入 / 退出 | ☐ |

### 4.3 内存稳定性

```
heap_caps_get_free_size(MALLOC_CAP_INTERNAL)  > 60 KB 全程
uxTaskGetStackHighWaterMark(motion_task)     > 512 B
```

| 项 | 期望 | 通过 |
|---|---|---|
| 启动后内部 RAM | > 60 KB | ☐ |
| 24h 后内部 RAM | > 60 KB · 不持续下降 | ☐ |
| motion_task 栈水位 | > 512 B | ☐ |

---

## 5. 已知问题（不影响通过 · 测试时记录现象即可）

| # | 问题 | 现象 | 后续修 |
|---|---|---|---|
| P0-known | 双击业务串行执行 | 双击 → 800ms 后短暂进对话 + 立即退 · 两次音效 | 等真机反馈触发再修 |
| P1-known | P31 swipe 控制中心 | 不响应（on_swipe=NULL） | 等 UiDisplay ShowMenu 暴露 |
| P1-known | P31 主菜单 | 单击走通用对话唤醒 · 不进主菜单 | 等 UiDisplay 接口补齐 |
| P3 | storm muted 期间无亮屏 | 4G 弱信号大量 storm 时触屏不亮屏 | RF 防御副作用 · 接受 |

---

## 6. 失败回报模板

测试不通过时，按以下格式贴 issue / 反馈：

```
板型：[ P30-4G / P30-WiFi / P31 ]
固件 commit：xxxxxxxx
失败项：§ 1.2 单击业务
现象：
  - 操作：触屏屏幕中央（坐标 ~140, 120）
  - 期望：log "tap (140,120)" + 进对话
  - 实际：log "tap (140,120)" 但无进对话业务
log 截取（前 50 行）：
  [粘贴 monitor 输出]
判断：业务 dispatch 链路异常 / 或 driver 未触发
环境：4G 信号 -90dBm / 充电中 / 室温 25℃
```

---

## 7. 测试通过的标准

**P30-4G 量产基线**：
- ✅ § 1.1 启动 log 全过
- ✅ § 1.2 - 1.5 全过
- ✅ § 4.1 桌面静置 24h 误触 ≤ 1 次
- ✅ § 4.3 内存稳定

**P30-WiFi 量产基线**：
- 同 P30-4G · 但 § 1.6 不期望出现 storm log

**P31 量产基线**：
- ✅ § 3.1 - 3.2 全过
- ✅ § 3.3 已知降级项不阻塞（可后续 PR 恢复）
- ✅ § 4.1 - 4.3 全过

全部通过 → 推 dev → main · 发版打 tag `v2.2.13-touch-v5.0`。
