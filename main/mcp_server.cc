/*
 * MCP Server Implementation
 * Reference: https://modelcontextprotocol.io/specification/2024-11-05
 */

#include "mcp_server.h"
#include <esp_log.h>
#include <esp_app_desc.h>
#include <algorithm>
#include <cstring>
#include <esp_pthread.h>

#include "application.h"
#include "display.h"
#include "oled_display.h"
#include "display/ui_display.h"
#include "board.h"
#include "system_info.h"
#include "settings.h"
#include "lvgl_theme.h"
#include "lvgl_display.h"
#include "audio/music_player.h"
#include "power_manager.h"
#include "assets/lang_config.h"

#define TAG "MCP"

static std::atomic<int> g_preview_inflight_{0};
static constexpr int kPreviewMaxInflight = 2;

McpServer::McpServer() {
}

McpServer::~McpServer() {
    std::lock_guard<std::mutex> lk(tools_mutex_);
    for (auto tool : tools_) {
        delete tool;
    }
    tools_.clear();
}

void McpServer::AddCommonTools() {
    // *Important* To speed up the response time, we add the common tools to the beginning of
    // the tools list to utilize the prompt cache.
    // **重要** 为了提升响应速度，我们把常用的工具放在前面，利用 prompt cache 的特性。

    // Backup the original tools list and restore it after adding the common tools.
    std::vector<McpTool*> original_tools;
    {
        std::lock_guard<std::mutex> lk(tools_mutex_);
        original_tools = std::move(tools_);
    }
    auto& board = Board::GetInstance();

    // Do not add custom tools here.
    // Custom tools must be added in the board's InitializeTools function.
    // 获取MAC地址工具
    AddTool("self.get_mac_address",
        "获取设备 MAC 地址。",
        PropertyList(),
        [&board](const PropertyList& properties) -> ReturnValue {
            return SystemInfo::GetMacAddress();
        });

    AddTool("self.get_device_status",
        "查询设备当前状态：音量、屏幕、电池、网络等。"
        "用户问『现在音量多少 / 电量 / 屏幕亮度』或要改这些之前先调用。",
        PropertyList(),
        [&board](const PropertyList& properties) -> ReturnValue {
            return board.GetDeviceStatusJson();
        });

    // AEC控制工具
    AddTool("self.audio.set_aec",
        "设置说话打断模式：mode='off' 关闭打断，mode='device' 开启任意打断（自然交流）。",
        PropertyList({Property("mode", kPropertyTypeString)}),
        [&board](const PropertyList& properties) -> ReturnValue {
            auto& app = Application::GetInstance();
            std::string mode = properties["mode"].value<std::string>();
            AecMode aec_mode = (mode == "device") ? kAecOnDeviceSide : kAecOff;
            app.SetAecMode(aec_mode);
            return "AEC set to " + mode;
        });

    // 说话结束提示音开关（确认服务器已收到+识别用户语音 · 持久化 audio.stt_popup · 默认开启）
    AddTool("self.audio.set_stt_popup",
        "开关『识别完那一声咚』的提示音。"
        "用户说『关掉提示音 / 别咚了 / 打开提示音』时调用。重启后保留。",
        PropertyList({
            Property("enabled", kPropertyTypeBoolean)
        }),
        [](const PropertyList& properties) -> ReturnValue {
            bool enabled = properties["enabled"].value<bool>();
            Application::GetInstance().SetSttPopupEnabled(enabled);
            return std::string("{\"success\":true,\"enabled\":") + (enabled ? "true" : "false") + "}";
        });

    AddTool("self.audio.get_stt_popup",
        "查识别提示音是开还是关。",
        PropertyList(),
        [](const PropertyList& properties) -> ReturnValue {
            bool enabled = Application::GetInstance().IsSttPopupEnabled();
            return std::string("{\"enabled\":") + (enabled ? "true" : "false") + "}";
        });

    // 唤醒词配置 · mode=afe 回归默认 · mode=custom + text 启用自定义（须在 MultiNet 词表内 · 重启生效）
    AddTool("self.audio.set_wakeword",
        "改唤醒词。mode='afe' 恢复默认；mode='custom' 启用自定义（text 必填，须在词表里）。重启生效。",
        PropertyList({
            Property("mode", kPropertyTypeString),
            Property("text", kPropertyTypeString, "")
        }),
        [](const PropertyList& properties) -> ReturnValue {
            std::string mode = properties["mode"].value<std::string>();
            std::string text = properties["text"].value<std::string>();
            Settings s("wakeword", true);
            s.SetString("mode", mode);
            s.SetString("text", text);
            Application::GetInstance().Schedule([]() {
                Application::GetInstance().Alert("唤醒词已更新", "重启后生效", "", Lang::Sounds::OGG_VIBRATION);
            });
            return std::string("{\"success\":true,\"mode\":\"") + mode + "\",\"text\":\"" + text + "\"}";
        });

    AddTool("self.audio.get_wakeword",
        "查当前唤醒词配置。",
        PropertyList(),
        [](const PropertyList&) -> ReturnValue {
            Settings s("wakeword", false);
            return std::string("{\"mode\":\"") + s.GetString("mode", "afe") +
                   "\",\"text\":\"" + s.GetString("text", "") + "\"}";
        });

    AddTool("self.audio_speaker.set_volume",
        "设置喇叭音量 0-100。如果不知道当前音量，先调 self.get_device_status 查。",
        PropertyList({
            Property("volume", kPropertyTypeInteger, 0, 100)
        }),
        [&board](const PropertyList& properties) -> ReturnValue {
            auto codec = board.GetAudioCodec();
            codec->SetOutputVolume(properties["volume"].value<int>());
            return true;
        });

    // ============================================================
    // 自动休眠开关（语音可控 · 持久化 NVS · 立即生效）
    //   关闭 → PowerSaveTimer 停止 · 永不进软省电（LCD 不降亮）也永不进深睡
    //   开启 → 60s 后软省电（LCD 降亮 + 状态上报）· 300s 后深睡（充电中跳过）
    // 充电场景：即使 sleep_mode=ON · OnShutdownRequest 内已加 IsChargingGlobal 跳过
    AddTool("self.power.set_sleep_mode",
        "开关自动休眠。关掉后屏幕一直亮不变暗，也不进深度睡眠。"
        "用户说『关闭休眠 / 别睡 / 一直亮着』时调用。充电时本来就不会深睡。重启后保留。",
        PropertyList({
            Property("enabled", kPropertyTypeBoolean)
        }),
        [&board](const PropertyList& properties) -> ReturnValue {
            bool enabled = properties["enabled"].value<bool>();
            board.EnableAutoSleep(enabled);
            return std::string("{\"success\":true,\"sleep_mode\":") + (enabled ? "true" : "false") + "}";
        });

    AddTool("self.power.get_sleep_mode",
        "查自动休眠开关和是否在充电（充电时深睡会被跳过）。",
        PropertyList(),
        [&board](const PropertyList&) -> ReturnValue {
            bool enabled = board.IsAutoSleepEnabled();
            bool charging = PowerManager::IsChargingGlobal();
            bool deep_sleep_skipped = enabled && charging;
            std::string r = "{\"enabled\":";
            r += enabled ? "true" : "false";
            r += ",\"charging\":";
            r += charging ? "true" : "false";
            r += ",\"deep_sleep_skipped\":";
            r += deep_sleep_skipped ? "true" : "false";
            r += "}";
            return r;
        });

    // MP3 流式播放 — 云端识别到 MP3 URL 后通过此 tool 让设备播放
    AddTool("self.music.play",
        "播放 MP3 音乐（HTTP/HTTPS 链接）。"
        "用户说『放首歌 / 来点音乐 / 播放 XXX』时调用。"
        "注意：播放期间设备听不到语音，要按按键或等播完才能再唤醒。",
        PropertyList({
            Property("url", kPropertyTypeString),
            Property("title", kPropertyTypeString, std::string(""))
        }),
        [](const PropertyList& properties) -> ReturnValue {
            std::string url = properties["url"].value<std::string>();
            std::string title = properties["title"].value<std::string>();

            // 同步校验（不开后台任务，快速返回给云端）
            if (url.empty()) {
                return std::string("{\"success\":false,\"error\":\"URL is empty\"}");
            }
            if (url.substr(0, 7) != "http://" && url.substr(0, 8) != "https://") {
                return std::string("{\"success\":false,\"error\":\"URL must start with http:// or https://\"}");
            }

            auto& app = Application::GetInstance();
            app.Schedule([url = std::move(url), title = std::move(title)]() {
                auto& app = Application::GetInstance();
                // 关闭 AFE voice communication，避免 Core 1 CPU 争抢 → AFE ringbuffer 饥饿
                auto state = app.GetDeviceState();
                if (state == kDeviceStateSpeaking) {
                    app.AbortSpeaking(kAbortReasonNone);
                }
                if (state == kDeviceStateListening || state == kDeviceStateSpeaking ||
                    state == kDeviceStateConnecting) {
                    app.CloseAudioChannel();
                }
                app.GetAudioService().ResetDecoder();

                std::string err;
                if (!MusicPlayer::GetInstance().Play(url, title, &err)) {
                    ESP_LOGW(TAG, "MCP music.play 启动失败: %s", err.c_str());
                    app.Alert("播放失败", err.c_str(), "", "");
                } else if (auto* ui = dynamic_cast<UiDisplay*>(Board::GetInstance().GetDisplay())) {
                    ui->SwitchToPlayerMode(title.empty() ? "正在播放" : title.c_str());
                    ui->OnPlayerPauseToggle([] {
                        auto& mp = MusicPlayer::GetInstance();
                        if (mp.IsPaused()) mp.Resume(); else mp.Pause();
                    });
                } else if (!title.empty()) {
                    Board::GetInstance().GetDisplay()->ShowNotification(title.c_str(), 3000);
                }
            });
            // 异步启动成功：后续错误（HTTP/解码）通过 UI Alert 反馈
            return std::string("{\"success\":true,\"playing\":true}");
        });

    AddTool("self.music.stop",
        "停止音乐。用户说『停 / 别放了 / 安静 / 关音乐』时调用。",
        PropertyList(),
        [](const PropertyList& properties) -> ReturnValue {
            bool was_playing = MusicPlayer::GetInstance().IsPlaying();
            Application::GetInstance().Schedule([]() {
                MusicPlayer::GetInstance().Stop();
                if (auto* ui = dynamic_cast<UiDisplay*>(Board::GetInstance().GetDisplay())) {
                    ui->SwitchOutPlayerMode();
                }
            });
            return std::string(was_playing ? "{\"was_playing\":true}" : "{\"was_playing\":false}");
        });
    
    auto backlight = board.GetBacklight();
    if (backlight) {
        AddTool("self.screen.set_brightness",
            "设置屏幕亮度 0-100。",
            PropertyList({
                Property("brightness", kPropertyTypeInteger, 0, 100)
            }),
            [backlight](const PropertyList& properties) -> ReturnValue {
                uint8_t brightness = static_cast<uint8_t>(properties["brightness"].value<int>());
                backlight->SetBrightness(brightness, true);
                return true;
            });
    }

#ifdef HAVE_LVGL
    auto display = board.GetDisplay();
    if (display && display->GetTheme() != nullptr) {
        AddTool("self.screen.set_theme",
            "设置屏幕主题：light 浅色 / dark 深色。",
            PropertyList({
                Property("theme", kPropertyTypeString)
            }),
            [display](const PropertyList& properties) -> ReturnValue {
                auto theme_name = properties["theme"].value<std::string>();
                auto& theme_manager = LvglThemeManager::GetInstance();
                auto theme = theme_manager.GetTheme(theme_name);
                if (theme != nullptr) {
                    display->SetTheme(theme);
                    return true;
                }
                return false;
            });
    }

#endif

    // Restore the original tools list to the end of the tools list
    {
        std::lock_guard<std::mutex> lk(tools_mutex_);
        tools_.insert(tools_.end(), original_tools.begin(), original_tools.end());
    }
}

void McpServer::AddUserOnlyTools() {
    // System tools
    AddUserOnlyTool("self.get_system_info",
        "查系统信息。",
        PropertyList(),
        [this](const PropertyList& properties) -> ReturnValue {
            auto& board = Board::GetInstance();
            return board.GetSystemInfoJson();
        });

    AddUserOnlyTool("self.reboot", "重启设备。",
        PropertyList(),
        [this](const PropertyList& properties) -> ReturnValue {
            auto& app = Application::GetInstance();
            app.Schedule([&app]() {
                ESP_LOGW(TAG, "User requested reboot");
                vTaskDelay(pdMS_TO_TICKS(1000));

                app.Reboot();
            });
            return true;
        });


    // Display control
#ifdef HAVE_LVGL
    auto display = dynamic_cast<LvglDisplay*>(Board::GetInstance().GetDisplay());
    if (display) {
        AddUserOnlyTool("self.screen.get_info", "查屏幕宽高、是否单色。",
            PropertyList(),
            [display](const PropertyList& properties) -> ReturnValue {
                cJSON *json = cJSON_CreateObject();
                cJSON_AddNumberToObject(json, "width", display->width());
                cJSON_AddNumberToObject(json, "height", display->height());
                if (dynamic_cast<OledDisplay*>(display)) {
                    cJSON_AddBoolToObject(json, "monochrome", true);
                } else {
                    cJSON_AddBoolToObject(json, "monochrome", false);
                }
                return json;
            });
    }
#endif // HAVE_LVGL
}

void McpServer::AddTool(McpTool* tool) {
    std::lock_guard<std::mutex> lk(tools_mutex_);
    // Prevent adding duplicate tools
    if (std::find_if(tools_.begin(), tools_.end(), [tool](const McpTool* t) { return t->name() == tool->name(); }) != tools_.end()) {
        ESP_LOGW(TAG, "Tool %s already added", tool->name().c_str());
        delete tool;  // 防泄漏：原代码同名 AddTool 直接 return 丢失 new 出来的对象
        return;
    }

    ESP_LOGI(TAG, "Add tool: %s%s", tool->name().c_str(), tool->user_only() ? " [user]" : "");
    tools_.push_back(tool);
}

void McpServer::AddTool(const std::string& name, const std::string& description, const PropertyList& properties, std::function<ReturnValue(const PropertyList&)> callback) {
    AddTool(new McpTool(name, description, properties, callback));
}

void McpServer::AddUserOnlyTool(const std::string& name, const std::string& description, const PropertyList& properties, std::function<ReturnValue(const PropertyList&)> callback) {
    auto tool = new McpTool(name, description, properties, callback);
    tool->set_user_only(true);
    AddTool(tool);
}

void McpServer::ParseMessage(const std::string& message) {
    cJSON* json = cJSON_Parse(message.c_str());
    if (json == nullptr) {
        ESP_LOGE(TAG, "Failed to parse MCP message: %s", message.c_str());
        return;
    }
    ParseMessage(json);
    cJSON_Delete(json);
}

void McpServer::ParseCapabilities(const cJSON* capabilities) {
    auto vision = cJSON_GetObjectItem(capabilities, "vision");
    if (cJSON_IsObject(vision)) {
        auto url = cJSON_GetObjectItem(vision, "url");
        auto token = cJSON_GetObjectItem(vision, "token");
        if (cJSON_IsString(url)) {
            auto camera = Board::GetInstance().GetCamera();
            if (camera) {
                std::string url_str = std::string(url->valuestring);
                std::string token_str;
                if (cJSON_IsString(token)) {
                    token_str = std::string(token->valuestring);
                }
                camera->SetExplainUrl(url_str, token_str);
            }
        }
    }
}

void McpServer::ParseMessage(const cJSON* json) {
    // Check JSONRPC version
    auto version = cJSON_GetObjectItem(json, "jsonrpc");
    if (version == nullptr || !cJSON_IsString(version) || strcmp(version->valuestring, "2.0") != 0) {
        ESP_LOGE(TAG, "Invalid JSONRPC version: %s", version ? version->valuestring : "null");
        return;
    }
    
    // Check method
    auto method = cJSON_GetObjectItem(json, "method");
    if (method == nullptr || !cJSON_IsString(method)) {
        ESP_LOGE(TAG, "Missing method");
        return;
    }
    
    auto method_str = std::string(method->valuestring);
    if (method_str.find("notifications") == 0) {
        return;
    }
    
    // Check params
    auto params = cJSON_GetObjectItem(json, "params");
    if (params != nullptr && !cJSON_IsObject(params)) {
        ESP_LOGE(TAG, "Invalid params for method: %s", method_str.c_str());
        return;
    }

    auto id = cJSON_GetObjectItem(json, "id");
    if (id == nullptr || !cJSON_IsNumber(id)) {
        ESP_LOGE(TAG, "Invalid id for method: %s", method_str.c_str());
        return;
    }
    auto id_int = id->valueint;
    
    if (method_str == "initialize") {
        if (cJSON_IsObject(params)) {
            auto capabilities = cJSON_GetObjectItem(params, "capabilities");
            if (cJSON_IsObject(capabilities)) {
                ParseCapabilities(capabilities);
            }
        }
        auto app_desc = esp_app_get_description();
        std::string message = "{\"protocolVersion\":\"2024-11-05\",\"capabilities\":{\"tools\":{}},\"serverInfo\":{\"name\":\"" BOARD_NAME "\",\"version\":\"";
        message += app_desc->version;
        message += "\"}}";
        ReplyResult(id_int, message);
    } else if (method_str == "tools/list") {
        std::string cursor_str = "";
        bool list_user_only_tools = false;
        if (params != nullptr) {
            auto cursor = cJSON_GetObjectItem(params, "cursor");
            if (cJSON_IsString(cursor)) {
                cursor_str = std::string(cursor->valuestring);
            }
            auto with_user_tools = cJSON_GetObjectItem(params, "withUserTools");
            if (cJSON_IsBool(with_user_tools)) {
                list_user_only_tools = with_user_tools->valueint == 1;
            }
        }
        GetToolsList(id_int, cursor_str, list_user_only_tools);
    } else if (method_str == "tools/call") {
        if (!cJSON_IsObject(params)) {
            ESP_LOGE(TAG, "tools/call: Missing params");
            ReplyError(id_int, "Missing params");
            return;
        }
        auto tool_name = cJSON_GetObjectItem(params, "name");
        if (!cJSON_IsString(tool_name)) {
            ESP_LOGE(TAG, "tools/call: Missing name");
            ReplyError(id_int, "Missing name");
            return;
        }
        auto tool_arguments = cJSON_GetObjectItem(params, "arguments");
        if (tool_arguments != nullptr && !cJSON_IsObject(tool_arguments)) {
            ESP_LOGE(TAG, "tools/call: Invalid arguments");
            ReplyError(id_int, "Invalid arguments");
            return;
        }
        DoToolCall(id_int, std::string(tool_name->valuestring), tool_arguments);
    } else {
        ESP_LOGE(TAG, "Method not implemented: %s", method_str.c_str());
        ReplyError(id_int, "Method not implemented: " + method_str);
    }
}

void McpServer::ReplyResult(int id, const std::string& result) {
    std::string payload = "{\"jsonrpc\":\"2.0\",\"id\":";
    payload += std::to_string(id) + ",\"result\":";
    payload += result;
    payload += "}";
    Application::GetInstance().SendMcpMessage(payload);
}

void McpServer::ReplyError(int id, const std::string& message) {
    std::string payload = "{\"jsonrpc\":\"2.0\",\"id\":";
    payload += std::to_string(id);
    payload += ",\"error\":{\"message\":\"";
    payload += message;
    payload += "\"}}";
    Application::GetInstance().SendMcpMessage(payload);
}

void McpServer::GetToolsList(int id, const std::string& cursor, bool list_user_only_tools) {
    std::lock_guard<std::mutex> lk(tools_mutex_);
    const int max_payload_size = 12000;
    std::string json = "{\"tools\":[";

    bool found_cursor = cursor.empty();
    auto it = tools_.begin();
    std::string next_cursor = "";
    
    while (it != tools_.end()) {
        // 如果我们还没有找到起始位置，继续搜索
        if (!found_cursor) {
            if ((*it)->name() == cursor) {
                found_cursor = true;
            } else {
                ++it;
                continue;
            }
        }

        if (!list_user_only_tools && (*it)->user_only()) {
            ++it;
            continue;
        }
        
        // 添加tool前检查大小
        std::string tool_json = (*it)->to_json() + ",";
        if (json.length() + tool_json.length() + 30 > max_payload_size) {
            // 如果添加这个tool会超出大小限制，设置next_cursor并退出循环
            next_cursor = (*it)->name();
            break;
        }
        
        json += tool_json;
        ++it;
    }
    
    if (json.back() == ',') {
        json.pop_back();
    }
    
    if (json.back() == '[' && !tools_.empty()) {
        // 如果没有添加任何tool，返回错误
        ESP_LOGE(TAG, "tools/list: Failed to add tool %s because of payload size limit", next_cursor.c_str());
        ReplyError(id, "Failed to add tool " + next_cursor + " because of payload size limit");
        return;
    }

    if (next_cursor.empty()) {
        json += "]}";
    } else {
        json += "],\"nextCursor\":\"" + next_cursor + "\"}";
    }

    // 实测 payload 字节数 + 是否触发分页（便于量产前审计 · 对齐 mcp-flows § 8.2 N1）
    ESP_LOGI(TAG, "tools/list: id=%d, payload=%u B, paged=%s, cursor=%s",
             id, (unsigned)json.size(), next_cursor.empty() ? "no" : "YES",
             cursor.empty() ? "(first)" : cursor.c_str());

    ReplyResult(id, json);
}

void McpServer::DoToolCall(int id, const std::string& tool_name, const cJSON* tool_arguments) {
    McpTool* tool = nullptr;
    {
        std::lock_guard<std::mutex> lk(tools_mutex_);
        auto tool_iter = std::find_if(tools_.begin(), tools_.end(),
                                     [&tool_name](const McpTool* t) {
                                         return t->name() == tool_name;
                                     });
        if (tool_iter != tools_.end()) {
            tool = *tool_iter;
        }
    }

    if (tool == nullptr) {
        ESP_LOGE(TAG, "tools/call: Unknown tool: %s", tool_name.c_str());
        ReplyError(id, "Unknown tool: " + tool_name);
        return;
    }

    PropertyList arguments = tool->properties();
    try {
        for (auto& argument : arguments) {
            bool found = false;
            if (cJSON_IsObject(tool_arguments)) {
                auto value = cJSON_GetObjectItem(tool_arguments, argument.name().c_str());
                if (argument.type() == kPropertyTypeBoolean && cJSON_IsBool(value)) {
                    argument.set_value<bool>(value->valueint == 1);
                    found = true;
                } else if (argument.type() == kPropertyTypeInteger && cJSON_IsNumber(value)) {
                    argument.set_value<int>(value->valueint);
                    found = true;
                } else if (argument.type() == kPropertyTypeString && cJSON_IsString(value)) {
                    argument.set_value<std::string>(value->valuestring);
                    found = true;
                }
            }

            if (!argument.has_default_value() && !found) {
                ESP_LOGE(TAG, "tools/call: Missing valid argument: %s", argument.name().c_str());
                ReplyError(id, "Missing valid argument: " + argument.name());
                return;
            }
        }
    } catch (const std::exception& e) {
        ESP_LOGE(TAG, "tools/call: %s", e.what());
        ReplyError(id, e.what());
        return;
    }

    // Use main thread to call the tool
    auto& app = Application::GetInstance();
    app.Schedule([this, id, tool, arguments = std::move(arguments)]() {
        try {
            ReplyResult(id, tool->Call(arguments));
        } catch (const std::exception& e) {
            ESP_LOGE(TAG, "tools/call: %s", e.what());
            ReplyError(id, e.what());
        }
    });
}
