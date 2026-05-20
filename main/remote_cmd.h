#ifndef REMOTE_CMD_H
#define REMOTE_CMD_H

#include <cJSON.h>
#include <esp_timer.h>
#include <functional>
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
 * │ gain          │ {"type":"gain", "input":15, "aec":6}                       │
 * │ download      │ {"type":"download", "files":[...], "emoji":"happy"}        │
 * │ audio_debug   │ {"type":"audio_debug", "server":"IP:8000", "mode":"raw"}   │
 * │ reload        │ {"type":"reload"}  (重新拉 OTA · 不重启切换平台/协议)        │
 * │ sleep         │ {"type":"sleep", "gyro":true}  (gyro=是否陀螺仪唤醒)        │
 * │ flow         │ {"type":"flow","action":"start/stop/status/load"} │
 * │ music_play    │ {"type":"music_play","url":"https://xxx.mp3","title":"xxx"} │
 * │ music_stop    │ {"type":"music_stop"}                                       │
 * │ music_pause   │ {"type":"music_pause"}                                      │
 * │ music_resume  │ {"type":"music_resume"}                                     │
 * │ update_prompt │ {"type":"update_prompt","model_type":2,"prompt":"..."}      │
 * │ mic_calibrate │ {"type":"mic_calibrate"}
 * │ wakeword      │ {"type":"wakeword","mode":"afe|custom","text":"..."} · 无 mode = 查询
 * └───────────────┴────────────────────────────────────────────────────────────┘
 *
 * 完整示例:
 * - 重启: {"type":"custom","payload":{"message":"{\"type\":\"reboot\"}"}}
 * - 音量: {"type":"custom","payload":{"message":"{\"type\":\"volume\",\"value\":70}"}}
 */
class RemoteCmd {
public:
    explicit RemoteCmd(Application* app);
    ~RemoteCmd();
    bool Handle(const cJSON* payload);

private:
    void ScheduleDelayedAction(int ms, std::function<void()> action);
    static void DelayTimerCallback(void* arg);
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
    void OnSleep(const cJSON* msg);
    void OnFlow(const cJSON* msg);
    void OnMusicPlay(const cJSON* msg);
    void OnMusicStop();
    void OnMusicPause();
    void OnMusicResume();
    void OnEduPool(const cJSON* msg);
    void OnUpdatePrompt(const cJSON* msg);
    void OnWakeWord(const cJSON* msg);

    Application* app_;

    esp_timer_handle_t delay_timer_ = nullptr;
    std::function<void()> pending_action_;
};

#endif
