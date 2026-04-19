#include "system_reset.h"

#include <esp_log.h>
#include <nvs_flash.h>
#include <driver/gpio.h>
#include <esp_partition.h>
#include <esp_system.h>
#include <freertos/FreeRTOS.h>

#include "board.h"
#include "system_info.h"


#define TAG "SystemReset"


// 请求服务器 /reset 解绑设备（失败不阻塞恢复出厂流程）
bool SystemReset::RequestServerUnbind() {
    std::string url = CONFIG_OTA_URL;
    if (url.find("mydazy") == std::string::npos) {
        ESP_LOGI(TAG, "Not mydazy server, skip unbind request");
        return false;
    }
    url += "/reset";

    auto& board = Board::GetInstance();
    auto network = board.GetNetwork();
    if (!network) {
        ESP_LOGW(TAG, "Network not available, skip server unbind");
        return false;
    }

    auto http = network->CreateHttp(0);
    if (!http) {
        ESP_LOGW(TAG, "Failed to create HTTP client");
        return false;
    }

    http->SetHeader("Device-Id", SystemInfo::GetMacAddress().c_str());
    http->SetHeader("Client-Id", board.GetUuid().c_str());
    http->SetHeader("User-Agent", SystemInfo::GetUserAgent().c_str());
    http->SetTimeout(5000);

    if (!http->Open("GET", url)) {
        ESP_LOGW(TAG, "Failed to connect to server for unbind");
        return false;
    }

    int status_code = http->GetStatusCode();
    std::string response = http->ReadAll();
    http->Close();

    ESP_LOGI(TAG, "Server unbind response: code=%d, body=%s", status_code, response.c_str());
    return status_code == 200 || status_code == 204;
}


void SystemReset::DoFactoryReset(bool unbind_server) {
    ESP_LOGW(TAG, "====== 执行恢复出厂 ======");
    if (unbind_server) {
        RequestServerUnbind();
    }
    ResetNvsFlash();
    ResetToFactory();
    // 返回后由调用方执行 app.Reboot()（内部会切 LDO 复位 LCD/音频）
}


SystemReset::SystemReset(gpio_num_t reset_nvs_pin, gpio_num_t reset_factory_pin) : reset_nvs_pin_(reset_nvs_pin), reset_factory_pin_(reset_factory_pin) {
    gpio_config_t io_conf;
    io_conf.intr_type = GPIO_INTR_DISABLE;
    io_conf.mode = GPIO_MODE_INPUT;
    io_conf.pin_bit_mask = (1ULL << reset_nvs_pin_) | (1ULL << reset_factory_pin_);
    io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io_conf.pull_up_en = GPIO_PULLUP_ENABLE;
    gpio_config(&io_conf);
}


void SystemReset::CheckButtons(bool force_reset) {
    if (force_reset) {
        ESP_LOGI(TAG, "Force reset to factory");
        DoFactoryReset(true);
        RestartInSeconds(3);
        return;
    }

    if (gpio_get_level(reset_factory_pin_) == 0) {
        ESP_LOGI(TAG, "Button is pressed, reset to factory");
        DoFactoryReset(true);
        RestartInSeconds(3);
    }

    if (gpio_get_level(reset_nvs_pin_) == 0) {
        ESP_LOGI(TAG, "Button is pressed, reset NVS flash");
        ResetNvsFlash();
    }
}

void SystemReset::ResetNvsFlash() {
    ESP_LOGI(TAG, "Resetting NVS flash");
    esp_err_t ret = nvs_flash_erase();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to erase NVS flash");
    }
    ret = nvs_flash_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize NVS flash");
    }
}

void SystemReset::ResetToFactory() {
    ESP_LOGI(TAG, "Resetting to factory");
    const esp_partition_t* partition = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_OTA, NULL);
    if (partition == NULL) {
        ESP_LOGE(TAG, "Failed to find otadata partition");
        return;
    }
    esp_partition_erase_range(partition, 0, partition->size);
    ESP_LOGI(TAG, "Erased otadata partition");
}

void SystemReset::RestartInSeconds(int seconds) {
    for (int i = seconds; i > 0; i--) {
        ESP_LOGI(TAG, "Resetting in %d seconds", i);
        vTaskDelay(1000 / portTICK_PERIOD_MS);
    }

    // 关 LDO 让 LCD/音频 CODEC 硬复位，避免 esp_restart() 后黑屏
    Board::GetInstance().PrepareForReboot();
    vTaskDelay(pdMS_TO_TICKS(200));
    esp_restart();
}
