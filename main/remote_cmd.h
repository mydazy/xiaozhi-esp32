#ifndef REMOTE_CMD_H
#define REMOTE_CMD_H

#include <cJSON.h>
#include <string>
#include <atomic>

class Application;

/**
 * 远程命令处理器
 *
 * 发送格式:
 * {"type":"custom","payload":{"message":"{\"type\":\"CMD\", ...}"}}
 *
 * 命令列表:
 * ┌───────────────┬────────────────────────────────────────────────────────────┐
 * │ 命令          │ 格式                                                        │
 * ├───────────────┼────────────────────────────────────────────────────────────┤
 * │ reboot        │ {"type":"reboot"}                                          │
 * │ update/ota    │ {"type":"update"}                                          │
 * │ reconnect     │ {"type":"reconnect"}                                       │
 * │ wakeup        │ {"type":"wakeup", "text":"你好"}                            │
 * │ tts           │ {"type":"tts", "text":"你好，我是小智"}                     │
 * │ ttai          │ {"type":"ttai", "text":"今天天气怎么样"}                   │
 * │ volume        │ {"type":"volume", "value":50}                              │
 * │ gain          │ {"type":"gain", "input":15, "ref":6, "aec":6}                      │
 * │ download      │ {"type":"download", "files":[...], "emoji":"happy"}        │
 * │ audio_debug   │ {"type":"audio_debug", "server":"IP:8000", "mode":"raw"}   │
 * │ vad_config    │ {"type":"vad_config", "min_speech":128, "min_noise":500}   │
 * │ sleep         │ {"type":"sleep", "gyro":true}  (gyro=是否陀螺仪唤醒)        │
 * │ flow         │ {"type":"flow","action":"start/stop/status/load"} │
 * │ stt_url       │ {"type":"stt_url", "url":"https://www.mydazy.com/v1/ota/pushstt"}  设置STT回调地址    │
 * │               │ {"type":"stt_url", "url":""}  清除STT回调                   │
 * │ music_play    │ {"type":"music_play","url":"https://xxx.mp3","title":"xxx"} │
 * │ music_stop    │ {"type":"music_stop"}                                       │
 * │ mic_calibrate │ {"type":"mic_calibrate"}
 * └───────────────┴────────────────────────────────────────────────────────────┘
 *
 * 完整示例:
 * - 重启: {"type":"custom","payload":{"message":"{\"type\":\"reboot\"}"}}
 * - 音量: {"type":"custom","payload":{"message":"{\"type\":\"volume\",\"value\":70}"}}
 * - STT回调: {"type":"custom","payload":{"message":"{\"type\":\"stt_url\",\"url\":\"https://example.com/stt\"}"}}
 */
class RemoteCmd {
public:
    explicit RemoteCmd(Application* app);
    bool Handle(const cJSON* payload);

    // STT 文本回调：在后台 POST 到 stt_url（由 Application 在收到完整 STT 时调用）
    void PostSttText(const std::string& text);

private:
    void OnReboot();
    void OnOta();
    void OnReconnect();
    void OnWakeup(const cJSON* msg);
    void OnTts(const cJSON* msg);
    void OnTtai(const cJSON* msg);
    void OnVolume(const cJSON* msg);
    void OnGain(const cJSON* msg);
    void OnMicCalibrate();
    void OnDownload(const cJSON* msg);
    void OnVadConfig(const cJSON* msg);
    void OnSleep(const cJSON* msg);
    void OnFlow(const cJSON* msg);
    void OnSttUrl(const cJSON* msg);
    void OnMusicPlay(const cJSON* msg);
    void OnMusicStop();

    Application* app_;
    std::string stt_url_;  // 运行时缓存，启动时从 NVS 加载
    std::atomic<bool> stt_posting_{false};  // 防止并发 POST
};

#endif
