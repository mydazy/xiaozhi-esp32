#ifndef _ACOUSTIC_PROFILE_H_
#define _ACOUSTIC_PROFILE_H_

#include <string>

/**
 * 声学档位（AcousticProfile）· P30 量产精简版
 * 参考 xiaozhi-esp32-189/main/audio/acoustic_calibration.cc 的设计但大幅简化
 *
 * 设计取舍：
 * - 不做全自动迭代校准（189 版本逻辑重 · 量产期不需要）
 * - 只提供 3 档预设 + MIC/REF 双通道实测诊断
 * - mic_gain 都是 ES7210 PGA 的 3 dB 整数倍（避免数字补偿误差）
 * - 不动 AFE 内部参数（VAD/AEC mode 由 ESP-SR 黑盒接管）
 *
 * 三档说明：
 * | 档位      | mic_gain | 适用场景                     | 风险                       |
 * |-----------|----------|------------------------------|----------------------------|
 * | robust    | 15 dB    | 嘈杂环境/外放音乐/误唤醒严重 | 远讲不灵敏                 |
 * | standard  | 21 dB    | 普通家庭使用（默认）         | 平衡设置                   |
 * | sensitive | 27 dB    | 远讲>1.5m/低噪声/咪头隔音差  | AEC 残留增大、易误唤醒     |
 *
 * 升级路径：
 * - 旧版 NVS audio.input_gain=18 的设备升级后默认走 standard=21 dB
 * - 用户主动通过 MCP 切档后写入 audio.ap_profile + audio.input_gain
 *
 * 诊断原理（实音 vs 回采）：
 * - ES7210 4 通道 TDM 中 CH0=MIC（咪头）、CH1=REF（DAC 硬件回采）
 * - 抓 200ms 双通道 PCM 计算 RMS · 比值反映"咪头 vs 喇叭回采"的强度差
 * - ratio_db > 6 dB → MIC 偏高 · AEC 残留风险（消不干净，喇叭声漏到 STT）
 * - ratio_db < -6 dB → MIC 偏低 · 远讲不灵敏（人声被 AEC 吃掉的风险）
 * - -2 ~ +2 dB → 健康区间（接近 189 版本 TARGET_RATIO=1.05/+0.4dB）
 */

class AcousticProfile {
public:
    enum Profile {
        kRobust    = 0,  // 抗噪
        kStandard  = 1,  // 标准（默认）
        kSensitive = 2,  // 灵敏
    };

    struct Preset {
        const char* name;        // "robust" / "standard" / "sensitive"
        const char* desc;        // 一句话场景说明
        float mic_gain;          // ES7210 PGA dB（3 dB 整数倍）
        const char* tradeoff;    // 切换前提示用户的代价
    };

    static AcousticProfile& GetInstance();

    /**
     * 启动时调用：从 NVS 读 ap_profile 并下发 mic_gain
     * 优先级：ap_profile > 旧版 input_gain（兼容升级）> 默认 standard
     */
    void Initialize();

    /**
     * 切档：立即下发到 codec + 持久化 NVS
     * @return false 如果 name 无效或 codec 未就绪
     */
    bool SetProfile(Profile p);
    bool SetProfileByName(const std::string& name);

    Profile GetProfile() const { return profile_; }
    const Preset& GetCurrentPreset() const;

    /**
     * 实音/回采诊断（同步 · 阻塞最多 max_ms + 500ms）
     * 抓双通道 PCM 算 RMS · 返回 JSON 字符串
     * 前提：input_reference=true（P30/P31 三板都满足）+ 设备处于音频活跃状态
     *
     * 触发场景：
     *   1. 设备 Listening / Speaking 期间调用 → 真实场景下的 MIC/REF 比
     *   2. 设备 Idle 时调用 → 仅有噪底数据 · 会提示用户"先唤醒设备再诊断"
     *
     * 返回 JSON 字段：
     *   success/profile/mic_gain_db/mic_rms/ref_rms/ratio_db/samples/judgment/recommendation
     */
    std::string Diagnose(int max_ms = 200);

    static const Preset& GetPreset(Profile p);
    static constexpr int kProfileCount = 3;

private:
    AcousticProfile() = default;
    AcousticProfile(const AcousticProfile&) = delete;
    AcousticProfile& operator=(const AcousticProfile&) = delete;

    Profile profile_ = kStandard;
    bool initialized_ = false;
};

#endif // _ACOUSTIC_PROFILE_H_
