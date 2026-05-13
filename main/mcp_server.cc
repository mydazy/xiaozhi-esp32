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
    // 启动期单线程跑 · 这里 move 和末尾 insert 各加一次锁（防御性 · 不和内嵌 AddTool 嵌套）
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
        "Get the device MAC address.",
        PropertyList(),
        [&board](const PropertyList& properties) -> ReturnValue {
            return SystemInfo::GetMacAddress();
        });

    AddTool("self.get_device_status",
        "Provides the real-time information of the device, including the current status of the audio speaker, screen, battery, network, etc.\n"
        "Use this tool for: \n"
        "1. Answering questions about current condition (e.g. what is the current volume of the audio speaker?)\n"
        "2. As the first step to control the device (e.g. turn up / down the volume of the audio speaker, etc.)",
        PropertyList(),
        [&board](const PropertyList& properties) -> ReturnValue {
            return board.GetDeviceStatusJson();
        });

    // AEC控制工具
    AddTool("self.audio.set_aec",
        "Set AEC mode: 'off' '关闭自然交流' (关闭打断), 'device' '开启自然交流' (开启任意打断)",
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
        "Enable or disable the 'pop' sound that plays after server recognizes user speech (STT confirmation). "
        "Default enabled. Persists across reboots. "
        "Use when user says: 关闭说话结束提示音/打开说话结束提示音/不要那个咚的声音.",
        PropertyList({
            Property("enabled", kPropertyTypeBoolean)
        }),
        [](const PropertyList& properties) -> ReturnValue {
            bool enabled = properties["enabled"].value<bool>();
            Application::GetInstance().SetSttPopupEnabled(enabled);
            return std::string("{\"success\":true,\"enabled\":") + (enabled ? "true" : "false") + "}";
        });

    AddTool("self.audio.get_stt_popup",
        "Get current STT confirmation popup sound state. Returns JSON {\"enabled\":bool}.",
        PropertyList(),
        [](const PropertyList& properties) -> ReturnValue {
            bool enabled = Application::GetInstance().IsSttPopupEnabled();
            return std::string("{\"enabled\":") + (enabled ? "true" : "false") + "}";
        });

    AddTool("self.audio_speaker.set_volume",
        "Set the volume of the audio speaker. If the current volume is unknown, you must call `self.get_device_status` tool first and then call this tool.",
        PropertyList({
            Property("volume", kPropertyTypeInteger, 0, 100)
        }),
        [&board](const PropertyList& properties) -> ReturnValue {
            auto codec = board.GetAudioCodec();
            codec->SetOutputVolume(properties["volume"].value<int>());
            return true;
        });

    // MP3 流式播放 — 云端识别到 MP3 URL 后通过此 tool 让设备播放
    AddTool("self.music.play",
        "Play an MP3 audio stream from an HTTP(S) URL.\n"
        "Use when the user asks to play music, a song, or any MP3 audio.\n"
        "'title' is optional and displayed/logged as current track name.\n"
        "\n"
        "## Side effects (IMPORTANT — tell user before calling if asked):\n"
        "- Stops any ongoing TTS and previous music automatically.\n"
        "- **Pauses voice listening (AFE off) during playback** — user CANNOT call AI by voice while music plays.\n"
        "- User must press a button or wake word (after music ends) to resume voice.\n"
        "- UI switches to Player mode (track title + pause/play affordance).\n"
        "\n"
        "## Returns JSON {\"success\":bool,\"playing\":bool,\"error\":string} — check 'success' before telling user.\n"
        "Failure surfaces via screen Alert (HTTP/decode errors).",
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
        "Stop the currently playing MP3 music. Use when user says stop/pause/quiet. "
        "Returns JSON {\"was_playing\":bool}.",
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
            "Set the brightness of the screen.",
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
            "Set the theme of the screen. The theme can be `light` or `dark`.",
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

    auto camera = board.GetCamera();
    if (camera) {
        AddTool("self.camera.take_photo",
            "Always remember you have a camera. If the user asks you to see something, use this tool to take a photo and then explain it.\n"
            "Args:\n"
            "  `question`: The question that you want to ask about the photo.\n"
            "Return:\n"
            "  A JSON object that provides the photo information.",
            PropertyList({
                Property("question", kPropertyTypeString)
            }),
            [camera](const PropertyList& properties) -> ReturnValue {
                // Lower the priority to do the camera capture
                TaskPriorityReset priority_reset(1);

                if (!camera->Capture()) {
                    throw std::runtime_error("Failed to capture photo");
                }
                auto question = properties["question"].value<std::string>();
                return camera->Explain(question);
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
        "Get the system information",
        PropertyList(),
        [this](const PropertyList& properties) -> ReturnValue {
            auto& board = Board::GetInstance();
            return board.GetSystemInfoJson();
        });

    AddUserOnlyTool("self.reboot", "Reboot the system",
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

    // Firmware upgrade
    AddUserOnlyTool("self.upgrade_firmware", "Upgrade firmware from a specific URL. This will download and install the firmware, then reboot the device.",
        PropertyList({
            Property("url", kPropertyTypeString, "The URL of the firmware binary file to download and install")
        }),
        [this](const PropertyList& properties) -> ReturnValue {
            auto url = properties["url"].value<std::string>();
            ESP_LOGI(TAG, "User requested firmware upgrade from URL: %s", url.c_str());
            
            auto& app = Application::GetInstance();
            app.Schedule([url, &app]() {
                bool success = app.UpgradeFirmware(url);
                if (!success) {
                    ESP_LOGE(TAG, "Firmware upgrade failed");
                }
            });
            
            return true;
        });

    // Display control
#ifdef HAVE_LVGL
    auto display = dynamic_cast<LvglDisplay*>(Board::GetInstance().GetDisplay());
    if (display) {
        AddUserOnlyTool("self.screen.get_info", "Information about the screen, including width, height, etc.",
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

#if CONFIG_LV_USE_SNAPSHOT
        AddUserOnlyTool("self.screen.snapshot", "Snapshot the screen and upload it to a specific URL",
            PropertyList({
                Property("url", kPropertyTypeString),
                Property("quality", kPropertyTypeInteger, 80, 1, 100)
            }),
            [display](const PropertyList& properties) -> ReturnValue {
                auto url = properties["url"].value<std::string>();
                auto quality = properties["quality"].value<int>();

                // ① 主线程同步截图（LVGL 必须在 main_loop）
                auto jpeg = std::make_shared<std::string>();
                if (!display->SnapshotToJpeg(*jpeg, quality)) {
                    throw std::runtime_error("Failed to snapshot screen");
                }
                ESP_LOGI(TAG, "Snapshot %u bytes, scheduling upload to %s", (unsigned)jpeg->size(), url.c_str());

                // ② 后台任务异步上传（弱网 30s+ 不再阻塞 main_loop）
                struct UploadCtx { std::shared_ptr<std::string> jpeg; std::string url; };
                auto* ctx = new UploadCtx{jpeg, url};
                BaseType_t r = xTaskCreatePinnedToCore([](void* arg) {
                    auto* c = static_cast<UploadCtx*>(arg);
                    const std::string boundary = "----ESP32_SCREEN_SNAPSHOT_BOUNDARY";
                    auto http = Board::GetInstance().GetNetwork()->CreateHttp(3);
                    http->SetHeader("Content-Type", "multipart/form-data; boundary=" + boundary);
                    bool ok = false;
                    if (http->Open("POST", c->url)) {
                        std::string head = "--" + boundary + "\r\n"
                            "Content-Disposition: form-data; name=\"file\"; filename=\"screenshot.jpg\"\r\n"
                            "Content-Type: image/jpeg\r\n\r\n";
                        http->Write(head.c_str(), head.size());
                        http->Write(c->jpeg->data(), c->jpeg->size());
                        std::string foot = "\r\n--" + boundary + "--\r\n";
                        http->Write(foot.c_str(), foot.size());
                        http->Write("", 0);
                        ok = (http->GetStatusCode() == 200);
                        if (ok) {
                            std::string result = http->ReadAll();
                            ESP_LOGI(TAG, "Snapshot upload result: %s", result.c_str());
                        } else {
                            ESP_LOGW(TAG, "Snapshot upload failed: status=%d url=%s",
                                     http->GetStatusCode(), c->url.c_str());
                        }
                        http->Close();
                    } else {
                        ESP_LOGW(TAG, "Snapshot HTTP open failed: %s", c->url.c_str());
                    }
                    delete c;
                    vTaskDelete(NULL);
                }, "snap_upload", 4096, ctx, 3 /* P3 后台 IO · 低于 main P10 */, nullptr, 0 /* Core 0 · 网络 */);
                if (r != pdPASS) {
                    delete ctx;
                    throw std::runtime_error("Failed to spawn snapshot upload task");
                }
                return std::string("OK: snapshot uploading async");
            });

        AddUserOnlyTool("self.screen.preview_image", "Preview an image on the screen",
            PropertyList({
                Property("url", kPropertyTypeString)
            }),
            [display](const PropertyList& properties) -> ReturnValue {
                auto url = properties["url"].value<std::string>();

                // N2 修复 2026-05-12：限并发 · 防连发 5 次吃满 ~2.5MB SPIRAM
                // 阈值 2：① 满足"前一张未完成 → 立即换新图"体验 ② 不允许 3+ 并发预下载
                int cur = g_preview_inflight_.fetch_add(1, std::memory_order_acq_rel);
                if (cur >= kPreviewMaxInflight) {
                    g_preview_inflight_.fetch_sub(1, std::memory_order_acq_rel);
                    ESP_LOGW(TAG, "preview_image: in_flight=%d ≥ %d · 拒绝", cur, kPreviewMaxInflight);
                    throw std::runtime_error("preview_image busy, please retry in a moment");
                }

                // P0-2：HTTP 下载在后台任务跑 · 下载完成 Schedule 回主线程做 LVGL SetPreviewImage
                struct PreviewCtx { std::string url; Display* display; };
                auto* ctx = new PreviewCtx{url, display};
                BaseType_t r = xTaskCreatePinnedToCore([](void* arg) {
                    auto* c = static_cast<PreviewCtx*>(arg);
                    auto http = Board::GetInstance().GetNetwork()->CreateHttp(3);
                    char* data = nullptr;
                    size_t content_length = 0;
                    bool ok = false;
                    if (http->Open("GET", c->url) && http->GetStatusCode() == 200) {
                        content_length = http->GetBodyLength();
                        if (content_length > 0 && content_length <= 512 * 1024 /* 512KB 上限防 OOM */) {
                            data = (char*)heap_caps_malloc(content_length, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
                            if (data) {
                                size_t total = 0;
                                while (total < content_length) {
                                    int n = http->Read(data + total, content_length - total);
                                    if (n <= 0) break;
                                    total += n;
                                }
                                ok = (total == content_length);
                                if (!ok) { heap_caps_free(data); data = nullptr; }
                            }
                        } else {
                            ESP_LOGW(TAG, "preview_image: bad content_length=%u", (unsigned)content_length);
                        }
                    }
                    http->Close();

                    if (ok && data) {
                        char* data_owned = data;
                        size_t len = content_length;
                        Display* d = c->display;
                        Application::GetInstance().Schedule([d, data_owned, len]() {
                            auto image = std::make_unique<LvglAllocatedImage>(data_owned, len);
                            d->SetPreviewImage(std::move(image));
                        });
                    } else if (data) {
                        heap_caps_free(data);
                    } else {
                        ESP_LOGW(TAG, "preview_image: download failed: %s", c->url.c_str());
                    }
                    delete c;
                    g_preview_inflight_.fetch_sub(1, std::memory_order_acq_rel);  // N2 释放槽位
                    vTaskDelete(NULL);
                }, "preview_dl", 4096, ctx, 3, nullptr, 0 /* Core 0 · 网络 */);
                if (r != pdPASS) {
                    delete ctx;
                    g_preview_inflight_.fetch_sub(1, std::memory_order_acq_rel);  // N2 spawn 失败也释放
                    throw std::runtime_error("Failed to spawn preview download task");
                }
                return std::string("OK: preview loading async");
            });
#endif // CONFIG_LV_USE_SNAPSHOT
    }
#endif // HAVE_LVGL

    // Assets download url
    auto& assets = Assets::GetInstance();
    if (assets.partition_valid()) {
        AddUserOnlyTool("self.assets.set_download_url", "Set the download url for the assets",
            PropertyList({
                Property("url", kPropertyTypeString)
            }),
            [](const PropertyList& properties) -> ReturnValue {
                auto url = properties["url"].value<std::string>();
                Settings settings("assets", true);
                settings.SetString("download_url", url);
                return true;
            });
    }
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
