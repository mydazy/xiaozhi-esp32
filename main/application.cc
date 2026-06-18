#include "application.h"
#include "board.h"
#include "display.h"
#include "display/ui_display.h"
#include "system_info.h"
#include "audio_codec.h"
#include "mqtt_protocol.h"
#include "websocket_protocol.h"
#include "websocket_joyai_protocol.h"
#include "websocket_baidu_protocol.h"
#include "assets/lang_config.h"
#include "mcp_server.h"
#include "assets.h"
#include "settings.h"
#include "audio/codecs/box_audio_codec.h"
#include "remote_cmd.h"
#include "flow_engine.h"
#include "device_state_event.h"
#include "audio/music_player.h"
#include "alarm_manager.h"
#include "audio/alarm_ringer.h"
#include "pomodoro_manager.h"

#include <cstring>
#include <cstdlib>   // setenv / tzset (P0：闹钟时区基准)
#include <ctime>
#include <vector>
#include <optional>
#include <algorithm>   // std::remove · 绑定 URL 过滤 MAC 冒号
#include <esp_log.h>
#include <esp_heap_caps.h>
#include <cJSON.h>
#include <driver/gpio.h>
#include <arpa/inet.h>
#include <font_awesome.h>

#define TAG "Application"


namespace {
constexpr float kWakeThresholdNormal = 0.60f;
constexpr float kWakeThresholdInterrupt = 0.45f;

struct EduCard {
    std::string main;   // 主秀：汉字（≤4 字）/ 英文单词（≤12 字符）
    std::string top;    // 辅助：拼音 / 中文释义
};

bool ParseEduCard(const std::string& s, size_t lp, size_t rp, EduCard* out) {
    auto sep = s.find('_', lp + 1);
    if (sep == std::string::npos || sep >= rp) return false;
    if (s.find('_', sep + 1) < rp) return false;  // 多于 1 个 _ 拒绝

    out->main = s.substr(lp + 1, sep - lp - 1);
    out->top  = s.substr(sep + 1, rp - sep - 1);

    auto trim = [](std::string& x) {
        while (!x.empty() && x.front() == ' ') x.erase(0, 1);
        while (!x.empty() && x.back() == ' ') x.pop_back();
    };
    trim(out->main); trim(out->top);

    if (out->main.empty() || out->top.empty()) return false;
    if (out->main.size() > 32 || out->top.size() > 24) return false;
    return true;
}

// 从一句 LLM 文本提取教育卡（产品决策严格单卡）
std::optional<EduCard> ExtractEduCard(const std::string& s) {
    auto lp = s.find('[');
    if (lp == std::string::npos) return std::nullopt;
    auto rp = s.find(']', lp + 1);
    if (rp == std::string::npos) return std::nullopt;
    EduCard hit;
    if (!ParseEduCard(s, lp, rp, &hit)) return std::nullopt;
    return hit;
}

// 投递到 main loop 显示教育卡（state + FontGIF 守护）
void TriggerEduCard(EduCard hit) {
    Application::GetInstance().Schedule([hit = std::move(hit)]() {
        auto state = Application::GetInstance().GetDeviceState();
        if (state != kDeviceStateSpeaking) {
            ESP_LOGI(TAG, "[edu] skip @Schedule: state=%d (need Speaking=%d) main=%s",
                     (int)state, (int)kDeviceStateSpeaking, hit.main.c_str());
            return;
        }
        auto* ui = dynamic_cast<UiDisplay*>(Board::GetInstance().GetDisplay());
        if (!ui) {
            ESP_LOGW(TAG, "[edu] skip @Schedule: display !UiDisplay (dynamic_cast nullptr)");
            return;
        }
        bool gif_active = ui->IsFontGifActive();
        bool gif_pending = ui->IsFontPending();
        if (gif_active || gif_pending) {
            ESP_LOGI(TAG, "[edu] skip @Schedule: font_gif active=%d pending=%d main=%s",
                     (int)gif_active, (int)gif_pending, hit.main.c_str());
            return;
        }
        ESP_LOGI(TAG, "[edu] -> ShowEduCard: main='%s' top='%s'",
                 hit.main.c_str(), hit.top.c_str());
        ui->ShowEduCard(hit.main.c_str(), hit.top.c_str());
    });
}
}  // namespace


Application::Application() {
    event_group_ = xEventGroupCreate();

#if CONFIG_USE_DEVICE_AEC && CONFIG_USE_SERVER_AEC
#error "CONFIG_USE_DEVICE_AEC and CONFIG_USE_SERVER_AEC cannot be enabled at the same time"
#elif CONFIG_USE_DEVICE_AEC
    Settings settings("aecMode", false);
    aec_mode_ = (AecMode)settings.GetInt("deviceAec", 0);
#elif CONFIG_USE_SERVER_AEC
    aec_mode_ = kAecOnServerSide;
#else
    aec_mode_ = kAecOff;
#endif

    // 说话结束提示音配置（从 NVS 读 · 默认开启）
    {
        Settings audio_settings("audio", false);
        stt_popup_enabled_ = audio_settings.GetInt("stt_popup", 1) != 0;
        ESP_LOGI(TAG, "STT 提示音: %s", stt_popup_enabled_ ? "开启" : "关闭");
    }

    esp_timer_create_args_t clock_timer_args = {
        .callback = [](void* arg) {
            Application* app = (Application*)arg;
            xEventGroupSetBits(app->event_group_, MAIN_EVENT_CLOCK_TICK);
        },
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "clock_timer",
        .skip_unhandled_events = true
    };
    esp_timer_create(&clock_timer_args, &clock_timer_handle_);
}

Application::~Application() {
    if (clock_timer_handle_ != nullptr) {
        esp_timer_stop(clock_timer_handle_);
        esp_timer_delete(clock_timer_handle_);
    }
    vEventGroupDelete(event_group_);
}

bool Application::SetDeviceState(DeviceState state) {
    return state_machine_.TransitionTo(state);
}

void Application::Initialize() {
    auto& board = Board::GetInstance();
    SetDeviceState(kDeviceStateStarting);

    // CST-8 == UTC+8（中国标准时间）· POSIX TZ 符号反向所以是 -8
    setenv("TZ", "CST-8", 1);
    tzset();

    // Setup the display
    auto display = board.GetDisplay();
    display->SetupUI();

    // 等 LVGL 任务完成首帧黑底刷新（避免 GRAM 默认白色透出），再点亮背光。
    vTaskDelay(pdMS_TO_TICKS(80));
    if (auto* backlight = board.GetBacklight()) {
        backlight->RestoreBrightness();
    }

    // Print board name/version info
    display->SetChatMessage("system", SystemInfo::GetUserAgent().c_str());

    // Setup the audio service
    auto codec = board.GetAudioCodec();
    audio_service_.Initialize(codec);

    // 首次开机：MIC 灵敏度校准（NVS mic_type=0 表示未校准 · 必须在 audio_service.Start 之前做，
    if (Settings("audio", false).GetInt("mic_type", 0) == 0) {
        if (auto* box = dynamic_cast<BoxAudioCodec*>(codec)) {
            ESP_LOGW(TAG, "首次开机 MIC 校准开始（约 1 秒，会发出短促 1kHz 提示音）");
            box->CalibrateMicOnce();
        }
    }

    audio_service_.Start();

    // MP3 流式播放器（远程 music_play 命令触发）
    MusicPlayer::GetInstance().Initialize(codec);

    AudioServiceCallbacks callbacks;
    callbacks.on_send_queue_available = [this]() {
        xEventGroupSetBits(event_group_, MAIN_EVENT_SEND_AUDIO);
    };
    callbacks.on_wake_word_detected = [this](const std::string& wake_word) {
        xEventGroupSetBits(event_group_, MAIN_EVENT_WAKE_WORD_DETECTED);
    };
    callbacks.on_vad_change = [this](bool speaking) {
        xEventGroupSetBits(event_group_, MAIN_EVENT_VAD_CHANGE);
    };
    audio_service_.SetCallbacks(callbacks);

    // Add state change listeners
    state_machine_.AddStateChangeListener([this](DeviceState old_state, DeviceState new_state) {
        xEventGroupSetBits(event_group_, MAIN_EVENT_STATE_CHANGED);
        // 广播设备状态变化事件（FlowEngine 等订阅方依赖此回调）
        DeviceStateEventManager::GetInstance().PostStateChangeEvent(old_state, new_state);

        // A1 · 开机自动对话：首次进 Idle 时触发 ToggleChat
        if (new_state == kDeviceStateIdle && auto_chat_pending_.exchange(false)) {
            ESP_LOGI(TAG, "开机自动对话：state→Idle 触发 ToggleChat");
            Schedule([this]() { ToggleChatState(); });
        }
    });

    // Start the clock timer to update the status bar
    esp_timer_start_periodic(clock_timer_handle_, 1000000);

    // Add MCP common tools (only once during initialization)
    auto& mcp_server = McpServer::GetInstance();
    mcp_server.AddCommonTools();
    mcp_server.AddUserOnlyTools();

    // Set network event callback for UI updates and network state handling
    board.SetNetworkEventCallback([this](NetworkEvent event, const std::string& data) {
        auto display = Board::GetInstance().GetDisplay();
        
        switch (event) {
            case NetworkEvent::Scanning:
                display->ShowNotification(Lang::Strings::SCANNING_WIFI, 30000);
                xEventGroupSetBits(event_group_, MAIN_EVENT_NETWORK_DISCONNECTED);
                break;
            case NetworkEvent::Connecting: {
                if (data.empty()) {
                    // Cellular network - registering without carrier info yet
                    display->SetStatus(Lang::Strings::REGISTERING_NETWORK);
                } else {
                    // WiFi or cellular with carrier info
                    std::string msg = Lang::Strings::CONNECT_TO;
                    msg += data;
                    msg += "...";
                    display->ShowNotification(msg.c_str(), 30000);
                }
                break;
            }
            case NetworkEvent::Connected: {
                std::string msg = Lang::Strings::CONNECTED_TO;
                msg += data;
                display->ShowNotification(msg.c_str(), 30000);
                xEventGroupSetBits(event_group_, MAIN_EVENT_NETWORK_CONNECTED);
                break;
            }
            case NetworkEvent::Disconnected:
                xEventGroupSetBits(event_group_, MAIN_EVENT_NETWORK_DISCONNECTED);
                break;
            case NetworkEvent::WifiConfigModeEnter:
                // WiFi config mode enter is handled by WifiBoard internally
                break;
            case NetworkEvent::WifiConfigModeExit:
                // WiFi config mode exit is handled by WifiBoard internally
                break;
            // Cellular modem specific events
            case NetworkEvent::ModemDetecting:
                display->SetStatus(Lang::Strings::DETECTING_MODULE);
                break;
            case NetworkEvent::ModemErrorNoSim:
                Alert(Lang::Strings::ERROR, Lang::Strings::PIN_ERROR, "triangle_exclamation", Lang::Sounds::OGG_ERR_PIN);
                break;
            case NetworkEvent::ModemErrorRegDenied:
                Alert(Lang::Strings::ERROR, Lang::Strings::REG_ERROR, "triangle_exclamation", Lang::Sounds::OGG_ERR_REG);
                break;
            case NetworkEvent::ModemErrorInitFailed:
                Alert(Lang::Strings::ERROR, Lang::Strings::MODEM_INIT_ERROR, "triangle_exclamation", Lang::Sounds::OGG_EXCLAMATION);
                break;
            case NetworkEvent::ModemErrorTimeout:
                display->SetStatus(Lang::Strings::REGISTERING_NETWORK);
                break;
        }
    });

    AlarmManager::GetInstance().SetAlarmCallback([](const AlarmConfig& evt) {
        AlarmRinger::GetInstance().Start(evt.message);
    });
    AlarmManager::GetInstance().RegisterMcpTools();
    if (auto* ui = dynamic_cast<UiDisplay*>(display)) {
        ui->OnPomodoroToggle([]() {
            PomodoroManager::GetInstance().TogglePaused();
        });
    }
    PomodoroManager::GetInstance().SetTickCallback(
        [](PomodoroManager::State state, uint32_t remain, uint32_t /*total*/) {
            bool running = (state == PomodoroManager::State::kRunning);
            bool active  = (state != PomodoroManager::State::kIdle);
            Application::GetInstance().Schedule([remain, running, active]() {
                auto* ui = dynamic_cast<UiDisplay*>(Board::GetInstance().GetDisplay());
                if (!ui) return;
                if (!active) { ui->SwitchOutPomodoroMode(); return; }
                if (!ui->IsPomodoroMode()) ui->SwitchToPomodoroMode(remain, running);
                else                      ui->UpdatePomodoro(remain, running);
            });
        });
    // 计时结束 → vibration 短促提示 + AI 唤醒鼓励对话
    PomodoroManager::GetInstance().SetFinishCallback([]() {
        Application::GetInstance().Schedule([]() {
            auto& app = Application::GetInstance();
            app.PlaySound(Lang::Sounds::OGG_VIBRATION);
            app.WakeWordInvoke("番茄钟到了鼓励一下");        // 9 字 ≤10 红线 ✅
        });
    });
    PomodoroManager::GetInstance().RegisterMcpTools();

    // Start network asynchronously
    board.StartNetwork();

    // Update the status bar immediately to show the network state
    display->UpdateStatusBar(true);
}

void Application::Run() {
    // Set the priority of the main task to 10
    vTaskPrioritySet(nullptr, 10);

    const EventBits_t ALL_EVENTS = 
        MAIN_EVENT_SCHEDULE |
        MAIN_EVENT_SEND_AUDIO |
        MAIN_EVENT_WAKE_WORD_DETECTED |
        MAIN_EVENT_VAD_CHANGE |
        MAIN_EVENT_CLOCK_TICK |
        MAIN_EVENT_ERROR |
        MAIN_EVENT_NETWORK_CONNECTED |
        MAIN_EVENT_NETWORK_DISCONNECTED |
        MAIN_EVENT_TOGGLE_CHAT |
        MAIN_EVENT_START_LISTENING |
        MAIN_EVENT_STOP_LISTENING |
        MAIN_EVENT_ACTIVATION_DONE |
        MAIN_EVENT_STATE_CHANGED;

    while (true) {
        auto bits = xEventGroupWaitBits(event_group_, ALL_EVENTS, pdTRUE, pdFALSE, portMAX_DELAY);

        if (bits & MAIN_EVENT_ERROR) {
            SetDeviceState(kDeviceStateIdle);
            Alert(Lang::Strings::ERROR, last_error_message_.c_str(), "circle_xmark", Lang::Sounds::OGG_DISCONNECT);
        }

        if (bits & MAIN_EVENT_NETWORK_CONNECTED) {
            HandleNetworkConnectedEvent();
        }

        if (bits & MAIN_EVENT_NETWORK_DISCONNECTED) {
            HandleNetworkDisconnectedEvent();
        }

        if (bits & MAIN_EVENT_ACTIVATION_DONE) {
            HandleActivationDoneEvent();
        }

        if (bits & MAIN_EVENT_STATE_CHANGED) {
            HandleStateChangedEvent();
        }

        if (bits & MAIN_EVENT_TOGGLE_CHAT) {
            HandleToggleChatEvent();
        }

        if (bits & MAIN_EVENT_START_LISTENING) {
            HandleStartListeningEvent();
        }

        if (bits & MAIN_EVENT_STOP_LISTENING) {
            HandleStopListeningEvent();
        }

        if (bits & MAIN_EVENT_SEND_AUDIO) {
            while (auto packet = audio_service_.PopPacketFromSendQueue()) {
                if (protocol_ && !protocol_->SendAudio(std::move(packet))) {
                    if (++audio_send_fail_count_ >= 100) {
                        ESP_LOGW(TAG, "Audio send 连续失败 100 帧 → 退出对话");
                        audio_send_fail_count_ = 0;
                        SetDeviceState(kDeviceStateIdle);
                        if (protocol_) protocol_->CloseAudioChannel();
                    }
                    break;
                }
                audio_send_fail_count_ = 0;
            }
        }

        if (bits & MAIN_EVENT_WAKE_WORD_DETECTED) {
            HandleWakeWordDetectedEvent();
        }

        if (bits & MAIN_EVENT_VAD_CHANGE) {
            if (GetDeviceState() == kDeviceStateListening) {
                auto led = Board::GetInstance().GetLed();
                led->OnStateChanged();
            }
        }

        if (bits & MAIN_EVENT_SCHEDULE) {
            std::unique_lock<std::mutex> lock(mutex_);
            auto tasks = std::move(main_tasks_);
            lock.unlock();
            for (auto& task : tasks) {
                task();
            }
        }

        if (bits & MAIN_EVENT_CLOCK_TICK) {
            clock_ticks_++;
            auto display = Board::GetInstance().GetDisplay();
            display->UpdateStatusBar();

#if CONFIG_ENABLE_RUNTIME_MONITOR
            if (clock_ticks_ % 10 == 0) {
                SystemInfo::PrintHeapStats();
            }
#endif

            // 闹钟检查（每秒 · 仅校时后生效 · 内部有防重触发）
            AlarmManager::GetInstance().CheckAndTrigger();
        }
    }
}

void Application::HandleNetworkConnectedEvent() {
    ESP_LOGI(TAG, "Network connected");
    auto state = GetDeviceState();

    if (state == kDeviceStateStarting || state == kDeviceStateWifiConfiguring) {
        // Network is ready, start activation
        SetDeviceState(kDeviceStateActivating);
        if (activation_task_handle_ != nullptr) {
            ESP_LOGW(TAG, "Activation task already running");
            return;
        }

        xTaskCreate([](void* arg) {
            Application* app = static_cast<Application*>(arg);
            app->ActivationTask();
            app->activation_task_handle_ = nullptr;
            vTaskDelete(NULL);
        }, "activation", 4096 * 2, this, 2, &activation_task_handle_);
    }

    // Update the status bar immediately to show the network state
    auto display = Board::GetInstance().GetDisplay();
    display->UpdateStatusBar(true);
}

void Application::HandleNetworkDisconnectedEvent() {
    // Close current conversation when network disconnected
    auto state = GetDeviceState();
    if (state == kDeviceStateConnecting || state == kDeviceStateListening || state == kDeviceStateSpeaking) {
        ESP_LOGI(TAG, "Closing audio channel due to network disconnection");
        protocol_->CloseAudioChannel();
    }

    // Update the status bar immediately to show the network state
    auto display = Board::GetInstance().GetDisplay();
    display->UpdateStatusBar(true);
}

void Application::HandleActivationDoneEvent() {
    ESP_LOGI(TAG, "Activation done");

    SystemInfo::PrintHeapStats();
    SetDeviceState(kDeviceStateIdle);

    has_server_time_ = ota_->HasServerTime();
    if (has_server_time_) {
        setenv("TZ", "CST-8", 1);
        tzset();
        AlarmManager::MarkTimeSynced();   // 校时成功 · 启用闹钟检查
    }

    auto display = Board::GetInstance().GetDisplay();
    display->HideQrCode();
    std::string message = std::string(Lang::Strings::VERSION) + ota_->GetCurrentVersion();
    display->ShowNotification(message.c_str());
    display->SetChatMessage("system", "");

    // Release OTA object after activation is complete
    ota_.reset();
    auto& board = Board::GetInstance();
    board.SetPowerSaveLevel(PowerSaveLevel::LOW_POWER);

    Schedule([this]() {
        // Play the success sound to indicate the device is ready
        audio_service_.PlaySound(Lang::Sounds::OGG_CONNECT);
        vTaskDelay(pdMS_TO_TICKS(1000));
    });
}

void Application::ActivationTask() {
    // Create OTA object for activation process
    ota_ = std::make_unique<Ota>();

    // 4G 模式：PDP 拿到 IP 后 DNS 服务器还需协商几秒，弱网更慢，先等够再 OTA。
    if (Board::GetInstance().GetBoardType() == "ml307") {
        vTaskDelay(pdMS_TO_TICKS(3000));
    }

    // Check for new assets version
    CheckAssetsVersion();

    // Check for new firmware version
    CheckNewVersion();

    // Initialize the protocol
    InitializeProtocol();

    // Signal completion to main loop
    xEventGroupSetBits(event_group_, MAIN_EVENT_ACTIVATION_DONE);
}

void Application::CheckAssetsVersion() {
    // Only allow CheckAssetsVersion to be called once
    if (assets_version_checked_) {
        return;
    }
    assets_version_checked_ = true;

    auto& board = Board::GetInstance();
    auto display = board.GetDisplay();
    auto& assets = Assets::GetInstance();

    if (!assets.partition_valid()) {
        ESP_LOGW(TAG, "Assets partition is disabled for board %s", BOARD_NAME);
        return;
    }
    
    Settings settings("assets", true);
    // Check if there is a new assets need to be downloaded
    std::string download_url = settings.GetString("download_url");

    if (!download_url.empty()) {
        settings.EraseKey("download_url");

        char message[256];
        snprintf(message, sizeof(message), Lang::Strings::FOUND_NEW_ASSETS, download_url.c_str());
        Alert(Lang::Strings::LOADING_ASSETS, message, "cloud_arrow_down", Lang::Sounds::OGG_UPGRADE);
        
        // Wait for the audio service to be idle for 3 seconds
        vTaskDelay(pdMS_TO_TICKS(3000));
        SetDeviceState(kDeviceStateUpgrading);
        board.SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);
        display->SetChatMessage("system", Lang::Strings::PLEASE_WAIT);

        bool success = assets.Download(download_url, [this, display](int progress, size_t speed) -> void {
            char buffer[32];
            snprintf(buffer, sizeof(buffer), "%d%% %uKB/s", progress, speed / 1024);
            Schedule([display, message = std::string(buffer)]() {
                display->SetChatMessage("system", message.c_str());
            });
        });

        board.SetPowerSaveLevel(PowerSaveLevel::LOW_POWER);
        vTaskDelay(pdMS_TO_TICKS(1000));

        if (!success) {
            Alert(Lang::Strings::ERROR, Lang::Strings::DOWNLOAD_ASSETS_FAILED, "circle_xmark", Lang::Sounds::OGG_DISCONNECT);
            vTaskDelay(pdMS_TO_TICKS(2000));
            SetDeviceState(kDeviceStateActivating);
            return;
        }
    }

    // Apply assets
    assets.Apply();
    display->SetChatMessage("system", "");
    display->SetEmotion("logo");
}

void Application::CheckNewVersion() {
    const int MAX_RETRY = 10;
    int retry_count = 0;
    int retry_delay = 10; // Initial retry delay in seconds

    auto& board = Board::GetInstance();
    while (true) {
        auto display = board.GetDisplay();
        display->SetStatus(Lang::Strings::CHECKING_NEW_VERSION);

        esp_err_t err = ota_->CheckVersion();
        if (err != ESP_OK) {
            retry_count++;
            if (retry_count >= MAX_RETRY) {
                ESP_LOGE(TAG, "Too many retries, exit version check");
                return;
            }

            char error_message[128];
            snprintf(error_message, sizeof(error_message), "code=%d, url=%s", err, ota_->GetCheckVersionUrl().c_str());
            char buffer[256];
            snprintf(buffer, sizeof(buffer), Lang::Strings::CHECK_NEW_VERSION_FAILED, retry_delay, error_message);
            Alert(Lang::Strings::ERROR, buffer, "logo", Lang::Sounds::OGG_DISCONNECT);

            ESP_LOGW(TAG, "Check new version failed, retry in %d seconds (%d/%d)", retry_delay, retry_count, MAX_RETRY);
            for (int i = 0; i < retry_delay; i++) {
                vTaskDelay(pdMS_TO_TICKS(1000));
                if (GetDeviceState() == kDeviceStateIdle) {
                    break;
                }
            }
            retry_delay *= 2; // Double the retry delay
            continue;
        }
        retry_count = 0;
        retry_delay = 10; // Reset retry delay

        if (ota_->HasNewVersion()) {
            if (UpgradeFirmware(ota_->GetFirmwareUrl(), ota_->GetFirmwareVersion())) {
                return; // This line will never be reached after reboot
            }
            // If upgrade failed, continue to normal operation
        }

        // No new version, mark the current version as valid
        ota_->MarkCurrentVersionValid();
        if (!ota_->HasActivationCode() && !ota_->HasActivationChallenge()) {
            // Exit the loop if done checking new version
            break;
        }

        display->SetStatus(Lang::Strings::ACTIVATION);
        // Activation code is shown to the user and waiting for the user to input
        if (ota_->HasActivationCode()) {
            ShowActivationCode(ota_->GetActivationCode(), ota_->GetActivationMessage());
        }

        // This will block the loop until the activation is done or timeout
        for (int i = 0; i < 10; ++i) {
            ESP_LOGI(TAG, "Activating... %d/%d", i + 1, 10);
            esp_err_t err = ota_->Activate();
            if (err == ESP_OK) {
                break;
            } else if (err == ESP_ERR_TIMEOUT) {
                vTaskDelay(pdMS_TO_TICKS(3000));
            } else {
                vTaskDelay(pdMS_TO_TICKS(10000));
            }
            if (GetDeviceState() == kDeviceStateIdle) {
                break;
            }
        }
    }
}

void Application::InitializeProtocol() {
    auto& board = Board::GetInstance();
    auto display = board.GetDisplay();
    auto codec = board.GetAudioCodec();

    display->SetStatus(Lang::Strings::LOADING_PROTOCOL);

    // 初始化远程命令处理器和直播伴侣
    if (!remote_cmd_) remote_cmd_ = std::make_unique<RemoteCmd>(this);
    if (!flow_engine_) flow_engine_ = std::make_unique<FlowEngine>(this);

    if (ota_->HasMqttConfig()) {
        protocol_ = std::make_unique<MqttProtocol>();
    } else if (ota_->HasWebsocketConfig()) {
        Settings ws_settings("websocket", false);
        std::string ws_url = ws_settings.GetString("url", "");
        if (ws_url.find("bcelive") != std::string::npos) {
            ESP_LOGI(TAG, "Using Baidu BRTC WebSocket protocol");
            protocol_ = std::make_unique<WebsocketBaiduProtocol>();
        } else if (ws_url.find("joyinside") != std::string::npos) {
            ESP_LOGI(TAG, "Using JoyAI WebSocket protocol");
            protocol_ = std::make_unique<WebsocketJoeaiProtocol>();
        } else {
            protocol_ = std::make_unique<WebsocketProtocol>();
        }
    } else {
        ESP_LOGW(TAG, "No protocol specified in the OTA config, using MQTT");
        protocol_ = std::make_unique<MqttProtocol>();
    }

    protocol_->OnConnected([this]() {
        DismissAlert();
    });

    protocol_->OnNetworkError([this](const std::string& message) {
        last_error_message_ = message;
        xEventGroupSetBits(event_group_, MAIN_EVENT_ERROR);
    });
    
    protocol_->OnIncomingAudio([this](std::unique_ptr<AudioStreamPacket> packet) {
        if (GetDeviceState() == kDeviceStateSpeaking) {
            audio_service_.PushPacketToDecodeQueue(std::move(packet), true);
        }
    });
    
    protocol_->OnAudioChannelOpened([this, codec, &board]() {
        board.SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);
        user_initiated_close_.store(false);  // 通道新开 → 清残留退出意图，防下次弱网掉线被误判
        server_initiated_close_.store(false);  // 同上，清服务器主动告别标志
        if (protocol_->server_sample_rate() != codec->output_sample_rate()) {
            ESP_LOGW(TAG, "Server sample rate %d does not match device output sample rate %d, resampling may cause distortion",
                protocol_->server_sample_rate(), codec->output_sample_rate());
        }
    });
    
    protocol_->OnAudioChannelClosed([this, &board]() {
        if (shutting_down_.load()) {
            return;
        }
        board.SetPowerSaveLevel(PowerSaveLevel::LOW_POWER);

        // 弱网保护：Listening/Speaking 中断线 → 后台重连 3 次（500/1000/1500ms 退避）
        auto state_at_close = GetDeviceState();
        if (state_at_close == kDeviceStateListening || state_at_close == kDeviceStateSpeaking) {
            if (user_initiated_close_.exchange(false)) {
                ESP_LOGI(TAG, "用户主动退出对话 → 回 Idle（跳过弱网重连）");
                reconnect_attempts_in_window_ = 0;
                Schedule([this]() {
                    Board::GetInstance().GetDisplay()->SetChatMessage("system", "");
                    SetDeviceState(kDeviceStateIdle);
                });
                return;
            }
            if (server_initiated_close_.exchange(false)) {
                ESP_LOGI(TAG, "服务器主动结束对话 → 回 Idle（跳过弱网重连）");
                reconnect_attempts_in_window_ = 0;
                Schedule([this]() {
                    Board::GetInstance().GetDisplay()->SetChatMessage("system", "");
                    SetDeviceState(kDeviceStateIdle);
                });
                return;
            }
            int64_t now_us = esp_timer_get_time();
            if (reconnect_attempts_in_window_ == 0 ||
                now_us - reconnect_window_start_us_ > kReconnectWindowUs) {
                reconnect_attempts_in_window_ = 0;
                reconnect_window_start_us_ = now_us;
            }
            if (++reconnect_attempts_in_window_ > kMaxReconnectInWindow) {
                reconnect_attempts_in_window_ = 0;
                Schedule([this]() {
                    Board::GetInstance().GetDisplay()->SetChatMessage("system", "");
                    SetDeviceState(kDeviceStateIdle);
                });
                return;
            }

            // P1 修复：弱网重连改为 esp_timer 退避 + Schedule 回主循环单次尝试。
            // 原实现在主循环闭包内 vTaskDelay 500+1000+1500=3s + 3 次同步 OpenAudioChannel，
            // 整段独占主循环线程 → 断网期间按键/触屏/唤醒/音频上行全部排队冻结 ≥3s。
            // 改后：退避延迟落在 esp_timer（不占主循环），连接尝试仍在主循环单次执行
            // （protocol_ 串行访问，沿用 shutting_down_ 保护，无跨线程 UAF）。
            Schedule([this]() {
                reconnect_attempt_ = 1;
                ScheduleReconnectAttempt();
            });
            return;
        }
        // 用户主动结束 / 其他状态：保持原逻辑
        Schedule([this]() {
            auto display = Board::GetInstance().GetDisplay();
            display->SetChatMessage("system", "");
            SetDeviceState(kDeviceStateIdle);
        });
    });
    
    protocol_->OnIncomingJson([this, display](const cJSON* root) {
        // Parse JSON data
        auto type = cJSON_GetObjectItem(root, "type");
        if (!cJSON_IsString(type)) {
            ESP_LOGW(TAG, "incoming json missing/invalid 'type', drop");
            return;
        }
        if (strcmp(type->valuestring, "tts") == 0) {
            auto state = cJSON_GetObjectItem(root, "state");
            if (!cJSON_IsString(state)) { return; }
            if (strcmp(state->valuestring, "start") == 0) {
                tts_streaming_.store(true);
                Schedule([this]() {
                    aborted_ = false;
                    SetDeviceState(kDeviceStateSpeaking);
                });
            } else if (strcmp(state->valuestring, "stop") == 0) {
                tts_streaming_.store(false);
                Schedule([this]() {
                    if (GetDeviceState() == kDeviceStateSpeaking) {
                        if (listening_mode_ == kListeningModeManualStop) {
                            SetDeviceState(kDeviceStateIdle);
                        } else {
                            SetDeviceState(kDeviceStateListening);
                        }
                    }
                });
            } else if (strcmp(state->valuestring, "sentence_start") == 0) {
                auto text = cJSON_GetObjectItem(root, "text");
                if (cJSON_IsString(text)) {
                    ESP_LOGI(TAG, "<< %s", text->valuestring);
                    std::string sentence = text->valuestring;
                    if (auto hit = ExtractEduCard(sentence); hit.has_value()) {
                        ESP_LOGI(TAG, "EduCard auto-trigger: %s", hit->main.c_str());
                        TriggerEduCard(std::move(*hit));
                    }

                    Schedule([display, message = sentence]() {
                        display->SetChatMessage("assistant", message.c_str());
                    });
                }
            }
        } else if (strcmp(type->valuestring, "stt") == 0) {
            auto text = cJSON_GetObjectItem(root, "text");
            if (cJSON_IsString(text)) {
                ESP_LOGI(TAG, ">> %s", text->valuestring);
                Schedule([this, display, message = std::string(text->valuestring)]() {
                    display->SetChatMessage("user", message.c_str());
                    // 触发：服务器 stt 文本回包 = 用户语音已被服务器收到+识别
                    if (stt_popup_enabled_ &&
                        listening_mode_ != kListeningModeManualStop &&
                        !skip_next_stt_popup_.exchange(false)) {
                        audio_service_.PlaySound(Lang::Sounds::OGG_POPUP);
                    }
                });
            }
        } else if (strcmp(type->valuestring, "llm") == 0) {
            auto emotion = cJSON_GetObjectItem(root, "emotion");
            if (cJSON_IsString(emotion)) {
                Schedule([display, emotion_str = std::string(emotion->valuestring)]() {
                    display->SetEmotion(emotion_str.c_str());
                });
            }
        } else if (strcmp(type->valuestring, "mcp") == 0) {
            auto payload = cJSON_GetObjectItem(root, "payload");
            if (cJSON_IsObject(payload)) {
                McpServer::GetInstance().ParseMessage(payload);
            }
        } else if (strcmp(type->valuestring, "system") == 0) {
            auto command = cJSON_GetObjectItem(root, "command");
            if (cJSON_IsString(command)) {
                ESP_LOGI(TAG, "System command: %s", command->valuestring);
                if (strcmp(command->valuestring, "reboot") == 0) {
                    // Do a reboot if user requests a OTA update
                    Schedule([this]() {
                        Reboot();
                    });
                } else {
                    ESP_LOGW(TAG, "Unknown system command: %s", command->valuestring);
                }
            }
        } else if (strcmp(type->valuestring, "alert") == 0) {
            auto status = cJSON_GetObjectItem(root, "status");
            auto message = cJSON_GetObjectItem(root, "message");
            auto emotion = cJSON_GetObjectItem(root, "emotion");
            if (cJSON_IsString(status) && cJSON_IsString(message) && cJSON_IsString(emotion)) {
                Alert(status->valuestring, message->valuestring, emotion->valuestring, Lang::Sounds::OGG_VIBRATION);
            } else {
                ESP_LOGW(TAG, "Alert command requires status, message and emotion");
            }
        } else if (strcmp(type->valuestring, "custom") == 0) {
            auto payload = cJSON_GetObjectItem(root, "payload");
            if (cJSON_IsObject(payload) && remote_cmd_) {
                remote_cmd_->Handle(payload);
            } else {
                ESP_LOGW(TAG, "Invalid custom message or remote_cmd not ready");
            }
        } else {
            ESP_LOGW(TAG, "Unknown message type: %s", type->valuestring);
        }
    });

    // 按 protocol 切 OPUS 帧长（百度 20 / 其它 60）
    audio_service_.SetFrameDuration(protocol_->client_frame_duration());

    protocol_->Start();
}

void Application::ShowActivationCode(const std::string& code, const std::string& message) {
    struct digit_sound {
        char digit;
        const std::string_view& sound;
    };
    static const std::array<digit_sound, 10> digit_sounds{{
        digit_sound{'0', Lang::Sounds::OGG_0},
        digit_sound{'1', Lang::Sounds::OGG_1},
        digit_sound{'2', Lang::Sounds::OGG_2},
        digit_sound{'3', Lang::Sounds::OGG_3},
        digit_sound{'4', Lang::Sounds::OGG_4},
        digit_sound{'5', Lang::Sounds::OGG_5},
        digit_sound{'6', Lang::Sounds::OGG_6},
        digit_sound{'7', Lang::Sounds::OGG_7},
        digit_sound{'8', Lang::Sounds::OGG_8},
        digit_sound{'9', Lang::Sounds::OGG_9}
    }};

    auto display = Board::GetInstance().GetDisplay();
    std::string mac = SystemInfo::GetMacAddress();
    ESP_LOGI(TAG, "Activation: scene=%s code=%s", mac.c_str(), code.c_str());

    // 通用 ShowQrCode：扫码跳转 H5/小程序绑定页，URL 携带 MAC 用于设备识别
    // URL 用无冒号 MAC（紧凑）· 屏幕 bottom 仍显示原 MAC（带冒号便于人眼阅读）
    std::string mac_url = mac;
    mac_url.erase(std::remove(mac_url.begin(), mac_url.end(), ':'), mac_url.end());
    std::string bind_url = "https://mydazy.cn/ota/bind?mac=" + mac_url;
    display->ShowQrCode(bind_url.c_str(), code.c_str(), "扫码绑定设备", mac.c_str());
    display->SetChatMessage("system", message.c_str());


    display->SetStatus(Lang::Strings::ACTIVATION);
    audio_service_.PlaySound(Lang::Sounds::OGG_ACTIVATION);
    for (const auto& digit : code) {
        auto it = std::find_if(digit_sounds.begin(), digit_sounds.end(),
            [digit](const digit_sound& ds) { return ds.digit == digit; });
        if (it != digit_sounds.end()) {
            audio_service_.PlaySound(it->sound);
        }
    }
}

void Application::Alert(const char* status, const char* message, const char* emotion, const std::string_view& sound) {
    ESP_LOGW(TAG, "Alert [%s] %s: %s", emotion, status, message);
    auto display = Board::GetInstance().GetDisplay();
    display->SetStatus(status);
    display->SetEmotion(emotion);
    display->SetChatMessage("system", message);
    if (!sound.empty()) {
        audio_service_.PlaySound(sound);
    }
}

void Application::DismissAlert() {
    if (GetDeviceState() == kDeviceStateIdle) {
        auto display = Board::GetInstance().GetDisplay();
        display->SetStatus("");
        display->SetEmotion("neutral");
        display->SetChatMessage("system", "");
    }
}

void Application::ToggleChatState() {
    xEventGroupSetBits(event_group_, MAIN_EVENT_TOGGLE_CHAT);
}

void Application::StartListening() {
    xEventGroupSetBits(event_group_, MAIN_EVENT_START_LISTENING);
}

void Application::StopListening() {
    xEventGroupSetBits(event_group_, MAIN_EVENT_STOP_LISTENING);
}

void Application::HandleToggleChatEvent() {
    // 任何"用户主动发起交互"（按键/触屏）先停 MP3
    MusicPlayer::GetInstance().Stop();

    auto state = GetDeviceState();

    if (state == kDeviceStateActivating) {
        SetDeviceState(kDeviceStateIdle);
        return;
    } else if (state == kDeviceStateWifiConfiguring) {
        audio_service_.EnableAudioTesting(true);
        SetDeviceState(kDeviceStateAudioTesting);
        return;
    } else if (state == kDeviceStateAudioTesting) {
        audio_service_.EnableAudioTesting(false);
        SetDeviceState(kDeviceStateWifiConfiguring);
        return;
    }

    if (!protocol_) {
        ESP_LOGE(TAG, "Protocol not initialized");
        return;
    }

    if (state == kDeviceStateIdle) {
        ListeningMode mode = GetDefaultListeningMode();
        if (!protocol_->IsAudioChannelOpened()) {
            SetDeviceState(kDeviceStateConnecting);
            // Schedule to let the state change be processed first (UI update)
            Schedule([this, mode]() {
                ContinueOpenAudioChannel(mode);
            });
            return;
        }
        SetListeningMode(mode);
    } else if (state == kDeviceStateSpeaking) {
        AbortSpeaking(kAbortReasonNone);
    } else if (state == kDeviceStateListening) {
        // 用户主动单击退出对话：标记意图，OnAudioChannelClosed 据此直接回 Idle，
        user_initiated_close_.store(true);
        protocol_->CloseAudioChannel();
    }
}

void Application::ContinueOpenAudioChannel(ListeningMode mode) {
    // Check state again in case it was changed during scheduling
    if (GetDeviceState() != kDeviceStateConnecting) {
        return;
    }

    // Switch to performance mode before connecting to reduce latency
    auto& board = Board::GetInstance();
    board.SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);

    if (!protocol_->IsAudioChannelOpened()) {
        if (!protocol_->OpenAudioChannel()) {
            ESP_LOGE(TAG, "OpenAudioChannel failed, fallback to Idle");
            Board::GetInstance().GetDisplay()->SetChatMessage("system", "");
            SetDeviceState(kDeviceStateIdle);
            return;
        }
    }

    SetListeningMode(mode);
}

void Application::HandleStartListeningEvent() {
    // 按键/外部 StartListening() 触发 → 先停 MP3
    MusicPlayer::GetInstance().Stop();

    auto state = GetDeviceState();

    if (state == kDeviceStateActivating) {
        SetDeviceState(kDeviceStateIdle);
        return;
    } else if (state == kDeviceStateWifiConfiguring) {
        audio_service_.EnableAudioTesting(true);
        SetDeviceState(kDeviceStateAudioTesting);
        return;
    }

    if (!protocol_) {
        ESP_LOGE(TAG, "Protocol not initialized");
        return;
    }
    
    if (state == kDeviceStateIdle) {
        if (!protocol_->IsAudioChannelOpened()) {
            SetDeviceState(kDeviceStateConnecting);
            // Schedule to let the state change be processed first (UI update)
            Schedule([this]() {
                ContinueOpenAudioChannel(kListeningModeManualStop);
            });
            return;
        }
        SetListeningMode(kListeningModeManualStop);
    } else if (state == kDeviceStateSpeaking) {
        AbortSpeaking(kAbortReasonNone);
        SetListeningMode(kListeningModeManualStop);
    }
}

void Application::HandleStopListeningEvent() {
    auto state = GetDeviceState();
    
    if (state == kDeviceStateAudioTesting) {
        audio_service_.EnableAudioTesting(false);
        SetDeviceState(kDeviceStateWifiConfiguring);
        return;
    } else if (state == kDeviceStateListening) {
        if (protocol_) {
            protocol_->SendStopListening();
        }
        SetDeviceState(kDeviceStateIdle);
    }
}

void Application::HandleWakeWordDetectedEvent() {
    if (!protocol_) {
        return;
    }

    // 唤醒词检测 → 打断 MP3 播放（"小智" 优先级最高）+ 退 Player UI 进对话
    bool was_playing = MusicPlayer::GetInstance().IsPlaying();
    MusicPlayer::GetInstance().Stop();
    if (was_playing) {
        if (auto* ui = dynamic_cast<UiDisplay*>(Board::GetInstance().GetDisplay())) {
            ui->SwitchOutPlayerMode();
        }
    }

    auto state = GetDeviceState();
    auto wake_word = audio_service_.GetLastWakeWord();
    ESP_LOGI(TAG, "Wake word detected: %s (state: %d)", wake_word.c_str(), (int)state);

    if (state == kDeviceStateIdle) {
#if CONFIG_SEND_WAKE_WORD_DATA
        audio_service_.EncodeWakeWord();
#else
        audio_service_.PlaySound(Lang::Sounds::OGG_WAKEUP);
#endif
        auto wake_word = audio_service_.GetLastWakeWord();

        // 跳过下一条 STT 提示音 · 避免唤醒词音频被 ASR 识别后立刻响（唤醒已有 OGG_WAKEUP 提示）
        skip_next_stt_popup_.store(true);

        if (!protocol_->IsAudioChannelOpened()) {
            SetDeviceState(kDeviceStateConnecting);
            // Schedule to let the state change be processed first (UI update),
            // then continue with OpenAudioChannel which may block for ~1 second
            Schedule([this, wake_word]() {
                ContinueWakeWordInvoke(wake_word);
            });
            return;
        }
        // Channel already opened, continue directly
        ContinueWakeWordInvoke(wake_word);
    } else if (state == kDeviceStateSpeaking || state == kDeviceStateListening) {
        AbortSpeaking(kAbortReasonWakeWordDetected);
        // Clear send queue to avoid sending residues to server
        while (audio_service_.PopPacketFromSendQueue());

        if (state == kDeviceStateListening) {
            protocol_->SendStartListening(GetDefaultListeningMode());
            audio_service_.ResetDecoder();
            audio_service_.PlaySound(Lang::Sounds::OGG_WAKEUP);
            // Re-enable wake word detection as it was stopped by the detection itself
            audio_service_.EnableWakeWordDetection(true);
        } else {
            pending_listening_sound_ = &Lang::Sounds::OGG_WAKEUP;
            SetListeningMode(GetDefaultListeningMode());
        }
    } else if (state == kDeviceStateActivating) {
        // Restart the activation check if the wake word is detected during activation
        SetDeviceState(kDeviceStateIdle);
    }
}

void Application::ContinueWakeWordInvoke(const std::string& wake_word) {
    // Check state again in case it was changed during scheduling
    if (GetDeviceState() != kDeviceStateConnecting) {
        return;
    }

    // Switch to performance mode before connecting to reduce latency
    auto& board = Board::GetInstance();
    board.SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);

    if (!protocol_->IsAudioChannelOpened()) {
        if (!protocol_->OpenAudioChannel()) {
            audio_service_.EnableWakeWordDetection(true);
            return;
        }
    }

    ESP_LOGI(TAG, "Wake word detected: %s", wake_word.c_str());
#if CONFIG_SEND_WAKE_WORD_DATA
    // Encode and send the wake word data to the server
    while (auto packet = audio_service_.PopWakeWordPacket()) {
        protocol_->SendAudio(std::move(packet));
    }
    // Set the chat state to wake word detected
    protocol_->SendWakeWordDetected(wake_word);
    SetListeningMode(GetDefaultListeningMode());
#else

    pending_listening_sound_ = &Lang::Sounds::OGG_POPUP;
    SetListeningMode(GetDefaultListeningMode());
#endif
}

void Application::HandleStateChangedEvent() {
    DeviceState new_state = state_machine_.GetState();
    clock_ticks_ = 0;

    auto& board = Board::GetInstance();
    auto display = board.GetDisplay();
    auto led = board.GetLed();
    led->OnStateChanged();

    auto* lcd = dynamic_cast<UiDisplay*>(display);

    switch (new_state) {
        case kDeviceStateUnknown:
        case kDeviceStateIdle:
            display->SetStatus("");
            display->ClearChatMessages();  // Clear messages first
//            display->SetEmotion("neutral"); // Then set emotion (wechat mode checks child count)
            audio_service_.EnableVoiceProcessing(false);
            if (lcd) {
                auto& pm = PomodoroManager::GetInstance();
                if (pm.IsActive()) {
                    bool running = (pm.GetState() == PomodoroManager::State::kRunning);
                    lcd->SwitchToPomodoroMode(pm.GetRemainSec(), running);
                } else {
                    lcd->SwitchToClockMode();
                }
            }
            audio_service_.EnableWakeWordDetection(true);
            break;
        case kDeviceStateConnecting:
            display->SetStatus(Lang::Strings::CONNECTING);
            display->SetEmotion("neutral");
            display->SetChatMessage("system", "");
            if (lcd) lcd->SwitchToChatMode();    // 对话开始，表情/消息可见
            break;
        case kDeviceStateListening:
            display->SetStatus(""); //#FF3030 ●#
            display->SetEmotion("neutral");
            display->SetChatMessage("system", "");
            if (lcd) lcd->SwitchToChatMode();

            // Make sure the audio processor is running
            if (pending_listening_sound_ != nullptr || !audio_service_.IsAudioProcessorRunning()) {
                if (listening_mode_ == kListeningModeAutoStop) {
                    audio_service_.WaitForPlaybackQueueEmpty();
                }
                
                // Send the start listening command
                protocol_->SendStartListening(listening_mode_);
                audio_service_.EnableVoiceProcessing(true);
            }

#ifdef CONFIG_WAKE_WORD_DETECTION_IN_LISTENING
            // Enable wake word detection in listening mode (configured via Kconfig)
            audio_service_.EnableWakeWordDetection(audio_service_.IsAfeWakeWord());
#else
            // Disable wake word detection in listening mode
            audio_service_.EnableWakeWordDetection(false);
#endif
            
            // Play pending sound after ResetDecoder (in EnableVoiceProcessing) has been called
            if (pending_listening_sound_ != nullptr) {
                audio_service_.PlaySound(*pending_listening_sound_);
                pending_listening_sound_ = nullptr;
            }
            break;
        case kDeviceStateSpeaking:
            display->SetStatus("");
            if (lcd) {
                lcd->HideEduCard();
                lcd->HideFontGif();
            }

            if (listening_mode_ != kListeningModeRealtime) {
                audio_service_.EnableVoiceProcessing(false);
                audio_service_.EnableWakeWordDetection(audio_service_.IsAfeWakeWord());
            }
            audio_service_.ResetDecoder();
            break;
        case kDeviceStateWifiConfiguring:
            audio_service_.EnableVoiceProcessing(false);
            audio_service_.EnableWakeWordDetection(false);
            if (auto* lcd = dynamic_cast<UiDisplay*>(display)) {
                lcd->SwitchToChatMode();
            }
            break;
        default:
            // Do nothing
            break;
    }
}

void Application::Schedule(std::function<void()>&& callback) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        main_tasks_.push_back(std::move(callback));
    }
    xEventGroupSetBits(event_group_, MAIN_EVENT_SCHEDULE);
}

void Application::AbortSpeaking(AbortReason reason) {
    ESP_LOGI(TAG, "Abort speaking");
    aborted_ = true;
    tts_streaming_.store(false);
    if (protocol_) {
        protocol_->SendAbortSpeaking(reason);
    }
}

void Application::CloseAudioChannel() {
    if (protocol_ && protocol_->IsAudioChannelOpened()) {
        protocol_->CloseAudioChannel();
    }
}

// 远程 / 本地触发的 TTS 朗读：自动唤醒 + 抢占当前 TTS
bool Application::SendTextToTts(const std::string& text) {
    if (text.empty() || !protocol_) return false;

    auto state = GetDeviceState();
    if (state == kDeviceStateSpeaking) {
        AbortSpeaking(kAbortReasonNone);
    } else if (state == kDeviceStateIdle) {
        SetDeviceState(kDeviceStateConnecting);
    }

    if (!protocol_->IsAudioChannelOpened() && !protocol_->OpenAudioChannel()) {
        ESP_LOGE(TAG, "TTS failed: OpenAudioChannel failed");
        return false;
    }

    // 协议适配 fallback 链：
    if (protocol_->SendTextToTts(text)) {
        return true;
    }
    ESP_LOGI(TAG, "SendTextToTts -> fallback ai channel (MQTT/WS)");
    if (protocol_->SendTextToAI(text)) {
        return true;
    }
    WakeWordInvoke(text);
    return true;
}

void Application::SendTextToAI(const std::string& text) {
    if (text.empty() || !protocol_) return;
    if (protocol_->IsAudioChannelOpened() && protocol_->SendTextToAI(text)) {
        return;
    }
    WakeWordInvoke(text);
}

bool Application::SendProtocolText(const std::string& text) {
    if (text.empty() || !protocol_) return false;
    return protocol_->SendRawText(text);
}

// 教育卡：动态切换 system prompt（无 channel 自动开通道；当前协议不实现则返回 false）
bool Application::UpdateSystemPrompt(int model_type, const std::string& prompt) {
    if (!protocol_) return false;
    if (!protocol_->IsAudioChannelOpened() && !protocol_->OpenAudioChannel()) {
        ESP_LOGW(TAG, "UpdateSystemPrompt: OpenAudioChannel failed");
        return false;
    }
    return protocol_->UpdateSystemPrompt(model_type, prompt);
}

void Application::ScheduleDelayedWake(const std::string& wake_text, uint64_t delay_us) {
    if (delayed_wake_timer_ != nullptr) {
        esp_timer_stop(delayed_wake_timer_);
    }
    if (delayed_wake_timer_ == nullptr) {
        esp_timer_create_args_t timer_args = {
            .callback = [](void* arg) {
                auto app = (Application*)arg;
                app->Schedule([app]() {
                    app->WakeWordInvoke(app->pending_wake_text_);
                });
            },
            .arg = this,
            .name = "delayed_wake",
        };
        esp_timer_create(&timer_args, &delayed_wake_timer_);
    }
    pending_wake_text_ = wake_text;
    esp_timer_start_once(delayed_wake_timer_, delay_us);
}

void Application::ScheduleReconnectAttempt() {
    if (reconnect_timer_ == nullptr) {
        esp_timer_create_args_t timer_args = {
            .callback = [](void* arg) {
                auto app = (Application*)arg;
                app->Schedule([app]() { app->DoReconnectAttempt(); });
            },
            .arg = this,
            .name = "reconnect",
        };
        esp_timer_create(&timer_args, &reconnect_timer_);
    }
    esp_timer_stop(reconnect_timer_);   // 防重复 arm（已在跑则先停）
    uint64_t delay_us = (uint64_t)reconnect_attempt_ * 500 * 1000;  // 500/1000/1500ms 退避
    esp_timer_start_once(reconnect_timer_, delay_us);
}

void Application::DoReconnectAttempt() {
    // 主循环上下文执行：protocol_ 与状态机串行访问，无跨线程 UAF
    if (shutting_down_.load() || !protocol_) {
        reconnect_attempt_ = 0;
        return;  // 关机/切网途中：放弃
    }
    auto state = GetDeviceState();
    if (state != kDeviceStateListening && state != kDeviceStateSpeaking) {
        reconnect_attempt_ = 0;
        return;  // 用户已操作改变状态：放弃重连
    }
    if (protocol_->OpenAudioChannel()) {
        reconnect_attempt_ = 0;
        SetListeningMode(kListeningModeManualStop);
        return;
    }
    if (++reconnect_attempt_ > 3) {
        reconnect_attempt_ = 0;
        Board::GetInstance().GetDisplay()->SetChatMessage("system", "");
        SetDeviceState(kDeviceStateIdle);
        return;
    }
    ScheduleReconnectAttempt();
}

void Application::SetListeningMode(ListeningMode mode) {
    listening_mode_ = mode;
    SetDeviceState(kDeviceStateListening);
}

ListeningMode Application::GetDefaultListeningMode() const {
    return aec_mode_ == kAecOff ? kListeningModeAutoStop : kListeningModeRealtime;
}

void Application::Reboot() {
    ESP_LOGI(TAG, "Rebooting...");
    shutting_down_.store(true);  // 先置位：随后 CloseAudioChannel 触发的回调将跳过弱网重连
    // Disconnect the audio channel
    if (protocol_ && protocol_->IsAudioChannelOpened()) {
        protocol_->CloseAudioChannel();
    }
    protocol_.reset();
    audio_service_.Stop();

    vTaskDelay(pdMS_TO_TICKS(1000));

    auto& board = Board::GetInstance();
    auto* backlight = board.GetBacklight();
    if (backlight) {
        backlight->SetBrightness(0);
    }

    esp_restart();
}

bool Application::UpgradeFirmware(const std::string& url, const std::string& version) {
    auto& board = Board::GetInstance();
    auto display = board.GetDisplay();

    std::string upgrade_url = url;
    std::string version_info = version.empty() ? "(Manual upgrade)" : version;

    // Close audio channel if it's open
    if (protocol_ && protocol_->IsAudioChannelOpened()) {
        ESP_LOGI(TAG, "Closing audio channel before firmware upgrade");
        protocol_->CloseAudioChannel();
    }
    ESP_LOGI(TAG, "Starting firmware upgrade from URL: %s", upgrade_url.c_str());

    Alert(Lang::Strings::OTA_UPGRADE, Lang::Strings::UPGRADING, "download", Lang::Sounds::OGG_UPGRADE);
    vTaskDelay(pdMS_TO_TICKS(3000));

    SetDeviceState(kDeviceStateUpgrading);

    std::string message = std::string(Lang::Strings::NEW_VERSION) + version_info;
    display->SetChatMessage("system", message.c_str());

    board.SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);
    audio_service_.Stop();
    vTaskDelay(pdMS_TO_TICKS(1000));

    bool upgrade_success = Ota::Upgrade(upgrade_url, [this, display](int progress, size_t speed) {
        char buffer[32];
        snprintf(buffer, sizeof(buffer), "%d%% %uKB/s", progress, speed / 1024);
        Schedule([display, message = std::string(buffer)]() {
            display->SetChatMessage("system", message.c_str());
        });
    });

    if (!upgrade_success) {
        // Upgrade failed, restart audio service and continue running
        ESP_LOGE(TAG, "Firmware upgrade failed, restarting audio service and continuing operation...");
        audio_service_.Start(); // Restart audio service
        board.SetPowerSaveLevel(PowerSaveLevel::LOW_POWER); // Restore power save level
        Alert(Lang::Strings::ERROR, Lang::Strings::UPGRADE_FAILED, "", Lang::Sounds::OGG_EXCLAMATION);
        vTaskDelay(pdMS_TO_TICKS(3000));
        return false;
    } else {
        // Upgrade success, reboot immediately
        ESP_LOGI(TAG, "Firmware upgrade successful, rebooting...");
        display->SetChatMessage("system", "Upgrade successful, rebooting...");
        vTaskDelay(pdMS_TO_TICKS(1000)); // Brief pause to show message
        Reboot();
        return true;
    }
}

void Application::WakeWordInvoke(const std::string& wake_word) {
    if (!protocol_) {
        return;
    }

    auto state = GetDeviceState();
    
    if (state == kDeviceStateIdle) {
        audio_service_.EncodeWakeWord();

        skip_next_stt_popup_.store(true);

        if (!protocol_->IsAudioChannelOpened()) {
            SetDeviceState(kDeviceStateConnecting);
            // Schedule to let the state change be processed first (UI update)
            Schedule([this, wake_word]() {
                ContinueWakeWordInvoke(wake_word);
            });
            return;
        }
        // Channel already opened, continue directly
        ContinueWakeWordInvoke(wake_word);
    } else if (state == kDeviceStateSpeaking) {
        Schedule([this]() {
            AbortSpeaking(kAbortReasonNone);
        });
    } else if (state == kDeviceStateListening) {   
        Schedule([this]() {
            if (protocol_) {
                protocol_->CloseAudioChannel();
            }
        });
    }
}

bool Application::CanEnterSleepMode() {
    if (GetDeviceState() != kDeviceStateIdle) {
        return false;
    }

    if (protocol_ && protocol_->IsAudioChannelOpened()) {
        return false;
    }

    if (!audio_service_.IsIdle()) {
        return false;
    }

    if (MusicPlayer::GetInstance().IsPlaying()) {
        return false;
    }

    // 番茄钟运行/暂停期间禁止休眠（孩子专注 25min 中途黑屏会打断 · LCD 持亮 + 1Hz tick 维持）
    if (PomodoroManager::GetInstance().IsActive()) {
        return false;
    }

    // Now it is safe to enter sleep mode
    return true;
}

void Application::SendMcpMessage(const std::string& payload) {
    // Always schedule to run in main task for thread safety
    Schedule([this, payload = std::move(payload)]() {
        if (protocol_) {
            protocol_->SendMcpMessage(payload);
        }
    });
}

void Application::SetAecMode(AecMode mode) {
    aec_mode_ = mode;
    Schedule([this]() {
        Settings settings("aecMode", true);
        auto& board = Board::GetInstance();
        auto display = board.GetDisplay();
        switch (aec_mode_) {
        case kAecOff:
            settings.SetInt("deviceAec", 0);
            Alert(Lang::Strings::RTC_MODE_OFF, "", "", Lang::Sounds::OGG_AEC_OFF);
            vTaskDelay(pdMS_TO_TICKS(2000));
            audio_service_.EnableDeviceAec(false);
            break;
        case kAecOnServerSide:
            settings.SetInt("deviceAec", 0);
            audio_service_.EnableDeviceAec(false);
            display->ShowNotification(Lang::Strings::RTC_MODE_ON);
            break;
        case kAecOnDeviceSide:
            settings.SetInt("deviceAec", 1);
            Alert(Lang::Strings::RTC_MODE_ON, "", "", Lang::Sounds::OGG_AEC_ON);
            vTaskDelay(pdMS_TO_TICKS(2000));
            audio_service_.EnableDeviceAec(true);
            break;
        }
        if (protocol_ && protocol_->IsAudioChannelOpened()) {
            protocol_->CloseAudioChannel();
        }
    });
}

void Application::PlaySound(const std::string_view& sound) {
    audio_service_.PlaySound(sound);
}

// 说话结束提示音开关 · 持久化到 NVS（audio.stt_popup）
void Application::SetSttPopupEnabled(bool enabled) {
    stt_popup_enabled_ = enabled;
    Settings audio_settings("audio", true);
    audio_settings.SetInt("stt_popup", enabled ? 1 : 0);
    ESP_LOGI(TAG, "STT 提示音: %s（已持久化）", enabled ? "开启" : "关闭");
}

void Application::RequestAutoChatOnIdle() {
    auto_chat_pending_.store(true);
    if (state_machine_.GetState() == kDeviceStateIdle && auto_chat_pending_.exchange(false)) {
        ESP_LOGI(TAG, "开机自动对话：当前已 Idle 立即触发 ToggleChat");
        Schedule([this]() { ToggleChatState(); });
    }
}

void Application::ResetProtocol() {
    shutting_down_.store(true);
    Schedule([this]() {
        // Close audio channel if opened
        if (protocol_ && protocol_->IsAudioChannelOpened()) {
            protocol_->CloseAudioChannel();
        }
        // Reset protocol
        protocol_.reset();
    });
}

