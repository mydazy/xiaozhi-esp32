# 第二轮补充-并发/缓冲

> 第二视角专攻：多任务/ISR 无锁读写、环形缓冲指针与帧长换算、I2S DMA 与帧长匹配、采样率/声道换算溢出、队列满空边界、信号量/锁释放与死锁、编解码缓冲长度校验、定时器与音频任务竞争、反复 malloc/free 碎片、唤醒词模型缓冲生命周期。
>
> 已去重：第一轮 01_audio_subsystem.md 全部 19 条（P0-1/2、P1-1~6、P2-1~6、P3-1~5）不再重复。本轮只列第一轮未覆盖的新发现。重点补充第一轮"用法安全/记为隐患"但实际存在并发缺陷的点，并给出第一轮未触及的缓冲区数学错误。
>
> 精读文件：audio_service.cc/.h、es7111_audio_codec.cc/.h、box_audio_codec.cc、no_audio_codec.cc、ogg_demuxer.cc、afe_wake_word.cc、custom_wake_word.cc、afe_audio_processor.cc、alarm_ringer.cc、music_player.cc、mp3_player.cc（components）。所有行号来自实际文件。

---

## P0（必崩 / 砖机 / 必然崩溃 / 安全）

### P0-A1 `BoxAudioCodec::Read/Write` 不持 `data_if_mutex_`，与 `EnableInput/EnableOutput/SetOutputVolume` 对 `esp_codec_dev` 并发 → close 期间读写已释放设备 → LoadProhibited
- 等级判定：**P0**。`EnableInput/EnableOutput/SetOutputVolume/SetInputGain` 全部 `std::lock_guard<std::mutex> lock(data_if_mutex_)` 保护对 `input_dev_/output_dev_` 的 open/close/set_vol；但热路径 `Read`(:257) 和 `Write`(:264) 完全不加锁。AudioInputTask（Core1 P10）持续 `Read`→`esp_codec_dev_read(input_dev_)`，而 `CheckAndUpdateAudioPowerState`（esp_timer task）在 15s 超时后调 `EnableInput(false)`→`esp_codec_dev_close(input_dev_)`，AudioOutputTask 同理 `Write`↔`EnableOutput(false)` close。close 会释放设备内部 buffer/状态，并发 read/write 命中半释放结构是 UB，必崩。
- 文件：`main/audio/codecs/box_audio_codec.cc:257-262`（Read 无锁）、`:264-269`（Write 无锁）；对比 `:211-233`（EnableInput 持锁 close）、`:235-255`（EnableOutput 持锁 close）、`:847-863`（power timer 触发 EnableInput/Output(false)）
- 问题代码：
```c
int BoxAudioCodec::Read(int16_t* dest, int samples) {
    if (input_enabled_) {                                    // 无锁读 input_enabled_
        ESP_ERROR_CHECK_WITHOUT_ABORT(esp_codec_dev_read(input_dev_, ...));  // 无锁
    }
    return samples;
}
// 同时另一线程（esp_timer task）：
//   EnableInput(false) → lock(data_if_mutex_) → esp_codec_dev_close(input_dev_)
```
- 根因：写侧（enable/close）用 `data_if_mutex_` 串行化，但读写热路径未纳入同一把锁；`input_enabled_` 是普通 bool，TOCTOU 后 close 已发生仍执行 read。Es7111/NoAudioCodec 的 Read/Write 反而用了锁（Es7111 用 `mutex_`、NoAudioCodec 用 `data_if_mutex_`），唯独 BoxAudioCodec 漏了，明显遗漏。
- 触发条件与影响面：BoxAudioCodec 是 P30 主路径 codec。任何"对话结束后静默 15s → power timer 关闭 input/output → 此刻仍有一帧 Read/Write 在途"即触发，属高频日常场景。Read 与 close 的竞争窗口虽小但 24h 长跑必然命中。
- 修复建议：`Read`/`Write` 开头加 `std::lock_guard<std::mutex> lock(data_if_mutex_);`（与 enable/vol 同锁）；注意 `esp_codec_dev_read/write` 内部含 I2S 阻塞，持锁期间 `EnableInput/Output` 会等待，需评估 15s 关闭延迟是否可接受（可接受，因为读一帧仅 10-60ms）。或改用读写锁 / 把 close 延后到下一次 Read 返回后由 input 任务自己执行。

---

## P1（高频崩溃 / 体验严重退化）

### P1-A1 `Es7111AudioCodec::Read/Write` 共享 `tdm_buf_`/`write_buf_` 与 ref_ringbuf，Read 持 `mutex_` 但 Write 也持 `mutex_` 却与 EnableOutput 持同锁 → ref 写读跨 I2S 阻塞期持锁，且 Write 的 ringbuf send 在 EnableOutput close 之后仍执行
- 等级判定：**P1**。`Read`(:236) 与 `Write`(:309) 均未加 `mutex_`（只有 SetHeadsetMode/SetInputGain/EnableInput/EnableOutput 加），但 `tdm_buf_`(:242 resize) 仅 Read 用、`write_buf_`(:319 resize) 仅 Write 用，二者各自单线程，buffer 本身无竞争；真正问题是 **`ref_ringbuf_` 的 Write 端 `xRingbufferSend`(:312) 与 Read 端 `xRingbufferReceiveUpTo`(:261) 跨线程并发**——Ringbuffer 本身线程安全，但 `input_reference_` 与 `headset_mode_` 这两个被 Read 读取的成员，会被 `EnableInput`/`SetHeadsetMode`（持 `mutex_`）在另一线程改写，而 Read 无锁读 `headset_mode_`(:268)，切换耳机瞬间 Read 取到撕裂的 slot 选择，输出错麦或读越界 slot（虽 slot 索引都 <4 不越界，但 AEC 参考错配）。更关键：`ref_ringbuf_` 的尺寸数学有 bug，见 P1-A2。
- 文件：`main/audio/codecs/es7111_audio_codec.cc:236-296`（Read 无锁读 headset_mode_/input_reference_）、`:309-341`（Write 无锁）、`:159-171`（SetHeadsetMode 持 mutex_ 改 headset_mode_）
- 根因：成员 `headset_mode_`/`input_enabled_`/`output_enabled_` 在写侧持 `mutex_`，读侧（Read/Write 热路径）裸读，无 atomic 无锁。项目红线"多核共享变量用 std::atomic"。
- 触发条件与影响面：Type-C 耳机插拔时 `SetHeadsetMode` 与正在进行的 Read 并发。插拔瞬间一帧 AEC 参考/麦克风选择错乱，偶发拾音异常；不直接崩但回声消除失效一帧。
- 修复建议：`headset_mode_`/`input_reference_` 改 `std::atomic`，或 Read/Write 开头短暂持 `mutex_` 快照这些标志后释放再做 I2S 阻塞读写（不要持锁做 I2S 阻塞，否则与 EnableInput 死等）。

### P1-A2 `Es7111` ref_ringbuf 容量按 24kHz 注释但实际按 output_sample_rate 写入，Write 一次写 `samples` 而 Read 一次取 `frames` 个 ref，速率不匹配导致 ref ring 单调堆积溢出丢弃 → AEC 参考整体滞后漂移
- 等级判定：**P1**。`kRefBufSamples = 4800`（注释"200ms @24kHz"），ringbuf 容量 `4800 * sizeof(int16_t)=9600 字节`。Write 端每次 `xRingbufferSend(data, samples*2, 0)`（超时 0，满则**静默丢弃**，:312）；Read 端每次只取 `frames*2` 字节（:259-262，frames = samples/input_channels，input_channels=2 时 frames=Write samples 的一半量级）。播放（Write）数据量与拾音（Read）消费量节拍不一致：TTS 连续播放时 Write 速率 > Read 消费速率，ring 很快填满，`xRingbufferSend` 超时 0 直接丢弃新参考帧，于是 ref ring 里始终是**旧的**参考信号，与当前麦克风信号时间错位 → AEC 拿到滞后参考，回声消除显著劣化甚至放大回声。第一轮 P2-3 仅提到"见底填 0"，未指出**反向的 ring 填满丢弃 + 时间漂移**这一主因。
- 文件：`main/audio/codecs/es7111_audio_codec.cc:71-73`（ringbuf 4800 samples）、`:309-313`（Write send 超时0 丢弃）、`:255-282`（Read 取 frames 个 + 不足填 0）、`.h:67`（kRefBufSamples 注释 24kHz 与实际 sr 不符）
- 问题代码：
```c
xRingbufferSend(ref_ringbuf_, data, samples * sizeof(int16_t), 0);  // 满则丢弃，无回压
// Read 侧：
size_t ref_bytes_needed = frames * sizeof(int16_t);                 // 只取 frames 个
int16_t* ref_data = xRingbufferReceiveUpTo(ref_ringbuf_, &ref_bytes_got, 0, ref_bytes_needed);
```
- 根因：Write/Read 无配对节拍，ring 既会见底（Read 快）也会溢出（Write 快），溢出时丢新留旧导致参考相位漂移；容量按错误采样率注释（实际 codec 多为 16kHz 或 24kHz，4800 在 16kHz=300ms / 24kHz=200ms），缓冲过深进一步放大固定延迟。
- 触发条件与影响面：长 TTS 播放 + 用户同时说话（打断场景）。AEC 参考滞后，自适应滤波器失锁，回声泄漏，体验严重退化。
- 修复建议：① Write 端 ring 满时丢弃**最旧**而非最新（先 receive 丢一帧再 send），保证 ring 内始终是最新参考；② ring 容量收敛到 ~2 帧（=AEC 对齐窗口），避免深缓冲固定延迟；③ EnableOutput(false) 时清空 ring（第一轮 P2-3 已提）；④ 修正 kRefBufSamples 注释/取值与真实 output_sample_rate 对齐。

### P1-A3 OGG demuxer 单段 >可用数据时 `seg_remaining` 半段恢复路径会把 `seg_table[seg_index]` 覆盖成 `seg_remaining`，下一轮 `seg_continued` 判定基于被改写的 seg_len 仍读原表，但 packet_continued 跨页拼接的 `seg_remaining` 复位时机错位 → 跨页 continued 包丢半段或拼错
- 等级判定：**P1**。PARSE_DATA 里 `seg_len` 局部变量在 `seg_remaining>0` 时被设为 `seg_remaining`(:203-204)，但"段是否 continued"用 `ctx_.seg_table[ctx_.seg_index]==255`(:236) 判定（读原表，正确）。问题在：当一个 255 段跨 `Process()` 调用边界被拆成两次（第一次 to_copy < seg_len，:221-232 return），下次进入时 `seg_remaining` 被正确续用；但**包结束分支**(:251-254、:262-265、:283-284) 多处在 `continue`/正常路径都把 `ctx_.seg_remaining = 0`，唯独溢出保护分支(:215)和跨页保留分支(:295) 的 seg_remaining 状态与 packet_continued 组合在"上一页最后一段恰为 255 且数据在页边界被截断"时，重入会用 seg_remaining=0 重新读整段 seg_len，导致已拷贝的半段数据被重复或丢弃。PlaySound 的 OGG 是本地静态资源（提示音），结构固定不触发；但服务端若下发分块流式 OGG（TTS over OGG）则跨页 + 跨 Process 边界高发。
- 文件：`main/audio/demuxer/ogg_demuxer.cc:198-305`，重点 `:203-207`（seg_remaining 恢复）、`:221-233`（半段 return）、`:236`（continued 判定）、`:294-297`（跨页保留 packet_len 但 seg_remaining 已被前面置 0）
- 根因：`seg_remaining` 作为"当前段已读进度"与 `packet_continued` 作为"包跨页标志"两个状态机变量在边界条件下耦合不清，半段重入与跨页重入共用同一字段但复位点不一致。
- 触发条件与影响面：流式分块喂入（每次 Process 给一小块）且 OGG 页在 255 段中间被切断。本地提示音一次性整块喂入(:816 `Process(buf, size)` 一次给全量)不触发，故当前 PlaySound 安全；但 demuxer 设计为流式（FIND_PAGE 跨块逻辑齐全），任何流式调用方会踩。标 [待确认]：当前代码库是否存在流式分块喂 OggDemuxer 的调用方（仅见 PlaySound 整块喂）。若无，降 P3。
- 修复建议：用单元测试覆盖"任意字节边界切分同一 OGG 流"等价性（喂整块 vs 逐字节喂结果须一致）。明确 `seg_remaining` 仅表示当前段剩余，包结束/页结束时统一在一处复位；跨页 continued 时不动 seg_remaining。

### P1-A4 underrun 补偿分支重建/使用 `output_resampler_` 全程无 `decoder_mutex_`，与 `SetDecodeSampleRate` 锁外重建 resampler 竞态 → 解码线程内自洽但 ResetDecoder 清队列后下一帧 resampler 句柄竞争
- 等级判定：**P1**（第一轮 P1-4 聚焦 `opus_decoder_` 重开竞态与 resampler 无锁，但**只覆盖了正常解码分支** :416-425；本轮补充 **underrun 补偿分支** :495-504 同样无锁使用 `output_resampler_`，且该分支在 `audio_decode_queue_` 为空、与 `SetDecodeSampleRate` 由别处触发重建 resampler 时窗口更大）。`OpusCodecTask` underrun 分支(:495)读 `output_resampler_` 做 rate_cvt_process，全程不持 `decoder_mutex_`；`SetDecodeSampleRate`(:599-608) 在锁外 close+重开 `output_resampler_`。两者并发：underrun 分支正用 resampler 时被 close → 野指针。
- 文件：`main/audio/audio_service.cc:495-504`（underrun 用 output_resampler_ 无锁）、`:599-608`（SetDecodeSampleRate 锁外重建 output_resampler_）、`:416-425`（正常分支同样无锁，第一轮已提）
- 问题代码：
```c
// underrun 分支（lock=audio_queue_mutex_ 持有，但 decoder_mutex_ 未持，resampler 无任何锁）：
if (decoder_sample_rate_ != codec_->output_sample_rate() && output_resampler_ != nullptr) {
    esp_ae_rate_cvt_get_max_out_sample_num(output_resampler_, ...);
    esp_ae_rate_cvt_process(output_resampler_, ...);   // 可能正被 SetDecodeSampleRate close
}
```
- 根因：`output_resampler_` 没有专属锁，其重建在 `SetDecodeSampleRate` 锁外，使用点散落在正常解码 + underrun 两处。`SetDecodeSampleRate` 虽多由 OpusCodecTask 同线程调（:386），但 underrun 分支不调用它，而服务端切采样率/帧长经 SetFrameDuration→Initialize 路径不重建 output_resampler_，唯一重建点是解码线程自身 :386→SetDecodeSampleRate，故同线程下 underrun 与重建不并发——**但 ResetDecoder（外部线程 :831）只 reset decoder 不碰 resampler**，若紧接着 output_sample_rate 变化触发重建则跨线程。窗口比正常分支更隐蔽。
- 触发条件与影响面：弱网 underrun 持续 + 期间发生采样率切换（提示音 48k 与对话 16k 交替）。偶发，但弱网正是 underrun 高发期，叠加切流概率上升。
- 修复建议：为 `output_resampler_` 单独加 `output_resampler_mutex_`（参照已有 `input_resampler_mutex_`），所有使用点（正常 :416、underrun :495）与重建点（:599）统一加锁。这是第一轮 P1-4 修复建议的必要补全。

### P1-A5 `AfeWakeWord::AudioDetectionTask` 在 `Stop()` 内调 `reset_buffer` 并清 input_buffer，但唤醒命中时在**检测任务上下文**调 `Stop()`(:166)，而 `Stop()` 持 `input_buffer_mutex_` 与 `Feed()`（input 任务）争锁，且 reset_buffer 与 Feed 的 `afe_iface_->feed` 对 afe_data_ 内部缓冲并发
- 等级判定：**P1**。唤醒命中(:165-172)在 AudioDetectionTask 调 `Stop()`→`afe_iface_->reset_buffer(afe_data_)`(:118)，同一时刻 AudioInputTask 可能正在 `Feed()`→`afe_iface_->feed(afe_data_, ...)`(:136)。`Stop()` 持 `input_buffer_mutex_`，`Feed()` 也持 `input_buffer_mutex_`，故 input_buffer 本身安全；但 `reset_buffer` 与 `feed` 都直接操作 esp-sr 的 `afe_data_` 内部 ring，**这两个 afe 调用不在同一把锁内**（feed 在 input_buffer_mutex_ 内，reset_buffer 也在 input_buffer_mutex_ 内——实际同锁，安全）。真正缺陷：`last_detected_wake_word_ = wake_words_[...]`(:167) 写，与 `GetLastDetectedWakeWord()`（外部线程读，audio_service.cc:671）无锁并发读写 `std::string`，回调触发后 application 线程读取 last wake word 时数据竞争。
- 文件：`main/audio/wake_words/afe_wake_word.cc:167`（写 last_detected_wake_word_）、`audio_service.cc:670-672`（GetLastWakeWord 无锁读）；CustomWakeWord 同构 `custom_wake_word.cc:193`
- 根因：`last_detected_wake_word_`（std::string）跨线程读写无同步，与第一轮 P3-3（g_current_title）同类但在唤醒主路径上，频次更高。
- 触发条件与影响面：唤醒命中回调里 application 读 `GetLastWakeWord()` 上送服务器，与检测任务下一次写竞争。std::string 拷贝期被改写 UB。唤醒是核心高频路径。
- 修复建议：`last_detected_wake_word_` 的写在回调前完成且回调同步执行（当前如此），但 `GetLastWakeWord()` 由别的线程异步读则需加锁或回调直接传值。建议回调签名已带 wake_word 字符串（:170 `wake_word_detected_callback_(last_detected_wake_word_)`），下游应使用回调参数而非事后 `GetLastWakeWord()`，并审计所有 `GetLastWakeWord()` 调用点。[待确认] 调用点线程归属。

---

## P2（偶发 / 边缘场景）

### P2-A1 `OpusCodecTask` 正常解码分支 `debug_statistics_.decode_count++` 被执行两次（:429 与 :450），解码统计翻倍 + 该计数若用于流控/日志判定会误判
- 等级判定：**P2**。解码成功路径在 :429 `decode_count++` 一次，函数尾部 :450 再 `decode_count++` 一次（无条件，连解码失败 lock.lock() 后也走到 :450）。导致 decode_count 既统计成功又统计每轮进入，语义混乱、翻倍。纯统计字段不崩，但若运维以此判断"解码吞吐正常"会被误导，且失败分支(:444-445 lock.lock())后仍 +1 把失败也计成 decode。
- 文件：`main/audio/audio_service.cc:429`、`:450`
- 问题代码：
```c
debug_statistics_.decode_count++;            // :429 成功分支
...
} else { ... lock.lock(); }
debug_statistics_.decode_count++;            // :450 无条件再 +1
```
- 根因：重构时遗留重复自增。
- 触发条件与影响面：每次解码。仅影响调试统计准确性。
- 修复建议：删除 :450 那一处，或明确两个字段（decode_attempt vs decode_ok）。

### P2-A2 `MAX_SEND_PACKETS_IN_QUEUE` / `MAX_DECODE_PACKETS_IN_QUEUE` 宏每次展开都运行期重算 `2400/OPUS_FRAME_DURATION_MS`，`OPUS_FRAME_DURATION_MS` 是非原子全局，在 OpusCodecTask 的 cv 谓词(:367-368)里读，与 SetFrameDuration 改写(:770)并发 → 队列上限瞬时取到 0 或巨值
- 等级判定：**P2**（第一轮 P3-1 提到 g_opus_frame_duration_ms 原子性，但聚焦"队列水位抖动不崩"；本轮补充更尖锐的**除法 by 切换中间态**风险）。`SetFrameDuration` 在 `encoder_mutex_` 内改 `g_opus_frame_duration_ms`(:770)，但 OpusCodecTask 的 cv 谓词(:367) `audio_send_queue_.size() < MAX_SEND_PACKETS_IN_QUEUE` 展开为 `< 2400/g_opus_frame_duration_ms`，该读取持 `audio_queue_mutex_` 而非 `encoder_mutex_`，两锁不互斥。32 位对齐整型读写虽单字原子（不会读到撕裂的中间字节），不会出现 0；但语义上切换瞬间上限从 40↔120 跳变，配合 `MAX_DECODE_PACKETS_IN_QUEUE=3600/dur` 在 PushPacketToDecodeQueue(:640) 的比较，极端时序下刚 push 进的包数 > 新上限导致 wait 永久阻塞一拍。比第一轮判定更值得修。
- 文件：`main/audio/audio_service.h:42-46`（宏运行期展开）、`audio_service.cc:367-368`（cv 谓词读）、`:640-642`（decode 队列上限读）、`:770`（写，仅 encoder_mutex_）
- 根因：可变全局参与多处运行期队列上限计算，写侧锁与读侧锁不一致。
- 触发条件与影响面：百度 20ms↔60ms 帧长切换瞬间。SetFrameDuration 文档要求"仅 audio 空闲态调用"，若严格遵守不触发；但无运行期 assert 保证。
- 修复建议：`g_opus_frame_duration_ms` 改 `std::atomic<int>`；或把队列上限算成成员变量，在 SetFrameDuration 持 `audio_queue_mutex_` + `encoder_mutex_` 双锁时一并更新，避免每次比较都做除法。配合第一轮 P3-1。

### P2-A3 `AlarmRinger` 的 `saved_volume_`/`cycle_step_`/`ring_count_` 在 esp_timer task（OnTick）写读，与 Start（应用线程）写、ShakeStop（按键线程）读 `ringing_` 之外的成员均无锁
- 等级判定：**P2**。`ringing_` 是 atomic（:49/115/177 正确用 acq/rel），但 `saved_volume_`、`start_us_`、`cycle_step_`、`ring_count_`、`message_`、`kind_` 都是普通成员。`Start()`（应用线程）写这些(:57-65)，`OnTick()`（esp_timer task）读写 `cycle_step_/ring_count_`(:127-151)，`Stop()`（可能 timeout timer task 或 ShakeStop 按键线程）读 `saved_volume_`(:195) 并 `SetOutputVolume`。Start 与首个 OnTick 之间若 Start 尚未写完 saved_volume_ 而 5s timer 已触发（不可能 5s 内，但 Start 与 Stop 竞争：Start 刚 exchange(true) 还没设 saved_volume_，并发 Stop 读到旧 saved_volume_）。`message_`(std::string) 被 Start 写、DispatchWakeWord lambda 捕获读(:43)，跨 esp_timer task 与应用线程。
- 文件：`main/audio/alarm_ringer.cc:48-108`（Start 写多个成员，仅 ringing_ atomic）、`:114-152`（OnTick 读写 cycle_step_/ring_count_）、`:176-203`（Stop 读 saved_volume_/message_）、`:41-46`（DispatchWakeWord 读 message_）
- 根因：只把 `ringing_` 做成 atomic，其余共享态裸用，依赖"5s tick 间隔足够大不会与 Start 重叠"的隐含假设；ShakeStop（按键）与 OnTick（timer task）并发改 shake_count_/调 Stop 也无锁。
- 触发条件与影响面：响铃 Start 与几乎同时的 ShakeStop/timeout（极小窗口）；或 message_ 在响铃中被第二次 Start 更新(:50-54) 而 OnTick 的 DispatchWakeWord 正读。偶发 message 错乱/音量恢复错值，不崩（std::string 读写竞争理论 UB）。
- 修复建议：Start/Stop/OnTick/ShakeStop 共享成员用一把 `std::mutex` 保护，或 message_ 改为 DispatchWakeWord 启动时快照传值；saved_volume_ 改 atomic<int>。

### P2-A4 `Es7111AudioCodec::Read` 在 `!input_enabled_` 时返回 `samples`（假装读满）而非 0，上游 ReadAudioData 误判成功 → 把 `tdm_buf_` 上一帧残留/未初始化数据当有效 PCM 喂给 AFE
- 等级判定：**P2**。`Read`(:236-237) `if (!input_enabled_) return samples;` 直接返回请求样本数表示"成功"，但 `dest` 缓冲未被写入（dest 是上游 ReadAudioData 里 resize 的 vector，新分配为 0，故是静音而非脏数据，影响较轻）。问题在语义：返回值 == samples 让上游认为读到有效数据，会把整帧静音喂 AFE/编码上送，浪费带宽且 VAD 可能误判。对比 BoxAudioCodec::Read 同样 `input_enabled_` 假时返回 samples（:257-261），NoAudioCodec::Read 则返回真实 bytes。
- 文件：`main/audio/codecs/es7111_audio_codec.cc:236-237`；`main/audio/codecs/box_audio_codec.cc:257-261`
- 根因：未启用时返回请求数而非 0，约定不一致。
- 触发条件与影响面：input 未启用但 AudioInputTask 已被事件位唤醒读一帧（warmup 边界）。喂静音帧，不崩。
- 修复建议：未启用应返回 0 让上游 `ReadAudioData` 返回 false 走重试延迟；或上游在 Read 前确保 EnableInput 已完成（当前 ReadAudioData:211 会 EnableInput，但 enable 是异步 set_fs，首帧可能仍未就绪）。

### P2-A5 `OggDemuxer::OpusHead` sample_rate 解析直接当 `int` 用于下游 `decoder_sample_rate_`，但未校验范围（8000-48000），异常 OGG 可置非法采样率 → SetDecodeSampleRate 用非法值 open 解码器/建 resampler
- 等级判定：**P2**。`opus_info_.sample_rate = ctx_.packet_buf[12..15]`(:245-248) 小端 32 位无范围校验。PlaySound(:809) 把它塞进 `packet->sample_rate`，OpusCodecTask→SetDecodeSampleRate(:386) 用它 `decoder_frame_size_ = sample_rate/1000*frame_duration`(:594)。若被构造成 0 或巨值：`decoder_frame_size_` 为 0 → `task->pcm.resize(0)` → 解码 out buffer len=0；或巨值 → resize 巨量 PSRAM 分配失败/OOM。本地静态提示音可信，故当前安全；但 demuxer 是通用组件，未来用于不可信流式 OGG 即风险。
- 文件：`main/audio/demuxer/ogg_demuxer.cc:244-249`；`audio_service.cc:594`（frame_size 计算）
- 根因：解析自外部容器的采样率未做合法性区间检查即用于内存分配尺寸计算。
- 触发条件与影响面：非可信来源 OGG。当前仅本地提示音，安全。
- 修复建议：解析后 clamp 到 {8000,12000,16000,24000,48000} 合法 Opus 采样率集合，非法则丢弃该流。

---

## P3（潜在远期风险）

### P3-A1 `Mp3Player` 三任务通过 `active_tasks_` 原子计数 join，但 ringbuffer 释放(:248-256)在确认 active_tasks_==0 后，与 `running_` 复位非同一原子操作，Play 重入与 AbortAndJoin 之间存在 ring 双删/泄漏窗口
- 等级判定：**P3**（第一轮 P1-5 聚焦卡死泄漏；本轮补充 join 完成后的 ring 生命周期与 running_ 复位顺序）。`AbortAndJoin`(:224) 先 `running_.load`，超时返回不删 ring（第一轮已述）；正常路径 :248 `running_.store(false)` 后删 ring。但 `Play`(:103-110) 开头也删 ring（防 AbortAndJoin 超时残留），若一个 Stop 的 AbortAndJoin 正在 :238 等待循环、与新 Play 的 :92 AbortAndJoin 并发（两个线程都调 Stop/Play），`compressed_ring_` 可能被双重 `vRingbufferDeleteWithCaps`。Play/Stop 无互斥锁保护，依赖调用方串行。
- 文件：`components/mydazy__esp_mp3_player/mp3_player.cc:92-110`、`:224-257`
- 根因：Play/Stop/AbortAndJoin 无单一互斥，ring 指针的删与置 nullptr 不在锁内原子完成，靠"调用方不并发 Play/Stop"的隐含约定。
- 触发条件与影响面：UI 快速连点播放/停止，或 MP3_EVENT_ERROR 回调里 Schedule 的 Stop 与用户新 Play 并发。双删 ringbuffer → heap 损坏。
- 修复建议：Play/Stop 用一把成员 `std::mutex` 串行化整个生命周期切换；ring 删除后立即置 nullptr 并在锁内，杜绝双删。

### P3-A2 `AfeAudioProcessor::AudioProcessorTask` 的 `output_buffer_`/`is_speaking_` 在 task 内单线程安全，但 `frame_samples_` 被 `Initialize`(:15) 在重配置（SetFrameDuration→Initialize）时改写，task 正用旧 frame_samples_ 切帧 → 帧长突变期输出半帧
- 等级判定：**P3**。`frame_samples_`(:15) 在 Initialize 设置，SetFrameDuration(:782) 会重调 Initialize（第一轮 P3-5 已指出 Initialize 非幂等泄漏）。本轮补充：即便不泄漏，`AudioProcessorTask`(:175) 的 `while (output_buffer_.size() >= frame_samples_)` 读 `frame_samples_`，与 Initialize 写 `frame_samples_`(:15) 跨线程无同步。帧长 20↔60 切换瞬间，task 用新旧混合的 frame_samples_ 切出错误长度帧喂编码器，编码器 `task->pcm.size()==encoder_frame_size_` 校验(:534)不匹配则丢帧（不崩，有保护）。
- 文件：`main/audio/processors/afe_audio_processor.cc:15`、`:175-185`；`audio_service.cc:779-784`
- 根因：`frame_samples_` 普通 int 跨线程，且 Initialize 重入时旧 task 未停就改其依赖的成员（与第一轮 P3-5/P1-3 同源——Initialize 应先停 task）。
- 触发条件与影响面：运行期 SetFrameDuration。切换瞬间几帧编码丢弃，有 :534 长度校验兜底不崩。
- 修复建议：随第一轮 P3-5 一并修——Initialize 重入先停 audio_communication 任务再改 frame_samples_/重建 afe_data_。

### P3-A3 `CustomWakeWord::Feed` 在 `input_buffer_mutex_` 内调 `multinet_->detect`（重计算）并触发回调，持锁期间执行用户回调 → 回调若回调进 audio_service 改状态可能与 Feed 持锁冲突（潜在锁顺序问题）
- 等级判定：**P3**。`Feed`(:159-213) 全程持 `input_buffer_mutex_`，在锁内 :184 `detect`（CPU 重）、:197-199 调 `wake_word_detected_callback_`（用户回调，可能 Schedule 到主任务或直接调 audio_service）。持锁执行长耗时 detect 阻塞 input 任务下一帧 Feed（同锁），且回调里若间接再触达 CustomWakeWord 的 Stop()（也要 input_buffer_mutex_）则自死锁。当前回调走 Application::Schedule 异步，不直接重入，故安全；但锁内调外部回调是脆弱设计。
- 文件：`main/audio/wake_words/custom_wake_word.cc:159-213`，回调 :197-199
- 根因：持业务锁执行重计算 + 外部回调，锁粒度过大。
- 触发条件与影响面：未来回调改为同步重入 CustomWakeWord 接口即死锁。当前异步 Schedule 安全。
- 修复建议：detect 与回调移出锁外——锁内仅取 chunk 拷贝，解锁后 detect + 回调；命中后重新取锁清 buffer。

### P3-A4 `no_audio_codec.cc` `NoAudioCodec::Write` 用 `portMAX_DELAY`(:236)、`NoAudioCodecSimplexPdm::Read` 用 `portMAX_DELAY`(:371)，违反"阻塞 API 必须给超时"红线（第一轮 P2-5 已列 :236/:371，本条确认无新增，不重复计数）
- 等级判定：与第一轮 P2-5 **重复**，此处仅备注核对一致，不计入本轮统计。

### P3-A5 `AudioInputTask` 多事件位共存时 testing 分支 `continue` 抢占 wake/processor（第一轮 P2-2 已述），本轮补充：`audio_input_need_warmup_` 是普通 bool，由 EnableVoiceProcessing(:722 主线程写)与 AudioInputTask(:265 读写)并发，warmup 标志竞争可能漏一次 120ms 预热
- 等级判定：**P3**。`audio_input_need_warmup_`(:196 声明) 主线程 EnableVoiceProcessing 写 true(:722)，AudioInputTask 读并置 false(:265-266)，普通 bool 跨核无同步。漏读则跳过 120ms 预热，AFE 首帧含未稳定数据，VAD 偶发误触发；不崩。
- 文件：`main/audio/audio_service.cc:196`、`:265-269`、`:722`
- 根因：跨核共享 bool 未用 atomic（项目红线）。
- 触发条件与影响面：进入 voice processing 瞬间。偶发漏预热，AFE 首帧质量下降。
- 修复建议：`audio_input_need_warmup_` 改 `std::atomic<bool>`。

---

## 统计

| 等级 | 数量 |
|------|------|
| P0   | 1 |
| P1   | 5 |
| P2   | 5 |
| P3   | 4 |
| **合计** | **15** |

说明：
- P0-A1（BoxAudioCodec Read/Write 无锁与 close 竞争）是本轮最硬的新发现——主路径 codec、与第一轮完全不同的竞态点、高频日常触发。
- P1-A2（Es7111 ref ring 溢出丢新留旧导致 AEC 参考漂移）补全了第一轮 P2-3 只看到的"见底填 0"，指出反向溢出才是 AEC 劣化主因。
- P1-A4（underrun 分支 output_resampler_ 无锁）是第一轮 P1-4 修复建议的必要补全。
- P3-A4 与第一轮 P2-5 重复，已标注不计入统计。
- 含 [待确认]：P1-A3（GetLastWakeWord 调用点线程归属）、P1-A4 跨线程窗口、P1-A3 OGG 流式调用方是否存在（P1-A3 OGG 实为 P1-A3 编号下的 demuxer 项，若无流式调用方降 P3）。
