# MyDazy P30 麦克风校准与增益配置

> 适用版本: P30-4G / P31 (BoxAudioCodec + ES8311 + ES7210)
> 文档版本: 2026-05-04

---

## 一、硬件物料规格

### 麦克风（MIC1）

| 项 | 规格 |
|----|------|
| 类型 | **驻极体电容麦克风（ECM）** |
| 接口 | 模拟输出（2 线，差分接 ES7210 ADC）|
| 尺寸 | **6.5 mm × 3 mm**（圆形金属外壳）|
| 主物料灵敏度 | **-26 dBV/Pa @ 1kHz, 1Pa**（量产高端版）|
| 备用物料灵敏度 | -36 dBV/Pa（中档）/ -42 dBV/Pa（低成本）|
| 工作电压 | 1.5V ~ 3.3V（ES7210 mic_bias 提供）|
| 长期漂移 | < 1 dB / 5 年 |

### 喇叭

| 项 | 规格 |
|----|------|
| 类型 | Box 喇叭，**朝下发音** |
| 功率 | **3 W** |
| 阻抗 | 8 Ω |
| 驱动 | ES8311 codec + 内置 PA |

### 设备结构

| 项 | 数值 |
|----|------|
| 外形 | **40 × 50 × 26 mm**（极小体积）|
| MIC ↔ 喇叭距离 | **40 mm**（垂直对穿，喇叭朝下 mic 朝上）|
| 物理结构衰减 | ~3-5 dB（中低档 mic 因装配/防尘网）|

---

## 二、声学结构干扰分析

40×50×26 mm 小体积 + 40mm MIC↔喇叭距离 → 4 类内部干扰：

```
┌──────────── MIC (朝上) ────────────┐ 顶面
│   ↑                                  │
│   │ ① 空气直耦  (40mm 内部腔体)       │
│   │                                  │ 26 mm
│   │ ④ 共振腔放大                      │
│   ↓                                  │
│ ② 机身振动                           │
└──── 喇叭 (朝下) ──────────────────┘ 底面
                ↓
                ↓ ③ 桌面反射 (距离 5-10mm)
═══════════ 桌面 ═══════════
```

| 干扰源 | 影响 | 处理 |
|-------|------|------|
| ① 空气直耦 | 强（路径仅 40mm）| AEC 主导处理 |
| ② 机身振动 | 中（3W 喇叭振动）| 结构传声，AEC 难消 |
| ③ 桌面反射 | 强（朝下喇叭）| 用户使用习惯影响 |
| ④ 腔体共振 | 1kHz 附近 +6dB | 校准时已"消化"在 RMS 中 |

校准方案对此结构已做适配（阈值基于实测，不是理论计算）。

---

## 三、增益配置三档（按 mic 灵敏度）

### 配置对照表（默认 = 室内模式，校准后立即生效）

| Mic 物料 | input | ref | aec | output_volume | 总等效灵敏度 |
|---------|-------|-----|-----|---------------|-------------|
| **-26 dBV**（量产主物料）| **9** | **6** | **6** | **80** | -17 dBV/Pa |
| **-36 dBV**（中档）| **18** | 6 | 6 | 80 | -18 dBV/Pa |
| **-42 dBV**（低档）| **24** | 6 | 6 | 80 | -18 dBV/Pa |

### 双模式设计：室内 / 室外（MIC + REF 同步 ±3 dB）

| 模式 | input 偏移 | ref | 适用场景 | MCP 调用 |
|------|----------|-----|---------|---------|
| **室内**（indoor·校准默认）| 室内基线（0）| **6** | 安静室内 / 近场 / 减少回声 | `set_mode("indoor")` |
| **室外**（outdoor）| **+3 dB** | **9** | 嘈杂 / 远场 / 1m+ | `set_mode("outdoor")` |

模式切换 = **MIC 和 REF 同步 ±3 dB**：
- `input ±3` 调拾音灵敏度（室外强 / 室内弱避反射）
- `ref ±3` 同步调 AEC 参考（室外强让 AEC 在大音量下收敛 / 室内弱避免参考过强）
- 差 3 dB 命中 ES7210 物理档位，切换温和不剧烈

#### 三档 mic × 两档模式实际值

| Mic | 室内（默认）| 室外 (+3) |
|-----|-----------|-----------|
| -26 dBV | input=**9**, ref=**6** | input=**12**, ref=**9** |
| -36 dBV | input=**18**, ref=**6** | input=**21**, ref=**9** |
| -42 dBV | input=**24**, ref=**6** | input=**27**, ref=**9** |

所有值均命中 ES7210 物理档位（0/3/6/9/12/15/18/21/24/27/30），无量化损失。

#### 模式持久化机制

- `audio/mic_type` (int, NVS) ← 校准时写入物料标识 (26/36/42)，永不变（除非重新校准）
- `audio/input_gain` (float, NVS) ← 当前生效值，模式切换时更新
- `audio/ref_gain` (float, NVS) ← 当前 REF 值，模式切换时更新
- 模式切换流程：MCP 读 `mic_type` 反推室外基线 → 算出新 input/ref → 写入 NVS
- 重启后从 NVS 读 `input_gain` / `ref_gain` → 自动保持上次模式

**关键设计原则**：
- 只有 `input` 跟随 mic 物料变化（模拟前级补偿灵敏度差）
- `ref` / `aec` / `volume` **三档统一**（不依赖 mic 类型）

### 各参数物理含义

| 参数 | 物理含义 | 调整影响 |
|------|---------|---------|
| `input` | ES7210 模拟前级增益 | 直接补偿 mic 灵敏度，每 +3 dB ≈ 拾音电平 ×1.4 |
| `ref` | ES7210 REF 通道增益（喇叭回采） | AEC 参考信号强度，仅与喇叭/PA 相关 |
| `aec` | AEC 后软件 PCM 增益 | 上行音量微调，不影响 SNR |
| `output_volume` | ES8311 喇叭输出音量 | 用户听到的回话响度 |

### 默认音量目标 = vol=80

```
vol=80  →  喇叭对话峰值 ~88 dB-C / 音乐峰值 100 dB-C
       →  用户耳边对话 ~70 dB SPL @ 1m  (自然对话音量)
       →  音乐场景有冲击力，对话不冲（人声动态范围小）
```

dB-C 声压计测的是峰值，**TTS 人声没有音乐那种瞬态冲击**，实际听感比标称值小一倍以上。
vol=70 对话偏弱，vol=80 是对话+音乐共享的最优平衡点。

---

## 四、自动校准机制

### 触发时机

| 触发 | 条件 | 时机 |
|------|------|------|
| 自动 | NVS `audio/mic_type == 0`（未校准）| `Application::Start()` 中 `audio_service.Start()` 之前 |
| 手动 | 远程命令 `{"type":"mic_calibrate"}` | 任意 idle 状态 |

### 校准流程（CalibrateMicOnce）

```cpp
void BoxAudioCodec::CalibrateMicOnce() {
    // 临时配置
    EnableInput(true) + EnableOutput(true)
    vol = 80 (校准固定，不受默认 vol=70 影响)
    input_gain = 12 dB (临时基准档位)

    // 生成 1kHz 500ms tone (amp=24000 ≈ -2.7 dBFS)
    // 异步播放 + 跳头 150ms + 同步录 200ms

    // 算 MIC1 通道 RMS
    int32_t rms = sqrt(sum_sq / N);

    // 三档判定 (基于实测 + 物理结构衰减)
    if      (rms >= 3000) gain = 15.0f;   // -26 dBV
    else if (rms >= 900)  gain = 24.0f;   // -36 dBV
    else                  gain = 30.0f;   // -42 dBV

    SetInputGain(gain);                    // 写 NVS
    Settings("audio", true).SetInt("mic_type", mic_type);   // 物料标识 + 校准标志（≠0=已校准）

    // 恢复 input/output 启用状态
}
```

### 校准 RMS 实测分布（基于 -26/-36/-42 三档）

| Mic 物料 | RMS 范围 | 中位 | 判定阈值 |
|---------|---------|------|---------|
| -26 dBV | 5000-9000 | ~7000 | ≥ 3000 |
| -36 dBV | 1200-2200 | ~1500 | 900 ~ 2999 |
| -42 dBV | 350-700 | ~500 | < 900 |

阈值取相邻两档的几何中位 √(RMS_a × RMS_b)，确保物理结构 ±10 dB 干扰也无法跨档误判。

---

## 五、远程调试命令

### 1. 触发重新校准

```json
{"type":"custom","payload":{"message":"{\"type\":\"mic_calibrate\"}"}}
```

执行后：
- 不停 audio_service（直接调用 CalibrateMicOnce）
- 设备发出 0.5s 1kHz 嘟声
- monitor 输出 `MIC校准 RMS=XXXX → input=YYdB (-XXdBV)`
- 自动写 NVS（input_gain + ref_gain + mic_type）
- 下次对话即用新值，无需重启

### 2. 强制覆盖某档配置

按 mic 类型直接发送对应配置（覆盖 NVS）：

```json
// -26 dBV mic
{"type":"custom","payload":{"message":"{\"type\":\"gain\",\"input\":15,\"ref\":6,\"aec\":6}"}}

// -36 dBV mic
{"type":"custom","payload":{"message":"{\"type\":\"gain\",\"input\":24,\"ref\":6,\"aec\":6}"}}

// -42 dBV mic
{"type":"custom","payload":{"message":"{\"type\":\"gain\",\"input\":30,\"ref\":6,\"aec\":6}"}}
```

### 3. 单独调整某项

```json
// 仅调 input（远场识别弱时升 +3）
{"type":"custom","payload":{"message":"{\"type\":\"gain\",\"input\":18}"}}

// 仅调 ref（AEC 残余回声时升）
{"type":"custom","payload":{"message":"{\"type\":\"gain\",\"ref\":9}"}}

// 仅调 aec（云端 ASR 觉得"声音小"时升）
{"type":"custom","payload":{"message":"{\"type\":\"gain\",\"aec\":9}"}}

// 调音量（用户嫌大/小）
{"type":"custom","payload":{"message":"{\"type\":\"volume\",\"value\":75}"}}
```

### 4. 查看当前生效值

monitor 输出：
```
I (xxxx) BoxAudioCodec: 增益配置: MIC=15.0dB(NVS) REF=6.0dB(NVS)  [缺省 15/6]
                                       ↑↑       ↑↑
                                       实际值   来源(NVS=持久化值, 默认=代码兜底)
```

---

## 六、室内场景调节建议

### 现象 → 调整对照表

| 现象 | 推断 | 调整 |
|------|------|------|
| 1m 远场说话需要重复多次唤醒 | input 不足 | input +3（如 24→27） |
| 近场说话有破音/失真 | input 过强 | input -3（如 24→21） |
| TTS 后开头瞬间漏回声 | REF 弱 | ref 6→9 |
| 云端 ASR 反馈"声音小" | aec 不足 | aec 6→9 |
| 静音时背景沙沙明显 | aec 放大底噪 | aec 6→3 |
| 用户觉得回话太冲 | 音量大 | volume 70→60 |
| 嘈杂环境听不清回话 | 音量小 | volume 70→80 |

### 室内典型场景配置

| 场景 | 距离 | 推荐配置（基于 -26 mic 默认）|
|------|------|---------------------------|
| 桌面对话 | 0.3-0.5 m | `input=15, ref=6, aec=3, vol=70` |
| **客厅自然对话**（基线）| 1 m | `input=15, ref=6, aec=6, vol=70` ✅ 默认 |
| 远场对话 | 1.5-2 m | `input=18, ref=6, aec=9, vol=80` |
| 嘈杂厨房 | 1 m | `input=15, ref=6, aec=9, vol=85` |

---

## 七、代码位置速查

| 内容 | 文件 | 行号 |
|------|------|------|
| 默认 input/ref/aec/volume | `main/audio/audio_codec.h` | 59-65 |
| 默认 kDefaultMicGain | `main/audio/codecs/box_audio_codec.cc` | 24-25 |
| 校准函数 CalibrateMicOnce | `main/audio/codecs/box_audio_codec.cc` | ~310 |
| 校准触发（首次开机） | `main/application.cc` | ~99 |
| 远程命令 OnGain / OnMicCalibrate | `main/remote_cmd.cc` | OnGain ~181 / OnMicCalibrate 末尾 |
| AEC 后软件增益处理 | `main/audio/audio_service.cc` | OnOutput lambda |

---

## 八、量产 SOP

### 出厂烧录流程

```bash
# 1. 烧录固件 + 擦 NVS（首次开机会自动校准）
idf.py -p /dev/cu.usbmodem2101 erase-flash
idf.py -p /dev/cu.usbmodem2101 -b 2000000 flash monitor

# 2. 设备上电 → 监听日志：
#    I (xxxx) Application: 首次开机 MIC 校准开始（约 1 秒...）
#    W (xxxx) BoxAudioCodec: MIC校准 RMS=XXXX → input=YYdB (-XXdBV)
#    I (xxxx) AudioCodec: MIC增益=YY.0dB

# 3. 检查判定结果：
#    -26 mic 板：RMS 应 5000-9000，input=15
#    -36 mic 板：RMS 应 1200-2200，input=24
#    -42 mic 板：RMS 应 350-700，input=30

# 4. 1m 距离对话验证：
#    - "小智小智" → 一次唤醒成功
#    - "今天天气怎么样" → ASR 准确识别
#    - 设备回应清晰 + 不爆破音

# 5. 通过 → 出货；不通过 → 流入维修
```

### 售后远程修复

用户反馈"听不清/听不到"时：
1. 云端发 `mic_calibrate` 重测
2. 看新 RMS 是否在合理区间
3. 异常 → 发对应 `gain` 命令强制覆盖
4. 仍异常 → 物料故障，召回
