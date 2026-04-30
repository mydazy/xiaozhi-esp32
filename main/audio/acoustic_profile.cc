#include "acoustic_profile.h"

#include <esp_log.h>
#include <cmath>
#include <cstdio>

#include "settings.h"
#include "board.h"
#include "audio_codec.h"
#include "audio_service.h"
#include "application.h"

#define TAG "AcousticProfile"

// 3 档预设 · mic_gain 都是 ES7210 PGA 3 dB 整数倍（0/3/6/.../37.5）
// 选 15/21/27 而非 12/18/24：
//   - 15 dB = 5 step（足够压抑环境噪声）
//   - 21 dB = 7 step（默认 · 比旧 18 dB 高 3 dB · 解决 "听不见" 问题）
//   - 27 dB = 9 step（远讲补偿 · 但接近 30 dB 上限 · 警告 AEC 压力）
static const AcousticProfile::Preset kPresets[3] = {
    {
        "robust",
        "嘈杂环境/外放音乐/误唤醒多",
        15.0f,
        "远讲>1m 会变迟钝 · 适合厨房/客厅 TV 旁",
    },
    {
        "standard",
        "默认 · 普通家庭近讲（30~80cm）",
        21.0f,
        "平衡设置 · 多数场景适用",
    },
    {
        "sensitive",
        "远讲>1.5m/低噪声卧室/咪头隔音差",
        27.0f,
        "AEC 残留风险变高 · 喇叭播放时可能误识别",
    },
};

AcousticProfile& AcousticProfile::GetInstance() {
    static AcousticProfile instance;
    return instance;
}

const AcousticProfile::Preset& AcousticProfile::GetPreset(Profile p) {
    if (p < 0 || p >= kProfileCount) {
        return kPresets[kStandard];
    }
    return kPresets[p];
}

const AcousticProfile::Preset& AcousticProfile::GetCurrentPreset() const {
    return GetPreset(profile_);
}

void AcousticProfile::Initialize() {
    if (initialized_) return;

    Settings settings("audio", false);
    int p = settings.GetInt("ap_profile", -1);

    if (p < 0 || p >= kProfileCount) {
        // 兼容升级：若已有旧 input_gain 则按值就近映射档位
        // 否则 → 默认 standard（21 dB · 比旧 18 dB 高 3 dB）
        int old_gain = settings.GetInt("input_gain", -1);
        if (old_gain < 0) {
            profile_ = kStandard;
        } else if (old_gain <= 17) {
            profile_ = kRobust;       // ≤17 → robust(15)
        } else if (old_gain <= 24) {
            profile_ = kStandard;     // 18~24 → standard(21)
        } else {
            profile_ = kSensitive;    // ≥25 → sensitive(27)
        }
        ESP_LOGI(TAG, "无 ap_profile 字段 · 旧 input_gain=%d → 映射到 %s",
                 old_gain, kPresets[profile_].name);
    } else {
        profile_ = static_cast<Profile>(p);
    }

    const Preset& preset = kPresets[profile_];
    ESP_LOGI(TAG, "档位: %s（%s）· mic_gain=%.0f dB", preset.name, preset.desc, preset.mic_gain);

    // 立即下发到 codec（覆盖旧 input_gain 字段，统一以 ap_profile 为准）
    auto codec = Board::GetInstance().GetAudioCodec();
    if (codec) {
        codec->SetInputGain(preset.mic_gain);  // SetInputGain 内部会写 NVS audio.input_gain
    }

    // 显式持久化 ap_profile（首次启动或升级路径）
    Settings ws("audio", true);
    ws.SetInt("ap_profile", static_cast<int>(profile_));

    initialized_ = true;
}

bool AcousticProfile::SetProfile(Profile p) {
    if (p < 0 || p >= kProfileCount) {
        ESP_LOGW(TAG, "档位编号越界: %d", static_cast<int>(p));
        return false;
    }
    profile_ = p;
    const Preset& preset = kPresets[profile_];

    auto codec = Board::GetInstance().GetAudioCodec();
    if (!codec) {
        ESP_LOGE(TAG, "codec 未就绪，切档失败");
        return false;
    }
    codec->SetInputGain(preset.mic_gain);

    Settings settings("audio", true);
    settings.SetInt("ap_profile", static_cast<int>(profile_));

    ESP_LOGI(TAG, "切到档位 %s · mic_gain=%.0f dB · %s",
             preset.name, preset.mic_gain, preset.tradeoff);
    return true;
}

bool AcousticProfile::SetProfileByName(const std::string& name) {
    for (int i = 0; i < kProfileCount; i++) {
        if (name == kPresets[i].name) {
            return SetProfile(static_cast<Profile>(i));
        }
    }
    ESP_LOGW(TAG, "未知档位名: %s", name.c_str());
    return false;
}

std::string AcousticProfile::Diagnose(int max_ms) {
    auto codec = Board::GetInstance().GetAudioCodec();
    if (!codec) {
        return std::string("{\"success\":false,\"error\":\"codec not ready\"}");
    }
    if (!codec->input_reference()) {
        return std::string("{\"success\":false,\"error\":\"hardware reference not enabled\"}");
    }

    auto& service = Application::GetInstance().GetAudioService();

    // 通过 audio_service 的诊断旁路抓取双通道 PCM
    int64_t sum_mic = 0, sum_ref = 0;
    int sample_count = 0;
    bool ok = service.SnoopInputForDiagnose(max_ms, sum_mic, sum_ref, sample_count);

    if (!ok || sample_count == 0) {
        return std::string("{\"success\":false,\"error\":\""
            "no audio samples captured · 设备可能处于 idle 状态 · "
            "请先唤醒设备进入 Listening 或 Speaking 后再诊断\"}");
    }

    float mic_rms = sqrtf((float)sum_mic / sample_count);
    float ref_rms = sqrtf((float)sum_ref / sample_count);
    float ratio = (ref_rms > 1.0f) ? (mic_rms / ref_rms) : 0.0f;
    float ratio_db = (ratio > 0.001f) ? 20.0f * log10f(ratio) : -99.0f;

    const Preset& preset = GetCurrentPreset();

    // 分类判断（参考 189 版本的 TARGET_RATIO=1.05 即 ~+0.4 dB · 这里放宽到 ±2 dB 健康区间）
    const char* judgment;
    const char* recommendation;

    if (ref_rms < 30.0f && mic_rms < 30.0f) {
        // 双通道都几乎静音 → 设备 idle 或刚开麦
        judgment = "音频几乎静音（喇叭未播放/咪头无声）";
        recommendation = "请在喇叭播放期间或对设备说话时再次诊断";
    } else if (ref_rms < 30.0f) {
        // REF 太小 → 设备没在 speaking
        judgment = "REF 信号过弱 · 设备未在 speaking 状态";
        recommendation = "请等设备播放语音回应时诊断（此时回采才有效）";
    } else if (ratio_db > 6.0f) {
        judgment = "MIC 远高于 REF · AEC 消不干净的风险高（喇叭声会漏进 STT）";
        recommendation = (profile_ == kRobust) ?
            "已是最低档 · 检查咪头与喇叭物理隔音"
            : "建议切到 robust 档（mic_gain=15dB）降低残留";
    } else if (ratio_db < -6.0f) {
        judgment = "MIC 远低于 REF · 远讲会被 AEC 吞掉（人声消失）";
        recommendation = (profile_ == kSensitive) ?
            "已是最高档 · 检查麦克风线路或更换灵敏度更高的咪头"
            : "建议切到 sensitive 档（mic_gain=27dB）提升远讲";
    } else if (ratio_db > 2.0f) {
        judgment = "MIC 略高于 REF · 接近 AEC 残留上限";
        recommendation = "如听到回声残留，切到 standard/robust";
    } else if (ratio_db < -2.0f) {
        judgment = "MIC 略低于 REF · 接近灵敏度下限";
        recommendation = "如远讲不灵敏，切到 sensitive";
    } else {
        judgment = "MIC/REF 健康区间（±2 dB）· AEC 与灵敏度平衡良好";
        recommendation = "保持当前档位";
    }

    char buf[640];
    int n = snprintf(buf, sizeof(buf),
        "{\"success\":true,"
        "\"profile\":\"%s\","
        "\"mic_gain_db\":%.0f,"
        "\"mic_rms\":%.0f,"
        "\"ref_rms\":%.0f,"
        "\"ratio_db\":%.1f,"
        "\"samples\":%d,"
        "\"judgment\":\"%s\","
        "\"recommendation\":\"%s\"}",
        preset.name, preset.mic_gain,
        mic_rms, ref_rms, ratio_db, sample_count,
        judgment, recommendation);
    if (n < 0 || n >= (int)sizeof(buf)) {
        return std::string("{\"success\":false,\"error\":\"snprintf overflow\"}");
    }
    ESP_LOGI(TAG, "诊断: %s", buf);
    return std::string(buf);
}
