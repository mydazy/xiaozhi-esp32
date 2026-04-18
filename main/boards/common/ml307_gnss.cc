#include "ml307_gnss.h"
#include <esp_log.h>
#include <cstring>
#include <cstdlib>
#include <cctype>

#define TAG "Ml307Gnss"

Ml307Gnss::Ml307Gnss(std::shared_ptr<AtUart> uart) : uart_(uart) {}

Ml307Gnss::~Ml307Gnss() { Stop(); }

bool Ml307Gnss::NmeaToDecimal(const char* raw, char hemisphere, double& out) {
    if (!raw || raw[0] == '\0') return false;
    char* end = nullptr;
    double val = strtod(raw, &end);
    if (end == raw) return false;
    int deg = static_cast<int>(val / 100.0);
    double min = val - deg * 100.0;
    out = deg + min / 60.0;
    if (toupper(hemisphere) == 'S' || toupper(hemisphere) == 'W') out = -out;
    return true;
}

int Ml307Gnss::SplitFields(char* buf, char* fields[], int max_fields) {
    int count = 0;
    char* cursor = buf;
    while (count < max_fields) {
        fields[count++] = cursor;
        char* comma = strchr(cursor, ',');
        if (!comma) break;
        *comma = '\0';
        cursor = comma + 1;
    }
    return count;
}

void Ml307Gnss::ParseGga(const char* sentence) {
    char buf[256];
    strncpy(buf, sentence, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    char* fields[16] = {};
    int n = SplitFields(buf, fields, 16);
    if (n < 9) return;

    int sats = (fields[7] && fields[7][0]) ? atoi(fields[7]) : 0;

    if (!fields[6] || fields[6][0] == '0') {
        ESP_LOGD(TAG, "GGA: no fix, sats_in_use=%d", sats);
        if (sat_callback_) sat_callback_(sats);
        return;
    }

    double lat, lon;
    if (!NmeaToDecimal(fields[2], fields[3][0], lat)) return;
    if (!NmeaToDecimal(fields[4], fields[5][0], lon)) return;

    last_fix_.valid = true;
    last_fix_.latitude = lat;
    last_fix_.longitude = lon;
    last_fix_.satellites = sats;
    last_fix_.hdop = strtod(fields[8], nullptr);
    strncpy(last_fix_.utc_time, fields[1], sizeof(last_fix_.utc_time) - 1);

    ESP_LOGI(TAG, "FIX: lat=%.6f lon=%.6f sats=%d hdop=%.1f utc=%s",
             lat, lon, sats, last_fix_.hdop, last_fix_.utc_time);

    if (fix_callback_) fix_callback_(last_fix_);
    if (sat_callback_) sat_callback_(last_fix_.satellites);
}

void Ml307Gnss::ParseRmc(const char* sentence) {
    char buf[256];
    strncpy(buf, sentence, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    char* fields[16] = {};
    int n = SplitFields(buf, fields, 16);
    if (n < 7 || !fields[2] || fields[2][0] != 'A') return;

    double lat, lon;
    if (!NmeaToDecimal(fields[3], fields[4][0], lat)) return;
    if (!NmeaToDecimal(fields[5], fields[6][0], lon)) return;

    last_fix_.valid = true;
    last_fix_.latitude = lat;
    last_fix_.longitude = lon;
    strncpy(last_fix_.utc_time, fields[1], sizeof(last_fix_.utc_time) - 1);

    if (fix_callback_) fix_callback_(last_fix_);
}

void Ml307Gnss::ParseGsv(const char* sentence) {
    char buf[256];
    strncpy(buf, sentence, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    char* fields[20] = {};
    int n = SplitFields(buf, fields, 20);
    if (n < 4) return;

    // 只在第一条 GSV 报告总可见卫星数
    if (fields[2] && fields[2][0] == '1') {
        int total = atoi(fields[3]);
        if (sat_callback_) sat_callback_(total);
    }
}

void Ml307Gnss::ParseNmea(const std::string& line) {
    if (line.size() < 6 || line[0] != '$') return;

    std::string clean = line;
    auto star = clean.find('*');
    if (star != std::string::npos) clean.resize(star);

    const char* type = clean.c_str() + 3;
    // 每 10 秒打印一次原始 NMEA（避免刷屏）
    static int64_t last_log_time = 0;
    int64_t now = esp_timer_get_time() / 1000;
    if (now - last_log_time > 10000) {
        ESP_LOGI(TAG, "NMEA: %.80s", clean.c_str());
        last_log_time = now;
    }

    if (strncmp(type, "GGA", 3) == 0) ParseGga(clean.c_str());
    else if (strncmp(type, "RMC", 3) == 0) ParseRmc(clean.c_str());
    else if (strncmp(type, "GSV", 3) == 0) ParseGsv(clean.c_str());
}

bool Ml307Gnss::Start(uint8_t systems) {
    if (running_) return true;

    ESP_LOGI(TAG, "Starting GNSS (systems=0x%02X)", systems);

    // 注册 URC 回调捕获 NMEA
    urc_handle_ = uart_->RegisterUrcCallback(
        [this](const std::string& cmd, const std::vector<AtArgumentValue>&) {
            if (cmd.size() > 3 && cmd[0] == '$') ParseNmea(cmd);
        });
    urc_registered_ = true;

    // NMEA mask: GGA(1) + GSV(8) + RMC(16) = 25
    bool ok1 = uart_->SendCommand("AT+MGNSSCFG=\"nmea/mask\",25", 1000);
    ESP_LOGI(TAG, "NMEA mask → %s", ok1 ? "OK" : "FAIL");
    vTaskDelay(pdMS_TO_TICKS(200));

    // 定位系统: 1=GPS, 2=BDS, 3=GPS+BDS, 5=GPS+GLONASS, 7=ALL
    int mode = 0;
    if (systems & kGnssGps) mode |= 1;
    if (systems & kGnssBds) mode |= 2;
    if (systems & kGnssGlonass) mode |= 4;
    if (mode == 0) mode = 3;
    char cmd[64];
    snprintf(cmd, sizeof(cmd), "AT+MGNSS=%d", mode);
    bool ok2 = uart_->SendCommand(cmd, 1000);
    ESP_LOGI(TAG, "MGNSS=%d → %s", mode, ok2 ? "OK" : "FAIL");
    vTaskDelay(pdMS_TO_TICKS(200));

    // 开启持续 NMEA 输出
    bool ok3 = uart_->SendCommand("AT+MGNSSLOC=1", 2000);
    ESP_LOGI(TAG, "MGNSSLOC=1 → %s", ok3 ? "OK" : "FAIL");

    running_ = true;
    ESP_LOGI(TAG, "GNSS started (mode=%d, mask=25, cmd_ok=%d/%d/%d)", mode, ok1, ok2, ok3);
    return true;
}

bool Ml307Gnss::Stop() {
    if (!running_) return true;

    ESP_LOGI(TAG, "Stopping GNSS");
    uart_->SendCommand("AT+MGNSSLOC=0", 1000);
    vTaskDelay(pdMS_TO_TICKS(200));

    if (urc_registered_) {
        uart_->UnregisterUrcCallback(urc_handle_);
        urc_registered_ = false;
    }

    running_ = false;
    last_fix_ = {};
    ESP_LOGI(TAG, "GNSS stopped");
    return true;
}
