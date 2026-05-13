#include "box_audio_codec.h"
#include "settings.h"

#include <esp_log.h>
#include <driver/i2c_master.h>
#include <driver/i2s_tdm.h>
#include <cmath>
#include <vector>
#include "mydazy_codec_ctrl_i2c.h"

#define TAG "BoxAudioCodec"

BoxAudioCodec::BoxAudioCodec(void* i2c_worker, int input_sample_rate, int output_sample_rate,
    gpio_num_t mclk, gpio_num_t bclk, gpio_num_t ws, gpio_num_t dout, gpio_num_t din,
    gpio_num_t pa_pin, uint8_t es8311_addr, uint8_t es7210_addr, bool input_reference) {
    duplex_ = true; // 是否双工
    input_reference_ = input_reference; // 是否使用参考输入，实现回声消除
    input_channels_ = input_reference_ ? 2 : 1; // 输入通道数
    input_sample_rate_ = input_sample_rate;
    output_sample_rate_ = output_sample_rate;
    Settings settings("audio", false);  // 只读
    input_gain_ = settings.GetFloat("input_gain", 15.0f);

    CreateDuplexChannels(mclk, bclk, ws, dout, din);

    // Do initialize of related interface: data_if, ctrl_if and gpio_if
    audio_codec_i2s_cfg_t i2s_cfg = {
        .port = I2S_NUM_0,
        .rx_handle = rx_handle_,
        .tx_handle = tx_handle_,
    };
    data_if_ = audio_codec_new_i2s_data(&i2s_cfg);
    assert(data_if_ != NULL);

    // Output (ES8311) — 通过 worker 路由
    mydazy_codec_i2c_cfg_t out_i2c_cfg = {
        .worker       = (i2c_worker_handle_t)i2c_worker,
        .addr         = es8311_addr,
        .scl_speed_hz = 100000,
    };
    out_ctrl_if_ = mydazy_codec_new_i2c_ctrl(&out_i2c_cfg);
    assert(out_ctrl_if_ != NULL);

    gpio_if_ = audio_codec_new_gpio();
    assert(gpio_if_ != NULL);

    es8311_codec_cfg_t es8311_cfg = {};
    es8311_cfg.ctrl_if = out_ctrl_if_;
    es8311_cfg.gpio_if = gpio_if_;
    es8311_cfg.codec_mode = ESP_CODEC_DEV_WORK_MODE_DAC;
    es8311_cfg.pa_pin = pa_pin;
    es8311_cfg.use_mclk = true;
    es8311_cfg.hw_gain.pa_voltage = 5.0;
    es8311_cfg.hw_gain.codec_dac_voltage = 3.3;
    out_codec_if_ = es8311_codec_new(&es8311_cfg);
    assert(out_codec_if_ != NULL);

    esp_codec_dev_cfg_t dev_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_OUT,
        .codec_if = out_codec_if_,
        .data_if = data_if_,
    };
    output_dev_ = esp_codec_dev_new(&dev_cfg);
    assert(output_dev_ != NULL);

    // Input (ES7210) — 通过 worker 路由
    mydazy_codec_i2c_cfg_t in_i2c_cfg = {
        .worker       = (i2c_worker_handle_t)i2c_worker,
        .addr         = es7210_addr,
        .scl_speed_hz = 100000,
    };
    in_ctrl_if_ = mydazy_codec_new_i2c_ctrl(&in_i2c_cfg);
    assert(in_ctrl_if_ != NULL);

    es7210_codec_cfg_t es7210_cfg = {};
    es7210_cfg.ctrl_if = in_ctrl_if_;
    es7210_cfg.mic_selected = ES7210_SEL_MIC1 | ES7210_SEL_MIC2 | ES7210_SEL_MIC3 | ES7210_SEL_MIC4;
    in_codec_if_ = es7210_codec_new(&es7210_cfg);
    assert(in_codec_if_ != NULL);

    dev_cfg.dev_type = ESP_CODEC_DEV_TYPE_IN;
    dev_cfg.codec_if = in_codec_if_;
    input_dev_ = esp_codec_dev_new(&dev_cfg);
    assert(input_dev_ != NULL);

    ESP_LOGI(TAG, "BoxAudioDevice initialized MIC=%.1fdB", input_gain_);
}

BoxAudioCodec::~BoxAudioCodec() {
    ESP_ERROR_CHECK(esp_codec_dev_close(output_dev_));
    esp_codec_dev_delete(output_dev_);
    ESP_ERROR_CHECK(esp_codec_dev_close(input_dev_));
    esp_codec_dev_delete(input_dev_);

    audio_codec_delete_codec_if(in_codec_if_);
    audio_codec_delete_ctrl_if(in_ctrl_if_);
    audio_codec_delete_codec_if(out_codec_if_);
    audio_codec_delete_ctrl_if(out_ctrl_if_);
    audio_codec_delete_gpio_if(gpio_if_);
    audio_codec_delete_data_if(data_if_);
}

void BoxAudioCodec::CreateDuplexChannels(gpio_num_t mclk, gpio_num_t bclk, gpio_num_t ws, gpio_num_t dout, gpio_num_t din) {
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

    i2s_std_config_t std_cfg = {
        .clk_cfg = {
            .sample_rate_hz = (uint32_t)output_sample_rate_,
            .clk_src = I2S_CLK_SRC_DEFAULT,
            .ext_clk_freq_hz = 0,
            .mclk_multiple = I2S_MCLK_MULTIPLE_256
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
            .bit_order_lsb = false
        },
        .gpio_cfg = {
            .mclk = mclk,
            .bclk = bclk,
            .ws = ws,
            .dout = dout,
            .din = I2S_GPIO_UNUSED,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false
            }
        }
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
            .total_slot = I2S_TDM_AUTO_SLOT_NUM
        },
        .gpio_cfg = {
            .mclk = mclk,
            .bclk = bclk,
            .ws = ws,
            .dout = I2S_GPIO_UNUSED,
            .din = din,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false
            }
        }
    };

    ESP_ERROR_CHECK(i2s_channel_init_std_mode(tx_handle_, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_init_tdm_mode(rx_handle_, &tdm_cfg));
    ESP_ERROR_CHECK(i2s_channel_enable(tx_handle_));
    ESP_ERROR_CHECK(i2s_channel_enable(rx_handle_));
    ESP_LOGI(TAG, "Duplex channels created");
}

void BoxAudioCodec::SetOutputVolume(int volume) {
    std::lock_guard<std::mutex> lock(data_if_mutex_);
    int set_volume = (int)(volume * 0.95);
    ESP_ERROR_CHECK(esp_codec_dev_set_out_vol(output_dev_, set_volume));
    AudioCodec::SetOutputVolume(volume);
}

void BoxAudioCodec::SetInputGain(float gain) {
    std::lock_guard<std::mutex> lock(data_if_mutex_);
    input_gain_ = gain;

    if (input_enabled_) {
        ESP_ERROR_CHECK(esp_codec_dev_set_in_channel_gain(input_dev_, ESP_CODEC_DEV_MAKE_CHANNEL_MASK(0), input_gain_));
        ESP_LOGI(TAG, "输入增益已实时更新: %.1fdB", input_gain_);
    }
    AudioCodec::SetInputGain(gain);
}

void BoxAudioCodec::EnableInput(bool enable) {
    std::lock_guard<std::mutex> lock(data_if_mutex_);
    if (enable == input_enabled_) {
        return;
    }
    if (enable) {
        esp_codec_dev_sample_info_t fs = {
            .bits_per_sample = 16,
            .channel = 4,
            .channel_mask = ESP_CODEC_DEV_MAKE_CHANNEL_MASK(0),
            .sample_rate = (uint32_t)output_sample_rate_,
            .mclk_multiple = 0,
        };
        if (input_reference_) {
            fs.channel_mask |= ESP_CODEC_DEV_MAKE_CHANNEL_MASK(1);
        }
        ESP_ERROR_CHECK(esp_codec_dev_open(input_dev_, &fs));
        ESP_ERROR_CHECK(esp_codec_dev_set_in_channel_gain(input_dev_, ESP_CODEC_DEV_MAKE_CHANNEL_MASK(0), input_gain_));
    } else {
        ESP_ERROR_CHECK(esp_codec_dev_close(input_dev_));
    }
    AudioCodec::EnableInput(enable);
}

void BoxAudioCodec::EnableOutput(bool enable) {
    std::lock_guard<std::mutex> lock(data_if_mutex_);
    if (enable == output_enabled_) {
        return;
    }
    if (enable) {
        // Play 16bit 1 channel
        esp_codec_dev_sample_info_t fs = {
            .bits_per_sample = 16,
            .channel = 1,
            .channel_mask = 0,
            .sample_rate = (uint32_t)output_sample_rate_,
            .mclk_multiple = 0,
        };
        ESP_ERROR_CHECK(esp_codec_dev_open(output_dev_, &fs));
        ESP_ERROR_CHECK(esp_codec_dev_set_out_vol(output_dev_, output_volume_));
    } else {
        ESP_ERROR_CHECK(esp_codec_dev_close(output_dev_));
    }
    AudioCodec::EnableOutput(enable);
}

int BoxAudioCodec::Read(int16_t* dest, int samples) {
    if (input_enabled_) {
        ESP_ERROR_CHECK_WITHOUT_ABORT(esp_codec_dev_read(input_dev_, (void*)dest, samples * sizeof(int16_t)));
    }
    return samples;
}

int BoxAudioCodec::Write(const int16_t* data, int samples) {
    if (output_enabled_) {
        ESP_ERROR_CHECK_WITHOUT_ABORT(esp_codec_dev_write(output_dev_, (void*)data, samples * sizeof(int16_t)));
    }
    return samples;
}

// MIC 灵敏度识别（出厂烧录后首次开机一次性）
//   基准: vol=80, input=15dB, 1kHz amp=24000 播 500ms, 跳 150ms 录 200ms
//   公式: input = 15 + 20*log10(5000/RMS), 量化 3dB（>31.5 跳 36 避 33 驱动 bug）
void BoxAudioCodec::CalibrateMicOnce() {
    ESP_LOGW(TAG, "MIC校准开始 (vol=80 input=15dB tone=1kHz/500ms)");

    if (!input_enabled_)  EnableInput(true);
    if (!output_enabled_) EnableOutput(true);
    {
        std::lock_guard<std::mutex> lk(data_if_mutex_);
        esp_codec_dev_set_out_vol(output_dev_, 76);
        esp_codec_dev_set_in_channel_gain(input_dev_, ESP_CODEC_DEV_MAKE_CHANNEL_MASK(0), 15.0f);
    }

    std::vector<int16_t> tone(8000);
    for (size_t i = 0; i < 8000; i++)
        tone[i] = (int16_t)(24000 * std::sin(2.0 * M_PI * 1000 * i / 16000));
    struct Ctx { BoxAudioCodec* self; std::vector<int16_t>* tone; } ctx{this, &tone};

    auto measure = [&]() -> int32_t {
        xTaskCreate([](void* a) {
            auto* c = (Ctx*)a;
            c->self->Write(c->tone->data(), c->tone->size());
            vTaskDelete(NULL);
        }, "calib", 4096, &ctx, 5, NULL);
        vTaskDelay(pdMS_TO_TICKS(150));
        std::vector<int16_t> rec(3200 * input_channels_);
        Read(rec.data(), rec.size());
        int64_t sum = 0;
        for (size_t i = 0; i < rec.size(); i += input_channels_)
            sum += (int64_t)rec[i] * rec[i];
        vTaskDelay(pdMS_TO_TICKS(400));
        return (int32_t)std::sqrt((double)sum / (rec.size() / input_channels_));
    };

    // 第一轮：测基准 RMS，公式反推 input + mic_type + aec
    int32_t rms = std::max(measure(), (int32_t)1);
    float in_raw = 15.0f + 20.0f * std::log10(5000.0f / rms);
    float input_gain = std::max(0.0f, std::min(36.0f,
        (in_raw <= 31.5f) ? std::round(in_raw / 3.0f) * 3.0f : 36.0f));
    int mic_type = std::max(22, std::min(50,
        (int)std::round((36.0f + 20.0f * std::log10(1758.0f / rms)) / 2.0f) * 2));
    // aec 三段式: 高灵敏(≤30)减小避爆顶 / 低灵敏(≥42)加大补 ASR / 中等基线
    float aec_gain = (mic_type <= 30) ? 3.0f : (mic_type >= 42) ? 9.0f : 6.0f;
    int32_t rms_expected = (int32_t)(rms * std::pow(10.0, (input_gain - 15.0) / 20.0));
    ESP_LOGW(TAG, "第一轮 RMS=%d → input=%.0fdB mic=-%ddBV aec=%.0fdB 预期=%d",
             rms, input_gain, mic_type, aec_gain, rms_expected);

    SetInputGain(input_gain);
    // SetRefGain(...);  // 测试模式: REF 保持开机初值
    SetAecGain(aec_gain);
    Settings("audio", true).SetInt("mic_type", mic_type);
    vTaskDelay(pdMS_TO_TICKS(100));

    // 第二轮：用判定后 input 验证等效响度
    int32_t rms_after = measure();
    int diff = (int)(100 * std::abs((double)(rms_after - rms_expected) / rms_expected));
    ESP_LOGW(TAG, "第二轮 RMS=%d (预期 %d 偏差 %d%%) %s",
             rms_after, rms_expected, diff, diff < 20 ? "✅" : "⚠");
}