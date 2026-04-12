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

    // 2. I2S 数据接口（TX+RX 共享，与 BoxAudioCodec 完全一致）
    // 注意：即使传 tx_handle=NULL，I2S_IF 也会自动从 rx_handle 找到 tx_handle
    // 所以不要试图"隔离"TX — 直接传入让框架统一管理
    audio_codec_i2s_cfg_t i2s_cfg = {
        .port = I2S_NUM_0,
        .rx_handle = rx_handle_,
        .tx_handle = tx_handle_,
    };
    data_if_ = audio_codec_new_i2s_data(&i2s_cfg);
    assert(data_if_ != NULL);

    // 3. PA GPIO
    gpio_if_ = audio_codec_new_gpio();

    // 4. ES7111 DAC 输出设备（codec_if=NULL — 纯 I2S DAC 无需 I2C 控制）
    // 与 BoxAudioCodec 唯一区别：ES8311 有 codec_if，ES7111 没有
    esp_codec_dev_cfg_t out_dev_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_OUT,
        .codec_if = NULL,
        .data_if = data_if_,
    };
    output_dev_ = esp_codec_dev_new(&out_dev_cfg);
    assert(output_dev_ != NULL);
    ESP_LOGI(TAG, "ES7111 DAC: output_dev created (no I2C, codec_dev manages I2S)");

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

    esp_codec_dev_cfg_t in_dev_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_IN,
        .codec_if = in_codec_if_,
        .data_if = data_if_,
    };
    input_dev_ = esp_codec_dev_new(&in_dev_cfg);
    if (input_dev_ == NULL) {
        ESP_LOGE(TAG, "Input device creation failed");
    }

    ESP_LOGI(TAG, "Initialized: ES7111(DAC,codec_dev) + ES7210(ADC,gain=%.0fdB)", input_gain_);
}

Es7111AudioCodec::~Es7111AudioCodec() {
    if (output_dev_) {
        esp_codec_dev_close(output_dev_);
        esp_codec_dev_delete(output_dev_);
    }
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
    // RX: TDM 模式 → ES7210 ADC（4 通道）
    // 与 BoxAudioCodec 完全一致的配置
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

    ESP_ERROR_CHECK(i2s_channel_init_std_mode(tx_handle_, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_init_tdm_mode(rx_handle_, &tdm_cfg));
    ESP_ERROR_CHECK(i2s_channel_enable(tx_handle_));
    ESP_ERROR_CHECK(i2s_channel_enable(rx_handle_));
    ESP_LOGI(TAG, "Duplex: TX(STD)→ES7111 RX(TDM)←ES7210 mclk=%d bclk=%d ws=%d dout=%d din=%d",
             mclk, bclk, ws, dout, din);
}

void Es7111AudioCodec::SetHeadsetMode(bool headset) {
    std::lock_guard<std::mutex> lock(data_if_mutex_);
    if (headset == headset_mode_) return;
    headset_mode_ = headset;
    ESP_LOGI(TAG, "Headset mode %s (output gain %s)", headset ? "ON" : "OFF",
             headset ? "2x" : "1x");
}

void Es7111AudioCodec::SetOutputVolume(int volume) {
    if (pa_pin_ != GPIO_NUM_NC) {
        gpio_set_level(pa_pin_, volume > 0 ? 1 : 0);
    }
    AudioCodec::SetOutputVolume(volume);
}

void Es7111AudioCodec::SetInputGain(float gain) {
    std::lock_guard<std::mutex> lock(data_if_mutex_);
    input_gain_ = gain;
    AudioCodec::SetInputGain(gain);
}

void Es7111AudioCodec::EnableInput(bool enable) {
    std::lock_guard<std::mutex> lock(data_if_mutex_);
    if (enable == input_enabled_) return;

    if (enable) {
        esp_codec_dev_sample_info_t fs = {
            .bits_per_sample = 16,
            .channel = 4,
            .channel_mask = ESP_CODEC_DEV_MAKE_CHANNEL_MASK(0) | ESP_CODEC_DEV_MAKE_CHANNEL_MASK(1)
                          | ESP_CODEC_DEV_MAKE_CHANNEL_MASK(2) | ESP_CODEC_DEV_MAKE_CHANNEL_MASK(3),
            .sample_rate = (uint32_t)input_sample_rate_,
            .mclk_multiple = 0,
        };
        ESP_LOGI(TAG, "EnableInput: 4ch TDM, sr=%d gain=%.0f headset=%d",
                 input_sample_rate_, input_gain_, headset_mode_);
        if (input_dev_) {
            ESP_ERROR_CHECK(esp_codec_dev_open(input_dev_, &fs));
            ESP_ERROR_CHECK(esp_codec_dev_set_in_channel_gain(input_dev_,
                ESP_CODEC_DEV_MAKE_CHANNEL_MASK(0) | ESP_CODEC_DEV_MAKE_CHANNEL_MASK(2), input_gain_));
        }
    } else {
        if (input_dev_) {
            ESP_ERROR_CHECK(esp_codec_dev_close(input_dev_));
        }
    }
    AudioCodec::EnableInput(enable);
}

void Es7111AudioCodec::EnableOutput(bool enable) {
    std::lock_guard<std::mutex> lock(data_if_mutex_);
    if (enable == output_enabled_) return;

    if (enable) {
        // 与 BoxAudioCodec 完全一致：通过 codec_dev 打开，让 I2S_IF 管理格式
        esp_codec_dev_sample_info_t fs = {
            .bits_per_sample = 16,
            .channel = 1,
            .channel_mask = 0,
            .sample_rate = (uint32_t)output_sample_rate_,
            .mclk_multiple = 0,
        };
        if (output_dev_) {
            ESP_ERROR_CHECK(esp_codec_dev_open(output_dev_, &fs));
        }
        if (pa_pin_ != GPIO_NUM_NC) {
            gpio_set_level(pa_pin_, 1);
        }
        ESP_LOGI(TAG, "EnableOutput: codec_dev opened, PA on, sr=%d", output_sample_rate_);
    } else {
        if (pa_pin_ != GPIO_NUM_NC) {
            gpio_set_level(pa_pin_, 0);
        }
        if (output_dev_) {
            ESP_ERROR_CHECK(esp_codec_dev_close(output_dev_));
        }
        ESP_LOGI(TAG, "EnableOutput: codec_dev closed, PA off");
    }
    AudioCodec::EnableOutput(enable);
}

int Es7111AudioCodec::Read(int16_t* dest, int samples) {
    if (!input_enabled_ || !input_dev_) return samples;

    int frames = samples / input_channels_;
    int tdm_samples = frames * 4;
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
            dest[f * 2]     = tdm_buf_[f * 4 + 0];
            dest[f * 2 + 1] = tdm_buf_[f * 4 + 2];
        }
    } else {
        for (int f = 0; f < frames; f++) {
            int32_t mixed = ((int32_t)tdm_buf_[f * 4 + 0] + (int32_t)tdm_buf_[f * 4 + 2]) / 2;
            dest[f] = (int16_t)mixed;
        }
    }
    static int read_count = 0;
    if (++read_count % 200 == 0) {
        int16_t ch_max[4] = {0};
        for (int f = 0; f < frames; f++) {
            for (int c = 0; c < 4; c++) {
                int16_t v = tdm_buf_[f * 4 + c];
                if (abs(v) > abs(ch_max[c])) ch_max[c] = v;
            }
        }
        ESP_LOGD(TAG, "AudioIn(%s): frames=%d ch_peak=[%d,%d,%d,%d]",
                 headset_mode_ ? "headset" : "speaker",
                 frames, ch_max[0], ch_max[1], ch_max[2], ch_max[3]);
    }
    return samples;
}

int Es7111AudioCodec::Write(const int16_t* data, int samples) {
    static int write_count = 0;
    if (++write_count % 50 == 0) {
        int16_t peak = 0;
        for (int i = 0; i < samples; i++) {
            if (abs(data[i]) > abs(peak)) peak = data[i];
        }
        ESP_LOGW(TAG, "Write: samples=%d enabled=%d vol=%d peak=%d headset=%d",
                 samples, output_enabled_, output_volume_, peak, headset_mode_);
    }
    if (output_enabled_ && output_dev_) {
        // 软件音量（ES7111 无硬件音量寄存器，BoxAudioCodec 用 esp_codec_dev_set_out_vol）
        int32_t vol_factor = static_cast<int32_t>(pow(output_volume_ / 100.0, 2) * 256);
        if (headset_mode_) vol_factor *= 2;
        std::vector<int16_t> vol_data(samples);
        for (int i = 0; i < samples; i++) {
            int32_t val = (static_cast<int32_t>(data[i]) * vol_factor) >> 8;
            if (val > INT16_MAX) val = INT16_MAX;
            if (val < INT16_MIN) val = INT16_MIN;
            vol_data[i] = static_cast<int16_t>(val);
        }
        // 通过 codec_dev 写入 — I2S_IF 自动处理 slot_bit 调整和 DMA 格式转换
        ESP_ERROR_CHECK_WITHOUT_ABORT(
            esp_codec_dev_write(output_dev_, vol_data.data(), vol_data.size() * sizeof(int16_t)));
        return samples;
    }
    return samples;
}
