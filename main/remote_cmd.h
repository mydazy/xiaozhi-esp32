#ifndef REMOTE_CMD_H
#define REMOTE_CMD_H

#include <cJSON.h>

class Application;

/**
 * RemoteCmd — application.cc 的 custom message 路由（精简版）
 *
 * 服务端推送格式：
 *   {"type":"custom","payload":{"message":"{\"type\":\"...\"}"}}
 *
 * 命令集：
 *   {"type":"music_play","url":"https://xxx.mp3","title":"xxx"}
 *   {"type":"music_stop"}
 *   {"type":"wakeup","text":"你好"}                // 文本唤醒 + 进入 listening（WakeWordInvoke）
 *   {"type":"send_text","text":"..."}              // 透传文本到当前协议通道，不切状态
 *   {"type":"reboot"}                              // 软重启
 *   {"type":"ota"}                                 // 检查并升级固件（CheckVersion + UpgradeFirmware）
 *
 * Handle(payload) 返回：
 *   true  = 已识别并已 schedule 处理；调用方吃掉消息即可
 *   false = 未识别；调用方可走兜底（例如 SetChatMessage 显示原文）
 */
class RemoteCmd {
public:
    explicit RemoteCmd(Application* app) : app_(app) {}

    bool Handle(const cJSON* payload);

private:
    void OnMusicPlay(const cJSON* msg);
    void OnMusicStop();
    void OnWakeup(const cJSON* msg);
    void OnSendText(const cJSON* msg);
    void OnReboot();
    void OnOta();

    Application* app_;
};

#endif  // REMOTE_CMD_H
