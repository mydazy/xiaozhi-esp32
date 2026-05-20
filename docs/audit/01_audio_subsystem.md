# 音频子系统缺陷审计报告（01_audio_subsystem）

> 子系统概述：本报告覆盖 ESP32-S3 固件音频链路，含 `main/audio/`（AudioService 编解码调度、AFE 处理器、唤醒词 AFE/Custom、OGG 解封装、ES8311/ES7210/ES7111/NoCodec 驱动、AlarmRinger、MusicPlayer adapter、AudioDebugger）以及组件 `mydazy__esp_mp3_player`（流式 MP3 三段流水线）与 `mydazy__codec_ctrl_i2c`（I2C worker 串行化 ctrl_if）。两条核心数据流：① MIC→处理器→编码队列→Opus 编码→发送队列；② 服务器→解码队列→Opus 解码→播放队列→扬声器。esp-sr 仅审本项目调用接口用法，不审其内部实现。
>
> 审计方法：静态精读全部 33 个源文件，逐文件查空指针/越界/竞态/锁/资源泄漏/超时缺失/看门狗/采样率与编解码初始化顺序。所有行号、代码片段均来自实际文件，未编造；不确定项标 [待确认]。

---

## P0（必崩 / 砖机 / 必然崩溃 / 安全）

### P0-1 codec_ctrl_i2c 的 `_open` 永不置 `is_open`，上游二次调用 open 后所有寄存器读写被拒 → codec 初始化失败 assert 砖机
- 等级判定：**P0**。`BoxAudioCodec` 构造里所有 `assert(out_codec_if_ != NULL)` 等都依赖 ctrl_if 能正常读写芯片 ID。一旦 esp_codec_dev/es8311 driver 走标准 `ctrl->open()` 路径，`is_open` 仍为 false，`_read_reg/_write_reg` 直接返回 `WRONG_STATE`，芯片探测失败 → 构造期 `ESP_ERROR_CHECK`/`assert` abort，设备开机即重启循环（量产视角等同砖机）。
- 文件：`components/mydazy__codec_ctrl_i2c/mydazy_codec_ctrl_i2c.c:28-51`，`:128-154`
- 问题代码：
```c
static int _open(const audio_codec_ctrl_if_t *ctrl, void *cfg, int cfg_size) {
    ...
    self->dev = i2c_worker_add_device(c->worker, addr_7bit, scl_speed);
    if (self->dev == NULL) { ... return ESP_CODEC_DEV_DRV_ERR; }
    return ESP_CODEC_DEV_OK;          // ← 从不写 self->is_open = true
}
// 构造函数里：
    int ret = _open(&self->base, (void *)cfg, sizeof(...));
    ...
    self->is_open = true;             // 仅构造路径置 true
```
- 根因：`is_open` 只在工厂函数事后赋值；标准 `audio_codec_ctrl_if_t` 约定 `open` 回调自身负责把状态置 open。当前实现把 open 语义和构造耦合，任何走接口 `->open()` 的二次调用都不会复位 `is_open`，且 `_close` 会把 `is_open` 置 false 后无法再恢复。
- 触发条件与影响面：依赖上游 es8311/es7210 driver 是否在 new/enable 时调用 `ctrl->open()`。esp_codec_dev 多数 codec driver 在 `open_codec`/`set_fs` 阶段会调用 `ctrl->open()`。一旦命中，整条 BoxAudioCodec 路径开机 abort。[待确认] 需对照本项目锁定的 esp_codec_dev 版本里 es8311/es7210 是否调用 `ctrl->open`；若该版本不调用，则降为 P2（潜在升级风险）。
- 修复建议：在 `_open` 末尾、`return ESP_CODEC_DEV_OK` 前加 `self->is_open = true;`；工厂函数中删去事后赋值（或保留作幂等）。同时 `_open` 重复调用前应先判 `self->dev` 是否已存在以避免 worker 设备重复注册泄漏。

### P0-2 AfeWakeWord `wake_word_pcm_` 在检测任务与编码任务间无锁并发读写 → deque 结构损坏 / 野指针崩溃
- 等级判定：**P0**。`std::deque` 同时被一个线程 `emplace_back`/`pop_front`、另一个线程遍历+`std::move`+`clear()`，是未定义行为，弱网双连唤醒高频触发，必然出现堆破坏或 LoadProhibited 崩溃。
- 文件：`main/audio/wake_words/afe_wake_word.cc:176-183`（StoreWakeWordData 在 AudioDetectionTask 上下文写）与 `:230-259`（encode task 读+move+clear）
- 问题代码：
```c
void AfeWakeWord::StoreWakeWordData(const int16_t* data, size_t samples) {
    wake_word_pcm_.emplace_back(std::vector<int16_t>(data, data + samples)); // 检测线程，无锁
    while (wake_word_pcm_.size() > 2000 / 30) wake_word_pcm_.pop_front();
}
// encode task（另一线程）:
    for (auto& pcm: this_->wake_word_pcm_) { in_buffer = std::move(pcm); ... }
    this_->wake_word_pcm_.clear();
```
- 根因：`wake_word_pcm_` 无 mutex 保护。`AudioDetectionTask` 在唤醒命中后调 `Stop()` 清 `DETECTION_RUNNING_EVENT`，但 `fetch_with_delay(portMAX_DELAY)` 可能已在阻塞中返回下一帧并再次 `StoreWakeWordData`；与此同时 `EncodeWakeWordData` 启动的 encode task 正在遍历同一 deque。两线程对同一容器并发结构性修改。
- 触发条件与影响面：唤醒后立即 `EncodeWakeWord()`（上行送服务器声纹），若检测任务尚有一帧在途即触发。线上"唤醒后偶发重启"高度可疑此处。CustomWakeWord 同构问题较轻（`Feed` 在 input 任务单线程内 Store，但 encode task 仍并发遍历 `wake_word_pcm_`，见 P1-1）。
- 修复建议：所有访问 `wake_word_pcm_` 的点统一用一把 mutex（可复用 `wake_word_mutex_` 或新增 `pcm_mutex_`）：`StoreWakeWordData`、encode task 遍历段、`clear()` 全部加锁；或在 `EncodeWakeWordData` 启动前把 `wake_word_pcm_` 整体 `swap` 到局部变量再喂给 encode task，彻底切断共享。

---

## P1（高频崩溃 / 体验严重退化）

### P1-1 唤醒词索引 `wakenet_model_index - 1` / `command_id - 1` 未校验，越界读 `wake_words_` / `commands_` → 崩溃
- 等级判定：**P1**。索引来自 esp-sr 返回值，若为 0 或越界则 `[-1]` / 越界访问 `std::vector::operator[]` 是 UB，触发概率取决于模型返回，属高频路径。
- 文件：`main/audio/wake_words/afe_wake_word.cc:165-167`；`main/audio/wake_words/custom_wake_word.cc:188-191`
- 问题代码：
```c
last_detected_wake_word_ = wake_words_[res->wakenet_model_index - 1];   // 未判 index 范围
...
auto& command = commands_[mn_result->command_id[i] - 1];               // 未判 command_id 范围
```
- 根因：直接信任第三方库返回的 1-based 索引并 `-1`，未做 `>=1 && <=size` 边界检查；`wake_words_` 也可能为空（模型未含 wakenet 时 `wake_words_` 空但 afe SR 仍可能上报）。
- 触发条件与影响面：多唤醒词模型 / 多命令词配置下偶发非法 index；越界读 `std::vector` 取到的字符串再被回调使用，可能崩溃或脏数据。
- 修复建议：访问前校验：`if (res->wakenet_model_index < 1 || (size_t)res->wakenet_model_index > wake_words_.size()) { 记日志 continue; }`；CustomWakeWord 同理校验 `command_id`。

### P1-2 `OpusCodecTask` 用 PSRAM 栈固定在 Core 0，违反项目"PSRAM 栈 + Core 0"红线
- 等级判定：**P1**。`~/.claude/rules/esp32-memory.md` 明确"PSRAM 栈 + Core 0 是已知红线（wake_encode 案例）"。Opus 解码是实时重计算任务，PSRAM 栈在 cache 禁用窗口（flash/PSRAM 操作）被换出会导致取指停顿，长期高频运行下崩溃或音频卡顿。
- 文件：`main/audio/audio_service.cc:170-174`
- 问题代码：
```c
xTaskCreatePinnedToCoreWithCaps([](void* arg){ ... audio_service->OpusCodecTask(); ... },
    "opus_codec", 2048 * 12, this, 7, &opus_codec_task_handle_, 0, MALLOC_CAP_SPIRAM);
//                                          ↑ Core 0           ↑ PSRAM 栈
```
- 根因：为省内部 RAM 把 24KB 栈放 PSRAM，却 pin 在 Core 0，正中红线。
- 触发条件与影响面：与 Core 0 上 WiFi/flash 写并发时偶发指令取指失败；属已被项目复盘标红的高危模式。
- 修复建议：按红线把 opus_codec 挪到 Core 1（与 audio 实时任务同核），或栈改回内部 RAM。若必须 PSRAM 栈则严格只挂 Core 1。需同时复核与现有 Core 1 任务的优先级冲突。

### P1-3 `AfeWakeWord`/`CustomWakeWord` 析构 `destroy(afe_data_)` 时检测任务仍在 `fetch_with_delay` 阻塞，访问已释放句柄 → use-after-free
- 等级判定：**P1**。析构未先停并 join `audio_detection` 任务；该任务循环里持续 `afe_iface_->fetch_with_delay(afe_data_, portMAX_DELAY)`，析构释放 `afe_data_` 后任务下一轮立刻 UAF。
- 文件：`main/audio/wake_words/afe_wake_word.cc:18-36`（析构）与 `:148-174`（永不退出的检测任务）
- 问题代码：
```c
AfeWakeWord::~AfeWakeWord() {
    if (afe_data_ != nullptr) afe_iface_->destroy(afe_data_);  // 未先停 AudioDetectionTask
    ...
}
void AfeWakeWord::AudioDetectionTask() {
    while (true) { ... afe_iface_->fetch_with_delay(afe_data_, portMAX_DELAY); ... } // 无退出条件
}
```
- 根因：检测任务是 `while(true)` 无退出标志，析构也不通知/join。WakeWord 对象虽多为单例长生命周期，但 `SetModelsList` 在 mode 切换时可 `make_unique` 重建 `wake_word_`（audio_service.cc:872-878），旧对象析构即触发。
- 触发条件与影响面：运行期切换 afe/custom 唤醒模式或重配置时。`AfeAudioProcessor` 析构同样存在（afe_audio_processor.cc:79-84 destroy 前不停 `audio_communication` 任务）。
- 修复建议：增加退出事件位，析构时 set 该位 + `fetch_with_delay` 用有限超时（如 100ms）轮询退出标志，join 任务后再 `destroy(afe_data_)`；保存 task handle 以便 `eTaskGetState` 等待 deleted。

### P1-4 `SetDecodeSampleRate` 在 `decoder_mutex_` 外重开解码器，与 `OpusCodecTask` 解码并发竞态
- 等级判定：**P1**。重开 decoder 的关键段未全程持 `decoder_mutex_`：先在锁内 close 并置 nullptr、解锁，然后在**锁外**调用 `esp_opus_dec_open` 并写 `opus_decoder_`/`decoder_*` 成员。OpusCodecTask 在 `decoder_mutex_` 内读 `opus_decoder_`，存在窗口读到半初始化句柄。
- 文件：`main/audio/audio_service.cc:576-610`
- 问题代码：
```c
std::unique_lock<std::mutex> decoder_lock(decoder_mutex_);
if (opus_decoder_ != nullptr) { esp_opus_dec_close(opus_decoder_); opus_decoder_ = nullptr; }
decoder_lock.unlock();                       // ← 解锁
esp_opus_dec_cfg_t opus_dec_cfg = OPUS_DEC_CFG(sample_rate, frame_duration);
auto ret = esp_opus_dec_open(&opus_dec_cfg, ..., &opus_decoder_);  // 锁外写 opus_decoder_
...
decoder_sample_rate_ = sample_rate; decoder_frame_size_ = ...;     // 锁外写解码状态
```
- 根因：为避免持锁期间长时间 open 而提前解锁，但 `opus_decoder_` 与 `decoder_frame_size_` 是 OpusCodecTask 解码路径直接依赖的共享态。`SetDecodeSampleRate` 由解码循环自身调用（:386 同线程），但 `output_resampler_` 重建段（:599-608）完全无锁，而 underrun 补偿分支也用 `output_resampler_`。
- 触发条件与影响面：解码线程内自调用时同线程安全；但 `ResetDecoder`（外部线程，:831 持 decoder_mutex_ 只 reset）与 SetFrameDuration/SetDecodeSampleRate 跨线程交错时，`output_resampler_` 无保护 → 解码用到正在 close 的 resampler 句柄崩溃。
- 修复建议：把 `opus_decoder_` 重开与 `decoder_*`/`output_resampler_` 写全部纳入 `decoder_mutex_`（接受短暂持锁），或为 resampler 单独加锁并在 OpusCodecTask 使用 resampler 处同样加锁。

### P1-5 MP3 `AbortAndJoin` 等待 2s 超时后直接 return 不释放 ring，且 `Play` 仅再等 20s 后放弃；任务"卡死在 TLS read"时 ring 句柄泄漏
- 等级判定：**P1**。卡死场景（4G 弱网 TLS read 阻塞 25s）下 `AbortAndJoin` 提前返回不删 ring；后续 `Play` 若 20s 内任务仍未退出则 `return false`，但 ring 与卡死任务都保留，反复触发将持续泄漏 512KB+32KB PSRAM ring，最终 PSRAM 耗尽起播失败。
- 文件：`components/mydazy__esp_mp3_player/mp3_player.cc:238-257`，`:92-110`
- 问题代码：
```c
for (int i=0;i<200 && active_tasks_.load()>0;i++) vTaskDelay(pdMS_TO_TICKS(10)); // 2s
int remaining = active_tasks_.load();
if (remaining > 0) { ESP_LOGW(...,"still alive after 2 s"); return; } // 不删 ring，running_ 仍 true
```
- 根因：HTTP `Read` 超时设为 25s（`kHttpTimeoutMs=25000`），远超 AbortAndJoin 的 2s 等待；任务退出依赖 read 返回。设计上承认"可能泄漏"但无回收路径。
- 触发条件与影响面：弱网/服务端 hang 时 Stop→Play 反复，PSRAM 单调下降。属体验严重退化（最终无法播放音乐 + 可能拖垮其它 PSRAM 分配）。
- 修复建议：把 `kHttpTimeoutMs` 降到与 join 等待量级匹配（如 5s），或给 Http 增加可中断 `Abort()` 接口在 abort 时主动关 socket 唤醒 read；任务退出后再删 ring。至少应在多次卡死后强制 `running_=false` 并接受句柄交 idle 回收前打点告警。

### P1-6 `BoxAudioCodec::CalibrateMicOnce` 用裸指针 `ctx`（栈上 `tone`/`Ctx`）喂给异步播放任务，函数返回风险 + 二次播放任务句柄重入
- 等级判定：**P1**。`measure()` lambda 每次 `xTaskCreate` 一个 "calib" 任务读取 `&ctx`（指向 `CalibrateMicOnce` 栈帧）。`measure` 内 `vTaskDelay(150)+Read+vTaskDelay(400)` 等待，但并未 join "calib" 任务；第二轮 `measure()` 再次创建同名任务，若上一个 calib 任务因调度延迟未结束，两个任务并发 `Write` 同一 codec + 共享 `tone`。函数返回后栈帧失效，残留 calib 任务访问悬垂 `ctx`。
- 文件：`main/audio/codecs/box_audio_codec.cc:288-324`
- 问题代码：
```c
struct Ctx { BoxAudioCodec* self; std::vector<int16_t>* tone; } ctx{this, &tone};
auto measure = [&]() -> int32_t {
    xTaskCreate([](void* a){ auto* c=(Ctx*)a; c->self->Write(...); vTaskDelete(NULL);}, "calib",4096,&ctx,5,NULL);
    vTaskDelay(pdMS_TO_TICKS(150)); ... vTaskDelay(pdMS_TO_TICKS(400)); // 不 join calib 任务
};
```
- 根因：异步任务生命周期未与栈帧/measure 调用绑定，靠 sleep 估算"应该结束了"。
- 触发条件与影响面：出厂首次开机一次性校准。CPU 高负载时 calib 任务被延后即悬垂访问；只在出厂烧录触发，但失败即影响 MIC 增益标定写入错误值（长期影响所有对话拾音）。
- 修复建议：calib 任务改同步直接调用 `Write`（本就阻塞 I2S，无需独立任务），或用 task handle + `eTaskGetState` join 后再返回；`tone`/`ctx` 改为成员或 `std::shared_ptr` 转移所有权。

---

## P2（偶发 / 边缘场景）

### P2-1 OGG 解封装 `OpusHead` 解析最少只校验 `>=8`，读 sample_rate 时虽判 `>=19` 但 `seg_table[i]` 累加 `body_size` 无溢出风险但回调指针为内部栈缓冲、`packet_continued` 跨页拼接逻辑可越界
- 等级判定：**P2**。`packet_buf[8192]` 有上限检查（ogg_demuxer.cc:210），但跨页 continued 包累积时 `packet_len` 仅在单段拷贝前检查 `+seg_len`，多段连续 255 拼接到接近 8192 边界仍受保护——主风险在 `on_demuxer_finished_` 回调把 `ctx_.packet_buf` 指针+`packet_len` 直接传出，调用方 `PlaySound` 的 lambda 立即 `memcpy` 是安全的；但若未来回调异步保存指针则悬垂。当前用法安全，记为边缘隐患。
- 文件：`main/audio/demuxer/ogg_demuxer.cc:269-272`；`main/audio/audio_service.cc:807-814`
- 根因：回调传内部缓冲裸指针，依赖调用方同步拷贝。
- 触发条件与影响面：仅当回调被改为异步使用。当前 PlaySound 同步 memcpy 安全。
- 修复建议：注释里明确"data 仅在回调内有效，须同步拷贝"；或回调直接传 `std::vector`。

### P2-2 `AudioInputTask` 读失败统一 `vTaskDelay(10ms)` 但唤醒/处理器均 enable 时只喂一种后 `continue`，10ms 帧节拍可能漂移
- 等级判定：**P2**。`bits & (WAKE_WORD|PROCESSOR)` 分支读 160 samples(10ms) 喂 wake_word 与 processor，正常；但 testing 分支 `continue` 与处理器分支独立，事件位同时置位时 testing 优先消费一帧，wake/processor 当轮丢帧。偶发拾音漏帧，不崩。
- 文件：`main/audio/audio_service.cc:272-307`
- 根因：多事件位共存时单轮只处理一类数据再 `continue`。
- 触发条件与影响面：audio testing 与正常拾音不应同时开启，正常态不触发。
- 修复建议：确认 testing 与 wake/processor 互斥（状态机层面保证）；或在一轮内对各 enabled 流分别 Read。

### P2-3 `Es7111AudioCodec::Read` 软件回采 ref 不足时填 0，长期 MIC 帧率 > 回采帧率会导致 AEC 参考漂移
- 等级判定：**P2**。`ref_ringbuf_` 由 `Write` 填、`Read` 取，二者帧率不一致（输出暂停而输入持续）时 ref 见底填 0，AEC 参考与实际播放错位，回声消除质量下降；不崩。
- 文件：`main/audio/codecs/es7111_audio_codec.cc:255-282`，`:309-313`
- 根因：Write/Read 无同步节拍，ref ring 无对齐机制。
- 触发条件与影响面：播放停止但拾音继续（如 TTS 结束瞬间）。仅影响 AEC 效果。
- 修复建议：在 EnableOutput(false) 时清空 ref_ringbuf_，避免陈旧参考；或记录 underrun 计数监控漂移。

### P2-4 `PushTaskToEncodeQueue` 时间戳逻辑：`timestamp_queue_.size() > MAX` 时丢弃 timestamp 但仍 `pop_front`，且 size 检查与 pop 之间语义偏差
- 等级判定：**P2**。当 `timestamp_queue_.size() <= MAX_TIMESTAMPS_IN_QUEUE(3)` 取 front，否则警告丢弃，但两分支都 `pop_front()`。当队列恰好 > 3 时丢弃当前 timestamp（不赋给 task）却仍弹出，导致后续帧时间戳整体错位一格。Server AEC 时间对齐偏差。
- 文件：`main/audio/audio_service.cc:620-627`
- 问题代码：
```c
if (timestamp_queue_.size() <= MAX_TIMESTAMPS_IN_QUEUE) task->timestamp = timestamp_queue_.front();
else ESP_LOGW(...,"full, dropping timestamp");
timestamp_queue_.pop_front();   // 两分支都弹
```
- 根因：溢出保护的丢弃策略与 pop 时机耦合导致错位而非简单丢弃最旧。
- 触发条件与影响面：仅 `CONFIG_USE_SERVER_AEC` 且 timestamp 堆积时；影响 server 端 AEC 对齐，不崩。
- 修复建议：溢出时应循环 pop 到 size<=MAX 再取 front，保证 task 永远拿到对齐的最新 timestamp；或明确丢弃最旧若干个。

### P2-5 `NoAudioCodec::Write` 用 `portMAX_DELAY` 阻塞 I2S 写，违反"阻塞 API 必须给超时"红线
- 等级判定：**P2**（NoAudioCodec 多为开发板路径，非量产 P30 主路径，故非 P1）。`i2s_channel_write(..., portMAX_DELAY)` 若 TX 通道异常永久阻塞 OutputTask，喂狗任务若依赖该核可触发看门狗。
- 文件：`main/audio/codecs/no_audio_codec.cc:236`；`:371`（Pdm Read 同样 portMAX_DELAY）
- 根因：裸用 portMAX_DELAY。
- 触发条件与影响面：仅使用 NoAudioCodec 的板型；TX/RX DMA 异常时阻塞。
- 修复建议：改有限超时（输出实时 100ms / 输入 200ms，与 BoxCodec/Es7111 的 500ms 风格统一），返回写入字节数让上层处理 underrun。

### P2-6 `AfeWakeWord::Initialize` 等创建 `audio_detection` 任务后才返回，但 `models_->num == -1` 检查在 `esp_srmodel_init` 外部传入 list 时不生效；模型加载失败仍继续 create AFE
- 等级判定：**P2**。当外部传入 `models_list` 且其有效但**不含 wakenet** 时，`wake_words_` 为空，`afe_config_init(..., models_, AFE_TYPE_SR,...)` 仍创建，后续唤醒永不命中且 `wake_words_[idx-1]` 必越界（与 P1-1 叠加）。
- 文件：`main/audio/wake_words/afe_wake_word.cc:48-94`
- 根因：只校验 `models_->num == -1`，未校验是否真的过滤到 wakenet 模型 (`wakenet_model_` 可能仍为 NULL / `wake_words_` 为空)。
- 触发条件与影响面：模型分区缺 wakenet 或配置错位。设备能启动但唤醒功能静默失效。
- 修复建议：`if (wakenet_model_ == NULL || wake_words_.empty()) { ESP_LOGE; return false; }` 提前失败，让上层回退到 null wake_word。

---

## P3（潜在远期风险）

### P3-1 `g_opus_frame_duration_ms` 为全局非原子 int，`SetFrameDuration` 在主线程改、`OPUS_FRAME_DURATION_MS` 宏在多任务读
- 等级判定：**P3**。int 读写在 32 位 ESP32 上单字访问基本原子，但 `MAX_SEND_PACKETS_IN_QUEUE` 等宏运行期展开依赖它，切换瞬间队列上限计算可能短暂不一致。
- 文件：`main/audio/audio_service.cc:35`；`audio_service.h:41-46`
- 根因：全局可变状态 + 宏内联计算，跨任务无内存序保证。
- 影响面：百度 20ms↔60ms 切换瞬间偶发队列水位判定抖动，不崩。
- 修复建议：改 `std::atomic<int>`，或切换帧长时确保 audio 处于空闲态（文档已要求"仅在 audio 空闲态调用"，建议加运行期 assert/IsIdle 校验）。

### P3-2 `AudioService::Stop` join 任务超时后"handle leaked"且不置 nullptr，注释依赖 idle 回收；若 Start 重入会覆盖 handle
- 等级判定：**P3**。Stop 超时分支注释明确接受 handle 泄漏由 RTOS idle 回收，但 `Start` 重新 create 会覆盖成员 handle，旧任务若仍存活则其 `vTaskDelete(NULL)` 自删，逻辑自洽；风险在统计/调试句柄失真。
- 文件：`main/audio/audio_service.cc:193-204`
- 根因：超时不强删任务（FreeRTOS 删他人任务本身有风险，设计取舍合理）。
- 影响面：极端超时场景句柄计数失真，功能不受影响。
- 修复建议：保留现状，建议在超时分支增加 `audio_input_task_handle_=nullptr` 前的诊断打点，避免与 Start 重入混淆。

### P3-3 `MusicPlayer::OnMp3Event` 在 worker task 上下文 `Schedule` 回主任务，但 `g_current_title` 跨线程读（worker 读、主线程 Play/Stop 写）无同步
- 等级判定：**P3**。`g_current_title` 是普通 `std::string`，`Play`（主线程）写、`OnMp3Event`（worker 线程）读拷贝。`std::string` 拷贝期间被改写是数据竞争，理论 UB；实际 title 短、事件少，崩溃概率低。
- 文件：`main/audio/music_player.cc:74`，`:77-82`，`:153`，`:164`
- 根因：共享 std::string 无锁。
- 影响面：极端时序下 title 显示错乱或崩溃。
- 修复建议：title 改为事件内传参（mp3_event_cb 的 msg 已带 title），或加锁/原子快照。

### P3-4 `AudioDebugger` UDP 阻塞 `sendto` 在拾音热路径 `ReadAudioData` 内同步调用
- 等级判定：**P3**。仅 `CONFIG_USE_AUDIO_DEBUGGER` 开启（调试构建），但 `sendto` 默认阻塞，弱网时拖慢 AudioInputTask 帧节拍。
- 文件：`main/audio/processors/audio_debugger.cc:54-66`；`main/audio/audio_service.cc:245-251`
- 根因：调试发送同步执行在实时拾音路径。
- 影响面：仅调试构建；量产关闭无影响。
- 修复建议：socket 设非阻塞（O_NONBLOCK）或独立低优先级任务发送；明确该选项不得用于量产。

### P3-5 `EnableDeviceAec` 在 `audio_processor_` 未 Start 时即 Initialize，但 `Initialize` 内部直接 `xTaskCreatePinnedToCore` 创建 `audio_communication` 任务，重复调用 Initialize（SetFrameDuration 路径）会重复 create 任务且不回收旧任务/旧 afe_data_
- 等级判定：**P3**（与 P1-3 相关，但此处聚焦重复 Initialize 的句柄/任务泄漏）。`SetFrameDuration`（audio_service.cc:782）和 `EnableVoiceProcessing`/`EnableDeviceAec` 都可能调 `audio_processor_->Initialize`，而 `AfeAudioProcessor::Initialize` 每次都 `create_from_config` 新 afe_data_ 并 create 新任务，旧的从不销毁。
- 文件：`main/audio/processors/afe_audio_processor.cc:13-77`；`main/audio/audio_service.cc:779-784`，`:752-760`
- 根因：Initialize 非幂等，无"已初始化则先销毁"保护；`audio_processor_initialized_` 标志只挡 audio_service 层部分路径，`SetFrameDuration` 绕过它直接 Initialize。
- 触发条件与影响面：运行期改帧长（百度协议切换）会泄漏 afe_data_（PSRAM 大块）+ 多一个 `audio_communication` 任务。多次切换累积泄漏。
- 修复建议：`AfeAudioProcessor::Initialize` 开头若 `afe_data_!=nullptr` 先停任务+`destroy`；或拆分 create-once 与 reconfig 两个接口。`SetFrameDuration` 重启 processor 时应走完整 destroy→create。

---

## 统计

| 等级 | 数量 |
|------|------|
| P0   | 2 |
| P1   | 6 |
| P2   | 6 |
| P3   | 5 |
| **合计** | **19** |

说明：P0-1 含 [待确认]（取决于锁定版本 esp_codec_dev 是否回调 `ctrl->open`，若不回调降为 P2）；P2-1、P2-2 为当前用法安全的边缘隐患，列出以备后续改动参考。
