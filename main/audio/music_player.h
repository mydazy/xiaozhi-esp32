#ifndef MUSIC_PLAYER_H
#define MUSIC_PLAYER_H

#include <string>

class AudioCodec;

/**
 * MusicPlayer — 项目侧 adapter，对外保持原有 API 兼容。
 *
 * 内部包装 mydazy::Mp3Player（component: mydazy/esp_mp3_player）：
 *   - AudioCodec        → mydazy::IAudioOutput
 *   - Board::GetNetwork → mydazy::IHttpFactory（每次 Play 时新建 Http 客户端）
 *   - 错误回调          → Application::Schedule + Alert（切主线程上屏）
 *
 * 调用点（mydazy_p30_board.cc / remote_cmd.cc 等）继续用 GetInstance() 单例。
 */
class MusicPlayer {
public:
    static MusicPlayer& GetInstance();

    // 一次性接入。codec 由 Application 持有，必须在 MusicPlayer 之前创建。
    void Initialize(AudioCodec* codec);

    // 启动流式播放（已有播放会自动 Stop）。err_msg 仅用于同步失败原因。
    bool Play(const std::string& url, const std::string& title, std::string* err_msg = nullptr);

    // 停止当前播放并阻塞等待任务退出（最多 ~2s，TLS read 阻塞场景下可能更久）。
    void Stop();

    bool IsPlaying() const;
    std::string GetCurrentTitle() const;

    MusicPlayer(const MusicPlayer&) = delete;
    MusicPlayer& operator=(const MusicPlayer&) = delete;

private:
    MusicPlayer() = default;
    ~MusicPlayer() = default;

    bool initialized_ = false;
};

#endif  // MUSIC_PLAYER_H
