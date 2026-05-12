#include "ml307_board.h"

#include "audio_codec.h"
#include "display.h"

#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <font_awesome.h>
#include <utility>

static const char *TAG = "Ml307Board";

// Maximum retry count for modem detection
static constexpr int MODEM_DETECT_MAX_RETRIES = 30;
// Maximum retry count for network registration
static constexpr int NETWORK_REG_MAX_RETRIES = 6;

Ml307Board::Ml307Board(gpio_num_t tx_pin, gpio_num_t rx_pin, gpio_num_t dtr_pin) : tx_pin_(tx_pin), rx_pin_(rx_pin), dtr_pin_(dtr_pin) {
}

std::string Ml307Board::GetBoardType() {
    return "ml307";
}

void Ml307Board::SetNetworkEventCallback(NetworkEventCallback callback) {
    network_event_callback_ = std::move(callback);
}

void Ml307Board::OnNetworkEvent(NetworkEvent event, const std::string& data) {
    switch (event) {
        case NetworkEvent::ModemDetecting:
            ESP_LOGI(TAG, "Detecting modem...");
            break;
        case NetworkEvent::Connecting:
            ESP_LOGI(TAG, "Registering network...");
            break;
        case NetworkEvent::Connected:
            ESP_LOGI(TAG, "Network connected");
            break;
        case NetworkEvent::Disconnected:
            ESP_LOGW(TAG, "Network disconnected");
            break;
        case NetworkEvent::ModemErrorNoSim:
            ESP_LOGE(TAG, "No SIM card detected");
            break;
        case NetworkEvent::ModemErrorRegDenied:
            ESP_LOGE(TAG, "Network registration denied");
            break;
        case NetworkEvent::ModemErrorInitFailed:
            ESP_LOGE(TAG, "Modem initialization failed");
            break;
        case NetworkEvent::ModemErrorTimeout:
            ESP_LOGE(TAG, "Operation timeout");
            break;
        default:
            break;
    }

    // Notify external callback if set
    if (network_event_callback_) {
        network_event_callback_(event, data);
    }
}

void Ml307Board::NetworkTask() {
    // ─── ML307R 模组上电稳定窗口（必须保留）────────────────────────────────
    vTaskDelay(pdMS_TO_TICKS(3000));

    // Notify modem detection started
    OnNetworkEvent(NetworkEvent::ModemDetecting);

    // Try to detect modem with retry limit
    int detect_retries = 0;
    while (detect_retries < MODEM_DETECT_MAX_RETRIES) {
        modem_ = AtModem::Detect(tx_pin_, rx_pin_, dtr_pin_, 921600);
        if (modem_ != nullptr) {
            break;
        }
        detect_retries++;
        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    if (modem_ == nullptr) {
        ESP_LOGE(TAG, "Failed to detect modem after %d retries", MODEM_DETECT_MAX_RETRIES);
        OnNetworkEvent(NetworkEvent::ModemErrorInitFailed);
        return;
    }

    ESP_LOGI(TAG, "Modem detected successfully");

    // Set up network state change callback
    // Note: Don't call GetCarrierName() here as it sends AT command and will block ReceiveTask
    modem_->OnNetworkStateChanged([this](bool network_ready) {
        if (network_ready) {
            OnNetworkEvent(NetworkEvent::Connected);
        } else {
            OnNetworkEvent(NetworkEvent::Disconnected);
        }
    });

    // Notify network registration started
    OnNetworkEvent(NetworkEvent::Connecting);

    // Wait for network ready with retry limit
    int reg_retries = 0;
    while (reg_retries < NETWORK_REG_MAX_RETRIES) {
        auto result = modem_->WaitForNetworkReady();
        if (result == NetworkStatus::Ready) {
            break;
        } else if (result == NetworkStatus::ErrorInsertPin) {
            OnNetworkEvent(NetworkEvent::ModemErrorNoSim);
        } else if (result == NetworkStatus::ErrorRegistrationDenied) {
            OnNetworkEvent(NetworkEvent::ModemErrorRegDenied);
        } else if (result == NetworkStatus::ErrorTimeout) {
            OnNetworkEvent(NetworkEvent::ModemErrorTimeout);
        }
        reg_retries++;
        vTaskDelay(pdMS_TO_TICKS(10000));
    }

    if (!modem_->network_ready()) {
        ESP_LOGE(TAG, "Failed to register network after %d retries", NETWORK_REG_MAX_RETRIES);
        return;
    }

    // Print the ML307 modem information
    std::string module_revision = modem_->GetModuleRevision();
    std::string imei = modem_->GetImei();
    std::string iccid = modem_->GetIccid();
    ESP_LOGI(TAG, "ML307 Revision: %s", module_revision.c_str());
    ESP_LOGI(TAG, "ML307 IMEI: %s", imei.c_str());
    ESP_LOGI(TAG, "ML307 ICCID: %s", iccid.c_str());

    // ─── CSQ 异步刷新常驻循环 (v32 P0 修复) ───────────────────────────────
    // 动机:原 GetCsq 同步发 AT 阻塞主循环 100ms × 每 10s 一次 → 改异步。
    //   主循环 UpdateStatusBar → modem->GetCsq() 改为纯读缓存(μs 级)。
    //   后台本任务 5s 一次 RefreshCsq → AT+CSQ → URC 回填 csq_ + csq_updated_us_。
    //   binary mode (mp3) 期间 SendCommand 被拒,csq_updated_us_ 不更新,
    //   30s 超阈值 GetCsq 返 -1 → icon 显示 SIGNAL_OFF (staleness 守护)。
    // 复用本 ml307_net 任务(P5 Core 0 · 栈 4096),不新增任务节省 INT RAM。
    constexpr TickType_t kCsqRefreshIntervalMs = 5000;
    // 网络注册成功立即首刷一次 CSQ,消除"已联网但 icon=SIGNAL_OFF"的 5s 启动窗口
    (void)modem_->RefreshCsq();
    ESP_LOGI(TAG, "首次 CSQ=%d · 进入后台刷新循环 (周期 %d ms)",
             modem_->GetCsq(), (int)kCsqRefreshIntervalMs);
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(kCsqRefreshIntervalMs));
        if (modem_ && modem_->network_ready()) {
            (void)modem_->RefreshCsq();
        }
    }
}

void Ml307Board::StartNetwork() {
    // NetworkTask 末尾进入常驻 CSQ 刷新循环,本任务永不返回
    // (lambda 末尾 vTaskDelete 实际上不会被执行,保留作防御性)
    xTaskCreatePinnedToCore([](void* arg) {
        Ml307Board* board = static_cast<Ml307Board*>(arg);
        board->NetworkTask();
        vTaskDelete(NULL);  // dead code (NetworkTask 内无限循环) · 保留防 NetworkTask 异常 return
    }, "ml307_net", 4096, this, 5, NULL, 0);
}

NetworkInterface* Ml307Board::GetNetwork() {
    return modem_.get();
}

const char* Ml307Board::GetNetworkStateIcon() {
    if (modem_ == nullptr || !modem_->network_ready()) {
        return FONT_AWESOME_SIGNAL_OFF;
    }
    int csq = modem_->GetCsq();
    if (csq == -1) {
        return FONT_AWESOME_SIGNAL_OFF;
    } else if (csq >= 0 && csq <= 9) {
        return FONT_AWESOME_SIGNAL_WEAK;
    } else if (csq >= 10 && csq <= 14) {
        return FONT_AWESOME_SIGNAL_FAIR;
    } else if (csq >= 15 && csq <= 19) {
        return FONT_AWESOME_SIGNAL_GOOD;
    } else if (csq >= 20 && csq <= 31) {
        return FONT_AWESOME_SIGNAL_STRONG;
    }

    ESP_LOGW(TAG, "Invalid CSQ: %d", csq);
    return FONT_AWESOME_SIGNAL_OFF;
}

std::string Ml307Board::GetBoardJson() {
    // Set the board type for OTA
    std::string board_json = std::string("{\"type\":\"" BOARD_TYPE "\",");
    board_json += "\"name\":\"" BOARD_NAME "\",";
    board_json += "\"revision\":\"" + modem_->GetModuleRevision() + "\",";
    board_json += "\"carrier\":\"" + modem_->GetCarrierName() + "\",";
    board_json += "\"csq\":\"" + std::to_string(modem_->GetCsq()) + "\",";
    board_json += "\"imei\":\"" + modem_->GetImei() + "\",";
    board_json += "\"iccid\":\"" + modem_->GetIccid() + "\",";
    board_json += "\"cereg\":" + modem_->GetRegistrationState().ToString() + "}";
    return board_json;
}

void Ml307Board::SetPowerSaveLevel(PowerSaveLevel level) {
    // TODO: Implement power save level for ML307
    (void)level;
}

std::string Ml307Board::GetDeviceStatusJson() {
    /*
     * 返回设备状态JSON
     * 
     * 返回的JSON结构如下：
     * {
     *     "audio_speaker": {
     *         "volume": 70
     *     },
     *     "screen": {
     *         "brightness": 100,
     *         "theme": "light"
     *     },
     *     "battery": {
     *         "level": 50,
     *         "charging": true
     *     },
     *     "network": {
     *         "type": "cellular",
     *         "carrier": "CHINA MOBILE",
     *         "csq": 10
     *     }
     * }
     */
    auto& board = Board::GetInstance();
    auto root = cJSON_CreateObject();

    // Audio speaker
    auto audio_speaker = cJSON_CreateObject();
    auto audio_codec = board.GetAudioCodec();
    if (audio_codec) {
        cJSON_AddNumberToObject(audio_speaker, "volume", audio_codec->output_volume());
    }
    cJSON_AddItemToObject(root, "audio_speaker", audio_speaker);

    // Screen brightness
    auto backlight = board.GetBacklight();
    auto screen = cJSON_CreateObject();
    if (backlight) {
        cJSON_AddNumberToObject(screen, "brightness", backlight->brightness());
    }
    auto display = board.GetDisplay();
    if (display && display->height() > 64) { // For LCD display only
        auto theme = display->GetTheme();
        if (theme != nullptr) {
            cJSON_AddStringToObject(screen, "theme", theme->name().c_str());
        }
    }
    cJSON_AddItemToObject(root, "screen", screen);

    // Battery
    int battery_level = 0;
    bool charging = false;
    bool discharging = false;
    if (board.GetBatteryLevel(battery_level, charging, discharging)) {
        cJSON* battery = cJSON_CreateObject();
        cJSON_AddNumberToObject(battery, "level", battery_level);
        cJSON_AddBoolToObject(battery, "charging", charging);
        cJSON_AddItemToObject(root, "battery", battery);
    }

    // Network
    auto network = cJSON_CreateObject();
    cJSON_AddStringToObject(network, "type", "cellular");
    cJSON_AddStringToObject(network, "carrier", modem_->GetCarrierName().c_str());
    int csq = modem_->GetCsq();
    if (csq == -1) {
        cJSON_AddStringToObject(network, "signal", "unknown");
    } else if (csq >= 0 && csq <= 14) {
        cJSON_AddStringToObject(network, "signal", "very weak");
    } else if (csq >= 15 && csq <= 19) {
        cJSON_AddStringToObject(network, "signal", "weak");
    } else if (csq >= 20 && csq <= 24) {
        cJSON_AddStringToObject(network, "signal", "medium");
    } else if (csq >= 25 && csq <= 31) {
        cJSON_AddStringToObject(network, "signal", "strong");
    }
    cJSON_AddNumberToObject(network, "csq", csq);
    cJSON_AddItemToObject(root, "network", network);

    auto json_str = cJSON_PrintUnformatted(root);
    std::string json(json_str);
    cJSON_free(json_str);
    cJSON_Delete(root);
    return json;
}
