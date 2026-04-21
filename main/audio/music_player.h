#ifndef MUSIC_PLAYER_H
#define MUSIC_PLAYER_H

#include <atomic>
#include <mutex>
#include <string>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/ringbuf.h>

class AudioCodec;

/**
 * MusicPlayer — MP3 流式播放器（远程推送专用）
 *
 * 命令：
 *   {"type":"music_play","url":"https://xxx.mp3","title":"xxx"}
 *   {"type":"music_stop"}
 *
 * 路径：
 *   HTTP chunked GET (下载任务, Core0, PSRAM 栈)
 *     → 32KB 环形缓冲 (PSRAM)
 *     → esp_audio_dec MP3 (解码任务, Core1, PSRAM 栈)
 *     → 重采样 + 立体声→单声道
 *     → codec->OutputData() (互斥锁已有)
 *
 * 内存：全 PSRAM（内部 RAM 零增量）。
 * 互斥：与 TTS 共享 codec，Play() 入口会先打断 TTS。
 *
 * 稳定性：
 *   - 用户主动 Stop() / 新 Play() 会 abort_=true，等旧任务 30ms 内自动退出
 *   - 下载超时 15s；解码数据不足时 200ms 等待；多次失败直接退出
 */
class MusicPlayer {
public:
    static MusicPlayer& GetInstance();

    // 绑定 codec（Application::Initialize 后调用）
    void Initialize(AudioCodec* codec);

    // 启动流式播放（若已有播放先自动 Stop）
    // err_msg: 同步错误原因回填（url 无效 / 未初始化 / PSRAM 不足 / 任务创建失败）；
    //         异步错误（HTTP 失败 / 解码异常）直接通过 Application::Alert 上屏
    bool Play(const std::string& url, const std::string& title, std::string* err_msg = nullptr);

    // 停止当前播放
    void Stop();

    bool IsPlaying() const { return running_.load(std::memory_order_acquire); }
    std::string GetCurrentTitle() const;

private:
    MusicPlayer() = default;
    ~MusicPlayer() = default;
    MusicPlayer(const MusicPlayer&) = delete;
    MusicPlayer& operator=(const MusicPlayer&) = delete;

    // 任务入口
    static void DownloadThunk(void* arg);
    static void DecodeThunk(void* arg);

    void DownloadLoop();
    void DecodeLoop();

    // 等待旧任务退出（Play/Stop 调用路径）
    void AbortAndJoin();

    AudioCodec* codec_ = nullptr;

    // 生命周期标志
    std::atomic<bool> running_{false};        // 播放管线是否运行（任一 task 活跃）
    std::atomic<bool> abort_{false};          // 中止标志（外部 Stop 或新 Play）
    std::atomic<bool> download_done_{false};  // 下载任务退出标志（数据流结束）
    std::atomic<int>  active_tasks_{0};       // 在跑任务数（Download+Decode，0=全部退出）

    // 状态保护
    mutable std::mutex state_mutex_;
    std::string current_url_;
    std::string current_title_;

    // 环形缓冲（PSRAM）
    // 128KB：按 128kbps MP3 折算约 8 秒缓冲，吸收 4G 弱网 / 网络抖动。
    // PSRAM 顺序读写对 MP3 解码速度影响 < 5%，不是瓶颈。
    RingbufHandle_t ring_buf_ = nullptr;
    static constexpr size_t kRingBufSize = 128 * 1024;
    static constexpr int kHttpTimeoutMs = 15000;
};

#endif  // MUSIC_PLAYER_H
