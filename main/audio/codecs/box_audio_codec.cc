#include "box_audio_codec.h"
#include "settings.h"

#include <esp_log.h>
#include <driver/i2c_master.h>
#include <driver/i2s_tdm.h>
#include <cmath>
#include <vector>

#define TAG "BoxAudioCodec"

BoxAudioCodec::BoxAudioCodec(void* i2c_master_handle, int input_sample_rate, int output_sample_rate,
    gpio_num_t mclk, gpio_num_t bclk, gpio_num_t ws, gpio_num_t dout, gpio_num_t din,
    gpio_num_t pa_pin, uint8_t es8311_addr, uint8_t es7210_addr, bool input_reference) {
    duplex_ = true; // 是否双工
    input_reference_ = input_reference; // 是否使用参考输入，实现回声消除
    input_channels_ = input_reference_ ? 2 : 1; // 输入通道数
    input_sample_rate_ = input_sample_rate;
    output_sample_rate_ = output_sample_rate;
    // ───── 麦克风/参考通道增益（NVS 持久化，可通过 codec.SetInputGain/SetRefGain 在线调）─────
    // ES7210 物理档位 0/3/6/9/12/15/18/21/24/27/30/34.5/36/37.5 dB（命中 3dB 倍数无量化损失）
    // 默认仅在 NVS 未写入时生效（首次开机/工厂复位）。校准会立即覆盖：
    //   -26 dBV → 15 dB · -36 dBV → 24 dB · -42 dBV → 30 dB
    constexpr float kDefaultMicGain = 15.0f;   // 默认 -26 dBV mic 配置（量产主物料）
    constexpr float kDefaultRefGain =  6.0f;   // REF 喇叭回采（与 mic 物料无关，固定 6）

    Settings settings("audio", false);  // 只读
    // 用 INT32_MIN sentinel 探测 NVS 是否曾经写入过（区分"默认值" vs "NVS 持久化覆盖值"）
    const bool mic_from_nvs = (settings.GetInt("input_gain", INT32_MIN) != INT32_MIN);
    const bool ref_from_nvs = (settings.GetInt("ref_gain",   INT32_MIN) != INT32_MIN);
    input_gain_ = settings.GetFloat("input_gain", kDefaultMicGain);
    ref_gain_   = settings.GetFloat("ref_gain",   kDefaultRefGain);

    ESP_LOGI(TAG, "增益配置: MIC=%.1fdB(%s) REF=%.1fdB(%s)  [缺省 %.0f/%.0f]",
             input_gain_, mic_from_nvs ? "NVS" : "默认",
             ref_gain_,   ref_from_nvs ? "NVS" : "默认",
             kDefaultMicGain, kDefaultRefGain);

    CreateDuplexChannels(mclk, bclk, ws, dout, din);

    // Do initialize of related interface: data_if, ctrl_if and gpio_if
    audio_codec_i2s_cfg_t i2s_cfg = {
        .port = I2S_NUM_0,
        .rx_handle = rx_handle_,
        .tx_handle = tx_handle_,
    };
    data_if_ = audio_codec_new_i2s_data(&i2s_cfg);
    assert(data_if_ != NULL);

    // Output
    audio_codec_i2c_cfg_t i2c_cfg = {
        .port = (i2c_port_t)1,
        .addr = es8311_addr,
        .bus_handle = i2c_master_handle,
    };
    out_ctrl_if_ = audio_codec_new_i2c_ctrl(&i2c_cfg);
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

    // Input
    i2c_cfg.addr = es7210_addr;
    in_ctrl_if_ = audio_codec_new_i2c_ctrl(&i2c_cfg);
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

    ESP_LOGI(TAG, "BoxAudioDevice initialized");
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

void BoxAudioCodec::SetRefGain(float gain) {
    ref_gain_ = gain;

    if (input_enabled_ && input_reference_) {
        ESP_ERROR_CHECK(esp_codec_dev_set_in_channel_gain(input_dev_, ESP_CODEC_DEV_MAKE_CHANNEL_MASK(1), ref_gain_));
        ESP_LOGI(TAG, "REF: %.1fdB", ref_gain_);
    }

    AudioCodec::SetRefGain(gain);
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
        if (input_reference_) {
            ESP_ERROR_CHECK(esp_codec_dev_set_in_channel_gain(input_dev_, ESP_CODEC_DEV_MAKE_CHANNEL_MASK(1), ref_gain_));
            ESP_LOGI(TAG, "ES7210 寄存器写入: MIC=%.1fdB REF=%.1fdB", input_gain_, ref_gain_);
        } else {
            ESP_LOGI(TAG, "ES7210 寄存器写入: MIC=%.1fdB", input_gain_);
        }
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
        // 先静音，避免开启时的 POP 噪声
        ESP_ERROR_CHECK(esp_codec_dev_set_out_mute(output_dev_, true));

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

        // 延迟后取消静音，让功放稳定
        vTaskDelay(pdMS_TO_TICKS(30));
        ESP_ERROR_CHECK(esp_codec_dev_set_out_mute(output_dev_, false));
    } else {
        // 先静音，避免关闭时的 POP 噪声
        ESP_ERROR_CHECK(esp_codec_dev_set_out_mute(output_dev_, true));
        vTaskDelay(pdMS_TO_TICKS(20));

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
// 固定参数: vol=80, input=12dB, 1kHz amp=24000 播 500ms, 跳 150ms 录 200ms 算 RMS
//   RMS ≥ 3000 → -26 dBV mic → input=15 dB
//   RMS ≥ 900  → -36 dBV mic → input=24 dB
//   RMS <  900 → -42 dBV mic → input=30 dB
void BoxAudioCodec::CalibrateMicOnce() {
    bool was_off_in  = !input_enabled_;
    bool was_off_out = !output_enabled_;
    if (was_off_in)  EnableInput(true);
    if (was_off_out) EnableOutput(true);

    {
        std::lock_guard<std::mutex> lk(data_if_mutex_);
        esp_codec_dev_set_out_vol(output_dev_, 76);   // vol=80 * 0.95
        esp_codec_dev_set_in_channel_gain(input_dev_, ESP_CODEC_DEV_MAKE_CHANNEL_MASK(0), 12.0f);
    }

    std::vector<int16_t> tone(8000);   // 500ms @ 16K
    for (size_t i = 0; i < 8000; i++)
        tone[i] = (int16_t)(24000 * std::sin(2.0 * M_PI * 1000 * i / 16000));
    struct Ctx { BoxAudioCodec* self; std::vector<int16_t>* tone; } ctx{this, &tone};
    xTaskCreate([](void* a) {
        auto* c = (Ctx*)a;
        c->self->Write(c->tone->data(), c->tone->size());
        vTaskDelete(NULL);
    }, "calib", 4096, &ctx, 5, NULL);

    vTaskDelay(pdMS_TO_TICKS(150));
    std::vector<int16_t> rec(3200 * input_channels_);   // 200ms × N通道
    Read(rec.data(), rec.size());

    int64_t sum = 0;
    for (size_t i = 0; i < rec.size(); i += input_channels_)
        sum += (int64_t)rec[i] * rec[i];
    int32_t rms = (int32_t)std::sqrt((double)sum / (rec.size() / input_channels_));

    float gain;
    const char* mic_type;
    if      (rms >= 3000) { gain = 15.0f; mic_type = "-26dBV"; }
    else if (rms >= 900)  { gain = 24.0f; mic_type = "-36dBV"; }
    else                  { gain = 30.0f; mic_type = "-42dBV"; }
    ESP_LOGW(TAG, "MIC校准 RMS=%d → input=%.0fdB (%s)", rms, gain, mic_type);
    SetInputGain(gain);
    Settings("audio", true).SetInt("mic_calib", 1);

    vTaskDelay(pdMS_TO_TICKS(400));
    if (was_off_in)  EnableInput(false);
    if (was_off_out) EnableOutput(false);
}