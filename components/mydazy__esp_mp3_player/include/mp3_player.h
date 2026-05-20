/*
 * SPDX-FileCopyrightText: 2026 mydazy
 * SPDX-License-Identifier: Apache-2.0
 *
 * Mydazy 流式 MP3 播放器 — v2.0.0 极简版
 *
 * 设计哲学：判断越少问题越少，减少头文件依赖，C 风格 event 取代 std::function table。
 *
 * v1.x → v2.0 主要变化：
 *   ① 减依赖：删除 .h 中的 <string> / <functional> / <mutex> / <vector>
 *      （IAudioOutput 接口仍保留 vector — 与 AudioCodec 接口兼容）
 *   ② 4 个独立 std::function callback → 1 个 mp3_event_cb_t 函数指针 + ctx
 *   ③ Pause / Resume 两个 API → PauseToggle 单一 API
 *   ④ GetCurrentTitle 删除（事件已含 title 信息；调用方自行维护）
 *   ⑤ URL/title 类型 std::string → const char*（外部责任 ownership）
 *
 * 高频使用场景（设计 3 个核心）：
 *   1. 故事 / 儿歌流式播放（OSS Range 断点续传）
 *   2. 中途打断（按键单击 / 切网 / 关机前）
 *   3. 暂停-续播（保留 HTTP 长连接 + ringbuffer）
 *
 * 架构（不变）：
 *   HTTP → compressed_ring (PSRAM 512KB) → Decode → pcm_ring (32KB INT) → Output → I2S
 *   三 task：mp3_dl(Core 1 P1 PSRAM) / mp3_dec(Core 0 P7 INT) / mp3_out(Core 1 P10 INT)
 */

#pragma once

#include <atomic>
#include <cstdint>
#include <cstddef>
#include <memory>
#include <string>     /* IHttpClient 内部 C++ 接口仍用 std::string */
#include <vector>     /* IAudioOutput::OutputData 接口（C++20 可换 std::span） */

#include <freertos/FreeRTOS.h>
#include <freertos/ringbuf.h>
#include <freertos/task.h>

namespace mydazy {

/* ────────────────────────────────────────────────────────────────
 * 接口（不变 — 保持 AudioCodec / HTTP 兼容）
 * ──────────────────────────────────────────────────────────────── */

struct IAudioOutput {
    virtual ~IAudioOutput() = default;
    virtual int  output_sample_rate() const = 0;
    virtual int  output_channels()    const = 0;
    virtual bool output_enabled()     const = 0;
    virtual void EnableOutput(bool enable) = 0;
    virtual void OutputData(std::vector<int16_t>& pcm) = 0;
};

/* IHttpClient 保留 std::string 参数（内部 C++ wrapper 接口，与 v1.x 兼容）。
   外部 Mp3Player public API 已 const char* 化。 */
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

/* ────────────────────────────────────────────────────────────────
 * 事件回调（极简 C 风格）
 * ──────────────────────────────────────────────────────────────── */

enum mp3_event_t {
    MP3_EVENT_STARTED = 0,        ///< 首帧解码出，extra = 0，msg = title
    MP3_EVENT_FINISHED,           ///< 自然结束（非 Stop()），extra = 0
    MP3_EVENT_ERROR,              ///< 异步错误，msg = "http:404" 等
    MP3_EVENT_PAUSE_TIMEOUT,      ///< 暂停 ≥ 50s（OSS keepalive 60s 前提示）
};

/// 事件回调（在 worker task 上下文调用，调用方必要时 Schedule 回主任务）
typedef void (*mp3_event_cb_t)(mp3_event_t ev, int extra, const char *msg, void *ctx);

/* ────────────────────────────────────────────────────────────────
 * 播放器（5 个核心 API + 3 个状态查询）
 * ──────────────────────────────────────────────────────────────── */

class Mp3Player {
public:
    static Mp3Player& GetInstance();

    /// 一次性 init（pointers 必须 outlive player）
    void Initialize(IAudioOutput* audio, IHttpFactory* http,
                    mp3_event_cb_t cb, void *ctx);

    /// 启动流式播放（已有播放自动 Stop）
    /// @param url    播放地址（OSS 签名 URL，UTF-8 中文需调用方先 percent-encode）
    /// @param title  曲名（可选，传 NULL = 不传）
    /// @return true 成功启动；false 见日志（可能 codec 未 init / 内存不足）
    bool Play(const char* url, const char* title);

    /// 立即停止（最多阻塞 ~100ms 等三 task 退出）
    void Stop();

    /// 暂停 ↔ 续播切换（替代 v1.x 的 Pause + Resume）
    /// 暂停期间保留 HTTP 长连接 + ringbuffer，无缝续播；
    /// 暂停 ≥ 50s 触发 MP3_EVENT_PAUSE_TIMEOUT，调用方应 Stop 释放资源。
    void PauseToggle();

    /* ── 状态查询（lock-free atomic）── */
    bool IsPlaying() const { return running_.load(std::memory_order_acquire); }
    bool IsPaused()  const { return paused_.load(std::memory_order_acquire); }
    int  GetPositionMs()      const { return position_ms_.load(std::memory_order_acquire); }
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
    void EmitEvent(mp3_event_t ev, int extra, const char *msg);
    /* 旧 API 兼容 wrapper：转发到 EmitEvent(MP3_EVENT_ERROR)，内部 9 处调用未改 */
    void EmitError(const char *status, const char *message);

    IAudioOutput*  audio_  = nullptr;
    IHttpFactory*  http_   = nullptr;
    mp3_event_cb_t event_cb_ = nullptr;
    void*          event_ctx_ = nullptr;

    std::atomic<bool> running_{false};
    std::atomic<bool> abort_{false};
    std::atomic<bool> paused_{false};
    std::atomic<bool> download_done_{false};
    std::atomic<bool> decode_done_{false};
    std::atomic<int>  active_tasks_{0};
    std::atomic<bool> prebuffered_{false};

    /* 进度 */
    std::atomic<int>     position_ms_{0};
    std::atomic<int>     total_duration_ms_{0};
    std::atomic<size_t>  body_length_{0};

    /* URL/title 内部存储（fixed-size buffer，无需 mutex/std::string） */
    static constexpr size_t kUrlBufSize   = 512;
    static constexpr size_t kTitleBufSize = 96;
    char current_url_[kUrlBufSize]     = {};
    char current_title_[kTitleBufSize] = {};

    /* 三段流水线 ringbuffer */
    RingbufHandle_t compressed_ring_ = nullptr;
    RingbufHandle_t pcm_ring_        = nullptr;
    static constexpr size_t kCompressedRingSize = 512 * 1024;
    static constexpr size_t kPcmRingSize        = 32 * 1024;
    static constexpr size_t kOutputChunkBytes   = 4 * 1024;
    static constexpr size_t kPrebufferThreshold = 32 * 1024;
    static constexpr int    kHttpTimeoutMs      = 12000;
    static constexpr int    kPauseTimeoutMs     = 50000;
    static constexpr int    kHttpRetryMax       = 5;
    static constexpr int    kPauseCloseConnMs   = 5000;
    static constexpr size_t kProactiveReconnectBytes = 5 * 1024 * 1024;

    /* 暂停超时定时器（懒创建） */
    void* pause_timeout_timer_ = nullptr;
    static void PauseTimeoutCb(void* arg);
};

}  // namespace mydazy
