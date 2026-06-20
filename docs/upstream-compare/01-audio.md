# 模块 01 · audio 音频链路（AEC / AGC / PCM）

> 对比基线：`v2.2.4`（官方 78/xiaozhi-esp32）。模式：A 对比 + 🔶深审。
> 范围：`main/audio/audio_service.*`、`processors/afe_audio_processor.*`、`audio_codec.*`、`codecs/box_audio_codec.*`、`codecs/no_audio_codec.cc`。
> 业务文件 `alarm_ringer`/`music_player` 在 audio/ 目录下但属业务模块，归后续模块。
> diff 规模：audio 核心累计 **+418 / −86**（9 文件）。

---

## 一、改动全景（逐处取证）

### A. 并发 / 内存安全类（修官方裸奔，红线）

| # | 改了什么 | 官方 v2.2.4 原实现 | 为什么 | 判定 |
|---|---------|------------------|--------|------|
| 1 | **afe 析构优雅退出**：`PROCESSOR_EXIT` 事件位 + `task_done_sem_` 信号量；析构先置位等任务退出（2s 超时）再 `destroy(afe_data_)` | 析构直接 `destroy(afe_data_)`，不管 `AudioProcessorTask` 是否还在 `fetch_with_delay(portMAX_DELAY)` 中用它 | 修 **use-after-free**：任务还在用 afe_data_ 就被销毁 → 崩溃 | 🟢 内存/并发红线（**未提交新修**）|
| 2 | `fetch_with_delay` 超时 `portMAX_DELAY → 100ms` | `portMAX_DELAY` 永久阻塞 | 让任务能周期检查 EXIT 位，否则永远退不出 | 🟢 配套 #1 |
| 3 | `service_stopped_` 改 `std::atomic<bool>` | 裸 `bool` | 多任务读写防 data race | 🟢 并发 |
| 4 | 新增 `encoder_mutex_` 保护 opus_encoder | 无（官方不支持运行时改帧长） | 帧长切换重建 encoder 与编码任务并发 | 🟢 并发（配套帧长切换）|
| 5 | `Stop()` 加 `join_task`：轮询 `eTaskGetState==eDeleted`，500ms 超时打日志 | 只清队列，不等任务退出 | 防 Stop 后任务残留访问已释放资源（音乐/AEC 切换时 Stop+重建）| 🟢 并发 |
| 6 | `SetDecodeSampleRate` 删除过早的 `decoder_lock.unlock()`，持锁到函数尾 | 重开 decoder 前就 `unlock()` | 重开期间锁已释放，他任务可能用到半初始化 decoder | 🟢 并发修复 |
| 7 | box codec：`Read`/`Write` 加锁；`data_if_mutex_` 拆成 `input_dev_mutex_`/`output_dev_mutex_` | **Read/Write 无锁**，Enable 共一把锁 | codec dev 多任务访问；拆两锁让 Read/Write 可并行 | 🟢 并发（官方裸奔）|
| 8 | opus_codec 任务栈改 `xTaskCreatePinnedToCoreWithCaps(..., MALLOC_CAP_SPIRAM)` | `xTaskCreate`（栈在内部 RAM）| 24KB 栈放 PSRAM，省内部 RAM（内部 RAM 60KB 红线）| 🟢 内存红线 |
| 9 | `PushTaskToEncodeQueue`：满队列 `wait` → `wait_for(200ms)` 超时丢帧 | 无限 `wait` 阻塞 | 编码跟不上时丢帧而非阻塞 audio_input 任务 → 防卡死 | 🟢 防任务卡死 |
| 10 | `PlaySound` 加 `codec_==nullptr` 守卫 | 直接解引用 codec_ | 防 Initialize 前调用崩溃 | 🟢 防崩 |

### B. 弱网 / 掉帧鲁棒类

| # | 改了什么 | 官方原实现 | 判定 |
|---|---------|-----------|------|
| 11 | 解码失败用 `ESP_AUDIO_DEC_RECOVERY_PLC` 重试一次 | 失败直接 `ESP_LOGE` 丢弃 | 🟢 掉帧补偿（标准做法）|
| 12 | 队列加深：encode 2→4 / playback 2→8 / decode (2400→3600)/frame | 浅队列 | 🟢 jitter buffer 被动缓冲（低成本有效）|
| 13 | **underrun 主动 PLC 补偿**（~60 行）：playback 快空时主动合成 PLC 帧填充，超 3 帧转 fade-out 静音 | 无 | 🔴 **见第二节**（治标 + 堆复杂度）|

### C. 校准 / 调音类（自研 MIC 校准体系）

| # | 改了什么 | 官方原实现 | 为什么 | 判定 |
|---|---------|-----------|--------|------|
| 14 | **`CalibrateMicOnce()`**（~60 行）：出厂首开播 1kHz tone→录音测 RMS→反推 input_gain/mic_type/aec_gain→写 NVS→两轮验偏差 | 无校准，`input_gain_=30` 写死 | 补偿 MIC 个体灵敏度差异（批次间差 10dB+ 显著影响 ASR），量产识别一致性 | 🟢 量产一致性（红线配套）⚠️ magic number 密集 |
| 15 | **AEC 软件增益放大**（OnOutput 回调）：AFE 输出后按 `aec_gain_linear`(默认×2) 放大，带 RMS 噪声门(`kNoiseGateRmsSq`) | 无（AFE 输出直接入编码队列）| #14 校准定的 aec_gain 的运行时执行端；AEC 后处理衰减信号，补一道增益提识别率 | 🟢 校准体系运行端 ⚠️ 可 A/B 验证 AFE 自带 AGC 能否替代 |
| 16 | `SetInputGain`/`SetAecGain` 持久化 NVS + 运行时实时下发硬件 | SetInputGain 不持久化、不下发 | 配套校准（存校准结果，6 连击重校）| 🟢 校准配套 |
| 17 | 默认值：output_volume 70→80、input_gain 0→15dB、aec_gain 9dB | 官方默认 | 校准基线值 | 🟢 调参基线 |
| 18 | `SetOutputVolumeTransient`：闹钟响铃临时音量只下发硬件不写 NVS | 无 | 防响铃中途断电/重启后 NVS 残留临时大音量 | 🟢 防 NVS 残留 |
| 19 | box codec `SetOutputVolume` **音量 ×0.95 折扣**再下发 | 直接下发 volume | 无注释说明 | 🔴 **见第二节**（magic number 收益不明）|

### D. 功能 / 扩展类

| # | 改了什么 | 官方原实现 | 判定 |
|---|---------|-----------|------|
| 20 | `SetFrameDuration` 运行时切 OPUS 帧长（百度 20ms / 其它 60ms）：重建 encoder + 重启 processor；`OPUS_FRAME_DURATION_MS` 宏改全局变量 `g_opus_frame_duration_ms` | 编译期固定 60ms | ⚪ 多协议适配配套（百度协议 20ms 帧）|
| 21 | wake_word 选择读 NVS `wakeword.mode`（custom/afe 运行时选）| 按模型存在性自动选 | ⚪ 自定义唤醒词配套（详见模块 02）|
| 22 | codec I2C 走 `mydazy_codec_new_i2c_ctrl`（worker 串行化）替代原生 `audio_codec_new_i2c_ctrl` | 原生直连 I2C | 🟢 4G 共线 I2C 串行化（详见模块 09）|
| 23 | 删 `esp_wake_word`（非 S3 分支）、非 S3/P4 直接 `wake_word_=nullptr` | 官方多目标支持 | 🟢 正常裁剪（只跑 S3）|

### E. 任务调度专项（绑核 + 优先级）

| 任务 | 官方 v2.2.4 | 我们 | 说明 |
|------|-----------|------|------|
| audio_input | prio 8, 不绑核 | prio **10**, pin **core1** | 音频实时性 |
| audio_output | prio 4, 不绑核 | prio **10**, pin **core0** | |
| opus_codec | prio 2, 不绑核, 栈内部RAM | prio **7**, pin **core1**, 栈 **PSRAM** | 栈省内部RAM(🟢) |
| afe processor | prio 3, 不绑核 | prio **7**, pin **core1** | |

**判定**：绑核分离（core0 网络/协议、core1 音频）服务 4G 共存场景音频实时性 + opus 栈 PSRAM 省内存 = 🟢。**但 audio_input/output 优先级提到 10 偏高**（接近系统任务），4G/LVGL 任务理论上可能被抢占饥饿。倾向 🟢（实时性是真实需求），**标注：建议实测高负载下 LVGL/4G 任务是否饥饿**。

---

## 二、🔴 过度优化候选（待用户确认）

### 🔴-01-A · underrun 主动 PLC 补偿（~60 行）

- **位置**：`audio_service.cc` OpusCodecTask 内 `else if (need_underrun_compensation() ...)` 整段 + `kMaxUnderrunPlcFrames/kPlaybackLowWatermark/kRecentDecodePushWindowMs` 常量 + `consecutive_underrun_plc_`/`last_plc_tail_sample_` 状态。
- **我们怎么改的**：TTS 播放中播放队列瞬时欠载（且最近 200ms 收到过解码包）时，主动用 opus PLC 合成假帧填充播放队列，连续 3 帧后转线性 fade-out 静音。
- **官方原实现**：无。队列空就空，由播放端静音兜底。
- **维护成本/风险**：60 行复杂逻辑（PLC 合成 + 尾样本 fade + 重采样 + 多计数器）。属**治标**——弱网断续真因疑在内存争抢 / D1 保活唤醒双喂（见 memory `realtime-aec-weaknet-ram`），主动造帧只缓解"播放欠载"这一表象。**不碰红线**（不影响内存/过放/脏帧/并发）。
- **处置建议**：保留被动队列加深(#12)即可覆盖多数 jitter；主动 PLC 段建议 **A/B 实测**（弱网现场关掉对比断续率），收益不显著则回退。

### 🔴-01-B · box codec SetOutputVolume ×0.95 折扣

- **位置**：`box_audio_codec.cc` `SetOutputVolume`：`int set_volume = (int)(volume * 0.95)`。
- **我们怎么改的**：所有音量下发硬件前打 0.95 折扣。
- **官方原实现**：`esp_codec_dev_set_out_vol(output_dev_, volume)` 直接下发。
- **维护成本/风险**：无注释的 magic number，语义不明（防削顶？PA 保护？）。导致"设 100 实际 95"，用户/上层无感知，排查音量问题时易困惑。成本低但留坑。
- **处置建议**：要么补注释说明依据（如 PA 额定上限），要么回退直接下发。**需硬件确认是否有 PA 保护诉求**（属电源/硬件域，确认前不动）。

---

## 三、结论

模块 01 audio 改动 **绝大多数是 🟢 必要**：修了一批官方裸奔的并发/内存问题（afe UAF、codec Read/Write 无锁、opus 栈占内部 RAM、service_stopped data race、过早 unlock），加了防卡死背压和弱网掉帧补偿，以及一套服务量产识别一致性的 MIC 自校准体系。这些都对应本项目量产红线（内存安全 / 并发 / 弱网）。

**🔴 过度候选仅 2 项**，且均**不碰红线**、维护成本低：
- 🔴-01-A underrun 主动 PLC 补偿（治标 + 60 行复杂度，需 A/B 实测）
- 🔴-01-B SetOutputVolume ×0.95（magic number 收益不明，需硬件确认）

**调音提示**（非🔴，但标注）：AEC 软件增益(#15) 虽属校准体系，仍建议 A/B 验证 AFE 自带 AGC 能否替代，以削减一道软件放大。

> 本阶段只分析不改代码。🔴 候选已登记到 PROGRESS.md 汇总清单。
