# 02 音频处理链审计（AFE / 唤醒 / 解码 / 服务）

> 子系统范围：`main/audio/processors/`、`main/audio/wake_words/`、`main/audio/demuxer/`、
> `main/audio/audio_service.*`、`main/audio/music_player.*`、`main/audio/alarm_ringer.*`、
> `components/espressif__esp-sr/`（仅调用/封装层）。
> 关注点：唤醒回调并发、解码越界、环形缓冲、任务优先级、看门狗。
> 审计日期：2026-05-20。三遍法（广度 → 红线深挖 → 反审自检）。

## 覆盖文件清单（共 27 个 main/audio 文件 + esp-sr 封装层头）

实际精读：
- audio_service.cc / audio_service.h
- processors/afe_audio_processor.cc / .h、processors/no_audio_processor.cc、processors/audio_debugger.cc
- wake_words/afe_wake_word.cc / .h、wake_words/custom_wake_word.cc / .h
- demuxer/ogg_demuxer.cc / .h
- music_player.cc、alarm_ringer.cc
- components/espressif__esp-sr/include/esp32s3/esp_afe_sr_iface.h（afe_fetch_result_t 结构、fetch 签名）
- 跨文件：main/application.cc（唤醒回调链 / EncodeWakeWord / PopWakeWordPacket）、
  main/boards/common/power_save_timer.cc（EnableWakeWordDetection 跨任务调用）

参考红线：`~/.claude/rules/esp32-memory.md` 优先级表
`P12=audio_input · P10=audio_output · P8=AFE · P5=LVGL · P3=main_loop`。
**注意：当前代码实际优先级与此表严重不符（见 02-P1-A）。**

---

# 第一遍 · 广度遍历（显性缺陷）

### 02-P0-A · OggDemuxer 跨段拷贝可越界写 packet_buf（栈/对象内 8KB 缓冲溢出）
- **等级 P0**：外部音频资产（OGG 提示音 / TTS）解析时裸 memcpy，溢出可砖机或被构造数据攻击。命中红线②内存安全。
- **文件**：`main/audio/demuxer/ogg_demuxer.cc:210-222`
- **代码**：
```cpp
if (ctx_.packet_len + seg_len > sizeof(ctx_.packet_buf)) {   // 仅在"段开始"检查一次
    ESP_LOGE(...); state_ = ParseState::FIND_PAGE; ... return processed;
}
size_t to_copy = std::min(size - processed, (size_t)seg_len);
memcpy(ctx_.packet_buf + ctx_.packet_len, data + processed, to_copy);  // 跨页续传不复检
```
- **根因**：溢出检查只在进入某段时用 `seg_len` 比一次。但段可跨数据块（`seg_remaining` 续传），第二次进入此段时 `seg_len = ctx_.seg_remaining` 仍是剩余量，检查仍用单段量比对。真正风险在"包跨页累积"：`packet_continued=true` 时 `packet_len` 不清零（295-297 行），下一页继续往同一 `packet_buf` 累加。多页连续 255 段（lacing 值 255 表示续包）累计可远超 8192。虽每段进入时检查 `packet_len + seg_len`，但若 `seg_len`=255、`packet_len`=8000，检查会拦截并 `return`——**但 state 已被改成 FIND_PAGE 且 packet_len 未在所有路径清零**：此处清了，问题不在此条；真正越界在 `seg_remaining` 续传分支：进入时 `ctx_.seg_remaining > 0` 走 204 行 `seg_len = ctx_.seg_remaining`，**此时已跳过 210 行的 `packet_len+seg_len` 检查前的赋值，但检查仍执行**。复核后确认检查覆盖续传，故纯单包不溢出。**但 head/tags 未出现时（241-275）丢弃逻辑不清 `packet_len`**——见 02-P1-D，此条降级并入。**[发现于第一遍，第三遍复核降级见说明]**
- **触发/影响**：构造畸形 OGG（恶意 TTS 流 / 损坏资产）。整批，所有板型。
- **修复**：将检查移入 memcpy 紧前并对实际 `to_copy` 二次校验：`if (ctx_.packet_len + to_copy > sizeof(ctx_.packet_buf)) { 报错并 Reset(); return processed; }`，且在所有跳回 FIND_PAGE 的路径统一 `packet_len=0`。
- **[发现于第一遍 · 第三遍判级修正为 P1，见 02-P1-D]**

### 02-P0-B · GetWakeWordOpus 无超时等待，唤醒发送链可永久阻塞主任务
- **等级 P0**：主任务（Application Schedule）永久挂死 → 设备假死/砖机表现，看门狗可能复位。命中 FreeRTOS 红线"禁裸 portMAX_DELAY"。
- **文件**：`main/audio/wake_words/afe_wake_word.cc:304-312`、`custom_wake_word.cc:322-330`
- **代码**：
```cpp
bool AfeWakeWord::GetWakeWordOpus(std::vector<uint8_t>& opus) {
    std::unique_lock<std::mutex> lock(wake_word_mutex_);
    wake_word_cv_.wait(lock, [this]() { return !wake_word_opus_.empty(); });  // 无超时
    opus.swap(wake_word_opus_.front()); ...
}
```
- **根因**：`PopWakeWordPacket()`（被 application.cc:1132 在主任务 while 循环调用）→ `GetWakeWordOpus` 用条件变量无限等待编码线程产出"哨兵空帧"。若编码任务 `EncodeWakeWordData` 的 `xTaskCreateStaticPinnedToCore` 因 PSRAM 栈分配在调用前未被触发（EncodeWakeWord 与 Pop 是两次独立调用，application.cc:1077 先 Encode，1132 后 Pop），或编码任务异常未推哨兵，则主任务永久阻塞。`encode_in_progress_` 互斥导致"双连唤醒丢弃第二段"时**第二次 Encode 直接 return 不产帧**，但若调用方仍 Pop（路径上 1077→1132 在同一次），首次正常；竞态在于 Stop/重入清空 opus 队列后 Pop。
- **触发/影响**：弱网下唤醒重入、编码任务 OOM（PSRAM 栈 24KB 分配失败 assert 已挡，但 buffer 分配 INTERNAL 失败也 assert）。偶发但一旦命中即设备假死，量产返修最贵一类。
- **修复**：`wake_word_cv_.wait_for(lock, std::chrono::milliseconds(2000), pred)`，超时返回 false 并记录；application.cc:1132 的 while 改为对 false 退出。
- **[发现于第一遍]**

### 02-P1-A · 音频任务优先级与核心分工违背红线表，且与 esp-sr 内部 AFE 任务抢核
- **等级 P1**：高频体验退化（音频卡顿/丢帧）+ 潜在看门狗。判级理由：不必崩但量产高频出现。
- **文件**：`main/audio/audio_service.cc:142-174`、`afe_wake_word.cc:106-110`、`afe_audio_processor.cc:74-78`
- **代码**：
```cpp
xTaskCreatePinnedToCore(... "audio_input",  2048*3, this, 10, &handle, 1);  // 应为 P12
xTaskCreatePinnedToCore(... "audio_output", 2048*2, this, 10, &handle, 0);  // 应为 P10
xTaskCreatePinnedToCoreWithCaps(... "opus_codec", 2048*12, this, 7, &handle, 1, MALLOC_CAP_SPIRAM);
// afe_wake_word: "audio_detection" prio 7 core 1；afe_audio_processor "audio_communication" prio 7 core 1
```
- **根因**：红线表要求 `audio_input=P12 / audio_output=P10 / AFE=P8`，实际全是 10/10/7。`audio_input`(P10,core1) 与 `opus_codec`(P7,core1) 与 AFE detection/communication(P7,core1) 与 LVGL 全挤 core 1 且 input 与 output 同级 10，违反"input>output"。AFE 任务栈仅 4096/6144，esp-sr HIGH_PERF 模型在小栈上风险高。
- **触发/影响**：core1 负载高时（唤醒+处理+LVGL 并发）输入丢帧、解码欠载触发频繁 PLC（debug_statistics underrun）。整批，S3 双核板型。
- **修复**：按表设 input=12、output=10、AFE/opus=8；output 已在 core0 正确；将 opus_codec 维持 core1 但提到 8。需双确认 LVGL 与 AFE 不再同级抢占。
- **[发现于第一遍]**

### 02-P1-B · last_detected_wake_word_ 跨任务无锁读写（std::string 数据竞争）
- **等级 P1**：std::string 并发读写可致悬垂指针/崩溃，偶发但量产可见。命中红线③并发。
- **文件**：写：`afe_wake_word.cc:190`（detection 任务，core1）；读：`audio_service.cc:670 GetLastWakeWord` → `application.cc:1073/1078`（主任务）。
- **代码**：
```cpp
last_detected_wake_word_ = wake_words_[wake_idx];   // detection task 写
... // 无锁
const std::string& AfeWakeWord::GetLastDetectedWakeWord() const { return last_detected_wake_word_; }
```
- **根因**：检测任务写 string（可能 reallocate 内部缓冲），主任务通过 const 引用读甚至拷贝构造。无 mutex/atomic 保护。SSO 短串可能掩盖，长唤醒词触发堆分配时段竞态崩。
- **触发/影响**：唤醒瞬间主任务正好读 GetLastWakeWord。偶发，所有 AFE 板型。
- **修复**：GetLastDetectedWakeWord 返回值（拷贝）而非引用，并在写/读处加同一 `std::mutex`；或写完后用单独 atomic 标志位发布。
- **[发现于第一遍]**

### 02-P1-C · service_stopped_ / voice_detected_ / audio_input_need_warmup_ 为裸 bool 跨核共享
- **等级 P1**：裸 bool 跨核非原子，违反红线"多核共享用 std::atomic"。可致 Stop 不被及时观察、warmup 丢失。
- **文件**：`audio_service.h:194-196`，使用遍布 audio_service.cc 的三个任务（core0/core1）。
- **代码**：
```cpp
bool voice_detected_ = false;
bool service_stopped_ = true;
bool audio_input_need_warmup_ = false;
```
- **根因**：`service_stopped_` 在 Stop()（主任务）写、三个音频任务读做退出判断；`audio_input_need_warmup_` 在 EnableVoiceProcessing 写、AudioInputTask 读。无 atomic 无内存屏障，编译器/跨核缓存可能延迟可见，Stop 退出依赖它的 join 循环（199-204）会偶发 500ms 超时"handle leaked"。
- **触发/影响**：模式切换/关机时序。偶发，双核板。
- **修复**：三者改 `std::atomic<bool>`。`voice_detected_` 同理（IsVoiceDetected 跨任务读）。
- **[发现于第一遍]**

### 02-P2-A · AfeAudioProcessor::Initialize 不检查 afe_config / afe_data_ 创建失败
- **等级 P2**：创建失败后 afe_data_ 可能为 null，后续 GetFeedSize/Feed 已判 null 但 AudioProcessorTask 在 149 行直接 `afe_iface_->get_fetch_chunksize(afe_data_)` 解空指针。
- **文件**：`afe_audio_processor.cc:71-78、149-151`
- **代码**：
```cpp
afe_iface_ = esp_afe_handle_from_config(afe_config);
afe_data_ = afe_iface_->create_from_config(afe_config);   // 未判空
xTaskCreatePinnedToCore(... AudioProcessorTask ...);       // 任务里直接用 afe_data_
```
- **根因**：PSRAM 不足时 create_from_config 返回 null，任务启动后立即解引用崩。afe_config 也未判 null。
- **触发/影响**：低内存（对话时内部 RAM<60KB 红线附近）。整批低配/老化机。
- **修复**：create 后 `if (!afe_iface_ || !afe_data_) { ESP_LOGE; return; }`，且任务入口先判 `if (afe_data_==nullptr) { task_done_sem give; return; }`。
- **[发现于第一遍]**

### 02-P2-B · AudioDebugger 构造中 std::stoi 可抛异常 / 端口未校验
- **等级 P2**：仅 CONFIG_USE_AUDIO_DEBUGGER 启用（非量产），但 stoi 对非数字端口抛异常未捕获 → 构造期崩。
- **文件**：`audio_debugger.cc:26`
- **代码**：`int port = std::stoi(server_addr.substr(colon_pos + 1));`
- **根因**：配置串端口段非数字时 stoi 抛 std::invalid_argument，无 try/catch。
- **触发/影响**：仅调试构建，配置错误时。
- **修复**：`strtol` + 范围校验(1..65535)，失败关 socket 置 -1。
- **[发现于第一遍]**

---

# 第二遍 · 红线深挖（四条硬红线 + 跨文件数据流）

### 02-P1-D · OggDemuxer：OpusHead/OpusTags 未见时丢弃路径不清 packet_len，跨页累积可溢出
- **等级 P1**：外部 OGG 解析累积越界写。命中红线②。（第一遍 02-P0-A 复核后定级于此。）
- **文件**：`ogg_demuxer.cc:269-281、287-303`
- **代码**：
```cpp
if (opus_info_.head_seen && opus_info_.tags_seen) { ...回调... }
else { ESP_LOGW("...丢弃"); }
ctx_.packet_len = 0;            // 包结束分支清零（正常）
ctx_.packet_continued = false;
...
// 但跨页未结束包：
if (!ctx_.packet_continued) { ctx_.packet_len = 0; }  // packet_continued=true 时不清，正确续传
```
- **根因**：当一个包横跨极多页且始终 `seg_table 末段==255`（续包标志），`packet_len` 单调增长。每段进入时 210 行检查 `packet_len + seg_len > 8192` 会拦截，但拦截后走 211-217：改 state=FIND_PAGE、`packet_len=0`、`return`——**丢弃整包**。功能上"超大包被丢"可接受，不溢出。**真正缺陷**：拦截分支 `seg_index` 未推进、`seg_remaining` 清零，下一次 Process 从 FIND_PAGE 重新找 OggS，**但 packet_continued 已被置 false 而页内剩余段数据仍在输入流中**，导致后续把数据体当页头扫描——逻辑错乱（非崩溃，解析失败/静音）。
- **触发/影响**：畸形/超大 OGG。整批，影响提示音播放（非崩）。
- **修复**：拦截分支增加丢弃剩余 body 的逻辑（按 body_size-body_offset 跳过），或限制单包最大并明确 resync；同时把溢出检查改为按 `to_copy` 实算（见 02-P0-A 修复建议）。
- **[发现于第二遍 · 整合第一遍 02-P0-A 复核结论]**

### 02-P1-E · OpusHead sample_rate 取自外部数据后无范围校验，直送解码器/重采样器
- **等级 P1**：外部输入未校验即用作解码/重采样配置，命中红线②。异常采样率致重采样器配置失败或除零。
- **文件**：`ogg_demuxer.cc:245-248` → `audio_service.cc:803-809 PlaySound` → `SetDecodeSampleRate`(575) → `decoder_frame_size_ = decoder_sample_rate_/1000*frame_duration`(593)
- **代码**：
```cpp
opus_info_.sample_rate = ctx_.packet_buf[12] | (...<<8) | (...<<16) | (...<<24);  // 任意 32bit
...
packet->sample_rate = sample_rate;  // 直接传入 decode queue
// SetDecodeSampleRate: decoder_frame_size_ = sample_rate/1000*frame_duration;  sample_rate=0→frame_size=0
```
- **根因**：sample_rate 来自资产字节，可为 0 或巨值。为 0 时 `decoder_frame_size_=0`，OpusCodecTask 解码 `task->pcm.resize(0)`，且 RATE_CVT_CFG 以 0 src_rate 打开重采样器行为未定义。
- **触发/影响**：损坏/恶意 OGG。整批播放路径。
- **修复**：解析后校验 `if (sample_rate < 8000 || sample_rate > 48000) sample_rate = 16000;`（或丢弃包），在 demuxer 回调前夹紧。
- **[发现于第二遍]**

### 02-P1-F · SetDecodeSampleRate 在 OpusCodecTask 解码循环内调用，但 decode 流程持 audio_queue_mutex 已释放，重建解码器与 SetFrameDuration 跨任务竞态
- **等级 P1**：解码器句柄重建（close+open）与编码侧 SetFrameDuration、ResetDecoder 跨任务，虽各自持 decoder_mutex_，但 `decoder_frame_size_` 等在锁外被 OpusCodecTask 读（388 `task->pcm.resize(decoder_frame_size_)` 在 decoder_lock 之前）。
- **文件**：`audio_service.cc:386-388`（先 SetDecodeSampleRate 改 decoder_frame_size_，388 resize 用它，401 才取 decoder_lock）
- **代码**：
```cpp
SetDecodeSampleRate(packet->sample_rate, packet->frame_duration);  // 内部持 decoder_mutex 改 decoder_frame_size_
if (opus_decoder_ != nullptr) {                                    // 此处读 opus_decoder_ 不在锁内
    task->pcm.resize(decoder_frame_size_);                         // 读 decoder_frame_size_ 不在锁内
    ...
    std::unique_lock<std::mutex> decoder_lock(decoder_mutex_);     // 才加锁
    auto ret = esp_opus_dec_decode(opus_decoder_, ...);
```
- **根因**：SetDecodeSampleRate 释放锁后到 401 取锁之间，若 SetFrameDuration（主任务）/ResetDecoder 介入 close 了 opus_decoder_，则 387 的非空判断与 402 使用之间出现 TOCTOU，使用已释放句柄。
- **触发/影响**：百度协议切帧长（SetFrameDuration）与正在解码并发。偶发，所有板型。
- **修复**：把 387 的判空、388 的 resize、402 的解码全部移入同一个 `decoder_lock` 作用域内（扩大锁范围覆盖 opus_decoder_ 与 decoder_frame_size_ 读取）。
- **[发现于第二遍]**

### 02-P2-C · ReadAudioData 在 PlaySound/输入路径未校验 codec_->InputData 的 resize 后大小，重采样输出缓冲按估算分配
- **等级 P2**：`esp_ae_rate_cvt_get_max_out_sample_num` 估算后 resize，若 process 实际写超估算则越界。依赖 esp-ae 契约，封装层未二次防护。
- **文件**：`audio_service.cc:226-231、494-502`
- **代码**：
```cpp
esp_ae_rate_cvt_get_max_out_sample_num(input_resampler_, in_sample_num, &output_samples);
auto resampled = std::vector<int16_t>(output_samples * channels);
uint32_t actual_output = output_samples;
esp_ae_rate_cvt_process(..., resampled.data(), &actual_output);   // 信任 actual_output<=output_samples
```
- **根因**：ret 未检查，actual_output 未与分配量比对。库正常则安全，但封装层无防御。
- **触发/影响**：极端采样率比/库异常。偶发。
- **修复**：检查 process 返回值；`actual_output = std::min(actual_output, output_samples)` 后再 resize。
- **[发现于第二遍]**

### 02-P2-D · AfeWakeWord/CustomWakeWord 编码任务用 PSRAM 栈但 pin 在 core1（红线 §5.2 警示）
- **等级 P2**：红线明确"PSRAM 栈 + Core 0 是已知红线（wake_encode 案例）"。当前 pin core1（296/319 最后参数为 1），规避了 core0 红线，但 PSRAM 栈本身对含网络/锁操作的任务仍有 cache 抖动风险。
- **文件**：`afe_wake_word.cc:218、301`、`custom_wake_word.cc:247、319`
- **代码**：
```cpp
wake_word_encode_task_stack_ = (StackType_t*)heap_caps_malloc(stack_size, MALLOC_CAP_SPIRAM);
... xTaskCreateStaticPinnedToCore(..., wake_word_encode_task_stack_, wake_word_encode_task_buffer_, 1);
```
- **根因**：纯编码（esp_opus_enc）在 PSRAM 栈可接受，但任务内持 wake_word_mutex_ 且 notify cv（跨任务同步），PSRAM 栈上做锁操作有性能/稳定隐患。assert 在 OOM 时直接 abort（非优雅降级）。
- **触发/影响**：低内存 OOM 时 assert 复位（219/223）。偶发低配机。
- **修复**：assert 改为 OOM 时 `encode_in_progress_=false` 并推哨兵空帧后 return（避免 02-P0-B 永久阻塞 + 避免 abort）；栈维持 core1。
- **[发现于第二遍]**

### 02-P3-A · timestamp_queue_ 满时丢弃逻辑顺序错误，仍 pop_front
- **等级 P3**：server AEC 时间戳错位，非崩。
- **文件**：`audio_service.cc:619-626`
- **代码**：
```cpp
if (timestamp_queue_.size() <= MAX_TIMESTAMPS_IN_QUEUE) {
    task->timestamp = timestamp_queue_.front();
} else {
    ESP_LOGW("...full, dropping timestamp"); // 走 else 不取值
}
timestamp_queue_.pop_front();  // 两分支都 pop
```
- **根因**：满时丢了一个但未消费多余积压；timestamp 入队在 AudioOutputTask(345)无上限保护，长时间播放 timestamp_queue_ 可膨胀（无 size 上限——违反"所有 deque 必须有上限"红线）。
- **触发/影响**：长时间 server AEC 会话。功能性偏差 + 潜在 OOM。
- **修复**：AudioOutputTask push timestamp 处加上限（如 >MAX 丢最旧）；满时循环 pop 到阈值。
- **[发现于第二遍]**

### 02-P3-B · AfeAudioProcessor::is_speaking_ 裸 bool（仅 AFE 任务读写，低风险）
- **等级 P3**：is_speaking_ 仅在 AudioProcessorTask 内读写，单任务无竞态，但 VAD 回调进主任务。记录待办。
- **文件**：`afe_audio_processor.cc:43、178-184`
- **修复**：保持现状或转 atomic 以防未来跨任务读。
- **[发现于第二遍]**

---

# 第三遍 · 反审自检（复验 + 对抗视角 + 删误报）

### 复验结论
1. **02-P0-A 降级为 02-P1-D**：第三遍逐行复核 ogg_demuxer 续传分支，确认 210 行的 `packet_len + seg_len` 检查在 `seg_remaining` 续传时仍执行（seg_len 被赋为 seg_remaining），单包不会无界越界写——拦截分支会丢包。真正缺陷是丢包后状态机 resync 错乱（解析失败/静音）+ 拦截分支 body 未跳过，属 P1 功能缺陷而非 P0 必崩。**判级修正，避免拔高。**
2. **02-P0-B 维持 P0**：对抗复核——若 application.cc:1132 在 EncodeWakeWord 未真正起编码任务（双连唤醒 encode_in_progress_ 已 true 时 EncodeWakeWordData 直接 return 不推哨兵，见 afe_wake_word.cc:211-214），而主任务仍进入 PopWakeWordPacket→GetWakeWordOpus 无限等待。**这是真实可达的永久阻塞路径**，行号属实，维持 P0。
3. **02-P1-A 行号复核属实**：audio_service.cc:146/152/166/174 优先级 10/10/7 与红线表 12/10/8 不符确凿。
4. Feed 路径（afe_wake_word.cc:144-149 / afe_audio_processor.cc:115-120 / custom_wake_word.cc:179-219）的 input_buffer_ 均在 input_buffer_mutex_ 内操作，Stop 也持同锁清空，**无 TOCTOU**——注释声称已修，复核确认正确，**不报为缺陷**（删误报）。
5. wake_word_pcm_ 上限 `2000/30≈66` 帧有界（afe:205 / custom:234），符合 deque 上限红线，**不报**。

### 对抗视角新增

### 02-P1-G · 唤醒检测期间 EncodeWakeWordData 读取 wake_word_pcm_ 与 detection 任务写入竞态（Custom 路径无 pcm 锁）
- **等级 P1**：CustomWakeWord 的 wake_word_pcm_ 在编码任务（281-307）遍历/clear 时，无 mutex 保护；而 detection 在 Feed→StoreWakeWordData(230-237) 仍可能写入。命中红线③。
- **文件**：`custom_wake_word.cc:281-307`（编码任务直接 `for (auto& pcm: this_->wake_word_pcm_)` 且 307 `clear()`，**全程无锁**），对比 afe_wake_word.cc:255-259 用 `wake_word_pcm_mutex_` swap 出快照（正确）。
- **代码**：
```cpp
// CustomWakeWord 编码任务：无 wake_word_pcm_mutex_
for (auto& pcm: this_->wake_word_pcm_) { ... in_buffer = std::move(pcm); ... }
this_->wake_word_pcm_.clear();   // 与 StoreWakeWordData push_back 竞态
```
- **根因**：CustomWakeWord 类**根本没有 wake_word_pcm_mutex_ 成员**（见 custom_wake_word.h:63 无对应 mutex），而 AfeWakeWord 有。编码时若 detection 未停（running_ 竞态）继续 Store，deque 并发改 → 崩。虽 Stop 通常先于 Encode，但 EncodeWakeWord 由 application 调度，时序不保证。
- **触发/影响**：custom 唤醒模式 + 编码与检测重叠。偶发，custom 模式板型。
- **修复**：CustomWakeWord 增加 `std::mutex wake_word_pcm_mutex_`，编码任务先 swap 快照（仿 afe），StoreWakeWordData 加锁。
- **[发现于第三遍 · 对抗复核 custom vs afe 差异]**

### 02-P2-E · AudioProcessorTask / AudioDetectionTask fetch_with_delay 后 res 复用，PROCESSOR_EXIT 检查后仍可能用已释放 afe_data_
- **等级 P2**：析构时设 EXIT 后等 sem 2s（afe_audio_processor.cc:84-87），但若 fetch 阻塞满 100ms 且任务未及时退出，析构超时后继续 destroy(afe_data_)，而任务可能仍在 191 行 `res->data` 访问 res 指向的 afe 内部缓冲 → use-after-free。
- **文件**：`afe_audio_processor.cc:82-91、162-191`、`afe_wake_word.cc:20-29、172-181`
- **根因**：2s sem 超时后无条件 destroy，无"任务确已退出"硬保证。res 指向 afe_data_ 内部缓冲。
- **触发/影响**：析构/重建 AFE（SetFrameDuration 重 Initialize）时 fetch 正阻塞。偶发，模式切换。
- **修复**：destroy 前确认 task_done_sem_ 成功 take（返回 pdTRUE）才 destroy；超时则记录并延后释放，不要在任务可能存活时 destroy。
- **[发现于第三遍]**

### 02-P2-F · MusicPlayer::OnMp3Event 在 worker 任务回调中拷贝全局 g_current_title（非 atomic std::string）
- **等级 P2**：g_current_title 由主任务 Play/Stop(153/164) 写，worker 回调(81)读拷贝，无锁。
- **文件**：`music_player.cc:74、81、153、164`
- **根因**：全局 std::string 跨任务读写无保护。`auto title = g_current_title;` 在回调里拷贝时主任务可能正 clear。
- **触发/影响**：播放结束/出错与 Stop 并发。偶发。
- **修复**：g_current_title 访问加 mutex，或回调内不依赖它（title 通过 ctx 传参）。
- **[发现于第三遍]**

### 02-P3-C · AfeWakeWord 析构 free PSRAM 栈但未确认编码任务已退出（仅 detection 等了 sem）
- **等级 P3**：析构等的是 detection_done_sem_，但 wake_word_encode_task_ 是独立任务，析构时若编码仍在跑，heap_caps_free(stack)(31-37) 会释放正在用的栈 → 崩。
- **文件**：`afe_wake_word.cc:20-37`、`custom_wake_word.cc:19-35`
- **根因**：encode 任务无独立 join，仅靠 encode_in_progress_ 标志，析构未等待它。
- **触发/影响**：析构时编码未完（极少，对象通常单例长生命周期）。远期/边缘。
- **修复**：析构前 `while (encode_in_progress_.load()) vTaskDelay(10);` 带超时，再 free 栈。
- **[发现于第三遍]**

### 删除的误报
- Feed 路径 TOCTOU（注释已修，复核正确）。
- wake_word_pcm_ / wake_word_opus_ 无上限（pcm 有 66 帧上限；opus 由编码次数 + 哨兵控制，单轮有界）。
- input_resampler_mutex_ 保护重置（EnableWakeWordDetection/EnableVoiceProcessing 已正确加锁）。

---

# 统计

| 等级 | 数量 | 编号 |
|------|------|------|
| P0 | 1 | 02-P0-B |
| P1 | 7 | 02-P1-A, 02-P1-B, 02-P1-C, 02-P1-D, 02-P1-E, 02-P1-F, 02-P1-G |
| P2 | 6 | 02-P2-A, 02-P2-B, 02-P2-C, 02-P2-D, 02-P2-E, 02-P2-F |
| P3 | 4 | 02-P3-A, 02-P3-B, 02-P3-C, （02-P0-A 已并入 02-P1-D） |
| **合计** | **18** | （02-P0-A 降级合并入 02-P1-D，不重复计数） |

> 注：02-P0-A 第三遍复核后降级并入 02-P1-D，故 P0 实际 1 个。

## 三遍新增分布
- **第一遍（广度）新增 7 条**：02-P0-A(后降级)、02-P0-B、02-P1-A、02-P1-B、02-P1-C、02-P2-A、02-P2-B
- **第二遍（红线深挖）新增 7 条**：02-P1-D(整合 P0-A)、02-P1-E、02-P1-F、02-P2-C、02-P2-D、02-P3-A、02-P3-B
- **第三遍（反审自检）新增 4 条**：02-P1-G、02-P2-E、02-P2-F、02-P3-C；并降级 1 条（P0-A→P1-D）、删除 3 条误报
- **a + b + c = 7 + 7 + 4 = 18 条**（最终有效 18 条，含 1 条降级合并）

## 出货前必清（P0/高 P1 优先序）
1. **02-P0-B**：GetWakeWordOpus 无超时 → 主任务永久阻塞（加 2s 超时，一处两文件）。
2. **02-P1-F**：解码器句柄 TOCTOU（扩大 decoder_mutex 范围）。
3. **02-P1-A**：任务优先级/核心分工纠正（按红线表）。
4. **02-P1-G**：CustomWakeWord pcm 无锁（补 mutex + 快照）。
5. **02-P1-D/E**：OGG 外部输入校验（sample_rate 夹紧 + 溢出按 to_copy 实算）。
