/**
 * MusicPlayer — MP3 流式播放器实现
 *
 * 任务拓扑：
 *   DownloadTask (Core 0, PSRAM 栈 6KB)
 *     ├─ HTTP GET stream → xRingbufferSend
 *     └─ 结束/abort → 退出 + 标记 download_done_
 *
 *   DecodeTask (Core 1, PSRAM 栈 8KB)
 *     ├─ xRingbufferReceiveUpTo → esp_audio_dec_process
 *     ├─ 重采样（首帧 info 拿到 src_rate 后懒初始化）
 *     ├─ 立体声 → 单声道（L/R 平均）
 *     └─ codec->OutputData()
 */

#include "music_player.h"

#include "audio_codec.h"
#include "application.h"
#include "board.h"

#include <esp_log.h>
#include <esp_heap_caps.h>
#include <cstring>
#include <vector>

extern "C" {
#include "esp_audio_dec.h"
#include "esp_audio_dec_default.h"
#include "esp_mp3_dec.h"
#include "esp_ae_rate_cvt.h"
}

#define TAG "MusicPlayer"

namespace {

// 全局 MP3 解码器是否已注册（整个进程一次即可）
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

// 异步 Alert（下载/解码任务上下文，不能直接调 UI API）
void AsyncAlert(const char* status, const char* msg) {
    std::string s = status ? status : "";
    std::string m = msg ? msg : "";
    Application::GetInstance().Schedule([s = std::move(s), m = std::move(m)]() {
        Application::GetInstance().Alert(s.c_str(), m.c_str(), "", "");
    });
}

}  // namespace

// ============================================================
// 单例 + 初始化
// ============================================================

MusicPlayer& MusicPlayer::GetInstance() {
    static MusicPlayer instance;
    return instance;
}

void MusicPlayer::Initialize(AudioCodec* codec) {
    codec_ = codec;
    EnsureMp3DecoderRegistered();
}

std::string MusicPlayer::GetCurrentTitle() const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    return current_title_;
}

// ============================================================
// Play / Stop
// ============================================================

bool MusicPlayer::Play(const std::string& url, const std::string& title, std::string* err_msg) {
    auto set_err = [err_msg](const char* s) {
        if (err_msg) *err_msg = s;
    };

    if (!codec_) {
        ESP_LOGE(TAG, "Play 失败: 播放器未初始化");
        set_err("播放器未初始化");
        return false;
    }
    if (url.empty()) {
        ESP_LOGW(TAG, "Play 失败: url 为空");
        set_err("URL 为空");
        return false;
    }
    if (url.substr(0, 7) != "http://" && url.substr(0, 8) != "https://") {
        ESP_LOGW(TAG, "Play 失败: URL 协议不支持 (%s)", url.c_str());
        set_err("URL 必须以 http:// 或 https:// 开头");
        return false;
    }

    // 有旧任务先中止
    AbortAndJoin();

    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        current_url_ = url;
        current_title_ = title;
    }

    // 创建 PSRAM 环形缓冲（字节流）
    ring_buf_ = xRingbufferCreateWithCaps(kRingBufSize, RINGBUF_TYPE_BYTEBUF, MALLOC_CAP_SPIRAM);
    if (!ring_buf_) {
        ESP_LOGE(TAG, "Play 失败: PSRAM 环形缓冲分配失败 (%zu bytes)", kRingBufSize);
        set_err("PSRAM 内存不足");
        return false;
    }

    abort_.store(false, std::memory_order_release);
    download_done_.store(false, std::memory_order_release);
    running_.store(true, std::memory_order_release);

    // 下载任务（Core 0，PSRAM 栈 6KB，一次性 I/O 任务）
    TaskHandle_t download_task = nullptr;
    BaseType_t ret = xTaskCreatePinnedToCoreWithCaps(
        DownloadThunk, "mp3_dl", 6 * 1024, this, 1, &download_task, 0, MALLOC_CAP_SPIRAM);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Play 失败: 下载任务创建失败");
        vRingbufferDeleteWithCaps(ring_buf_);
        ring_buf_ = nullptr;
        running_.store(false, std::memory_order_release);
        set_err("下载任务创建失败");
        return false;
    }

    // 解码任务（Core 1，PSRAM 栈 8KB，避免与 Core 0 flash op 冲突）
    TaskHandle_t decode_task = nullptr;
    ret = xTaskCreatePinnedToCoreWithCaps(
        DecodeThunk, "mp3_dec", 8 * 1024, this, 6, &decode_task, 1, MALLOC_CAP_SPIRAM);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Play 失败: 解码任务创建失败");
        abort_.store(true, std::memory_order_release);  // 让下载任务尽快退出
        set_err("解码任务创建失败");
        return false;
    }

    ESP_LOGI(TAG, "Play: %s (%s)", title.c_str(), url.c_str());
    if (err_msg) err_msg->clear();
    return true;
}

void MusicPlayer::Stop() {
    AbortAndJoin();
    ESP_LOGI(TAG, "Stop");
}

// 等待旧任务自然退出（最多 500ms）
void MusicPlayer::AbortAndJoin() {
    if (!running_.load(std::memory_order_acquire)) {
        return;
    }
    abort_.store(true, std::memory_order_release);

    // 等解码任务退出（它会在循环里检查 abort_）
    for (int i = 0; i < 50 && running_.load(std::memory_order_acquire); i++) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (running_.load(std::memory_order_acquire)) {
        ESP_LOGW(TAG, "AbortAndJoin 超时 500ms，强制清理");
        running_.store(false, std::memory_order_release);
    }

    // ring_buf_ 由解码任务退出时 vRingbufferDeleteWithCaps；
    // 此处兜底以防解码任务未起来（Play 分支中途失败）
    if (ring_buf_) {
        vRingbufferDeleteWithCaps(ring_buf_);
        ring_buf_ = nullptr;
    }
}

// ============================================================
// 下载任务：HTTP GET → 环形缓冲
// ============================================================

void MusicPlayer::DownloadThunk(void* arg) {
    static_cast<MusicPlayer*>(arg)->DownloadLoop();
    vTaskDeleteWithCaps(nullptr);
}

void MusicPlayer::DownloadLoop() {
    std::string url;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        url = current_url_;
    }

    auto network = Board::GetInstance().GetNetwork();
    if (!network) {
        ESP_LOGE(TAG, "下载失败: 网络未就绪");
        AsyncAlert("播放失败", "网络未就绪");
        download_done_.store(true, std::memory_order_release);
        return;
    }

    auto http = network->CreateHttp(1);
    http->SetTimeout(kHttpTimeoutMs);

    if (!http->Open("GET", url)) {
        ESP_LOGE(TAG, "下载失败: HTTP 连接失败 %s", url.c_str());
        AsyncAlert("播放失败", "连接服务器失败");
        download_done_.store(true, std::memory_order_release);
        return;
    }

    int status = http->GetStatusCode();
    if (status != 200) {
        ESP_LOGE(TAG, "下载失败: HTTP %d", status);
        char buf[48];
        if (status == 404)      snprintf(buf, sizeof(buf), "音频文件不存在 (404)");
        else if (status == 403) snprintf(buf, sizeof(buf), "无访问权限 (403)");
        else if (status >= 500) snprintf(buf, sizeof(buf), "服务器错误 (%d)", status);
        else                    snprintf(buf, sizeof(buf), "HTTP 错误 (%d)", status);
        AsyncAlert("播放失败", buf);
        http->Close();
        download_done_.store(true, std::memory_order_release);
        return;
    }

    size_t total_len = http->GetBodyLength();
    ESP_LOGI(TAG, "下载开始: %zu bytes", total_len);

    // 栈上 2KB 读缓冲（栈在 PSRAM，任意用）
    constexpr size_t kReadChunk = 2048;
    char buf[kReadChunk];
    size_t total_read = 0;

    while (!abort_.load(std::memory_order_acquire) &&
           running_.load(std::memory_order_acquire)) {
        int n = http->Read(buf, sizeof(buf));
        if (n < 0) {
            ESP_LOGW(TAG, "下载：Read 错误 %d", n);
            break;
        }
        if (n == 0) {
            // 数据流结束
            break;
        }

        // 写入环形缓冲（阻塞 200ms，缓冲满时让解码端消费）
        size_t written = 0;
        while (written < (size_t)n && !abort_.load(std::memory_order_acquire)) {
            size_t chunk = (size_t)n - written;
            BaseType_t ok = xRingbufferSend(ring_buf_, buf + written, chunk,
                                             pdMS_TO_TICKS(200));
            if (ok == pdTRUE) {
                written += chunk;
            }
            // ok==pdFALSE：缓冲满，下一轮重试
        }
        total_read += written;
    }

    http->Close();
    ESP_LOGI(TAG, "下载结束: 已读 %zu bytes", total_read);
    download_done_.store(true, std::memory_order_release);
}

// ============================================================
// 解码任务：环形缓冲 → MP3 decoder → 重采样 → 送 codec
// ============================================================

void MusicPlayer::DecodeThunk(void* arg) {
    static_cast<MusicPlayer*>(arg)->DecodeLoop();
    // 解码任务是生命周期所有者，负责清理
    auto* self = static_cast<MusicPlayer*>(arg);
    if (self->ring_buf_) {
        vRingbufferDeleteWithCaps(self->ring_buf_);
        self->ring_buf_ = nullptr;
    }
    self->running_.store(false, std::memory_order_release);
    vTaskDeleteWithCaps(nullptr);
}

void MusicPlayer::DecodeLoop() {
    esp_audio_dec_cfg_t dec_cfg = {};
    dec_cfg.type = ESP_AUDIO_TYPE_MP3;
    esp_audio_dec_handle_t dec = nullptr;
    if (esp_audio_dec_open(&dec_cfg, &dec) != ESP_AUDIO_ERR_OK) {
        ESP_LOGE(TAG, "MP3 解码器打开失败");
        AsyncAlert("播放失败", "MP3 解码器初始化失败");
        return;
    }

    // 输入/输出缓冲：栈上分配（栈在 PSRAM）
    constexpr size_t kInBufSize = 4096;
    constexpr size_t kOutBufSize = 2 * 1152 * sizeof(int16_t) + 512;  // 1 MP3 帧 = 1152 samples × 2ch
    uint8_t in_buf[kInBufSize];
    uint8_t out_buf[kOutBufSize];
    size_t in_len = 0;

    // 延迟初始化的重采样器（首帧 info 后才知道 src_rate）
    esp_ae_rate_cvt_handle_t resampler = nullptr;
    uint32_t src_rate = 0;
    uint8_t src_channel = 0;
    uint32_t dst_rate = (uint32_t)codec_->output_sample_rate();
    uint8_t dst_channel = (uint8_t)codec_->output_channels();

    auto enable_output = [this]() {
        if (codec_ && !codec_->output_enabled()) codec_->EnableOutput(true);
    };
    enable_output();

    int consecutive_errors = 0;

    while (running_.load(std::memory_order_acquire) &&
           !abort_.load(std::memory_order_acquire)) {
        // 1. 填充输入缓冲（若不够一帧）
        if (in_len < 1024) {
            size_t want = kInBufSize - in_len;
            size_t got = 0;
            char* recv = (char*)xRingbufferReceiveUpTo(ring_buf_, &got, pdMS_TO_TICKS(200), want);
            if (recv) {
                memcpy(in_buf + in_len, recv, got);
                in_len += got;
                vRingbufferReturnItem(ring_buf_, recv);
            } else {
                // 超时未拿到数据：若下载已结束 + 缓冲空，正常退出
                if (download_done_.load(std::memory_order_acquire) && in_len == 0) {
                    ESP_LOGI(TAG, "解码完成（数据耗尽）");
                    break;
                }
                continue;  // 否则继续等
            }
        }

        if (in_len == 0) continue;

        // 2. 解码一帧
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

            // 3. 获取解码信息（首次）
            if (src_rate == 0) {
                esp_audio_dec_info_t info = {};
                if (esp_audio_dec_get_info(dec, &info) == ESP_AUDIO_ERR_OK) {
                    src_rate = info.sample_rate;
                    src_channel = info.channel;
                    ESP_LOGI(TAG, "MP3: %u Hz, %u ch → codec %u Hz, %u ch",
                             (unsigned)src_rate, src_channel,
                             (unsigned)dst_rate, dst_channel);
                }
            }

            // 4. PCM 转换：int16_t
            size_t sample_count = out.decoded_size / sizeof(int16_t);
            std::vector<int16_t> pcm(reinterpret_cast<int16_t*>(out_buf),
                                      reinterpret_cast<int16_t*>(out_buf) + sample_count);

            // 5. 立体声 → 单声道（L/R 平均）
            if (src_channel == 2 && dst_channel == 1) {
                std::vector<int16_t> mono(sample_count / 2);
                for (size_t i = 0; i < mono.size(); i++) {
                    int32_t avg = (pcm[2 * i] + pcm[2 * i + 1]) / 2;
                    mono[i] = (int16_t)avg;
                }
                pcm = std::move(mono);
            }

            // 6. 采样率转换（懒创建 resampler）
            if (src_rate != 0 && src_rate != dst_rate) {
                if (!resampler) {
                    esp_ae_rate_cvt_cfg_t cfg = {};
                    cfg.src_rate = src_rate;
                    cfg.dest_rate = dst_rate;
                    cfg.channel = dst_channel;  // 此时已是单声道
                    cfg.bits_per_sample = ESP_AUDIO_BIT16;
                    cfg.complexity = 2;
                    cfg.perf_type = ESP_AE_RATE_CVT_PERF_TYPE_SPEED;
                    if (esp_ae_rate_cvt_open(&cfg, &resampler) != ESP_AE_ERR_OK) {
                        ESP_LOGE(TAG, "重采样器打开失败");
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

            // 7. 送 codec（AudioCodec 内部已有 mutex 保护）
            if (!pcm.empty()) {
                codec_->OutputData(pcm);
            }

        } else if (ret == ESP_AUDIO_ERR_CONTINUE) {
            // 部分解码成功，下次继续
            consecutive_errors = 0;
        } else if (ret == ESP_AUDIO_ERR_BUFF_NOT_ENOUGH) {
            ESP_LOGW(TAG, "PCM 缓冲不足 (need %u)，跳过此帧", (unsigned)out.needed_size);
            consecutive_errors = 0;
        } else if (ret == ESP_AUDIO_ERR_DATA_LACK) {
            // 帧数据不够：等下一轮读更多
            if (download_done_.load(std::memory_order_acquire) && in_len < 4) {
                break;
            }
        } else {
            if (++consecutive_errors >= 16) {
                ESP_LOGE(TAG, "连续解码错误 %d 次，退出", consecutive_errors);
                // 若从未解出过样本（src_rate==0）→ 文件格式不是 MP3 或已损坏
                AsyncAlert("播放失败",
                           src_rate == 0 ? "音频格式非 MP3 或已损坏" : "音频解码异常");
                break;
            }
            // 跳过 1 字节寻找同步字
            if (in_len > 1) {
                memmove(in_buf, in_buf + 1, in_len - 1);
                in_len -= 1;
            } else {
                in_len = 0;
            }
        }
    }

    // 清理
    if (resampler) esp_ae_rate_cvt_close(resampler);
    esp_audio_dec_close(dec);
    ESP_LOGI(TAG, "解码任务退出");
}
