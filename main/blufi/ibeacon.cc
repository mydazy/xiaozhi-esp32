#include "ibeacon.h"

#include <cstring>
#include <cmath>

#include "esp_log.h"

#ifdef CONFIG_BT_NIMBLE_ENABLED
#include "host/ble_hs.h"
#include "host/ble_gap.h"
#endif

#define TAG "IBeacon"

// iBeacon 广播数据前缀
static const uint8_t IBEACON_PREFIX[] = {
    0x02, 0x01, 0x06,  // Flags
    0x1A, 0xFF,        // 厂商数据长度 + 类型
    0x4C, 0x00,        // Apple 公司 ID
    0x02, 0x15         // iBeacon 子类型 + 数据长度
};

// ============ 单例 ============

IBeacon& IBeacon::GetInstance() {
    static IBeacon instance;
    return instance;
}

// ============ 公共接口 ============

bool IBeacon::Start() {
#ifndef CONFIG_BT_NIMBLE_ENABLED
    ESP_LOGE(TAG, "NimBLE not enabled");
    return false;
#endif

    if (scanning_) {
        ESP_LOGW(TAG, "Already scanning");
        return true;
    }

    if (!ble_hs_synced()) {
        ESP_LOGE(TAG, "BLE not synced");
        return false;
    }

    // 省电模式扫描参数
    struct ble_gap_disc_params disc_params = {};
    disc_params.itvl = 2048;         // 1280ms
    disc_params.window = 18;         // 11.25ms
    disc_params.filter_policy = 0;
    disc_params.limited = 0;
    disc_params.passive = 1;         // 被动扫描 (省电)
    disc_params.filter_duplicates = 1;

    int rc = ble_gap_disc(BLE_OWN_ADDR_PUBLIC, BLE_HS_FOREVER, &disc_params,
                          ScanCallback, this);
    if (rc != 0) {
        ESP_LOGE(TAG, "Failed to start: %d", rc);
        return false;
    }

    scanning_ = true;
    initialized_ = true;
    ESP_LOGI(TAG, "Started (省电: 1280ms/11.25ms, ~3-5mA)");
    return true;
}

void IBeacon::Stop() {
    if (!scanning_) {
        return;
    }

    ble_gap_disc_cancel();
    scanning_ = false;
    ESP_LOGI(TAG, "Stopped");
}

void IBeacon::OnDetected(IBeaconCallback callback) {
    on_detected_ = callback;
}

// ============ 内部方法 ============

int IBeacon::ScanCallback(struct ble_gap_event* event, void* arg) {
    auto* self = static_cast<IBeacon*>(arg);

    if (event->type != BLE_GAP_EVENT_DISC) {
        return 0;
    }

    IBeaconInfo beacon;
    if (ParseIBeacon(event->disc.data, event->disc.length_data, beacon)) {
        beacon.rssi = event->disc.rssi;

        ESP_LOGI(TAG, "检测: UUID=%s, Major=%u, Minor=%u, RSSI=%d, 距离≈%.2fm",
                 beacon.uuid.c_str(), beacon.major, beacon.minor, beacon.rssi,
                 beacon.CalculateDistance());

        if (self->on_detected_) {
            self->on_detected_(beacon);
        }
    }

    return 0;
}

bool IBeacon::ParseIBeacon(const uint8_t* adv_data, size_t len, IBeaconInfo& beacon) {
    if (len < 30) {
        return false;
    }

    if (memcmp(adv_data, IBEACON_PREFIX, sizeof(IBEACON_PREFIX)) != 0) {
        return false;
    }

    const uint8_t* data = adv_data + sizeof(IBEACON_PREFIX);

    beacon.uuid = UuidToString(data);
    data += 16;

    beacon.major = (data[0] << 8) | data[1];
    data += 2;

    beacon.minor = (data[0] << 8) | data[1];
    data += 2;

    beacon.tx_power = (int8_t)data[0];

    return true;
}

std::string IBeacon::UuidToString(const uint8_t* uuid) {
    char buffer[37];
    snprintf(buffer, sizeof(buffer),
             "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
             uuid[0], uuid[1], uuid[2], uuid[3],
             uuid[4], uuid[5],
             uuid[6], uuid[7],
             uuid[8], uuid[9],
             uuid[10], uuid[11], uuid[12], uuid[13], uuid[14], uuid[15]);
    return std::string(buffer);
}

// ============ IBeaconInfo 方法 ============

std::string IBeaconInfo::ToString() const {
    char buffer[128];
    snprintf(buffer, sizeof(buffer),
             "UUID=%s, Major=%u, Minor=%u, RSSI=%d, TxPower=%d",
             uuid.c_str(), major, minor, rssi, tx_power);
    return std::string(buffer);
}

float IBeaconInfo::CalculateDistance() const {
    if (rssi == 0) {
        return -1.0f;
    }

    const float n = 2.0f;  // 路径损耗指数
    float ratio = (tx_power - rssi) / (10.0f * n);
    return powf(10.0f, ratio);
}