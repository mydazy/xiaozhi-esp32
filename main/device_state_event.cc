#include "device_state_event.h"
#include <esp_log.h>
#include <freertos/FreeRTOS.h>

#define TAG "DeviceStateEvent"
static constexpr TickType_t kStateEventPostTimeoutTicks = pdMS_TO_TICKS(100);
static constexpr int kStateEventPostRetryCount = 3;

ESP_EVENT_DEFINE_BASE(XIAOZHI_STATE_EVENTS);

DeviceStateEventManager& DeviceStateEventManager::GetInstance() {
    static DeviceStateEventManager instance;
    return instance;
}

void DeviceStateEventManager::RegisterStateChangeCallback(std::function<void(DeviceState, DeviceState)> callback) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (callbacks_.size() >= MAX_STATE_CALLBACKS) {
        ESP_LOGW(TAG, "Max state callbacks reached (%u), cannot register new callback", MAX_STATE_CALLBACKS);
        return;
    }
    callbacks_.push_back(callback);
}

void DeviceStateEventManager::PostStateChangeEvent(DeviceState previous_state, DeviceState current_state) {
    device_state_event_data_t event_data = {
        .previous_state = previous_state,
        .current_state = current_state
    };
    esp_err_t err = ESP_FAIL;
    for (int attempt = 1; attempt <= kStateEventPostRetryCount; ++attempt) {
        err = esp_event_post(XIAOZHI_STATE_EVENTS,
                             XIAOZHI_STATE_CHANGED_EVENT,
                             &event_data,
                             sizeof(event_data),
                             kStateEventPostTimeoutTicks);
        if (err == ESP_OK) {
            return;
        }
        if (err != ESP_ERR_TIMEOUT) {
            ESP_LOGE(TAG, "Failed to post state event (%d -> %d), err=0x%x",
                     previous_state, current_state, err);
            return;
        }
        ESP_LOGW(TAG, "Post state event timeout (%d -> %d), retry %d/%d",
                 previous_state, current_state, attempt, kStateEventPostRetryCount);
    }

    ESP_LOGE(TAG, "Drop state event after timeout retries (%d -> %d)",
             previous_state, current_state);
}

std::vector<std::function<void(DeviceState, DeviceState)>> DeviceStateEventManager::GetCallbacks() {
    std::lock_guard<std::mutex> lock(mutex_);
    return callbacks_;
}

DeviceStateEventManager::DeviceStateEventManager() {
    esp_err_t err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_ERROR_CHECK(err);
    }

    ESP_ERROR_CHECK(esp_event_handler_register(XIAOZHI_STATE_EVENTS, XIAOZHI_STATE_CHANGED_EVENT, 
        [](void* handler_args, esp_event_base_t base, int32_t id, void* event_data) {
            auto* data = static_cast<device_state_event_data_t*>(event_data);
            auto& manager = DeviceStateEventManager::GetInstance();
            for (const auto& callback : manager.GetCallbacks()) {
                callback(data->previous_state, data->current_state);
            }
        }, nullptr));
}

DeviceStateEventManager::~DeviceStateEventManager() {
    esp_event_handler_unregister(XIAOZHI_STATE_EVENTS, XIAOZHI_STATE_CHANGED_EVENT, nullptr);
}
