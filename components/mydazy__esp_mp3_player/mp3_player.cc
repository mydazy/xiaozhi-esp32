// mp3_player — streaming MP3 → PCM, project-agnostic.
//
// Pipeline:
//   DownloadTask (Core 0, PSRAM stack 6 KB, prio 1)
//     ├─ IHttpClient::Open/Read → xRingbufferSend
//     └─ end / abort → exit + set download_done_
//
//   DecodeTask (Core 0, internal-RAM stack 10 KB, prio 7)
//     ├─ xRingbufferReceiveUpTo → esp_audio_dec_process
//     ├─ resample (lazy, sample_rate from first frame)
//     ├─ stereo → mono fold
//     └─ IAudioOutput::OutputData

#include "mp3_player.h"

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/idf_additions.h>

#include <cstring>

extern "C" {
#include "esp_audio_dec.h"
#include "esp_audio_dec_default.h"
#include "esp_mp3_dec.h"
#include "esp_ae_rate_cvt.h"
}

namespace mydazy {

namespace {

constexpr const char* TAG = "Mp3Player";

std::atomic<bool> g_mp3_registered{false};

void EnsureMp3DecoderRegistered() {
    bool expected = false;
    if (g_mp3_registered.compare_exchange_strong(expected, true)) {
        auto ret = esp_mp3_dec_register();
        if (ret != ESP_AUDIO_ERR_OK) {
            ESP_LOGE(TAG, "esp_mp3_dec_register failed: %d", ret);
            g_mp3_registered.store(false);
        } else {
            ESP_LOGI(TAG, "MP3 decoder registered");
        }
    }
}

}  // namespace

Mp3Player& Mp3Player::GetInstance() {
    static Mp3Player instance;
    return instance;
}

void Mp3Player::Initialize(IAudioOutput* audio,
                           IHttpFactory* http,
                           const Callbacks& callbacks) {
    audio_ = audio;
    http_ = http;
    callbacks_ = callbacks;
    EnsureMp3DecoderRegistered();
}

std::string Mp3Player::GetCurrentTitle() const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    return current_title_;
}

void Mp3Player::EmitError(const char* status, const char* message) {
    if (callbacks_.on_error) callbacks_.on_error(status, message);
}

bool Mp3Player::Play(const std::string& url, const std::string& title, std::string* err_msg) {
    auto set_err = [err_msg](const char* s) { if (err_msg) *err_msg = s; };

    if (!audio_ || !http_) {
        ESP_LOGE(TAG, "Play failed: not initialized");
        set_err("player not initialized");
        return false;
    }
    if (url.empty()) {
        ESP_LOGW(TAG, "Play failed: empty url");
        set_err("URL is empty");
        return false;
    }
    if (url.compare(0, 7, "http://") != 0 && url.compare(0, 8, "https://") != 0) {
        ESP_LOGW(TAG, "Play failed: unsupported scheme (%s)", url.c_str());
        set_err("URL must start with http:// or https://");
        return false;
    }

    AbortAndJoin();

    // Belt-and-suspenders: AbortAndJoin already waits up to 2 s; if a TLS read
    // is still blocking, give it another ~20 s before giving up entirely.
    for (int i = 0; i < 2000 && active_tasks_.load(std::memory_order_acquire) > 0; i++) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (active_tasks_.load(std::memory_order_acquire) > 0) {
        ESP_LOGE(TAG, "Play failed: previous tasks still alive (stuck in TLS read)");
        set_err("previous playback not finished, retry later");
        return false;
    }
    if (compressed_ring_) {
        vRingbufferDeleteWithCaps(compressed_ring_);
        compressed_ring_ = nullptr;
    }
    if (pcm_ring_) {
        vRingbufferDeleteWithCaps(pcm_ring_);
        pcm_ring_ = nullptr;
    }

    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        current_url_ = url;
        current_title_ = title;
    }

    compressed_ring_ = xRingbufferCreateWithCaps(
        kCompressedRingSize, RINGBUF_TYPE_BYTEBUF, MALLOC_CAP_SPIRAM);
    if (!compressed_ring_) {
        ESP_LOGE(TAG, "Play failed: compressed ring alloc failed (%zu bytes)",
                 kCompressedRingSize);
        set_err("PSRAM exhausted");
        return false;
    }
    pcm_ring_ = xRingbufferCreateWithCaps(
        kPcmRingSize, RINGBUF_TYPE_BYTEBUF, MALLOC_CAP_SPIRAM);
    if (!pcm_ring_) {
        ESP_LOGE(TAG, "Play failed: pcm ring alloc failed (%zu bytes)", kPcmRingSize);
        vRingbufferDeleteWithCaps(compressed_ring_);
        compressed_ring_ = nullptr;
        set_err("PSRAM exhausted");
        return false;
    }

    abort_.store(false, std::memory_order_release);
    paused_.store(false, std::memory_order_release);
    download_done_.store(false, std::memory_order_release);
    decode_done_.store(false, std::memory_order_release);
    prebuffered_.store(false, std::memory_order_release);
    active_tasks_.store(0, std::memory_order_release);
    position_ms_.store(0, std::memory_order_release);
    total_duration_ms_.store(0, std::memory_order_release);
    body_length_.store(0, std::memory_order_release);
    running_.store(true, std::memory_order_release);

    auto cleanup_rings = [this]() {
        if (compressed_ring_) {
            vRingbufferDeleteWithCaps(compressed_ring_);
            compressed_ring_ = nullptr;
        }
        if (pcm_ring_) {
            vRingbufferDeleteWithCaps(pcm_ring_);
            pcm_ring_ = nullptr;
        }
    };

    // Download: PSRAM stack OK — one-shot I/O at low priority.
    // 🔴 修红线（2026-04-28 P0）：Core 0 → Core 1（PSRAM 栈保持 · 同 wifi_ap 已验证模式）。
    // 根因：CLAUDE.md "PSRAM 栈 + Core 0" 红线 · 虽然 mp3_dl 大部分时间在 socket recv（cache 禁用期被 sleep 不被调度），
    //       但严格遵守红线规则更稳。挪 Core 1 与 wifi_ap "config_ok"（PSRAM Core 1 已验证）同模式。
    TaskHandle_t download_task = nullptr;
    active_tasks_.fetch_add(1, std::memory_order_acq_rel);
    BaseType_t ret = xTaskCreatePinnedToCoreWithCaps(
        DownloadThunk, "mp3_dl", 6 * 1024, this, 1, &download_task, 1, MALLOC_CAP_SPIRAM);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Play failed: download task creation failed");
        active_tasks_.fetch_sub(1, std::memory_order_acq_rel);
        cleanup_rings();
        running_.store(false, std::memory_order_release);
        set_err("failed to create download task");
        return false;
    }

    // Decode: prio 7 (== opus_codec), Core 0, internal-RAM stack — see README "Why internal-RAM".
    TaskHandle_t decode_task = nullptr;
    active_tasks_.fetch_add(1, std::memory_order_acq_rel);
    ret = xTaskCreatePinnedToCore(
        DecodeThunk, "mp3_dec", 10 * 1024, this, 7, &decode_task, 0);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Play failed: decode task creation failed");
        active_tasks_.fetch_sub(1, std::memory_order_acq_rel);
        abort_.store(true, std::memory_order_release);
        set_err("failed to create decode task");
        return false;
    }

    // Output: prio 10 / Core 1 / internal-RAM stack 4 KB.
    // 修复杂音：原本 mp3_out @ Core0 P7 与 mp3_dec @ Core0 P7 同优先级时间片轮转，
    // mp3_dec 跑 RFFT 重计算时 mp3_out 拿不到 CPU → pcm_ring 见底 → I2S DMA underrun → 爆音。
    // 移到 Core 1 P10（与 audio_output 同位，MP3 期间 audio_output 在 cv 上 wait 不抢），
    // 与 mp3_dec 分核运行，且优先级高于 LVGL P5 / AFE P8（MP3 期间 AFE 也闲）→ 永远抢占消费。
    TaskHandle_t output_task = nullptr;
    active_tasks_.fetch_add(1, std::memory_order_acq_rel);
    ret = xTaskCreatePinnedToCore(
        OutputThunk, "mp3_out", 4 * 1024, this, 10, &output_task, 1);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Play failed: output task creation failed");
        active_tasks_.fetch_sub(1, std::memory_order_acq_rel);
        abort_.store(true, std::memory_order_release);
        set_err("failed to create output task");
        return false;
    }

    ESP_LOGI(TAG, "Play: %s (%s)", title.c_str(), url.c_str());
    if (err_msg) err_msg->clear();
    return true;
}

void Mp3Player::Stop() {
    // Stop 前先停 paused 超时定时器，防止 AbortAndJoin 期间被回调 emit 误操作
    if (pause_timeout_timer_) {
        esp_timer_stop(static_cast<esp_timer_handle_t>(pause_timeout_timer_));
    }
    paused_.store(false, std::memory_order_release);  // 让任何 paused 闸口立即放行退出
    AbortAndJoin();
    ESP_LOGI(TAG, "Stop");
}

void Mp3Player::AbortAndJoin() {
    if (!running_.load(std::memory_order_acquire)) {
        if (compressed_ring_) {
            vRingbufferDeleteWithCaps(compressed_ring_);
            compressed_ring_ = nullptr;
        }
        if (pcm_ring_) {
            vRingbufferDeleteWithCaps(pcm_ring_);
            pcm_ring_ = nullptr;
        }
        return;
    }
    abort_.store(true, std::memory_order_release);

    for (int i = 0; i < 200 && active_tasks_.load(std::memory_order_acquire) > 0; i++) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    int remaining = active_tasks_.load(std::memory_order_acquire);
    if (remaining > 0) {
        ESP_LOGW(TAG, "AbortAndJoin: %d task(s) still alive after 2 s (likely TLS read)", remaining);
        // Don't free rings — tasks may still be writing to them.
        return;
    }

    running_.store(false, std::memory_order_release);
    if (compressed_ring_) {
        vRingbufferDeleteWithCaps(compressed_ring_);
        compressed_ring_ = nullptr;
    }
    if (pcm_ring_) {
        vRingbufferDeleteWithCaps(pcm_ring_);
        pcm_ring_ = nullptr;
    }
}

void Mp3Player::DownloadThunk(void* arg) {
    auto* self = static_cast<Mp3Player*>(arg);
    self->DownloadLoop();
    self->active_tasks_.fetch_sub(1, std::memory_order_acq_rel);
    vTaskDeleteWithCaps(nullptr);
}

void Mp3Player::DownloadLoop() {
    std::string url;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        url = current_url_;
    }

    // Read scratch in PSRAM — TLS handshake leaves limited stack room.
    // 用途：HTTP 下行临时读 buffer；生命周期：DownloadLoop；申请方：本函数
    constexpr size_t kReadChunk = 2048;
    char* buf = static_cast<char*>(heap_caps_malloc(kReadChunk, MALLOC_CAP_SPIRAM));
    if (!buf) {
        ESP_LOGE(TAG, "Download buffer alloc failed");
        EmitError("Playback failed", "PSRAM exhausted");
        download_done_.store(true, std::memory_order_release);
        return;
    }

    size_t total_read = 0;        // 已成功写入 ringbuffer 的字节数（也是 Range 续传起点）
    bool   eof_reached = false;   // 正常 EOF 标记
    int    last_status = 0;       // 末次 HTTP status，用于错误归因
    bool   header_emitted_total = false;  // body_length 仅首次（200）赋值，后续 206 不重置
    // 速率统计窗口：每 kStatLogIntervalMs 打一次 [DL速度 | ring占用 | 进度]
    constexpr int64_t kStatLogIntervalMs = 2000;
    int64_t stat_window_start_us = esp_timer_get_time();
    size_t  stat_window_bytes = 0;

    for (int retry = 0; retry <= kHttpRetryMax && !eof_reached; retry++) {
        if (abort_.load(std::memory_order_acquire) ||
            !running_.load(std::memory_order_acquire)) break;

        auto http = http_->CreateHttp();
        if (!http) {
            ESP_LOGE(TAG, "Download: http factory returned null (retry %d)", retry);
            if (retry == kHttpRetryMax) {
                EmitError("Playback failed", "HTTP transport unavailable");
                abort_.store(true, std::memory_order_release);
            } else {
                vTaskDelay(pdMS_TO_TICKS(500));
                continue;
            }
            break;
        }
        http->SetTimeout(kHttpTimeoutMs);

        // 续传：从 total_read 字节开始
        if (total_read > 0) {
            char range_buf[64];
            snprintf(range_buf, sizeof(range_buf), "bytes=%u-", (unsigned)total_read);
            http->SetHeader("Range", range_buf);
            ESP_LOGI(TAG, "Download retry %d: Range %s (resume from %u)",
                     retry, range_buf, (unsigned)total_read);
        }

        if (!http->Open("GET", url)) {
            ESP_LOGW(TAG, "Download: HTTP open failed (retry %d)", retry);
            http->Close();
            if (retry == kHttpRetryMax) {
                EmitError("Playback failed", "Failed to connect to server");
                abort_.store(true, std::memory_order_release);
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }

        int status = http->GetStatusCode();
        last_status = status;
        // 首次允许 200；续传期望 206（Partial Content），但部分 server 仍返 200 + 全文，
        // 此时 total_read 对齐失效——保险起见直接 abort（避免数据错位导致解码崩）
        bool ok_status = (total_read == 0 && status == 200) ||
                         (total_read > 0 && status == 206);
        if (!ok_status) {
            ESP_LOGE(TAG, "Download: HTTP %d (retry %d, resume offset %u)",
                     status, retry, (unsigned)total_read);
            http->Close();
            // 4xx 永久错误立即退出，5xx / 网络错误才重试
            if (status >= 400 && status < 500) {
                char err[64];
                if (status == 404)      snprintf(err, sizeof(err), "Audio file not found (404)");
                else if (status == 403) snprintf(err, sizeof(err), "Forbidden (403)");
                else if (status == 416) snprintf(err, sizeof(err), "Range not satisfiable (416)");
                else                    snprintf(err, sizeof(err), "HTTP error (%d)", status);
                EmitError("Playback failed", err);
                abort_.store(true, std::memory_order_release);
                break;
            }
            if (retry == kHttpRetryMax) {
                char err[64];
                snprintf(err, sizeof(err), "Server error (%d)", status);
                EmitError("Playback failed", err);
                abort_.store(true, std::memory_order_release);
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }

        if (!header_emitted_total) {
            size_t total_len = http->GetBodyLength();
            ESP_LOGI(TAG, "Download started: %u bytes", (unsigned)total_len);
            body_length_.store(total_len, std::memory_order_release);
            header_emitted_total = true;
        }

        // 内层：read loop
        int64_t pause_started_us = 0;
        bool    inner_break_for_retry = false;
        while (!abort_.load(std::memory_order_acquire) &&
               running_.load(std::memory_order_acquire)) {
            // Pause 闸口：>= 5s 主动关连接，触发外层 Range 重连
            if (paused_.load(std::memory_order_acquire)) {
                if (pause_started_us == 0) pause_started_us = esp_timer_get_time();
                int64_t elapsed_ms = (esp_timer_get_time() - pause_started_us) / 1000;
                if (elapsed_ms >= kPauseCloseConnMs) {
                    ESP_LOGI(TAG, "Pause >= %dms, closing socket (will Range-resume)",
                             kPauseCloseConnMs);
                    inner_break_for_retry = true;
                    break;
                }
                vTaskDelay(pdMS_TO_TICKS(100));
                continue;
            }
            pause_started_us = 0;  // 已 Resume，复位

            int n = http->Read(buf, kReadChunk);
            if (n < 0) {
                ESP_LOGW(TAG, "Download: Read error %d at offset %u (will retry)",
                         n, (unsigned)total_read);
                inner_break_for_retry = true;
                break;
            }
            if (n == 0) {
                eof_reached = true;
                break;
            }

            size_t written = 0;
            while (written < (size_t)n && !abort_.load(std::memory_order_acquire)) {
                size_t chunk = (size_t)n - written;
                BaseType_t ok = xRingbufferSend(compressed_ring_, buf + written, chunk,
                                                 pdMS_TO_TICKS(200));
                if (ok == pdTRUE) written += chunk;
                // else: ring full, retry next iteration
            }
            total_read += written;
            stat_window_bytes += written;

            // 首帧预热闸口：累计 >= 32KB 放行 OutputLoop 起播
            if (!prebuffered_.load(std::memory_order_acquire) &&
                total_read >= kPrebufferThreshold) {
                prebuffered_.store(true, std::memory_order_release);
                ESP_LOGI(TAG, "Prebuffer ready: %u bytes", (unsigned)total_read);
            }

            // 周期性速率日志：[DL速度 | comp占用 | pcm占用 | 已下/总 | 播放进度]
            // 4G 弱网调试命脉——脱网时 DL 速度归零、comp 占用线性下降可一眼看出
            int64_t now_us = esp_timer_get_time();
            if ((now_us - stat_window_start_us) / 1000 >= kStatLogIntervalMs) {
                int64_t elapsed_ms = (now_us - stat_window_start_us) / 1000;
                int kbps = (int)((stat_window_bytes * 1000) / (elapsed_ms * 1024 + 1));
                size_t comp_free = compressed_ring_
                    ? xRingbufferGetCurFreeSize(compressed_ring_) : 0;
                size_t comp_used = kCompressedRingSize - comp_free;
                size_t pcm_free = pcm_ring_
                    ? xRingbufferGetCurFreeSize(pcm_ring_) : 0;
                size_t pcm_used = kPcmRingSize - pcm_free;
                size_t total_len = body_length_.load(std::memory_order_acquire);
                int pos_ms = position_ms_.load(std::memory_order_acquire);
                int dur_ms = total_duration_ms_.load(std::memory_order_acquire);
                ESP_LOGI(TAG,
                         "DL %d KB/s | comp %u/%u KB | pcm %u/%u KB | %u/%u B | %d.%ds/%d.%ds",
                         kbps,
                         (unsigned)(comp_used / 1024), (unsigned)(kCompressedRingSize / 1024),
                         (unsigned)(pcm_used / 1024),  (unsigned)(kPcmRingSize / 1024),
                         (unsigned)total_read, (unsigned)total_len,
                         pos_ms / 1000, (pos_ms % 1000) / 100,
                         dur_ms / 1000, (dur_ms % 1000) / 100);
                stat_window_start_us = now_us;
                stat_window_bytes = 0;
            }
        }

        http->Close();
        if (eof_reached) break;
        if (!inner_break_for_retry) break;  // abort 或 running 转 false
        if (retry == kHttpRetryMax) {
            ESP_LOGE(TAG, "Download: retry exhausted at offset %u", (unsigned)total_read);
            EmitError("网络中断", "音乐流读取失败");
            abort_.store(true, std::memory_order_release);
            break;
        }
        // retry 间隔：指数退避（500ms / 1s / 2s）防止瞬时风暴
        vTaskDelay(pdMS_TO_TICKS(500 << retry));
    }

    heap_caps_free(buf);
    // 起播闸口兜底放行（无论成功 / 失败 / abort），防止 OutputLoop 永久阻塞
    prebuffered_.store(true, std::memory_order_release);
    ESP_LOGI(TAG, "Download done: %u bytes read (status=%d, eof=%d)",
             (unsigned)total_read, last_status, eof_reached);
    download_done_.store(true, std::memory_order_release);
}

void Mp3Player::DecodeThunk(void* arg) {
    auto* self = static_cast<Mp3Player*>(arg);
    self->DecodeLoop();
    // Mark decode pipeline drained so the OutputTask can exit when the
    // PCM ring goes empty. Ring buffers are freed by AbortAndJoin once
    // all three tasks exit.
    self->decode_done_.store(true, std::memory_order_release);
    self->active_tasks_.fetch_sub(1, std::memory_order_acq_rel);
    vTaskDelete(nullptr);
}

void Mp3Player::DecodeLoop() {
    esp_audio_dec_cfg_t dec_cfg = {};
    dec_cfg.type = ESP_AUDIO_TYPE_MP3;
    esp_audio_dec_handle_t dec = nullptr;
    if (esp_audio_dec_open(&dec_cfg, &dec) != ESP_AUDIO_ERR_OK) {
        ESP_LOGE(TAG, "Failed to open MP3 decoder");
        EmitError("Playback failed", "MP3 decoder init failed");
        return;
    }

    constexpr size_t kInBufSize  = 8192;
    constexpr size_t kOutBufSize = 2 * 1152 * sizeof(int16_t) + 512;
    uint8_t* in_buf  = static_cast<uint8_t*>(heap_caps_malloc(kInBufSize,  MALLOC_CAP_SPIRAM));
    uint8_t* out_buf = static_cast<uint8_t*>(heap_caps_malloc(kOutBufSize, MALLOC_CAP_SPIRAM));
    if (!in_buf || !out_buf) {
        ESP_LOGE(TAG, "Decode buffer alloc failed");
        EmitError("Playback failed", "PSRAM exhausted");
        if (in_buf)  heap_caps_free(in_buf);
        if (out_buf) heap_caps_free(out_buf);
        esp_audio_dec_close(dec);
        return;
    }
    size_t in_len = 0;

    esp_ae_rate_cvt_handle_t resampler = nullptr;
    uint32_t src_rate = 0;
    uint8_t  src_channel = 0;
    uint32_t dst_rate    = (uint32_t)audio_->output_sample_rate();
    uint8_t  dst_channel = (uint8_t)audio_->output_channels();

    if (audio_ && !audio_->output_enabled()) audio_->EnableOutput(true);

    int consecutive_errors = 0;
    bool started_emitted = false;
    bool finished_emitted = false;

    while (running_.load(std::memory_order_acquire) &&
           !abort_.load(std::memory_order_acquire)) {
        // Pause 闸口：让出 CPU，保留 in_buf / 解码器状态，Resume 后无缝继续
        if (paused_.load(std::memory_order_acquire)) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }
        if (in_len < 1024) {
            size_t want = kInBufSize - in_len;
            size_t got = 0;
            char* recv = static_cast<char*>(
                xRingbufferReceiveUpTo(compressed_ring_, &got, pdMS_TO_TICKS(200), want));
            if (recv) {
                memcpy(in_buf + in_len, recv, got);
                in_len += got;
                vRingbufferReturnItem(compressed_ring_, recv);
            } else {
                if (download_done_.load(std::memory_order_acquire) && in_len == 0) {
                    ESP_LOGI(TAG, "Decode complete (data exhausted)");
                    finished_emitted = true;
                    break;
                }
                continue;
            }
        }
        if (in_len == 0) continue;

        esp_audio_dec_in_raw_t raw = {};
        raw.buffer = in_buf;
        raw.len = (uint32_t)in_len;
        esp_audio_dec_out_frame_t out = {};
        out.buffer = out_buf;
        out.len = (uint32_t)kOutBufSize;

        auto ret = esp_audio_dec_process(dec, &raw, &out);

        if (raw.consumed > 0) {
            memmove(in_buf, in_buf + raw.consumed, in_len - raw.consumed);
            in_len -= raw.consumed;
        }

        if (ret == ESP_AUDIO_ERR_OK && out.decoded_size > 0) {
            consecutive_errors = 0;

            if (src_rate == 0) {
                esp_audio_dec_info_t info = {};
                if (esp_audio_dec_get_info(dec, &info) == ESP_AUDIO_ERR_OK) {
                    src_rate = info.sample_rate;
                    src_channel = info.channel;
                    ESP_LOGI(TAG, "MP3: %u Hz, %u ch -> sink %u Hz, %u ch",
                             (unsigned)src_rate, src_channel,
                             (unsigned)dst_rate, dst_channel);
                    // CBR 总时长估算：用首帧消耗字节数推算 bitrate（VBR 偏差 ±10%）
                    // MPEG-1 Layer 3 标准帧 = 1152 samples
                    size_t body = body_length_.load(std::memory_order_acquire);
                    if (body > 0 && raw.consumed > 0 && src_rate > 0) {
                        int frame_ms = 1152 * 1000 / (int)src_rate;
                        int total_ms = (int)((uint64_t)body * frame_ms / raw.consumed);
                        total_duration_ms_.store(total_ms, std::memory_order_release);
                        ESP_LOGI(TAG, "Estimated total duration: %d ms (CBR)", total_ms);
                    }
                    if (!started_emitted && callbacks_.on_started) {
                        callbacks_.on_started(GetCurrentTitle());
                        started_emitted = true;
                    }
                }
            }

            size_t sample_count = out.decoded_size / sizeof(int16_t);
            std::vector<int16_t> pcm(reinterpret_cast<int16_t*>(out_buf),
                                      reinterpret_cast<int16_t*>(out_buf) + sample_count);

            if (src_channel == 2 && dst_channel == 1) {
                std::vector<int16_t> mono(sample_count / 2);
                for (size_t i = 0; i < mono.size(); i++) {
                    int32_t avg = (pcm[2 * i] + pcm[2 * i + 1]) / 2;
                    mono[i] = (int16_t)avg;
                }
                pcm = std::move(mono);
            }

            if (src_rate != 0 && src_rate != dst_rate) {
                if (!resampler) {
                    esp_ae_rate_cvt_cfg_t cfg = {};
                    cfg.src_rate = src_rate;
                    cfg.dest_rate = dst_rate;
                    cfg.channel = dst_channel;
                    cfg.bits_per_sample = ESP_AUDIO_BIT16;
                    cfg.complexity = 2;
                    cfg.perf_type = ESP_AE_RATE_CVT_PERF_TYPE_SPEED;
                    if (esp_ae_rate_cvt_open(&cfg, &resampler) != ESP_AE_ERR_OK) {
                        ESP_LOGE(TAG, "Resampler open failed");
                        resampler = nullptr;
                    }
                }
                if (resampler) {
                    uint32_t max_out = 0;
                    esp_ae_rate_cvt_get_max_out_sample_num(resampler, pcm.size(), &max_out);
                    std::vector<int16_t> out_pcm(max_out);
                    uint32_t actual = max_out;
                    esp_ae_rate_cvt_process(resampler,
                                            (esp_ae_sample_t)pcm.data(), pcm.size(),
                                            (esp_ae_sample_t)out_pcm.data(), &actual);
                    out_pcm.resize(actual);
                    pcm = std::move(out_pcm);
                }
            }

            // Push PCM to the output ring (do NOT call codec directly — that
            // would couple HTTP-read jitter back into the I2S DMA consumer).
            // The OutputTask drains this ring at codec-paced cadence.
            if (!pcm.empty()) {
                const uint8_t* bytes = reinterpret_cast<const uint8_t*>(pcm.data());
                size_t total_bytes = pcm.size() * sizeof(int16_t);
                size_t written = 0;
                while (written < total_bytes &&
                       !abort_.load(std::memory_order_acquire) &&
                       running_.load(std::memory_order_acquire)) {
                    size_t chunk = total_bytes - written;
                    BaseType_t ok = xRingbufferSend(pcm_ring_, bytes + written, chunk,
                                                    pdMS_TO_TICKS(500));
                    if (ok == pdTRUE) written += chunk;
                    // else: PCM ring full -> Output task is draining,
                    //       try again next iteration.
                }
            }

        } else if (ret == ESP_AUDIO_ERR_CONTINUE) {
            consecutive_errors = 0;
        } else if (ret == ESP_AUDIO_ERR_BUFF_NOT_ENOUGH) {
            ESP_LOGW(TAG, "PCM buffer too small (need %u), skipping frame",
                     (unsigned)out.needed_size);
            consecutive_errors = 0;
        } else if (ret == ESP_AUDIO_ERR_DATA_LACK) {
            if (download_done_.load(std::memory_order_acquire) && in_len < 4) break;
        } else {
            if (++consecutive_errors >= 16) {
                ESP_LOGE(TAG, "Decode errors %d in a row, exiting", consecutive_errors);
                EmitError("Playback failed",
                          src_rate == 0 ? "Audio is not MP3 or corrupted"
                                        : "Decoder error");
                break;
            }
            if (in_len > 1) {
                memmove(in_buf, in_buf + 1, in_len - 1);
                in_len -= 1;
            } else {
                in_len = 0;
            }
        }
    }

    if (resampler) esp_ae_rate_cvt_close(resampler);
    esp_audio_dec_close(dec);
    heap_caps_free(in_buf);
    heap_caps_free(out_buf);

    if (finished_emitted && callbacks_.on_finished) callbacks_.on_finished();

    ESP_LOGI(TAG, "Decode task exit");
}

// ============================================================
// Output task: PCM ring -> audio sink (I2S DMA)
// ============================================================
//
// This is the third stage of the pipeline. It exists for one reason: to make
// the rate at which we write into the audio sink independent of every upstream
// hiccup (HTTP read 200 ms timeout, MP3 frame edge cases, resampler open).
// Without this task, a single TLS-read stall longer than the codec's DMA
// depth (~60 ms on most ESP32-S3 boards) is enough to cause an audible glitch.

void Mp3Player::OutputThunk(void* arg) {
    auto* self = static_cast<Mp3Player*>(arg);
    self->OutputLoop();
    self->active_tasks_.fetch_sub(1, std::memory_order_acq_rel);
    vTaskDelete(nullptr);
}

void Mp3Player::OutputLoop() {
    if (audio_ && !audio_->output_enabled()) {
        audio_->EnableOutput(true);
    }

    // 首帧预热闸口：等 DownloadLoop 累积 ≥ kPrebufferThreshold 字节再开始消费 PCM。
    // 避免 TLS 握手刚结束 / 4G PPP 抖动期就起播 → pcm_ring 见底 → I2S DMA underrun。
    // DownloadLoop 在成功累积或失败兜底时都会置 prebuffered_，OutputLoop 不会永久阻塞。
    while (!prebuffered_.load(std::memory_order_acquire) &&
           !abort_.load(std::memory_order_acquire) &&
           running_.load(std::memory_order_acquire)) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }

    while (running_.load(std::memory_order_acquire) &&
           !abort_.load(std::memory_order_acquire)) {
        // Pause 闸口：保留 pcm_ring_ 已解码数据，Resume 后立即无缝接续输出
        if (paused_.load(std::memory_order_acquire)) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }
        size_t got = 0;
        // Long timeout: when decode is slow (e.g. first-frame setup) we
        // simply wait, no busy-loop.
        char* recv = static_cast<char*>(
            xRingbufferReceiveUpTo(pcm_ring_, &got, pdMS_TO_TICKS(500), kOutputChunkBytes));
        if (!recv) {
            // Drain finished: decode task exited and the ring is empty.
            if (decode_done_.load(std::memory_order_acquire)) {
                ESP_LOGI(TAG, "Output: pipeline drained, exiting");
                break;
            }
            continue;
        }

        size_t samples = got / sizeof(int16_t);
        if (samples > 0 && audio_) {
            // The blocking codec write naturally rate-limits this loop to
            // the codec's playback speed. PCM ring fills/empties around it,
            // absorbing upstream jitter.
            std::vector<int16_t> pcm(reinterpret_cast<int16_t*>(recv),
                                      reinterpret_cast<int16_t*>(recv) + samples);
            audio_->OutputData(pcm);

            // 进度累加：基于 sink 输出采样率推算实际播放时长（与 source bitrate 解耦）
            int sink_rate = audio_->output_sample_rate();
            int sink_ch   = audio_->output_channels();
            if (sink_rate > 0 && sink_ch > 0) {
                int frame_count = (int)samples / sink_ch;
                int delta_ms = frame_count * 1000 / sink_rate;
                position_ms_.fetch_add(delta_ms, std::memory_order_acq_rel);
            }
        }
        vRingbufferReturnItem(pcm_ring_, recv);
    }

    ESP_LOGI(TAG, "Output task exit");
}

// ============================================================
// Pause / Resume：保留三个 task 不退出，只在 loop 顶部让出 CPU。
// HTTP 长连接、ringbuffer、解码器、resampler 全部保留，Resume 后无缝接续。
// ============================================================

void Mp3Player::PauseTimeoutCb(void* arg) {
    auto* self = static_cast<Mp3Player*>(arg);
    if (!self) return;
    // 仅在仍处于 paused 态触发；Resume/Stop 会先 stop 此 timer
    if (!self->paused_.load(std::memory_order_acquire)) return;
    ESP_LOGW(TAG, "Pause timeout (%d ms) — TCP keepalive expired, notify caller to Stop",
             kPauseTimeoutMs);
    if (self->callbacks_.on_pause_timeout) self->callbacks_.on_pause_timeout();
}

void Mp3Player::Pause() {
    if (!running_.load(std::memory_order_acquire)) return;
    if (paused_.exchange(true, std::memory_order_acq_rel)) return;  // 已暂停
    if (audio_) audio_->EnableOutput(false);
    ESP_LOGI(TAG, "Paused at %d ms", position_ms_.load(std::memory_order_acquire));

    // 启动 50s one-shot：超时即 emit on_pause_timeout（OSS keepalive 60s 前提前通知调用方）
    if (!pause_timeout_timer_) {
        esp_timer_create_args_t args = {
            .callback = PauseTimeoutCb,
            .arg = this,
            .dispatch_method = ESP_TIMER_TASK,   // 在 esp_timer task 上下文，可以阻塞
            .name = "mp3_pause_to",
            .skip_unhandled_events = true,
        };
        esp_timer_handle_t h = nullptr;
        if (esp_timer_create(&args, &h) == ESP_OK) {
            pause_timeout_timer_ = h;
        } else {
            ESP_LOGE(TAG, "Pause timeout timer create failed");
            return;
        }
    }
    esp_timer_stop(static_cast<esp_timer_handle_t>(pause_timeout_timer_));  // 幂等
    esp_timer_start_once(static_cast<esp_timer_handle_t>(pause_timeout_timer_),
                          (uint64_t)kPauseTimeoutMs * 1000);
}

void Mp3Player::Resume() {
    if (!running_.load(std::memory_order_acquire)) return;
    if (!paused_.exchange(false, std::memory_order_acq_rel)) return;  // 未暂停
    if (pause_timeout_timer_) {
        esp_timer_stop(static_cast<esp_timer_handle_t>(pause_timeout_timer_));
    }
    if (audio_) audio_->EnableOutput(true);
    ESP_LOGI(TAG, "Resumed at %d ms", position_ms_.load(std::memory_order_acquire));
}

}  // namespace mydazy
