#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <freertos/FreeRTOS.h>
#include <freertos/ringbuf.h>
#include <freertos/task.h>

namespace mydazy {

// Audio sink. The component never owns the codec; the caller keeps it alive.
struct IAudioOutput {
    virtual ~IAudioOutput() = default;
    virtual int  output_sample_rate() const = 0;
    virtual int  output_channels()    const = 0;
    virtual bool output_enabled()     const = 0;
    virtual void EnableOutput(bool enable) = 0;
    virtual void OutputData(std::vector<int16_t>& pcm) = 0;
};

// HTTP transport. One IHttpClient per request; component closes & destroys it.
struct IHttpClient {
    virtual ~IHttpClient() = default;
    virtual bool   Open(const std::string& method, const std::string& url) = 0;
    virtual int    GetStatusCode() = 0;
    virtual size_t GetBodyLength() = 0;
    virtual int    Read(char* buf, size_t size) = 0;
    virtual void   Close() = 0;
    virtual void   SetTimeout(int ms) = 0;
    virtual void   SetHeader(const std::string& key, const std::string& value) {}
};

struct IHttpFactory {
    virtual ~IHttpFactory() = default;
    virtual std::unique_ptr<IHttpClient> CreateHttp() = 0;
};

// Streaming MP3 player. Single-instance (`GetInstance()`); MP3 decoder is
// process-global in esp_audio_codec, so a second player would not buy parallelism.
class Mp3Player {
public:
    struct Callbacks {
        // Async error from download / decode. Always invoked from a worker task —
        // the caller is responsible for thread-hopping back to UI if needed.
        std::function<void(const char* status, const char* message)> on_error;
        // Optional: called once decode produces its first frame.
        std::function<void(const std::string& title)> on_started;
        // Optional: called on graceful end-of-stream (not on Stop()/abort).
        std::function<void()> on_finished;
        // Optional: paused 超过 kPauseTimeoutMs（默认 50s）触发。
        // 调用方应该 Schedule 回主任务调 Stop() + 通知用户暂停超时（OSS keepalive ~60s
        // 后 TCP 会被 server 关，继续 Resume 会被 DownloadLoop 误判为"播完"）。
        std::function<void()> on_pause_timeout;
    };

    static Mp3Player& GetInstance();

    // One-time. Pointers must outlive the player.
    void Initialize(IAudioOutput* audio,
                    IHttpFactory* http,
                    const Callbacks& callbacks = {});

    // Auto-aborts any previous playback before starting.
    bool Play(const std::string& url,
              const std::string& title = "",
              std::string* err_msg = nullptr);

    void Stop();
    bool IsPlaying() const { return running_.load(std::memory_order_acquire); }
    std::string GetCurrentTitle() const;

    // 暂停/继续：保留 HTTP 连接 + ringbuffer 缓冲，三个 loop 在 paused_ 闸口让出 CPU。
    // Pause 后 IAudioOutput::EnableOutput(false) 关功放静音；Resume 反之。
    void Pause();
    void Resume();
    bool IsPaused() const { return paused_.load(std::memory_order_acquire); }

    // 进度查询（毫秒）。Position 由 OutputLoop 累计 PCM 输出样本得到，
    // Duration 由首帧解析的 sample_rate + body_length 估算（CBR 准；VBR 误差 ±10%，可能为 0）。
    int  GetPositionMs() const { return position_ms_.load(std::memory_order_acquire); }
    int  GetTotalDurationMs() const { return total_duration_ms_.load(std::memory_order_acquire); }

    Mp3Player(const Mp3Player&) = delete;
    Mp3Player& operator=(const Mp3Player&) = delete;

private:
    Mp3Player() = default;
    ~Mp3Player() = default;

    static void DownloadThunk(void* arg);
    static void DecodeThunk(void* arg);
    static void OutputThunk(void* arg);
    void DownloadLoop();
    void DecodeLoop();
    void OutputLoop();

    void AbortAndJoin();
    void EmitError(const char* status, const char* message);

    IAudioOutput* audio_ = nullptr;
    IHttpFactory* http_ = nullptr;
    Callbacks callbacks_;

    std::atomic<bool> running_{false};
    std::atomic<bool> abort_{false};
    std::atomic<bool> paused_{false};
    std::atomic<bool> download_done_{false};
    std::atomic<bool> decode_done_{false};
    std::atomic<int>  active_tasks_{0};
    std::atomic<bool> prebuffered_{false};

    // 进度跟踪：OutputLoop 每次 OutputData() 累加输出 PCM 时长到 position_ms_
    std::atomic<int>     position_ms_{0};
    std::atomic<int>     total_duration_ms_{0};
    std::atomic<size_t>  body_length_{0};         // HTTP Content-Length，DecodeLoop 首帧估算总时长用

    mutable std::mutex state_mutex_;
    std::string current_url_;
    std::string current_title_;

    // Three-stage pipeline: HTTP -> compressed_ring -> Decode -> pcm_ring -> Output -> sink
    // The PCM ring decouples decode jitter / HTTP read stalls from the I2S
    // DMA consumer (~60 ms deep on most boards), eliminating the classic
    // "single-task pipeline" underrun pattern.
    RingbufHandle_t compressed_ring_ = nullptr;       // MP3 byte stream from HTTP
    RingbufHandle_t pcm_ring_ = nullptr;              // resampled int16 PCM bytes
    static constexpr size_t kCompressedRingSize = 512 * 1024;  // ~32-64 s 缓冲
    static constexpr size_t kPcmRingSize = 32 * 1024;          // ~340 ms @ 24 kHz mono
    static constexpr size_t kOutputChunkBytes = 4 * 1024;      // ~85 ms @ 24 kHz mono
    static constexpr size_t kPrebufferThreshold = 32 * 1024;
    static constexpr int    kHttpTimeoutMs = 15000;
    static constexpr int    kPauseTimeoutMs = 50000;           // 50s（小于 OSS 默认 keepalive 60s）
    static constexpr int    kHttpRetryMax = 3;
    static constexpr int    kPauseCloseConnMs = 5000;
    // 主动周期断流重连：每下载满 N 字节主动 Close + Range 续传，治 OSS 长连接被 NAT 切。
    // 2026-04-30 v2：2MB → 10MB
    // 理由：4G 实测 2MB 重连后 OSS 频繁 15s timeout（疑似反爬虫限流 + ML307 modem
    //       HTTP context 复用慢），快速重连反致 retry 用尽。NAT 老化通常 2-5 分钟
    //       才发生，10MB ≈ 4-7 分钟刚好规避，且大幅减少 modem context 切换压力。
    // 主动重连不消耗 kHttpRetryMax 计数（仅错误重连消耗）。
    static constexpr size_t kProactiveReconnectBytes = 10 * 1024 * 1024;

    // 暂停超时定时器（懒创建，one-shot；Pause 启动 / Resume / Stop 取消）
    void* pause_timeout_timer_ = nullptr;   // 实际类型 esp_timer_handle_t（避免污染头文件 esp_timer.h 依赖）
    static void PauseTimeoutCb(void* arg);
};

}  // namespace mydazy
