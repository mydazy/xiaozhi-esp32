---
type: note
created: 2026-05-20
updated: 2026-05-20
tags: [固件审计, 音频驱动, Codec, MP3, P30, ESP32]
---

# 01 · 音频驱动层（Codec / MP3）审计报告

> 子系统范围：`main/audio/audio_codec.*`、`main/audio/codecs/`、
> `components/mydazy__codec_ctrl_i2c/`、`components/mydazy__esp_mp3_player/`。
> 关注点：I2S/I2C 时序、DMA 缓冲、采样率切换、codec 寄存器配置。
> 方法：三遍评审（广度遍历 → 红线深挖 → 反审自检）。

## 审计文件清单（本子系统在范围内的源文件 = 9 个）

| # | 文件 | 行数 | 说明 |
|---|------|------|------|
| 1 | main/audio/audio_codec.cc | 84 | 基类：音量/增益/使能、Start 读 NVS |
| 2 | main/audio/audio_codec.h | 66 | 基类接口 + DMA 宏（DESC=6 FRAME=240）|
| 3 | main/audio/codecs/box_audio_codec.cc | 327 | ES8311(DAC)+ES7210(ADC) 双工，I2S STD+TDM，MIC 校准 |
| 4 | main/audio/codecs/box_audio_codec.h | 45 | BoxAudioCodec 声明 |
| 5 | main/audio/codecs/no_audio_codec.cc | 386 | 裸 I2S（STD/PDM）软件音量 |
| 6 | main/audio/codecs/no_audio_codec.h | 42 | NoAudioCodec 系列声明 |
| 7 | components/mydazy__codec_ctrl_i2c/mydazy_codec_ctrl_i2c.c | 162 | esp_codec_dev I2C ctrl 转发到 i2c_bus_worker |
| 8 | components/mydazy__codec_ctrl_i2c/include/mydazy_codec_ctrl_i2c.h | 51 | 上面的接口头 |
| 9 | components/mydazy__esp_mp3_player/mp3_player.cc | 833 | 三段流水线流式 MP3（DL/Decode/Output）|

参考头：`components/mydazy__esp_mp3_player/include/mp3_player.h`、
`components/mydazy__i2c_bus_worker/include/i2c_bus_worker.h`（worker API 签名核对，未计入审计文件）。
二进制/数据文件无（仅 README/CHANGELOG/CHECKSUMS，未逐行读）。

---

# 第一遍 · 广度遍历（显性缺陷）

### 01-P0-A · BoxAudioCodec::Read 失败/未使能时返回未初始化缓冲，且谎报采集成功
- 严重等级：**P0**。判级理由：麦克风未使能或 I2C/I2S 读失败时，`dest` 是未清零的栈/堆垃圾，但函数返回 `samples`（声称读满）。上游 `AudioCodec::InputData` 见 `samples>0` 即 `return true`，整段垃圾被当作有效 PCM 喂给 AFE/唤醒词/上行编码——量产整批机器在任何"开麦但 codec 异常"瞬间会把内存垃圾当语音上传，且可掩盖真实的 codec 故障使其无法被发现。
- 文件：`main/audio/codecs/box_audio_codec.cc:256-262`
- 问题代码：
```cpp
int BoxAudioCodec::Read(int16_t* dest, int samples) {
    std::lock_guard<std::mutex> lock(input_dev_mutex_);
    if (input_enabled_) {
        ESP_ERROR_CHECK_WITHOUT_ABORT(esp_codec_dev_read(input_dev_, (void*)dest, samples * sizeof(int16_t)));
    }
    return samples;          // 未读也返回 samples；读失败也返回 samples
}
```
- 根因：返回值固定为入参 `samples`，与 `esp_codec_dev_read` 的真实结果脱钩；`!input_enabled_` 分支不清零 `dest`。
- 触发条件/影响面：所有 BoxAudioCodec 板型（P30-WiFi/P30-4G）。开麦窗口内 codec I2C 抖动（4G 共线场景概率高）即触发；整批。
- 修复建议：
  - `!input_enabled_` 分支：`memset(dest, 0, samples*sizeof(int16_t)); return samples;`（静音占位）或 `return 0;`。
  - 读失败分支：捕获 `esp_codec_dev_read` 返回值，失败时 `memset` 清零并 `return 0;`（让 InputData 返回 false）。
- [发现于第一遍]

### 01-P1-B · 构造函数大量 assert，Release(NDEBUG) 下编译消失 → 空指针解引用
- 严重等级：**P1**。判级理由：`assert()` 在定义 `NDEBUG` 的发布构建中被预处理掉，所有 `data_if_`/`out_ctrl_if_`/`out_codec_if_`/`output_dev_`/`input_dev_` 等返回 NULL 的失败将不被拦截，后续 `EnableOutput/Read/Write` 直接对 NULL 句柄操作崩溃。codec 创建失败（I2C ACK 失败、内存不足）在量产线上并非罕见。
- 文件：`main/audio/codecs/box_audio_codec.cc:34,42,45,56,64,72,78,83`
- 问题代码：
```cpp
data_if_ = audio_codec_new_i2s_data(&i2s_cfg);
assert(data_if_ != NULL);     // NDEBUG 下消失
...
output_dev_ = esp_codec_dev_new(&dev_cfg);
assert(output_dev_ != NULL);
```
- 根因：用 `assert` 做生产期错误处理。
- 触发条件/影响面：发布固件（量产固件通常带 NDEBUG）+ codec 初始化失败。整批的小概率个例，但崩在初始化路径影响开机。
- 修复建议：改为运行期判空并 `ESP_LOGE` + 安全降级（例如把 `audio_codec_` 置空、上层走 NoAudio 兜底），不要用 assert；至少改 `ESP_ERROR_CHECK`/`configASSERT` 等运行期生效的宏并明确策略。
- [发现于第一遍]

### 01-P1-C · 析构函数中用 ESP_ERROR_CHECK 包裹 close，关闭失败直接 abort
- 严重等级：**P1**。判级理由：析构里 `ESP_ERROR_CHECK(esp_codec_dev_close(...))`，若设备已处于异常态（如 I2C 掉线）close 返回非 OK 会触发 `abort()`，把"清理资源"变成"崩溃"。析构通常发生在切板/重建 codec 路径。
- 文件：`main/audio/codecs/box_audio_codec.cc:89,91`
- 问题代码：
```cpp
BoxAudioCodec::~BoxAudioCodec() {
    ESP_ERROR_CHECK(esp_codec_dev_close(output_dev_));   // 关闭失败=abort
    esp_codec_dev_delete(output_dev_);
    ESP_ERROR_CHECK(esp_codec_dev_close(input_dev_));
```
- 根因：析构路径不应 abort。
- 触发条件/影响面：codec 异常时重建/销毁。偶发但后果是崩溃。
- 修复建议：析构内改 `ESP_ERROR_CHECK_WITHOUT_ABORT` 或仅 `ESP_LOGW` 记录，继续后续 delete。
- [发现于第一遍]

### 01-P1-D · NoAudioCodec::Write 使用裸 portMAX_DELAY 阻塞 + ESP_ERROR_CHECK abort
- 严重等级：**P1**。判级理由：违反项目 FreeRTOS 红线（禁止裸 `portMAX_DELAY`）。`i2s_channel_write` 用 `portMAX_DELAY` 永久阻塞——若 I2S 因时钟/PA 状态卡死，输出任务（P10）将永久挂起，看门狗/喂狗链断裂；同时 `ESP_ERROR_CHECK` 在写失败时 abort。
- 文件：`main/audio/codecs/no_audio_codec.cc:236`
- 问题代码：
```cpp
ESP_ERROR_CHECK(i2s_channel_write(tx_handle_, buffer.data(),
        samples * sizeof(int32_t), &bytes_written, portMAX_DELAY));
```
- 根因：无超时 + 写失败 abort。
- 触发条件/影响面：使用 NoAudioCodec 的板型（PDM/裸 I2S 机型）；I2S DMA 卡死时整机假死。
- 修复建议：`portMAX_DELAY` → `pdMS_TO_TICKS(100)`（实时路径 100ms 上限，与 Read 的 200ms 对齐），返回值改 `ESP_ERROR_CHECK_WITHOUT_ABORT` 并按 `bytes_written` 真实返回。
- [发现于第一遍]

### 01-P2-E · NoAudioCodecSimplexPdm::Read 使用裸 portMAX_DELAY
- 严重等级：**P2**。判级理由：同样违反超时红线，但仅 PDM 简单板型，且 Read 失败仅丢一帧。
- 文件：`main/audio/codecs/no_audio_codec.cc:371`
- 问题代码：
```cpp
if (i2s_channel_read(rx_handle_, dest, samples * sizeof(int16_t),
        &bytes_read, portMAX_DELAY) != ESP_OK) {
```
- 根因：无超时（注意：基类 `NoAudioCodec::Read` 已用 200ms，此子类重写却退回 portMAX_DELAY）。
- 触发条件/影响面：PDM 机型；麦克风时钟异常时输入任务永久阻塞。
- 修复建议：`portMAX_DELAY` → `pdMS_TO_TICKS(200)`，与基类一致。
- [发现于第一遍]

### 01-P3-F · CreateDuplexChannels 全程 ESP_ERROR_CHECK，I2S 配置失败开机即 abort
- 严重等级：**P3**。判级理由：I2S 通道创建/使能若失败会 abort。这类失败基本是固定的配置错误（开发期暴露），量产固件配置已冻结后稳定，故列潜在；但属硬红线相邻（DMA/时序），保留记录。
- 文件：`main/audio/codecs/box_audio_codec.cc:114,185-188`；`no_audio_codec.cc:32,72-73,93,133,…`
- 根因：初始化阶段 abort 策略。
- 触发条件/影响面：配置错误/资源耗尽。冻结后偶发。
- 修复建议：保留 abort 可接受（fail-fast），但建议出货固件统一封装为带日志的 fatal handler，便于现场归因，而非裸 abort。
- [发现于第一遍]

---

# 第二遍 · 红线深挖（电源/内存/并发/OTA + 跨文件数据流）

### 01-P0-G · CalibrateMicOnce 中 `rms_expected` 可为 0，diff 计算除零（UB/崩溃）
- 严重等级：**P0**。判级理由：除零是必崩级。校准第二轮用
  `diff = 100 * |rms_after - rms_expected| / rms_expected`，分母 `rms_expected` 由
  `rms * pow(10,(input_gain-15)/20)` 取整得来；当首轮 `rms` 很小（安静环境/麦克风未起振，rms 被钳到 1）且 `input_gain` 被钳到 0 时，`rms_expected` 可取整为 0 → 整数除零（ESP32 触发异常复位）。该函数是**出厂烧录后首次开机一次性**执行，安静装配线环境恰好高概率命中。
- 文件：`main/audio/codecs/box_audio_codec.cc:314,324`
- 问题代码：
```cpp
int32_t rms_expected = (int32_t)(rms * std::pow(10.0, (input_gain - 15.0) / 20.0));
...
int diff = (int)(100 * std::abs((double)(rms_after - rms_expected) / rms_expected));  // 除零
```
- 根因：`rms_expected` 未做下限保护即作除数。
- 触发条件/影响面：所有 Box 板型出厂首开校准；安静/麦克风异常环境即触发；整批装配线风险。
- 修复建议：`int32_t rms_expected = std::max((int32_t)(...), (int32_t)1);` 与 `rms` 同样钳到 ≥1。
- [发现于第二遍]

### 01-P1-H · CalibrateMicOnce 起 calib 子任务 Write，与主线程 Read 抢同一 codec，且校准全程不响应 abort
- 严重等级：**P1**。判级理由：`measure()` 用 `xTaskCreate("calib")` 在另一任务里 `Write`，主线程 150ms 后 `Read`；`Write` 持 `output_dev_mutex_`、`Read` 持 `input_dev_mutex_`，锁不冲突，但二者经由同一 `i2c_bus_worker` 串行化访问同一 I2S/codec——`esp_codec_dev_write`(8000 样本约 0.5s) 与 `Read` 并发挤兑 worker 队列，叠加上层 audio_service 的 Input/Output 任务也在跑，热路径竞争导致校准 RMS 抖动、偶发 worker 超时。且整个校准过程（含 `vTaskDelay` 累计数秒）不检查 `abort_`/关机信号，关机/打断时无法中断。
- 文件：`main/audio/codecs/box_audio_codec.cc:287-305`
- 问题代码：
```cpp
xTaskCreate([](void* a) {
    auto* c = (PlayCtx*)a;
    c->self->Write(c->tone->data(), c->tone->size());   // 与主线程 Read 并发挤 worker
    c->done.store(true);
    vTaskDelete(NULL);
}, "calib", 4096, &pctx, 5, NULL);
vTaskDelay(pdMS_TO_TICKS(150));
...
Read(rec.data(), rec.size());
```
- 根因：校准期未独占 codec / 未停 audio_service 常规采集播放；流程不可中断。
- 触发条件/影响面：出厂首开校准，且常规 audio 任务已在运行时。偶发校准失准（影响出厂 MIC 增益参数写入精度，连带影响整批音质一致性）。
- 修复建议：校准前暂停 audio_service 的 input/output 流水（独占 codec），或用 `i2c_worker_lock_session` 包住校准序列；循环里加入 `abort` 检查点。
- [发现于第二遍]

### 01-P2-I · `_read_reg` 的 `addr_len` 未做边界校验，`addr_data[2]` 可被越界传读长度
- 严重等级：**P2**。判级理由：内存安全红线相邻。`addr_data` 为固定 2 字节，但 `i2c_worker_write_read(..., addr_data, addr_len, ...)` 直接把 caller 传入的 `addr_len` 当写长度；若 `addr_len > 2`，worker 会从 `addr_data` 读取越界字节发到 I2C 总线（信息泄漏 + 写错寄存器地址）。当前 `addr_len` 仅由上游 esp_codec_dev 内部以 1/2 传入，外部不可控，故判 P2；但作为兼容上游的"输入未先校验边界再用"违反红线。
- 文件：`components/mydazy__codec_ctrl_i2c/mydazy_codec_ctrl_i2c.c:73-82`
- 问题代码：
```cpp
uint8_t addr_data[2] = {0};
if (addr_len > 1) { addr_data[0]=...; addr_data[1]=...; } else { addr_data[0]=...; }
esp_err_t ret = i2c_worker_write_read(self->dev, addr_data, addr_len, ...);  // addr_len 未夹取
```
- 根因：使用前未对 `addr_len` 做 `<=2` 校验。
- 触发条件/影响面：仅当上游或未来调用方传 addr_len>2；当前个例。
- 修复建议：函数入口加 `if (addr_len < 1 || addr_len > 2) return ESP_CODEC_DEV_INVALID_ARG;`（`_write_reg` 已有 len>4 拦截，但同样建议显式夹 addr_len）。
- [发现于第二遍]

### 01-P2-J · DecodeLoop 立体声折单声道未校验奇偶；resampler 的 max-out 查询返回值未检查
- 严重等级：**P2**。判级理由：内存安全。`src_channel==2 && dst_channel==1` 时按 `pcm[2*i]/pcm[2*i+1]` 折半，假设 `sample_count` 为偶数；若解码器某帧吐出奇数样本数（损坏 MP3 / VBR 边界帧），`mono.size()=sample_count/2`，最后一个 `pcm[2*i+1]` 仍在范围内（因 mono.size 向下取整），不越界——但会丢半样本相位错乱，属偶发音质问题。更需注意：`esp_ae_rate_cvt_get_max_out_sample_num` 返回值未检查，失败时 `max_out` 为未初始化栈值 → `std::vector<int16_t> out_pcm(max_out)` 可能巨量分配（OOM）或 0 长。
- 文件：`components/mydazy__esp_mp3_player/mp3_player.cc:611-618,635-637`
- 问题代码：
```cpp
uint32_t max_out = 0;
esp_ae_rate_cvt_get_max_out_sample_num(resampler, pcm.size(), &max_out);  // ret 未检查
std::vector<int16_t> out_pcm(max_out);
```
- 根因：外部库返回值未检查即用其输出做分配尺寸。
- 触发条件/影响面：仅当 resampler 查询失败（罕见），但后果是 OOM/异常。所有走重采样的流式播放（源采样率≠sink）。
- 修复建议：检查 `get_max_out_sample_num` 返回值，非 OK 则跳过本帧；`max_out` 初值已为 0，可加 `if (max_out == 0) continue;` 兜底。
- [发现于第二遍]

### 01-P3-K · NoAudioCodec::Write 每帧在热路径 new 一个 std::vector<int32_t>
- 严重等级：**P3**。判级理由：性能/碎片化技术债。输出任务每帧 `std::vector<int32_t> buffer(samples)` 堆分配再释放，高频音频路径上造成堆抖动/碎片，长期可致内部 RAM 碎片化（项目内部 RAM >60KB 红线相关）。非崩溃，列潜在。
- 文件：`main/audio/codecs/no_audio_codec.cc:219`
- 根因：热路径未复用缓冲。
- 触发条件/影响面：NoAudio 机型持续播放。远期。
- 修复建议：用成员级预分配缓冲（受 data_if_mutex_ 保护）复用，避免每帧 malloc/free。
- [发现于第二遍]

### 01-P3-L · mp3 `kProactiveReconnectBytes` 常量(5MB)与注释/设计描述(2MB)不一致
- 严重等级：**P3**。判级理由：非功能缺陷，是文档/常量漂移，易误导后续维护者调参（影响 4G NAT 老化策略）。
- 文件：`components/mydazy__esp_mp3_player/mp3_player.h:173`（=5MB）vs `mp3_player.cc:391-393` 注释"每 2MB"。
- 根因：改常量未同步注释。
- 修复建议：统一注释为 5MB，或按设计意图改回常量；二选一并对齐。
- [发现于第二遍]

---

# 第三遍 · 反审自检（复验行号/判级 + 对抗视角反查漏报）

复验结论（对前两遍逐条核对行号与代码片段，真实无误）：
- 01-P0-A：行号 256-262 经二次 Read 确认无误。判级维持 P0（喂垃圾给上行链路 + 掩盖故障，双重危害）。
- 01-P0-G：行 314/324 确认；`rms` 在 307 已 `std::max(...,1)`，但 `rms_expected` 未保护，除零判级成立，维持 P0。
- 01-P1-B/C/D：abort/portMAX_DELAY 片段确认。NoAudioCodec::Read（基类）已用 200ms 超时，仅 Write(236) 与 PDM 子类 Read(371) 是 portMAX_DELAY——D 维持 P1（输出热路径 P10 永久阻塞断喂狗），E 压为 P2（PDM 输入丢帧）。
- 01-P2-I/J：维持，未拔高（当前外部不可控触发）。

对抗视角反查（在最高风险文件 box_audio_codec.cc / mp3_player.cc 用"如何让它崩"反查）：

### 01-P1-M · [新] 采样率切换无运行期再配置：input/output 采样率假定恒等且全程不变
- 严重等级：**P1**。判级理由：关注点"采样率切换"。`CreateDuplexChannels` 入口 `assert(input_sample_rate_ == output_sample_rate_)`（NDEBUG 下消失），且 I2S/codec 的采样率在构造时一次性写死，`EnableInput/EnableOutput` 用 `output_sample_rate_` 作为 fs。系统无任何运行期切换采样率的安全路径——一旦上层（如 MP3 16k vs 唤醒 16k 不同源）期望异采样率，要么 assert 被吃掉后用错配置静默跑偏，要么不支持。MP3 路径靠软件 resampler 规避（DecodeLoop 重采样到 sink 率），故未崩；但若未来新增异采样率源，缺乏防护会静默出错。
- 文件：`main/audio/codecs/box_audio_codec.cc:103`；`EnableInput` fs.sample_rate 用 output_sample_rate_ (220)。
- 根因：采样率契约靠 assert 表达，无运行期校验/再配置机制。
- 触发条件/影响面：异采样率需求出现时；当前设计回避，列 P1 提醒红线。
- 修复建议：把 `assert` 改运行期 `if (in!=out){ESP_LOGE;…}` 并在 EnableInput 显式用 `input_sample_rate_`；新增采样率源前补运行期 reconfigure 路径。
- [发现于第三遍]

### 01-P2-N · [新] BoxAudioCodec::EnableInput 打开失败用 ESP_ERROR_CHECK abort（开麦即崩）
- 严重等级：**P2**。判级理由：`esp_codec_dev_open(input_dev_,&fs)` 与 `set_in_channel_gain` 用 `ESP_ERROR_CHECK`，I2C/codec 异常时开麦直接 abort 复位。属偶发（依赖 codec 异常），但触发点是高频动作（每次进入对话开麦）。
- 文件：`main/audio/codecs/box_audio_codec.cc:226-227,229`（EnableOutput 248-251 同形）
- 问题代码：
```cpp
ESP_ERROR_CHECK(esp_codec_dev_open(input_dev_, &fs));
ESP_ERROR_CHECK(esp_codec_dev_set_in_channel_gain(input_dev_, ...));
```
- 根因：高频使能路径上 abort 而非降级。
- 触发条件/影响面：codec/I2C 异常 + 开麦/开扬声器。偶发但用户高频触发。
- 修复建议：改 `ESP_ERROR_CHECK_WITHOUT_ABORT`，失败时不置 `input_enabled_=true`、上报错误事件、保持静音降级。
- [发现于第三遍]

### 01-P2-O · [新] mp3 DownloadLoop 内层 ring 满重试可在 abort 时空转，Range 头 %u 截断风险
- 严重等级：**P2**。判级理由：(1) `xRingbufferSend` 失败（ring 满）时 `// else: retry next iteration`，内层 while 条件含 `!abort_`，abort 能跳出，无死锁——复核后此点不算缺陷。(2) Range 头用 `snprintf("bytes=%u-", (unsigned)total_read)`，`total_read` 为 `size_t`，>4GB 文件会截断——音频文件不可能超 4GB，实际不触发。综合保留为 P2 记录：续传 offset 用 `%u` 强转 unsigned，文件 >4GB 理论失配。
- 文件：`components/mydazy__esp_mp3_player/mp3_player.cc:310-311,419-425`
- 根因：偏移量打印用 32-bit 格式符。
- 触发条件/影响面：理论极大文件；实际音频不触发。
- 修复建议：用 `%llu` + `(unsigned long long)`，或保持现状并注释"音频 <4GB 假设"。判 P2 偏保守，可降 P3。
- [发现于第三遍]

### 复核删除的疑似项（避免误报）
- mp3 三 task 生命周期：`active_tasks_` 用 atomic acq_rel 计数，`AbortAndJoin` 在计数归零前不释放 ring（注释明确），未发现 UAF——**非缺陷**。
- mp3 `current_url_`/`current_title_` char[]：仅 Play 写、task 启动后只读，靠 `running_` release/acquire 建立 happens-before——逻辑成立，**非缺陷**。
- `CalibrateMicOnce` 的 `pctx`/`tone` 栈生命周期：`measure()` 返回前已 `while(!done) vTaskDelay` 等子任务结束，子任务访问的 `tone` 在外层 CalibrateMicOnce 作用域存活——**非悬垂，非缺陷**（但并发挤兑见 01-P1-H）。

---

# 统计

| 等级 | 数量 |
|------|------|
| P0 | 2 |
| P1 | 4 |
| P2 | 5 |
| P3 | 3 |
| **合计** | **14** |

三遍新增分布：
- 第一遍（广度）：6 个（01-P0-A、01-P1-B、01-P1-C、01-P1-D、01-P2-E、01-P3-F）
- 第二遍（红线）：6 个（01-P0-G、01-P1-H、01-P2-I、01-P2-J、01-P3-K、01-P3-L）
- 第三遍（反审，新增漏报）：3 个（01-P1-M、01-P2-N、01-P2-O）；并删除 3 个误报、复验前两遍全部成立。
- 合计 6 + 6 + 3 = **14**。

Top P0（出货前必须清零）：
1. **01-P0-A** `box_audio_codec.cc:256-262`：麦克风未使能/读失败时返回未初始化垃圾且谎报成功，垃圾被当语音上行 + 掩盖 codec 故障。
2. **01-P0-G** `box_audio_codec.cc:314,324`：出厂首开 MIC 校准中 `rms_expected` 可为 0 导致整数除零复位，安静装配线高概率命中。
