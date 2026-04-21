#include <esp_log.h>
#include <esp_err.h>
#include <nvs.h>
#include <nvs_flash.h>
#include <driver/gpio.h>
#include <esp_event.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <sys/time.h>
#include <time.h>
#include <cstring>

#include "application.h"

#define TAG "main"

// 把编译时间设为系统默认时间 —— 避免 ClockPage 在联网 NTP 同步前显示 1970-01-01。
// NTP 同步成功后会自动被真实时间覆盖。
static void SetCompileTimeAsDefault() {
    struct tm compile_time = {};

    const char* months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                            "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    char month_str[4];
    int day = 1, year = 2026;
    sscanf(__DATE__, "%s %d %d", month_str, &day, &year);

    for (int i = 0; i < 12; i++) {
        if (strcmp(month_str, months[i]) == 0) {
            compile_time.tm_mon = i;
            break;
        }
    }
    compile_time.tm_mday = day;
    compile_time.tm_year = year - 1900;

    int hour = 0, min = 0, sec = 0;
    sscanf(__TIME__, "%d:%d:%d", &hour, &min, &sec);
    compile_time.tm_hour = hour;
    compile_time.tm_min = min;
    compile_time.tm_sec = sec;

    time_t compile_timestamp = mktime(&compile_time);
    struct timeval tv = { .tv_sec = compile_timestamp, .tv_usec = 0 };
    settimeofday(&tv, nullptr);

    ESP_LOGI(TAG, "系统默认时间已设为编译时间: %s %s", __DATE__, __TIME__);
}

extern "C" void app_main(void)
{
    // Initialize NVS flash for WiFi configuration
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "Erasing NVS flash to fix corruption");
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // 在 Application 初始化前设置默认时间（ClockPage Timer 起跑时就能拿到合理值，不是 1970）
    SetCompileTimeAsDefault();

    // Initialize and run the application
    auto& app = Application::GetInstance();
    app.Initialize();
    app.Run();  // This function runs the main event loop and never returns
}
