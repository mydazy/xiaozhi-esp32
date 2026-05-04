#ifndef DISPLAY_H
#define DISPLAY_H

#include "emoji_collection.h"

#ifndef CONFIG_USE_EMOTE_MESSAGE_STYLE
#define HAVE_LVGL 1
#include <lvgl.h>
#endif

#include <esp_timer.h>
#include <esp_log.h>
#include <esp_pm.h>

#include <string>
#include <chrono>
#include <functional>

class Theme {
public:
    Theme(const std::string& name) : name_(name) {}
    virtual ~Theme() = default;

    inline std::string name() const { return name_; }
private:
    std::string name_;
};

class Display {
public:
    Display();
    virtual ~Display();

    virtual void SetStatus(const char* status);
    virtual void ShowNotification(const char* notification, int duration_ms = 3000);
    virtual void ShowNotification(const std::string &notification, int duration_ms = 3000);
    virtual void SetEmotion(const char* emotion);
    virtual void SetChatMessage(const char* role, const char* content);
    virtual void ClearChatMessages();
    virtual void SetTheme(Theme* theme);
    virtual Theme* GetTheme() { return current_theme_; }
    virtual void UpdateStatusBar(bool update_all = false);
    virtual void SetPowerSaveMode(bool on);
    virtual void SetupUI() { 
        setup_ui_called_ = true;
    }

    /**
     * 通用二维码显示页（覆盖所有场景：配网 BLUFI/AP / 设备绑定 / 未来付费二维码 等）
     * 仅支持显示二维码的 Display 实现会覆盖；其他 Display 子类回退到空实现。
     *
     * 参数语义：第 2 参数固定为"加亮核心信息"（设备名/激活码/付款金额），蓝色大字突出。
     *
     * @param qr_content    完整二维码内容（调用方拼好：URL / WIFI:T:...;S:xxx;; / 任意字符串）
     * @param highlight     高亮大字（核心信息：BLUFI/AP 设备名 / 激活码 / 付款金额）
     * @param top           顶部提示词（如"微信扫码配网"/"绑定设备"/"扫码支付"）
     * @param bottom        底部辅助文字（操作说明，可选）
     * @param left_label    左色条标签（nullptr=不显示色条 → 简单二维码场景）
     * @param right_label   右色条标签
     * @param active_left   true=左色条高亮，false=右色条高亮（仅显示色条时生效）
     * @param on_double_click  整页双击 callback（仅显示色条时生效，用于切换模式）
     */
    virtual void ShowQrCode(const char* qr_content,
                            const char* highlight = nullptr,
                            const char* top = nullptr,
                            const char* bottom = nullptr,
                            const char* left_label = nullptr,
                            const char* right_label = nullptr,
                            bool active_left = true,
                            std::function<void()> on_double_click = nullptr) {}
    virtual void HideQrCode() {}

    inline int width() const { return width_; }
    inline int height() const { return height_; }
    inline bool IsSetupUICalled() const { return setup_ui_called_; }

protected:
    int width_ = 0;
    int height_ = 0;
    bool setup_ui_called_ = false;  // Track if SetupUI() has been called

    Theme* current_theme_ = nullptr;

    friend class DisplayLockGuard;
    virtual bool Lock(int timeout_ms = 0) = 0;
    virtual void Unlock() = 0;
};


class DisplayLockGuard {
public:
    DisplayLockGuard(Display *display) : display_(display) {
        if (!display_->Lock(30000)) {
            ESP_LOGE("Display", "Failed to lock display");
        }
    }
    ~DisplayLockGuard() {
        display_->Unlock();
    }

private:
    Display *display_;
};

class NoDisplay : public Display {
private:
    virtual bool Lock(int timeout_ms = 0) override {
        return true;
    }
    virtual void Unlock() override {}
};

#endif
