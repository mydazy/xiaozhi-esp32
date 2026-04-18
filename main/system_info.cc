#include "system_info.h"

#include <freertos/task.h>
#include <esp_log.h>
#include <esp_flash.h>
#include <esp_mac.h>
#include <esp_system.h>
#include <esp_partition.h>
#include <esp_app_desc.h>
#include <esp_ota_ops.h>
#include <esp_clk_tree.h>
#include <esp_pm.h>
#if CONFIG_IDF_TARGET_ESP32P4
#include "esp_wifi_remote.h"
#endif

// ESP32-S3/C3/C6 支持温度传感器
#if SOC_TEMP_SENSOR_SUPPORTED
#include <driver/temperature_sensor.h>
#endif

#define TAG "SystemInfo"

size_t SystemInfo::GetFlashSize() {
    uint32_t flash_size;
    if (esp_flash_get_size(NULL, &flash_size) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get flash size");
        return 0;
    }
    return (size_t)flash_size;
}

size_t SystemInfo::GetMinimumFreeHeapSize() {
    return esp_get_minimum_free_heap_size();
}

size_t SystemInfo::GetFreeHeapSize() {
    return esp_get_free_heap_size();
}

std::string SystemInfo::GetMacAddress() {
    uint8_t mac[6];
#if CONFIG_IDF_TARGET_ESP32P4
    esp_wifi_get_mac(WIFI_IF_STA, mac);
#else
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
#endif
    char mac_str[18];
    snprintf(mac_str, sizeof(mac_str), "%02x:%02x:%02x:%02x:%02x:%02x", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return std::string(mac_str);
}

std::string SystemInfo::GetMacAddressLast4() {
    uint8_t mac[6];
#if CONFIG_IDF_TARGET_ESP32P4
    esp_wifi_get_mac(WIFI_IF_STA, mac);
#else
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
#endif
    char mac_str[5];
    snprintf(mac_str, sizeof(mac_str), "%02X%02X", mac[4], mac[5]);
    return std::string(mac_str);
}

std::string SystemInfo::GetChipModelName() {
    return std::string(CONFIG_IDF_TARGET);
}

std::string SystemInfo::GetUserAgent() {
    auto app_desc = esp_app_get_description();
    auto user_agent = std::string(BOARD_NAME "/") + app_desc->version;
    return user_agent;
}

esp_err_t SystemInfo::PrintTaskCpuUsage(TickType_t xTicksToWait) {
    #define ARRAY_SIZE_OFFSET 5
    TaskStatus_t *start_array = NULL, *end_array = NULL;
    UBaseType_t start_array_size, end_array_size;
    configRUN_TIME_COUNTER_TYPE start_run_time, end_run_time;
    esp_err_t ret;
    uint32_t total_elapsed_time;

    //Allocate array to store current task states
    start_array_size = uxTaskGetNumberOfTasks() + ARRAY_SIZE_OFFSET;
    start_array = (TaskStatus_t*)malloc(sizeof(TaskStatus_t) * start_array_size);
    if (start_array == NULL) {
        ret = ESP_ERR_NO_MEM;
        goto exit;
    }
    //Get current task states
    start_array_size = uxTaskGetSystemState(start_array, start_array_size, &start_run_time);
    if (start_array_size == 0) {
        ret = ESP_ERR_INVALID_SIZE;
        goto exit;
    }

    vTaskDelay(xTicksToWait);

    //Allocate array to store tasks states post delay
    end_array_size = uxTaskGetNumberOfTasks() + ARRAY_SIZE_OFFSET;
    end_array = (TaskStatus_t*)malloc(sizeof(TaskStatus_t) * end_array_size);
    if (end_array == NULL) {
        ret = ESP_ERR_NO_MEM;
        goto exit;
    }
    //Get post delay task states
    end_array_size = uxTaskGetSystemState(end_array, end_array_size, &end_run_time);
    if (end_array_size == 0) {
        ret = ESP_ERR_INVALID_SIZE;
        goto exit;
    }

    //Calculate total_elapsed_time in units of run time stats clock period.
    total_elapsed_time = (end_run_time - start_run_time);
    if (total_elapsed_time == 0) {
        ret = ESP_ERR_INVALID_STATE;
        goto exit;
    }

    printf("| Task | Run Time | Percentage\n");
    //Match each task in start_array to those in the end_array
    for (int i = 0; i < start_array_size; i++) {
        int k = -1;
        for (int j = 0; j < end_array_size; j++) {
            if (start_array[i].xHandle == end_array[j].xHandle) {
                k = j;
                //Mark that task have been matched by overwriting their handles
                start_array[i].xHandle = NULL;
                end_array[j].xHandle = NULL;
                break;
            }
        }
        //Check if matching task found
        if (k >= 0) {
            uint32_t task_elapsed_time = end_array[k].ulRunTimeCounter - start_array[i].ulRunTimeCounter;
            uint32_t percentage_time = (task_elapsed_time * 100UL) / (total_elapsed_time * CONFIG_FREERTOS_NUMBER_OF_CORES);
            printf("| %-16s | %8lu | %4lu%%\n", start_array[i].pcTaskName, task_elapsed_time, percentage_time);
        }
    }

    //Print unmatched tasks
    for (int i = 0; i < start_array_size; i++) {
        if (start_array[i].xHandle != NULL) {
            printf("| %s | Deleted\n", start_array[i].pcTaskName);
        }
    }
    for (int i = 0; i < end_array_size; i++) {
        if (end_array[i].xHandle != NULL) {
            printf("| %s | Created\n", end_array[i].pcTaskName);
        }
    }
    ret = ESP_OK;

exit:    //Common return path
    free(start_array);
    free(end_array);
    return ret;
}

void SystemInfo::PrintTaskList() {
    char buffer[1000];
    vTaskList(buffer);
    ESP_LOGI(TAG, "Task list: \n%s", buffer);
}

void SystemInfo::PrintHeapStats() {
    // 第1行：内存统计
    ESP_LOGW(TAG, "SRAM: %.1f KB (min: %.1f KB) | PSRAM: %.1f KB (min: %.1f KB)",
             heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024.0f,
             heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL) / 1024.0f,
             heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024.0f,
             heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM) / 1024.0f);

    // 第3行：芯片温度
    float temperature = 0.0f;
    if (GetChipTemperature(temperature) == ESP_OK) {
        if (temperature > 75.0f) {
            ESP_LOGE(TAG, "Temp: %.1f°C (Overheat!)", temperature);
        } else if (temperature > 65.0f) {
            ESP_LOGW(TAG, "Temp: %.1f°C (High)", temperature);
        } else {
            ESP_LOGI(TAG, "Temp: %.1f°C", temperature);
        }
    }
}

// 获取芯片温度（ESP32-S3/C3/C6 支持）
esp_err_t SystemInfo::GetChipTemperature(float& temperature) {
#if SOC_TEMP_SENSOR_SUPPORTED
    static temperature_sensor_handle_t temp_sensor = nullptr;

    // 首次调用时初始化温度传感器
    if (temp_sensor == nullptr) {
        temperature_sensor_config_t temp_sensor_config = TEMPERATURE_SENSOR_CONFIG_DEFAULT(-10, 80);
        esp_err_t ret = temperature_sensor_install(&temp_sensor_config, &temp_sensor);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "温度传感器初始化失败: %s", esp_err_to_name(ret));
            return ret;
        }

        ret = temperature_sensor_enable(temp_sensor);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "温度传感器启用失败: %s", esp_err_to_name(ret));
            temperature_sensor_uninstall(temp_sensor);
            temp_sensor = nullptr;
            return ret;
        }
    }

    // 读取温度
    esp_err_t ret = temperature_sensor_get_celsius(temp_sensor, &temperature);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "温度读取失败: %s", esp_err_to_name(ret));
    }
    return ret;
#else
    ESP_LOGW(TAG, "当前芯片不支持内部温度传感器");
    temperature = 0.0f;
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

void SystemInfo::PrintPmLocks() {
    esp_pm_dump_locks(stdout);
}
