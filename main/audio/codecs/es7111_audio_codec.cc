#include "es7111_audio_codec.h"
#include "settings.h"

#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <driver/i2c_master.h>
#include <driver/i2s_std.h>
#include <driver/i2s_tdm.h>
#include <driver/gpio.h>
#include <cmath>

#define TAG "Es7111AudioCodec"

namespace {
constexpr int kEs7210SlotCount = 4;
constexpr int kPrimaryMicSlot = 0;
constexpr int kSecondaryMicSlot = 2;
constexpr uint32_t kEs7210AllSlotMask =
    ESP_CODEC_DEV_MAKE_CHANNEL_MASK(0) |
    ESP_CODEC_DEV_MAKE_CHANNEL_MASK(1) |
    ESP_CODEC_DEV_MAKE_CHANNEL_MASK(2) |
    ESP_CODEC_DEV_MAKE_CHANNEL_MASK(3);
constexpr uint32_t kEs7210ActiveMicMask =
    ESP_CODEC_DEV_MAKE_CHANNEL_MASK(kPrimaryMicSlot) |
    ESP_CODEC_DEV_MAKE_CHANNEL_MASK(kSecondaryMicSlot);
}  // namespace

Es7111AudioCodec::Es7111AudioCodec(
    void* i2c_master_handle,
    int input_sample_rate, int output_sample_rate,
    gpio_num_t mclk, gpio_num_t bclk, gpio_num_t ws,
    gpio_num_t dout, gpio_num_t din,
    gpio_num_t pa_pin,
    uint8_t es7210_addr,
    bool input_reference)
    : pa_pin_(pa_pin) {

    duplex_ = true;
    input_reference_ = input_reference;
    input_channels_ = input_reference ? 2 : 1;
    input_sample_rate_ = input_sample_rate;
    output_sample_rate_ = output_sample_rate;

    Settings settings("audio", false);
    input_gain_ = static_cast<float>(settings.GetInt("input_gain", 36));

    // 1. 创建 duplex I2S 通道
    CreateDuplexChannels(mclk, bclk, ws, dout, din);

    // 2. I2S 数据接口（仅 RX，供 ES7210 codec_dev 使用）
    // 不传 tx_handle：ES7111 输出用直接 i2s_channel_write，不由 codec_dev 管理
    // 若传入 tx_handle，esp_codec_dev_open(input) 会 disable→reconfig TX 导致输出中断
    audio_codec_i2s_cfg_t i2s_cfg = {
        .port = I2S_NUM_0,
        .rx_handle = rx_handle_,
        .tx_handle = NULL,
    };
    data_if_ = audio_codec_new_i2s_data(&i2s_cfg);
    assert(data_if_ != NULL);

    // 3. PA GPIO
    gpio_if_ = audio_codec_new_gpio();

    // 4. ES7111 DAC: 不需要任何初始化（纯 I2S，硬件自配置）
    ESP_LOGI(TAG, "ES7111 DAC: no I2C needed (hardware auto-config)");

    // 5. ES7210 ADC: I2C 初始化
    audio_codec_i2c_cfg_t i2c_cfg = {
        .port = (i2c_port_t)1,
        .addr = es7210_addr,
        .bus_handle = i2c_master_handle,
    };
    in_ctrl_if_ = audio_codec_new_i2c_ctrl(&i2c_cfg);
    if (in_ctrl_if_ == NULL) {
        ESP_LOGE(TAG, "ES7210 I2C ctrl init failed");
    }

    es7210_codec_cfg_t es7210_cfg = {};
    es7210_cfg.ctrl_if = in_ctrl_if_;
    es7210_cfg.mic_selected = ES7210_SEL_MIC1 | ES7210_SEL_MIC2 | ES7210_SEL_MIC3 | ES7210_SEL_MIC4;
    in_codec_if_ = es7210_codec_new(&es7210_cfg);
    if (in_codec_if_ == NULL) {
        ESP_LOGE(TAG, "ES7210 codec init failed");
    }

    esp_codec_dev_cfg_t dev_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_IN,
        .codec_if = in_codec_if_,
        .data_if = data_if_,
    };
    input_dev_ = esp_codec_dev_new(&dev_cfg);
    if (input_dev_ == NULL) {
        ESP_LOGE(TAG, "Input device creation failed");
    }

    ESP_LOGI(TAG, "Initialized: ES7111(DAC,direct-I2S) + ES7210(ADC,box-like-RX,gain=%.0fdB)", input_gain_);
}

Es7111AudioCodec::~Es7111AudioCodec() {
    if (input_dev_) {
        esp_codec_dev_close(input_dev_);
        esp_codec_dev_delete(input_dev_);
    }
    if (in_codec_if_) audio_codec_delete_codec_if(in_codec_if_);
    if (in_ctrl_if_) audio_codec_delete_ctrl_if(in_ctrl_if_);
    if (gpio_if_) audio_codec_delete_gpio_if(gpio_if_);
    if (data_if_) audio_codec_delete_data_if(data_if_);
}

void Es7111AudioCodec::CreateDuplexChannels(
    gpio_num_t mclk, gpio_num_t bclk, gpio_num_t ws,
    gpio_num_t dout, gpio_num_t din) {

    assert(input_sample_rate_ == output_sample_rate_);

    i2s_chan_config_t chan_cfg = {
        .id = I2S_NUM_0,
        .role = I2S_ROLE_MASTER,
        .dma_desc_num = AUDIO_CODEC_DMA_DESC_NUM,
        .dma_frame_num = AUDIO_CODEC_DMA_FRAME_NUM,
        .auto_clear_after_cb = true,
        .auto_clear_before_cb = false,
        .intr_priority = 0,
    };
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &tx_handle_, &rx_handle_));

    // TX: Standard 模式 → ES7111 DAC
    // ES7111 规格书定义为标准 I2S，从左声道取单声道数据。
    // 保持 16-bit slot，软件层补成 L/R 双 slot，兼容项目现有 PCM 输出链。
    i2s_std_config_t std_cfg = {
        .clk_cfg = {
            .sample_rate_hz = (uint32_t)output_sample_rate_,
            .clk_src = I2S_CLK_SRC_DEFAULT,
            .ext_clk_freq_hz = 0,
            .mclk_multiple = I2S_MCLK_MULTIPLE_256,
        },
        .slot_cfg = {
            .data_bit_width = I2S_DATA_BIT_WIDTH_16BIT,
            .slot_bit_width = I2S_SLOT_BIT_WIDTH_AUTO,
            .slot_mode = I2S_SLOT_MODE_STEREO,
            .slot_mask = I2S_STD_SLOT_BOTH,
            .ws_width = I2S_DATA_BIT_WIDTH_16BIT,
            .ws_pol = false,
            .bit_shift = true,
            .left_align = true,
            .big_endian = false,
            .bit_order_lsb = false,
        },
        .gpio_cfg = {
            .mclk = mclk,
            .bclk = bclk,
            .ws = ws,
            .dout = dout,
            .din = I2S_GPIO_UNUSED,
            .invert_flags = { false, false, false },
        },
    };

    i2s_tdm_config_t tdm_cfg = {
        .clk_cfg = {
            .sample_rate_hz = (uint32_t)input_sample_rate_,
            .clk_src = I2S_CLK_SRC_DEFAULT,
            .ext_clk_freq_hz = 0,
            .mclk_multiple = I2S_MCLK_MULTIPLE_256,
            .bclk_div = 8,
        },
        .slot_cfg = {
            .data_bit_width = I2S_DATA_BIT_WIDTH_16BIT,
            .slot_bit_width = I2S_SLOT_BIT_WIDTH_AUTO,
            .slot_mode = I2S_SLOT_MODE_STEREO,
            .slot_mask = i2s_tdm_slot_mask_t(I2S_TDM_SLOT0 | I2S_TDM_SLOT1 | I2S_TDM_SLOT2 | I2S_TDM_SLOT3),
            .ws_width = I2S_TDM_AUTO_WS_WIDTH,
            .ws_pol = false,
            .bit_shift = true,
            .left_align = false,
            .big_endian = false,
            .bit_order_lsb = false,
            .skip_mask = false,
            .total_slot = I2S_TDM_AUTO_SLOT_NUM,
        },
        .gpio_cfg = {
            .mclk = mclk,
            .bclk = bclk,
            .ws = ws,
            .dout = I2S_GPIO_UNUSED,
            .din = din,
            .invert_flags = { false, false, false },
        },
    };

    tx_std_cfg_ = std_cfg;  // 保存 TX 配置，EnableInput 后恢复用

    ESP_ERROR_CHECK(i2s_channel_init_std_mode(tx_handle_, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_init_tdm_mode(rx_handle_, &tdm_cfg));
    ESP_ERROR_CHECK(i2s_channel_enable(tx_handle_));
    ESP_ERROR_CHECK(i2s_channel_enable(rx_handle_));
    ESP_LOGI(TAG, "Duplex: TX(STD)→ES7111 RX(TDM)←ES7210 mclk=%d bclk=%d ws=%d dout=%d din=%d",
             mclk, bclk, ws, dout, din);
}

void Es7111AudioCodec::ReinitTxChannel() {
    // esp_codec_dev_open(input) 会通过 I2S 端口内部 duplex 配对找到 TX 并重配，
    // 导致 slot_bit 从 16→32，破坏 ES7111 期望的标准 I2S 时序。
    // 这里分别重配 clock/slot/gpio 恢复正确配置。
    ESP_ERROR_CHECK(i2s_channel_disable(tx_handle_));
    ESP_ERROR_CHECK(i2s_channel_reconfig_std_clock(tx_handle_, &tx_std_cfg_.clk_cfg));
    ESP_ERROR_CHECK(i2s_channel_reconfig_std_slot(tx_handle_, &tx_std_cfg_.slot_cfg));
    ESP_ERROR_CHECK(i2s_channel_reconfig_std_gpio(tx_handle_, &tx_std_cfg_.gpio_cfg));
    ESP_ERROR_CHECK(i2s_channel_enable(tx_handle_));

    // 恢复 PA 状态（I2S 重配可能影响 GPIO 矩阵）
    if (pa_pin_ != GPIO_NUM_NC && output_enabled_) {
        gpio_set_level(pa_pin_, 1);
    }
    ESP_LOGW(TAG, "TX channel re-initialized after input open");
}

void Es7111AudioCodec::SetHeadsetMode(bool headset) {
    std::lock_guard<std::mutex> lock(data_if_mutex_);
    if (headset == headset_mode_) return;
    headset_mode_ = headset;
    ESP_LOGI(TAG, "Headset mode %s (output gain %s)", headset ? "ON" : "OFF",
             headset ? "2x" : "1x");
}

void Es7111AudioCodec::SetOutputVolume(int volume) {
    ESP_LOGW(TAG, "SetOutputVolume: %d→%d pa_pin=%d output_enabled=%d",
             output_volume_, volume, pa_pin_, output_enabled_);
    if (pa_pin_ != GPIO_NUM_NC) {
        gpio_set_level(pa_pin_, volume > 0 ? 1 : 0);
    }
    AudioCodec::SetOutputVolume(volume);
}

void Es7111AudioCodec::SetInputGain(float gain) {
    std::lock_guard<std::mutex> lock(data_if_mutex_);
    input_gain_ = gain;
    ApplyInputGainLocked();
    AudioCodec::SetInputGain(gain);
}

void Es7111AudioCodec::EnableInput(bool enable) {
    ESP_LOGW(TAG, "EnableInput: %d→%d output_enabled=%d", input_enabled_, enable, output_enabled_);
    std::lock_guard<std::mutex> lock(data_if_mutex_);
    if (enable == input_enabled_) return;

    if (enable) {
        esp_codec_dev_sample_info_t fs = {
            .bits_per_sample = 16,
            .channel = kEs7210SlotCount,
            // 和 BoxAudioCodec 的差异点：P31 需要保留完整 4-slot TDM，
            // 后续在 Read() 中从 slot0/slot2 提取或混合双麦数据。
            .channel_mask = kEs7210AllSlotMask,
            .sample_rate = (uint32_t)input_sample_rate_,
            .mclk_multiple = 0,
        };
        ESP_LOGI(TAG, "EnableInput: 4ch TDM, sr=%d gain=%.0f headset=%d",
                 input_sample_rate_, input_gain_, headset_mode_);
        if (input_dev_) {
            ESP_ERROR_CHECK(esp_codec_dev_open(input_dev_, &fs));
            // esp_codec_dev_open 会通过 duplex 配对重配 TX（slot_bit 16→32），
            // 必须立即恢复 TX 的 STD 模式配置，否则 ES7111 DAC 输出时序错误
            ReinitTxChannel();
            ApplyInputGainLocked();
        }
    } else {
        if (input_dev_) {
            ESP_ERROR_CHECK(esp_codec_dev_close(input_dev_));
        }
    }
    AudioCodec::EnableInput(enable);
}

void Es7111AudioCodec::EnableOutput(bool enable) {
    ESP_LOGW(TAG, "EnableOutput: %d→%d pa_pin=%d input_enabled=%d vol=%d",
             output_enabled_, enable, pa_pin_, input_enabled_, output_volume_);
    std::lock_guard<std::mutex> lock(data_if_mutex_);
    if (enable == output_enabled_) return;

    if (pa_pin_ != GPIO_NUM_NC) {
        gpio_set_level(pa_pin_, enable ? 1 : 0);
        ESP_LOGW(TAG, "PA pin %d → %d", pa_pin_, enable ? 1 : 0);
    }
    AudioCodec::EnableOutput(enable);
}

int Es7111AudioCodec::Read(int16_t* dest, int samples) {
    if (!input_enabled_ || !input_dev_) return samples;

    int frames = samples / input_channels_;
    int tdm_samples = frames * kEs7210SlotCount;
    if ((int)tdm_buf_.size() < tdm_samples) {
        tdm_buf_.resize(tdm_samples);
    }
    int ret = esp_codec_dev_read(input_dev_, tdm_buf_.data(), tdm_samples * sizeof(int16_t));
    if (ret != 0) {
        ESP_LOGE(TAG, "esp_codec_dev_read failed: %d", ret);
        return 0;
    }
    if (input_reference_) {
        for (int f = 0; f < frames; f++) {
            dest[f * 2]     = tdm_buf_[f * kEs7210SlotCount + kPrimaryMicSlot];
            dest[f * 2 + 1] = tdm_buf_[f * kEs7210SlotCount + kSecondaryMicSlot];
        }
    } else {
        for (int f = 0; f < frames; f++) {
            int32_t mixed = ((int32_t)tdm_buf_[f * kEs7210SlotCount + kPrimaryMicSlot]
                           + (int32_t)tdm_buf_[f * kEs7210SlotCount + kSecondaryMicSlot]) / 2;
            dest[f] = (int16_t)mixed;
        }
    }
    static int read_count = 0;
    if (++read_count % 200 == 0) {
        int16_t ch_max[kEs7210SlotCount] = {0};
        for (int f = 0; f < frames; f++) {
            for (int c = 0; c < kEs7210SlotCount; c++) {
                int16_t v = tdm_buf_[f * kEs7210SlotCount + c];
                if (abs(v) > abs(ch_max[c])) ch_max[c] = v;
            }
        }
        ESP_LOGD(TAG, "AudioIn(%s): frames=%d ch_peak=[%d,%d,%d,%d]",
                 headset_mode_ ? "headset" : "speaker",
                 frames, ch_max[0], ch_max[1], ch_max[2], ch_max[3]);
    }
    return samples;
}

void Es7111AudioCodec::ApplyInputGainLocked() {
    if (!input_dev_ || !input_enabled_) {
        return;
    }
    ESP_ERROR_CHECK(esp_codec_dev_set_in_channel_gain(input_dev_, kEs7210ActiveMicMask, input_gain_));
}

int Es7111AudioCodec::Write(const int16_t* data, int samples) {
    static int write_count = 0;
    static bool was_enabled = true;
    int16_t peak = 0;
    for (int i = 0; i < samples; i++) {
        if (abs(data[i]) > abs(peak)) peak = data[i];
    }
    // 每 50 次或状态变化时打印
    if (++write_count % 50 == 0 || was_enabled != output_enabled_) {
        ESP_LOGW(TAG, "Write[%d]: samples=%d enabled=%d vol=%d peak=%d headset=%d pa=%d",
                 write_count, samples, output_enabled_, output_volume_, peak, headset_mode_,
                 (pa_pin_ != GPIO_NUM_NC) ? gpio_get_level(pa_pin_) : -1);
        was_enabled = output_enabled_;
    }
    if (output_enabled_) {
        // mono→stereo: ES7111 标准 I2S DAC，单声道放左声道，右声道填 0
        std::vector<int16_t> stereo(samples * 2);
        int32_t vol_factor = static_cast<int32_t>(pow(output_volume_ / 100.0, 2) * 256);
        if (headset_mode_) vol_factor *= 2;
        for (int i = 0; i < samples; i++) {
            int32_t val = (static_cast<int32_t>(data[i]) * vol_factor) >> 8;
            if (val > INT16_MAX) val = INT16_MAX;
            if (val < INT16_MIN) val = INT16_MIN;
            stereo[i * 2] = static_cast<int16_t>(val);
            stereo[i * 2 + 1] = 0;
        }
        size_t bytes_written = 0;
        esp_err_t ret = i2s_channel_write(tx_handle_, stereo.data(), stereo.size() * sizeof(int16_t),
                          &bytes_written, pdMS_TO_TICKS(500));
        if (ret != ESP_OK || bytes_written == 0) {
            ESP_LOGE(TAG, "i2s_channel_write failed: ret=%s bytes=%zu", esp_err_to_name(ret), bytes_written);
        }
        return samples;
    }
    return samples;
}
