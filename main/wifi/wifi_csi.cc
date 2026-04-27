#include "wifi_csi.h"
#include "settings.h"

#include <cmath>
#include <cstring>
#include <esp_log.h>
#include <esp_wifi.h>
#include <lwip/inet.h>
#include <lwip/sockets.h>

#define TAG "WifiCSI"

WifiCsi& WifiCsi::GetInstance() {
    static WifiCsi instance;
    return instance;
}

WifiCsi::WifiCsi() {
    memset(var_buf_, 0, sizeof(var_buf_));
    // 从 NVS 加载开关状态（默认关闭）
    Settings s("wifi_csi", false);
    enabled_.store(s.GetInt("enabled", 0) != 0);
}

WifiCsi::~WifiCsi() {
    Stop();
}

void WifiCsi::SetEnabled(bool enabled) {
    enabled_.store(enabled);
    Settings s("wifi_csi", true);
    s.SetInt("enabled", enabled ? 1 : 0);
    ESP_LOGI(TAG, "CSI %s（已持久化）", enabled ? "已开启" : "已关闭");

    if (!enabled && running_.load()) {
        Stop();
    }
}

void WifiCsi::Start() {
    if (running_.load()) {
        return;
    }
    if (!enabled_.load()) {
        ESP_LOGI(TAG, "CSI 未启用，跳过启动");
        return;
    }

    wifi_csi_config_t cfg = {
        .lltf_en = true,
        .htltf_en = true,
        .stbc_htltf2_en = true,
        .ltf_merge_en = true,
        .channel_filter_en = true,
        .manu_scale = false,
        .shift = 0,
        .dump_ack_en = false,
    };

    esp_err_t ret = esp_wifi_set_csi_config(&cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Set CSI config failed: %s", esp_err_to_name(ret));
        return;
    }

    ret = esp_wifi_set_csi_rx_cb(CsiRxCallback, this);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Set CSI callback failed: %s", esp_err_to_name(ret));
        return;
    }

    ret = esp_wifi_set_csi(true);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Enable CSI failed: %s", esp_err_to_name(ret));
        return;
    }

    // 分析定时器 1.5s
    esp_timer_create_args_t timer_args = {
        .callback = AnalysisCallback,
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "csi_analysis",
        .skip_unhandled_events = true,
    };
    ret = esp_timer_create(&timer_args, &timer_);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Create analysis timer failed: %s", esp_err_to_name(ret));
        esp_wifi_set_csi(false);
        return;
    }
    esp_timer_start_periodic(timer_, 1500000);  // 1.5s

    // Ping 定时器：每 500ms 发 UDP 包给网关，产生 WiFi 流量触发 CSI 回调
    esp_timer_create_args_t ping_args = {
        .callback = PingCallback,
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "csi_ping",
        .skip_unhandled_events = true,
    };
    ret = esp_timer_create(&ping_args, &ping_timer_);
    if (ret == ESP_OK) {
        esp_timer_start_periodic(ping_timer_, 500000);  // 500ms
    }

    buf_idx_ = 0;
    buf_count_ = 0;
    sum_var_.store(0.0f);
    frames_.store(0);
    zone_.store(kCsiZoneNone);
    confirm_count_ = 0;
    pending_zone_ = kCsiZoneNone;
    baseline_ = 0.0f;
    initial_baseline_ = 0.0f;
    baseline_samples_ = 0;

    running_.store(true);
    ESP_LOGI(TAG, "Started（前 %.1fs 建立基线）", kBaselineSamples * 1.5f);
}

void WifiCsi::Stop() {
    if (!running_.load()) {
        return;
    }

    running_.store(false);

    if (timer_) {
        esp_timer_stop(timer_);
        esp_timer_delete(timer_);
        timer_ = nullptr;
    }

    if (ping_timer_) {
        esp_timer_stop(ping_timer_);
        esp_timer_delete(ping_timer_);
        ping_timer_ = nullptr;
    }

    esp_wifi_set_csi(false);
    esp_wifi_set_csi_rx_cb(nullptr, nullptr);

    zone_.store(kCsiZoneNone);
    ESP_LOGI(TAG, "Stopped");
}

void WifiCsi::OnZoneChange(std::function<void(const CsiEvent&)> callback) {
    on_zone_change_ = callback;
}

void WifiCsi::SetThresholds(float near_ratio, float medium_ratio, float far_ratio) {
    th_near_enter_ = near_ratio;
    th_medium_enter_ = medium_ratio;
    th_far_enter_ = far_ratio;
    // 离开阈值 = 进入阈值的 60%
    th_near_leave_ = near_ratio * 0.6f;
    th_medium_leave_ = medium_ratio * 0.6f;
    th_far_leave_ = far_ratio * 0.6f;
    ESP_LOGI(TAG, "Thresholds: near=%.1f/%.1f, mid=%.1f/%.1f, far=%.1f/%.1f",
             th_near_enter_, th_near_leave_,
             th_medium_enter_, th_medium_leave_,
             th_far_enter_, th_far_leave_);
}

// 每 500ms 发 UDP 包给网关，产生 WiFi 数据帧让 CSI 回调触发
void WifiCsi::PingCallback(void* arg) {
    auto* self = static_cast<WifiCsi*>(arg);
    if (!self->running_.load()) return;

    esp_netif_t* netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (!netif) return;

    esp_netif_ip_info_t ip_info;
    if (esp_netif_get_ip_info(netif, &ip_info) != ESP_OK || ip_info.gw.addr == 0) {
        return;
    }

    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) return;

    struct sockaddr_in dest = {};
    dest.sin_family = AF_INET;
    dest.sin_port = htons(9);
    dest.sin_addr.s_addr = ip_info.gw.addr;

    uint8_t ping_byte = 0;
    sendto(sock, &ping_byte, 1, 0, (struct sockaddr*)&dest, sizeof(dest));
    close(sock);
}

// WiFi 驱动回调，必须快速返回
void WifiCsi::CsiRxCallback(void* ctx, wifi_csi_info_t* data) {
    auto* self = static_cast<WifiCsi*>(ctx);
    if (!self->running_.load() || !data || !data->buf || data->len < 4) {
        return;
    }

    int start = data->first_word_invalid ? 4 : 0;
    int n = (data->len - start) / 2;
    if (n < 2) {
        return;
    }

    float sum = 0;
    float sum_sq = 0;
    const int8_t* buf = data->buf;

    for (int i = 0; i < n; i++) {
        int idx = start + i * 2;
        float r = static_cast<float>(buf[idx]);
        float im = static_cast<float>(buf[idx + 1]);
        float amp = sqrtf(r * r + im * im);
        sum += amp;
        sum_sq += amp * amp;
    }

    float mean = sum / n;
    float var = (sum_sq / n) - (mean * mean);
    if (var < 0) var = 0;

    float prev = self->sum_var_.load();
    self->sum_var_.store(prev + var);
    self->frames_.fetch_add(1);
}

void WifiCsi::AnalysisCallback(void* arg) {
    static_cast<WifiCsi*>(arg)->Analyze();
}

void WifiCsi::Analyze() {
    int frame_count = frames_.exchange(0);
    float total_var = sum_var_.exchange(0.0f);

    if (frame_count == 0) {
        return;
    }

    // 帧数过滤：对话期间 WiFi 流量大（f>10），CSI 数据被网络流量污染，
    // 只取 ping 产生的帧（正常 1-3 帧/周期），跳过高流量周期
    if (frame_count > 8) {
        ESP_LOGD(TAG, "[skip] f=%d（WiFi 流量过大，跳过）", frame_count);
        return;
    }

    float avg_var = total_var / frame_count;

    // 存入滑动窗口
    var_buf_[buf_idx_] = avg_var;
    buf_idx_ = (buf_idx_ + 1) % kWindow;
    if (buf_count_ < kWindow) {
        buf_count_++;
    }

    // 计算窗口内方差均值
    float var_sum = 0;
    for (int i = 0; i < buf_count_; i++) {
        var_sum += var_buf_[i];
    }
    float var_mean = var_sum / buf_count_;

    // 计算窗口方差（变化剧烈程度）
    float window_var = Variance(var_buf_, buf_count_);

    // 综合 score
    float score = var_mean + window_var * 0.3f;

    // 自适应基线：前 N 个样本建立"无人"基线
    if (baseline_samples_ < kBaselineSamples) {
        baseline_samples_++;
        baseline_ = (baseline_ * (baseline_samples_ - 1) + score) / baseline_samples_;
        if (baseline_samples_ == kBaselineSamples) {
            initial_baseline_ = baseline_;  // 快照初始基线
            ESP_LOGI(TAG, "基线建立完成: %.0f", baseline_);
        } else {
            ESP_LOGD(TAG, "基线建立 %d/%d score=%.0f",
                     baseline_samples_, kBaselineSamples, score);
        }
        return;
    }

    if (baseline_ < 10.0f) baseline_ = 10.0f;

    float ratio = score / baseline_;

    // 校准日志降为 DEBUG 级别，减少刷屏
    const char* zone_names[] = {"None", "Far", "Mid", "Near"};
    ESP_LOGD(TAG, "[cal] score=%.0f base=%.0f ratio=%.1fx f=%d → %s",
             score, baseline_, ratio, frame_count, zone_names[zone_.load()]);

    // 基线漂移修正：
    // 1. 只在 ratio 接近 1.0 时更新（说明环境平稳，无人在旁）
    // 2. 极低 ratio（<0.3）可能是 WiFi 信号异常，不更新
    // 3. 基线有上限，不超过 initial_baseline_ * kBaselineCeilRatio
    float baseline_ceil = initial_baseline_ * kBaselineCeilRatio;
    if (ratio > 0.3f && ratio < 1.8f) {
        float new_base = baseline_ * 0.95f + score * 0.05f;
        if (new_base <= baseline_ceil) {
            baseline_ = new_base;
        }
    }
    // 如果长期无人且 score 很低，加速基线下降
    if (ratio < 0.5f && baseline_ > initial_baseline_ * 1.2f) {
        baseline_ = baseline_ * 0.90f + score * 0.10f;
    }

    // 带迟滞的区域判定：进入需要更高 ratio，离开需要更低 ratio
    CsiZone current = zone_.load();
    CsiZone detected;

    if (current >= kCsiZoneNear) {
        // 当前在 Near，检查是否应降级
        if (ratio >= th_near_leave_) detected = kCsiZoneNear;
        else if (ratio >= th_medium_leave_) detected = kCsiZoneMedium;
        else if (ratio >= th_far_leave_) detected = kCsiZoneFar;
        else detected = kCsiZoneNone;
    } else if (current >= kCsiZoneMedium) {
        if (ratio >= th_near_enter_) detected = kCsiZoneNear;
        else if (ratio >= th_medium_leave_) detected = kCsiZoneMedium;
        else if (ratio >= th_far_leave_) detected = kCsiZoneFar;
        else detected = kCsiZoneNone;
    } else if (current >= kCsiZoneFar) {
        if (ratio >= th_near_enter_) detected = kCsiZoneNear;
        else if (ratio >= th_medium_enter_) detected = kCsiZoneMedium;
        else if (ratio >= th_far_leave_) detected = kCsiZoneFar;
        else detected = kCsiZoneNone;
    } else {
        // 当前 None，进入需要更高阈值
        if (ratio >= th_near_enter_) detected = kCsiZoneNear;
        else if (ratio >= th_medium_enter_) detected = kCsiZoneMedium;
        else if (ratio >= th_far_enter_) detected = kCsiZoneFar;
        else detected = kCsiZoneNone;
    }

    // 去抖：升级（进入更近区域）需要 kConfirmEnter 次，
    //       降级（离开到更远区域）需要 kConfirmLeave 次
    if (detected == pending_zone_) {
        confirm_count_++;
    } else {
        pending_zone_ = detected;
        confirm_count_ = 1;
    }

    bool is_upgrade = (detected > current);
    int required = is_upgrade ? kConfirmEnter : kConfirmLeave;

    if (confirm_count_ >= required) {
        if (detected != current) {
            zone_.store(detected);

            ESP_LOGI(TAG, "%s → %s (ratio=%.1fx, score=%.0f, base=%.0f)",
                     zone_names[current], zone_names[detected], ratio, score, baseline_);

            if (on_zone_change_) {
                CsiEvent event = {
                    .zone = detected,
                    .previous_zone = current,
                    .variance = score,
                    .amplitude = ratio,
                };
                on_zone_change_(event);
            }
        }
    }
}

float WifiCsi::Variance(const float* data, int len) {
    if (len < 2) return 0;

    float sum = 0;
    float sum_sq = 0;
    for (int i = 0; i < len; i++) {
        sum += data[i];
        sum_sq += data[i] * data[i];
    }
    float mean = sum / len;
    float v = (sum_sq / len) - (mean * mean);
    return v < 0 ? 0 : v;
}

void WifiCsi::PrintStats() {
    const char* names[] = {"None", "Far", "Medium", "Near"};
    ESP_LOGI(TAG, "zone=%s enabled=%d baseline=%.1f initial=%.1f buf=%d/%d",
             names[zone_.load()], enabled_.load() ? 1 : 0,
             baseline_, initial_baseline_,
             buf_count_, kWindow);
}
