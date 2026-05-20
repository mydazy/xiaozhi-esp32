#include "ml307_at_modem.h"
#include <esp_log.h>
#include <esp_err.h>
#include <cassert>
#include <sstream>
#include <iomanip>
#include <cstring>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include "ml307_tcp.h"
#include "ml307_ssl.h"
#include "ml307_udp.h"
#include "ml307_mqtt.h"
#include "ml307_http.h"
#include "web_socket.h"

#define TAG "Ml307AtModem"

namespace {
constexpr int kRadioCommandTimeoutMs   = 1500;
constexpr int kRadioCommandRetryDelayMs = 200;
}


Ml307AtModem::Ml307AtModem(std::shared_ptr<AtUart> at_uart) : AtModem(at_uart) {
    // 子类特定的初始化在这里
    // Reset HTTP instances
    ResetConnections();
    // Patch A · 开机锁 LTE-only（弱信号区不再 2G/3G 反复回退）
    ConfigureRadioProfile();
}

void Ml307AtModem::ResetConnections() {
    at_uart_->SendCommand("AT+MHTTPDEL=0");
    at_uart_->SendCommand("AT+MHTTPDEL=1");
    at_uart_->SendCommand("AT+MHTTPDEL=2");
    at_uart_->SendCommand("AT+MHTTPDEL=3");
}

// Patch A · AT 命令重试包装（弱网下单次失败不立即视为模组挂掉）
bool Ml307AtModem::SendRadioCommandWithRetry(const char* command, const char* description, int max_attempts) {
    for (int attempt = 1; attempt <= max_attempts; ++attempt) {
        if (at_uart_->SendCommand(command, kRadioCommandTimeoutMs)) {
            ESP_LOGI(TAG, "%s ok (%d/%d)", description, attempt, max_attempts);
            return true;
        }
        ESP_LOGW(TAG, "%s failed (%d/%d), cme=%d",
                 description, attempt, max_attempts, at_uart_->GetCmeErrorCode());
        vTaskDelay(pdMS_TO_TICKS(kRadioCommandRetryDelayMs));
    }
    ESP_LOGW(TAG, "%s not applied, keep modem default strategy", description);
    return false;
}

void Ml307AtModem::ConfigureRadioProfile() {
    SendRadioCommandWithRetry("AT+MRATLIST=\"LTE\"", "Set LTE-only RAT");
//    SendRadioCommandWithRetry("AT+MBAND=3,1,8", "Lock LTE bands to B1/B3/B8");

    // 查询当前配置，写入日志便于量产期排查
    if (at_uart_->SendCommand("AT+MRATLIST?", 1000)) {
        ESP_LOGI(TAG, "MRATLIST: %s", at_uart_->GetResponse().c_str());
    }
    if (at_uart_->SendCommand("AT+MBAND?", 1000)) {
        ESP_LOGI(TAG, "MBAND: %s", at_uart_->GetResponse().c_str());
    }
}

void Ml307AtModem::HandleUrc(const std::string& command, const std::vector<AtArgumentValue>& arguments) {
    // Handle Common URC
    AtModem::HandleUrc(command, arguments);
    // Handle ML307 URC
    if (command == "MIPCALL" && arguments.size() >= 3) {
        if (arguments[1].int_value == 1) {
            auto ip = arguments[2].string_value;
            ESP_LOGI(TAG, "PDP Context %d IP: %s", arguments[0].int_value, ip.c_str());
            network_ready_ = true;
            xEventGroupSetBits(event_group_handle_, AT_EVENT_NETWORK_READY);
        }
    } else if (command == "MATREADY") {
        if (network_ready_) {
            network_ready_ = false;
            if (on_network_state_changed_) {
                on_network_state_changed_(false);
            }
        }
    } else if (command == "__UART_OVERFLOW__") {
        ESP_LOGE(TAG, "UART overflow → force disconnect (TCP stream corrupted)");
        if (network_ready_) {
            network_ready_ = false;
            if (on_network_state_changed_) {
                on_network_state_changed_(false);
            }
        }
    }
}

void Ml307AtModem::Reboot() {
    at_uart_->SendCommand("AT+MREBOOT=0");
}

bool Ml307AtModem::SetSleepMode(bool enable, int delay_seconds) {
    if (enable) {
        if (delay_seconds > 0) {
            at_uart_->SendCommand("AT+MLPMCFG=\"delaysleep\"," + std::to_string(delay_seconds));
        }
        return at_uart_->SendCommand("AT+MLPMCFG=\"sleepmode\",2,0");
    } else {
        return at_uart_->SendCommand("AT+MLPMCFG=\"sleepmode\",0,0");
    }
}

NetworkStatus Ml307AtModem::WaitForNetworkReady(int timeout_ms) {
    NetworkStatus status = AtModem::WaitForNetworkReady(timeout_ms);
    if (status == NetworkStatus::Ready) {
        // Wait for IP address, maximum total wait time is 4270ms
        int delay_ms = 10;
        for (int i = 0; i < 10; i++) {
            at_uart_->SendCommand("AT+MIPCALL?");
            auto bits = xEventGroupWaitBits(event_group_handle_, AT_EVENT_NETWORK_READY, pdFALSE, pdTRUE, pdMS_TO_TICKS(delay_ms));
            if (bits & AT_EVENT_NETWORK_READY) {
                return NetworkStatus::Ready;
            }
            delay_ms = std::min(delay_ms * 2, 1000);
        }
        ESP_LOGE(TAG, "Network ready but no IP address");
    }
    return status;
}

std::unique_ptr<Http> Ml307AtModem::CreateHttp(int connect_id) {
    return std::make_unique<Ml307Http>(at_uart_);
}

std::unique_ptr<Tcp> Ml307AtModem::CreateTcp(int connect_id) {
    assert(connect_id >= 0);
    return std::make_unique<Ml307Tcp>(at_uart_, connect_id);
}

std::unique_ptr<Tcp> Ml307AtModem::CreateSsl(int connect_id) {
    assert(connect_id >= 0);
    return std::make_unique<Ml307Ssl>(at_uart_, connect_id);
}

std::unique_ptr<Udp> Ml307AtModem::CreateUdp(int connect_id) {
    assert(connect_id >= 0);
    return std::make_unique<Ml307Udp>(at_uart_, connect_id);
}

std::unique_ptr<Mqtt> Ml307AtModem::CreateMqtt(int connect_id) {
    assert(connect_id >= 0);
    return std::make_unique<Ml307Mqtt>(at_uart_, connect_id);
}

std::unique_ptr<WebSocket> Ml307AtModem::CreateWebSocket(int connect_id) {
    assert(connect_id >= 0);
    return std::make_unique<WebSocket>(this, connect_id);
}
