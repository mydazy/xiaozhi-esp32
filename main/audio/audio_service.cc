#include "audio_service.h"
#include "settings.h"

#include <esp_log.h>
#include <cstring>
#include <cmath>

#define RATE_CVT_CFG(_src_rate, _dest_rate, _channel)        \
    (esp_ae_rate_cvt_cfg_t)                                  \
    {                                                        \
        .src_rate        = (uint32_t)(_src_rate),            \
        .dest_rate       = (uint32_t)(_dest_rate),           \
        .channel         = (uint8_t)(_channel),              \
        .bits_per_sample = ESP_AUDIO_BIT16,                  \
        .complexity      = 2,                                \
        .perf_type       = ESP_AE_RATE_CVT_PERF_TYPE_SPEED,  \
    }

#define OPUS_DEC_CFG(_sample_rate, _frame_duration_ms)                                                    \
    (esp_opus_dec_cfg_t)                                                                                  \
    {                                                                                                     \
        .sample_rate    = (uint32_t)(_sample_rate),                                                       \
        .channel        = ESP_AUDIO_MONO,                                                                 \
        .frame_duration = (esp_opus_dec_frame_duration_t)AS_OPUS_GET_FRAME_DRU_ENUM(_frame_duration_ms),  \
        .self_delimited = false,                                                                          \
    }

#include "processors/afe_audio_processor.h"
#include "wake_words/afe_wake_word.h"
#include "wake_words/custom_wake_word.h"

#define TAG "AudioService"

// OPUS 帧长 · 默认 60ms · 百度协议时 SetFrameDuration(20) 切换
int g_opus_frame_duration_ms = 60;

AudioService::AudioService() {
    event_group_ = xEventGroupCreate();
}

AudioService::~AudioService() {
    if (event_group_ != nullptr) {
        vEventGroupDelete(event_group_);
    }
    if (opus_encoder_ != nullptr) {
        esp_opus_enc_close(opus_encoder_);
    }
    if (opus_decoder_ != nullptr) {
        esp_opus_dec_close(opus_decoder_);
    }
    if (input_resampler_ != nullptr) {
        esp_ae_rate_cvt_close(input_resampler_);
    }
    if (output_resampler_ != nullptr) {
        esp_ae_rate_cvt_close(output_resampler_);
    }
}

void AudioService::Initialize(AudioCodec* codec) {
    codec_ = codec;
    codec_->Start();

    esp_opus_dec_cfg_t opus_dec_cfg = OPUS_DEC_CFG(codec->output_sample_rate(), OPUS_FRAME_DURATION_MS);
    auto ret = esp_opus_dec_open(&opus_dec_cfg, sizeof(esp_opus_dec_cfg_t), &opus_decoder_);
    if (opus_decoder_ == nullptr) {
        ESP_LOGE(TAG, "Failed to create audio decoder, error code: %d", ret);
    } else {
        decoder_sample_rate_ = codec->output_sample_rate();
        decoder_duration_ms_ = OPUS_FRAME_DURATION_MS;
        decoder_frame_size_ = decoder_sample_rate_ / 1000 * OPUS_FRAME_DURATION_MS;
    }
    esp_opus_enc_config_t opus_enc_cfg = AS_OPUS_ENC_CONFIG();
    ret = esp_opus_enc_open(&opus_enc_cfg, sizeof(esp_opus_enc_config_t), &opus_encoder_);
    if (opus_encoder_ == nullptr) {
        ESP_LOGE(TAG, "Failed to create audio encoder, error code: %d", ret);
    } else {
        encoder_sample_rate_ = 16000;
        encoder_duration_ms_ = OPUS_FRAME_DURATION_MS;
        esp_opus_enc_get_frame_size(opus_encoder_, &encoder_frame_size_, &encoder_outbuf_size_);
        encoder_frame_size_ = encoder_frame_size_ / sizeof(int16_t);
    }

    if (codec->input_sample_rate() != 16000) {
        esp_ae_rate_cvt_cfg_t input_resampler_cfg = RATE_CVT_CFG(
            codec->input_sample_rate(), ESP_AUDIO_SAMPLE_RATE_16K, codec->input_channels());
        auto resampler_ret = esp_ae_rate_cvt_open(&input_resampler_cfg, &input_resampler_);
        if (input_resampler_ == nullptr) {
            ESP_LOGE(TAG, "Failed to create input resampler, error code: %d", resampler_ret);
        }
    }

#if CONFIG_USE_AUDIO_PROCESSOR
    audio_processor_ = std::make_unique<AfeAudioProcessor>();
#else
    audio_processor_ = std::make_unique<NoAudioProcessor>();
#endif

    audio_processor_->OnOutput([this](std::vector<int16_t>&& data) {
        const float g = codec_ ? codec_->aec_gain_linear() : 1.0f;

        int64_t sum_in = 0;
        for (int16_t s : data) sum_in += (int64_t)s * s;
        const int32_t avg_in = data.empty() ? 0 : (int32_t)(sum_in / (int64_t)data.size());

        if (g > 1.01f && avg_in >= kNoiseGateRmsSq) {
            for (int16_t& s : data) {
                int32_t v = (int32_t)(s * g);
                s = v > 32767 ? 32767 : (v < -32768 ? -32768 : (int16_t)v);
            }
        }
        PushTaskToEncodeQueue(kAudioTaskTypeEncodeToSendQueue, std::move(data));
    });

    audio_processor_->OnVadStateChange([this](bool speaking) {
        voice_detected_ = speaking;
        if (callbacks_.on_vad_change) {
            callbacks_.on_vad_change(speaking);
        }
    });

    esp_timer_create_args_t audio_power_timer_args = {
        .callback = [](void* arg) {
            AudioService* audio_service = (AudioService*)arg;
            audio_service->CheckAndUpdateAudioPowerState();
        },
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "audio_power_timer",
        .skip_unhandled_events = true,
    };
    esp_timer_create(&audio_power_timer_args, &audio_power_timer_);
}

void AudioService::Start() {
    service_stopped_ = false;
    xEventGroupClearBits(event_group_, AS_EVENT_AUDIO_TESTING_RUNNING | AS_EVENT_WAKE_WORD_RUNNING | AS_EVENT_AUDIO_PROCESSOR_RUNNING);

    esp_timer_start_periodic(audio_power_timer_, 1000000);

#if CONFIG_USE_AUDIO_PROCESSOR
    /* Start the audio input task */
    xTaskCreatePinnedToCore([](void* arg) {
        AudioService* audio_service = (AudioService*)arg;
        audio_service->AudioInputTask();
        vTaskDelete(NULL);
    }, "audio_input", 2048 * 3, this, 10, &audio_input_task_handle_, 1);

    xTaskCreatePinnedToCore([](void* arg) {
        AudioService* audio_service = (AudioService*)arg;
        audio_service->AudioOutputTask();
        vTaskDelete(NULL);
    }, "audio_output", 2048 * 2, this, 10, &audio_output_task_handle_, 0);
#else
    /* Start the audio input task */
    xTaskCreatePinnedToCore([](void* arg) {
        AudioService* audio_service = (AudioService*)arg;
        audio_service->AudioInputTask();
        vTaskDelete(NULL);
    }, "audio_input", 2048 * 2, this, 10, &audio_input_task_handle_, 1);

    /* Start the audio output task */
    xTaskCreatePinnedToCore([](void* arg) {
        AudioService* audio_service = (AudioService*)arg;
        audio_service->AudioOutputTask();
        vTaskDelete(NULL);
    }, "audio_output", 2048, this, 10, &audio_output_task_handle_, 0);
#endif

    /* Start the opus codec task */
    xTaskCreatePinnedToCoreWithCaps([](void* arg) {
        AudioService* audio_service = (AudioService*)arg;
        audio_service->OpusCodecTask();
        vTaskDeleteWithCaps(NULL);
    }, "opus_codec", 2048 * 12, this, 7, &opus_codec_task_handle_, 1, MALLOC_CAP_SPIRAM);
}

void AudioService::Stop() {
    esp_timer_stop(audio_power_timer_);
    service_stopped_ = true;
    xEventGroupSetBits(event_group_, AS_EVENT_AUDIO_TESTING_RUNNING |
        AS_EVENT_WAKE_WORD_RUNNING |
        AS_EVENT_AUDIO_PROCESSOR_RUNNING);

    {
        std::lock_guard<std::mutex> lock(audio_queue_mutex_);
        audio_encode_queue_.clear();
        audio_decode_queue_.clear();
        audio_playback_queue_.clear();
        audio_testing_queue_.clear();
        audio_queue_cv_.notify_all();
    }

    auto join_task = [](TaskHandle_t& h, const char* name) {
        if (!h) return;
        TickType_t start = xTaskGetTickCount();
        while (eTaskGetState(h) != eDeleted) {
            if ((xTaskGetTickCount() - start) > pdMS_TO_TICKS(500)) {
                ESP_LOGE(TAG, "Stop: %s task exit timeout · handle leaked", name);
                return;
            }
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        h = nullptr;
    };
    join_task(audio_input_task_handle_,  "audio_input");
    join_task(audio_output_task_handle_, "audio_output");
    join_task(opus_codec_task_handle_,   "opus_codec");
}

bool AudioService::ReadAudioData(std::vector<int16_t>& data, int sample_rate, int samples) {
    if (!codec_->input_enabled()) {
        esp_timer_stop(audio_power_timer_);
        esp_timer_start_periodic(audio_power_timer_, AUDIO_POWER_CHECK_INTERVAL_MS * 1000);
        codec_->EnableInput(true);
    }

    if (codec_->input_sample_rate() != sample_rate) {
        data.resize(samples * codec_->input_sample_rate() / sample_rate * codec_->input_channels());
        if (!codec_->InputData(data)) {
            return false;
        }
        if (input_resampler_ != nullptr) {
            std::lock_guard<std::mutex> lock(input_resampler_mutex_);
            uint32_t in_sample_num = data.size() / codec_->input_channels();
            uint32_t output_samples = 0;
            esp_ae_rate_cvt_get_max_out_sample_num(input_resampler_, in_sample_num, &output_samples);
            auto resampled = std::vector<int16_t>(output_samples * codec_->input_channels());
            uint32_t actual_output = output_samples;
            esp_ae_rate_cvt_process(input_resampler_, (esp_ae_sample_t)data.data(), in_sample_num,
                                   (esp_ae_sample_t)resampled.data(), &actual_output);
            resampled.resize(actual_output * codec_->input_channels());
            data = std::move(resampled);
        }
    } else {
        data.resize(samples * codec_->input_channels());
        if (!codec_->InputData(data)) {
            return false;
        }
    }

    /* Update the last input time */
    last_input_time_ = std::chrono::steady_clock::now();
    debug_statistics_.input_count++;

#if CONFIG_USE_AUDIO_DEBUGGER
    // 音频调试：发送原始音频数据
    if (audio_debugger_ == nullptr) {
        audio_debugger_ = std::make_unique<AudioDebugger>();
    }
    audio_debugger_->Feed(data);
#endif

    return true;
}

void AudioService::AudioInputTask() {
    while (true) {
        EventBits_t bits = xEventGroupWaitBits(event_group_, AS_EVENT_AUDIO_TESTING_RUNNING |
            AS_EVENT_WAKE_WORD_RUNNING | AS_EVENT_AUDIO_PROCESSOR_RUNNING,
            pdFALSE, pdFALSE, portMAX_DELAY);

        if (service_stopped_) {
            break;
        }
        if (audio_input_need_warmup_) {
            audio_input_need_warmup_ = false;
            vTaskDelay(pdMS_TO_TICKS(120));
            continue;
        }

        /* Used for audio testing in NetworkConfiguring mode by clicking the BOOT button */
        if (bits & AS_EVENT_AUDIO_TESTING_RUNNING) {
            if (audio_testing_queue_.size() >= AUDIO_TESTING_MAX_DURATION_MS / OPUS_FRAME_DURATION_MS) {
                ESP_LOGW(TAG, "Audio testing queue is full, stopping audio testing");
                EnableAudioTesting(false);
                continue;
            }
            std::vector<int16_t> data;
            int samples = OPUS_FRAME_DURATION_MS * 16000 / 1000;
            if (ReadAudioData(data, 16000, samples)) {
                // If input channels is 2, we need to fetch the left channel data
                if (codec_->input_channels() == 2) {
                    auto mono_data = std::vector<int16_t>(data.size() / 2);
                    for (size_t i = 0, j = 0; i < mono_data.size(); ++i, j += 2) {
                        mono_data[i] = data[j];
                    }
                    data = std::move(mono_data);
                }
                PushTaskToEncodeQueue(kAudioTaskTypeEncodeToTestingQueue, std::move(data));
                continue;
            }
        }

        /* Feed the wake word and/or audio processor */
        if (bits & (AS_EVENT_WAKE_WORD_RUNNING | AS_EVENT_AUDIO_PROCESSOR_RUNNING)) {
            int samples = 160; // 10ms
            std::vector<int16_t> data;
            if (ReadAudioData(data, 16000, samples)) {
                if (bits & AS_EVENT_WAKE_WORD_RUNNING) {
                    wake_word_->Feed(data);
                }
                if (bits & AS_EVENT_AUDIO_PROCESSOR_RUNNING) {
                    audio_processor_->Feed(std::move(data));
                }
                continue;
            }
        }

        // Read timeout/error should not terminate the input task.
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    ESP_LOGW(TAG, "Audio input task stopped");
}

void AudioService::AudioOutputTask() {
    while (true) {
        std::unique_lock<std::mutex> lock(audio_queue_mutex_);
        audio_queue_cv_.wait(lock, [this]() { return !audio_playback_queue_.empty() || service_stopped_; });
        if (service_stopped_) {
            break;
        }

        auto task = std::move(audio_playback_queue_.front());
        audio_playback_queue_.pop_front();
        audio_queue_cv_.notify_all();
        lock.unlock();

        if (!codec_->output_enabled()) {
            esp_timer_stop(audio_power_timer_);
            esp_timer_start_periodic(audio_power_timer_, AUDIO_POWER_CHECK_INTERVAL_MS * 1000);
            codec_->EnableOutput(true);
        }

        codec_->OutputData(task->pcm);

        /* Update the last output time */
        last_output_time_ = std::chrono::steady_clock::now();
        debug_statistics_.playback_count++;

#if CONFIG_USE_SERVER_AEC
        /* Record the timestamp for server AEC */
        if (task->timestamp > 0) {
            lock.lock();
            timestamp_queue_.push_back(task->timestamp);
        }
#endif
    }

    ESP_LOGW(TAG, "Audio output task stopped");
}

void AudioService::OpusCodecTask() {
    while (true) {
        std::unique_lock<std::mutex> lock(audio_queue_mutex_);
        auto need_underrun_compensation = [this]() {
            if (!audio_decode_queue_.empty()) return false;
            if (audio_playback_queue_.size() >= kPlaybackLowWatermark) return false;
            if (consecutive_underrun_plc_ >= kMaxUnderrunPlcFrames + 5) return false;
            if (last_decode_push_time_.time_since_epoch().count() == 0) return false;
            auto since = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - last_decode_push_time_).count();
            return since < kRecentDecodePushWindowMs;
        };
        audio_queue_cv_.wait(lock, [this, &need_underrun_compensation]() {
            return service_stopped_ ||
                (!audio_encode_queue_.empty() && audio_send_queue_.size() < MAX_SEND_PACKETS_IN_QUEUE) ||
                (!audio_decode_queue_.empty() && audio_playback_queue_.size() < MAX_PLAYBACK_TASKS_IN_QUEUE) ||
                need_underrun_compensation();
        });
        if (service_stopped_) {
            break;
        }

        /* Decode the audio from decode queue */
        if (!audio_decode_queue_.empty() && audio_playback_queue_.size() < MAX_PLAYBACK_TASKS_IN_QUEUE) {
            auto packet = std::move(audio_decode_queue_.front());
            audio_decode_queue_.pop_front();
            audio_queue_cv_.notify_all();
            lock.unlock();

            auto task = std::make_unique<AudioTask>();
            task->type = kAudioTaskTypeDecodeToPlaybackQueue;
            task->timestamp = packet->timestamp;

            SetDecodeSampleRate(packet->sample_rate, packet->frame_duration);
            if (opus_decoder_ != nullptr) {
                task->pcm.resize(decoder_frame_size_);
                esp_audio_dec_in_raw_t raw = {
                    .buffer = (uint8_t *)(packet->payload.data()),
                    .len = (uint32_t)(packet->payload.size()),
                    .consumed = 0,
                    .frame_recover = ESP_AUDIO_DEC_RECOVERY_NONE,
                };
                esp_audio_dec_out_frame_t out_frame = {
                    .buffer = (uint8_t *)(task->pcm.data()),
                    .len = (uint32_t)(task->pcm.size() * sizeof(int16_t)),
                    .decoded_size = 0,
                };
                esp_audio_dec_info_t dec_info = {};
                std::unique_lock<std::mutex> decoder_lock(decoder_mutex_);
                auto ret = esp_opus_dec_decode(opus_decoder_, &raw, &out_frame, &dec_info);
                bool used_plc = false;
                if (ret != ESP_AUDIO_ERR_OK) {
                    out_frame.decoded_size = 0;
                    raw.buffer = nullptr;
                    raw.len = 0;
                    raw.consumed = 0;
                    raw.frame_recover = ESP_AUDIO_DEC_RECOVERY_PLC;
                    ret = esp_opus_dec_decode(opus_decoder_, &raw, &out_frame, &dec_info);
                    used_plc = true;
                }
                decoder_lock.unlock();
                if (ret == ESP_AUDIO_ERR_OK && out_frame.decoded_size > 0) {
                    task->pcm.resize(out_frame.decoded_size / sizeof(int16_t));
                    if (decoder_sample_rate_ != codec_->output_sample_rate() && output_resampler_ != nullptr) {
                        uint32_t target_size = 0;
                        esp_ae_rate_cvt_get_max_out_sample_num(output_resampler_, task->pcm.size(), &target_size);
                        std::vector<int16_t> resampled(target_size);
                        uint32_t actual_output = target_size;
                        esp_ae_rate_cvt_process(output_resampler_, (esp_ae_sample_t)task->pcm.data(), task->pcm.size(),
                                                (esp_ae_sample_t)resampled.data(), &actual_output);
                        resampled.resize(actual_output);
                        task->pcm = std::move(resampled);
                    }
                    lock.lock();
                    audio_playback_queue_.push_back(std::move(task));
                    audio_queue_cv_.notify_all();
                    debug_statistics_.decode_count++;
                    consecutive_underrun_plc_ = 0;
                    if (used_plc) {
                        debug_statistics_.decode_plc_count++;
                        if (debug_statistics_.decode_plc_count % 20 == 1) {
                            ESP_LOGW(TAG, "OPUS PLC synthesized (total=%lu)",
                                     (unsigned long)debug_statistics_.decode_plc_count);
                        }
                    }
                } else {
                    debug_statistics_.decode_drop_count++;
                    if (debug_statistics_.decode_drop_count % 20 == 1) {
                        ESP_LOGW(TAG, "Decode failed even after PLC (ret=%d, total drop=%lu)",
                                 ret, (unsigned long)debug_statistics_.decode_drop_count);
                    }
                    lock.lock();
                }
            } else {
                ESP_LOGE(TAG, "Audio decoder is not configured");
                lock.lock();
            }
            debug_statistics_.decode_count++;
        }
        else if (need_underrun_compensation() && decoder_frame_size_ > 0) {
            auto task = std::make_unique<AudioTask>();
            task->type = kAudioTaskTypeDecodeToPlaybackQueue;
            task->timestamp = 0;
            task->pcm.assign(decoder_frame_size_, 0);
            bool used_opus_plc = false;

            if (consecutive_underrun_plc_ < kMaxUnderrunPlcFrames && opus_decoder_ != nullptr) {
                static uint8_t s_plc_dummy_byte = 0;
                lock.unlock();
                esp_audio_dec_in_raw_t plc_raw = { &s_plc_dummy_byte, 0, 0, ESP_AUDIO_DEC_RECOVERY_PLC };
                esp_audio_dec_out_frame_t plc_frame = {
                    (uint8_t *)task->pcm.data(),
                    (uint32_t)(task->pcm.size() * sizeof(int16_t)),
                    0,
                };
                esp_audio_dec_info_t dec_info = {};
                std::unique_lock<std::mutex> dec_lock(decoder_mutex_);
                auto ret = esp_opus_dec_decode(opus_decoder_, &plc_raw, &plc_frame, &dec_info);
                dec_lock.unlock();
                lock.lock();
                if (ret == ESP_AUDIO_ERR_OK && plc_frame.decoded_size > 0) {
                    task->pcm.resize(plc_frame.decoded_size / sizeof(int16_t));
                    used_opus_plc = true;
                    if (!task->pcm.empty()) last_plc_tail_sample_ = task->pcm.back();
                } else {
                    consecutive_underrun_plc_ = kMaxUnderrunPlcFrames;
                    task->pcm.assign(decoder_frame_size_, 0);
                }
            }
            if (!used_opus_plc) {
                if (consecutive_underrun_plc_ == kMaxUnderrunPlcFrames && last_plc_tail_sample_ != 0) {
                    size_t n = task->pcm.size();
                    int32_t denom = (n > 1) ? (int32_t)(n - 1) : 1;
                    for (size_t i = 0; i < n; ++i) {
                        int32_t scale = (int32_t)(n - 1 - i);
                        task->pcm[i] = (int16_t)((int32_t)last_plc_tail_sample_ * scale / denom);
                    }
                    last_plc_tail_sample_ = 0;
                }
            }

            if (decoder_sample_rate_ != codec_->output_sample_rate() && output_resampler_ != nullptr) {
                uint32_t target_size = 0;
                esp_ae_rate_cvt_get_max_out_sample_num(output_resampler_, task->pcm.size(), &target_size);
                std::vector<int16_t> resampled(target_size);
                uint32_t actual_output = target_size;
                esp_ae_rate_cvt_process(output_resampler_, (esp_ae_sample_t)task->pcm.data(), task->pcm.size(),
                                        (esp_ae_sample_t)resampled.data(), &actual_output);
                resampled.resize(actual_output);
                task->pcm = std::move(resampled);
            }
            audio_playback_queue_.push_back(std::move(task));
            audio_queue_cv_.notify_all();
            consecutive_underrun_plc_++;
            if (used_opus_plc) {
                debug_statistics_.underrun_plc_count++;
            } else {
                debug_statistics_.underrun_silence_count++;
            }
            uint32_t total = debug_statistics_.underrun_plc_count + debug_statistics_.underrun_silence_count;
            if (total % 50 == 1) {
                ESP_LOGW(TAG, "Underrun compensation: plc=%lu silence=%lu (consec=%d)",
                         (unsigned long)debug_statistics_.underrun_plc_count,
                         (unsigned long)debug_statistics_.underrun_silence_count,
                         consecutive_underrun_plc_);
            }
        }
        /* Encode the audio to send queue */
        if (!audio_encode_queue_.empty() && audio_send_queue_.size() < MAX_SEND_PACKETS_IN_QUEUE) {
            auto task = std::move(audio_encode_queue_.front());
            audio_encode_queue_.pop_front();
            audio_queue_cv_.notify_all();
            lock.unlock();

            auto packet = std::make_unique<AudioStreamPacket>();
            packet->frame_duration = OPUS_FRAME_DURATION_MS;
            packet->sample_rate = 16000;
            packet->timestamp = task->timestamp;

            std::lock_guard<std::mutex> enc_lock(encoder_mutex_);
            if (opus_encoder_ != nullptr && task->pcm.size() == encoder_frame_size_) {
                std::vector<uint8_t> buf(encoder_outbuf_size_);
                esp_audio_enc_in_frame_t in = {
                    .buffer = (uint8_t *)(task->pcm.data()),
                    .len = (uint32_t)(encoder_frame_size_ * sizeof(int16_t)),
                };
                esp_audio_enc_out_frame_t out = {
                    .buffer = buf.data(),
                    .len = (uint32_t)encoder_outbuf_size_,
                    .encoded_bytes = 0,
                };
                auto ret = esp_opus_enc_process(opus_encoder_, &in, &out);
                if (ret == ESP_AUDIO_ERR_OK) {
                    packet->payload.assign(buf.data(), buf.data() + out.encoded_bytes);

                    if (task->type == kAudioTaskTypeEncodeToSendQueue) {
                        {
                            std::lock_guard<std::mutex> lock2(audio_queue_mutex_);
                            audio_send_queue_.push_back(std::move(packet));
                        }
                        if (callbacks_.on_send_queue_available) {
                            callbacks_.on_send_queue_available();
                        }
                    } else if (task->type == kAudioTaskTypeEncodeToTestingQueue) {
                        std::lock_guard<std::mutex> lock2(audio_queue_mutex_);
                        audio_testing_queue_.push_back(std::move(packet));
                    }
                    debug_statistics_.encode_count++;
                } else {
                    ESP_LOGE(TAG, "Failed to encode audio, error code: %d", ret);
                }
            } else {
                ESP_LOGE(TAG, "Failed to encode audio: encoder not configured or invalid frame size (got %u, expected %u)",
                         task->pcm.size(), encoder_frame_size_);
            }
            lock.lock();
        }
    }

    ESP_LOGW(TAG, "Opus codec task stopped");
}

void AudioService::SetDecodeSampleRate(int sample_rate, int frame_duration) {
    if (decoder_sample_rate_ == sample_rate && decoder_duration_ms_ == frame_duration) {
        return;
    }
    std::unique_lock<std::mutex> decoder_lock(decoder_mutex_);
    if (opus_decoder_ != nullptr) {
        esp_opus_dec_close(opus_decoder_);
        opus_decoder_ = nullptr;
    }
    esp_opus_dec_cfg_t opus_dec_cfg = OPUS_DEC_CFG(sample_rate, frame_duration);
    auto ret = esp_opus_dec_open(&opus_dec_cfg, sizeof(esp_opus_dec_cfg_t), &opus_decoder_);
    if (opus_decoder_ == nullptr) {
        ESP_LOGE(TAG, "Failed to create audio decoder, error code: %d", ret);
        return;
    }
    decoder_sample_rate_ = sample_rate;
    decoder_duration_ms_ = frame_duration;
    decoder_frame_size_ = decoder_sample_rate_ / 1000 * frame_duration;

    auto codec = Board::GetInstance().GetAudioCodec();
    if (decoder_sample_rate_ != codec->output_sample_rate()) {
        ESP_LOGI(TAG, "Resampling audio from %d to %d", decoder_sample_rate_, codec->output_sample_rate());
        if (output_resampler_ != nullptr) {
            esp_ae_rate_cvt_close(output_resampler_);
            output_resampler_ = nullptr;
        }
        esp_ae_rate_cvt_cfg_t output_resampler_cfg = RATE_CVT_CFG(
            decoder_sample_rate_, codec->output_sample_rate(), ESP_AUDIO_MONO);
        auto resampler_ret = esp_ae_rate_cvt_open(&output_resampler_cfg, &output_resampler_);
        if (output_resampler_ == nullptr) {
            ESP_LOGE(TAG, "Failed to create output resampler, error code: %d", resampler_ret);
        }
    }
}

void AudioService::PushTaskToEncodeQueue(AudioTaskType type, std::vector<int16_t>&& pcm) {
    auto task = std::make_unique<AudioTask>();
    task->type = type;
    task->pcm = std::move(pcm);
    /* Push the task to the encode queue */
    std::unique_lock<std::mutex> lock(audio_queue_mutex_);

    /* If the task is to send queue, we need to set the timestamp */
    if (type == kAudioTaskTypeEncodeToSendQueue && !timestamp_queue_.empty()) {
        if (timestamp_queue_.size() <= MAX_TIMESTAMPS_IN_QUEUE) {
            task->timestamp = timestamp_queue_.front();
        } else {
            ESP_LOGW(TAG, "Timestamp queue (%u) is full, dropping timestamp", timestamp_queue_.size());
        }
        timestamp_queue_.pop_front();
    }

    if (!audio_queue_cv_.wait_for(lock, std::chrono::milliseconds(200),
            [this]() { return audio_encode_queue_.size() < MAX_ENCODE_TASKS_IN_QUEUE; })) {
        ESP_LOGW(TAG, "Encode queue backpressure (200ms), dropping PCM frame");
        return;
    }
    audio_encode_queue_.push_back(std::move(task));
    audio_queue_cv_.notify_all();
}

bool AudioService::PushPacketToDecodeQueue(std::unique_ptr<AudioStreamPacket> packet, bool wait) {
    std::unique_lock<std::mutex> lock(audio_queue_mutex_);
    if (audio_decode_queue_.size() >= MAX_DECODE_PACKETS_IN_QUEUE) {
        if (wait) {
            audio_queue_cv_.wait(lock, [this]() { return audio_decode_queue_.size() < MAX_DECODE_PACKETS_IN_QUEUE; });
        } else {
            return false;
        }
    }
    audio_decode_queue_.push_back(std::move(packet));
    last_decode_push_time_ = std::chrono::steady_clock::now();
    audio_queue_cv_.notify_all();
    return true;
}

std::unique_ptr<AudioStreamPacket> AudioService::PopPacketFromSendQueue() {
    std::lock_guard<std::mutex> lock(audio_queue_mutex_);
    if (audio_send_queue_.empty()) {
        return nullptr;
    }
    auto packet = std::move(audio_send_queue_.front());
    audio_send_queue_.pop_front();
    audio_queue_cv_.notify_all();
    return packet;
}

void AudioService::EncodeWakeWord() {
    if (wake_word_) {
        wake_word_->EncodeWakeWordData();
    }
}

const std::string& AudioService::GetLastWakeWord() const {
    return wake_word_->GetLastDetectedWakeWord();
}

std::unique_ptr<AudioStreamPacket> AudioService::PopWakeWordPacket() {
    auto packet = std::make_unique<AudioStreamPacket>();
    if (wake_word_->GetWakeWordOpus(packet->payload)) {
        return packet;
    }
    return nullptr;
}

void AudioService::EnableWakeWordDetection(bool enable) {
    if (!wake_word_) {
        return;
    }

    ESP_LOGD(TAG, "%s wake word detection", enable ? "Enabling" : "Disabling");
    if (enable) {
        if (!wake_word_initialized_) {
            if (!wake_word_->Initialize(codec_, models_list_)) {
                ESP_LOGE(TAG, "Failed to initialize wake word");
                return;
            }
            wake_word_initialized_ = true;
        }
        // Reset input resampler to clear cached data from previous mode (e.g. AudioProcessor)
        // This prevents buffer overflow when switching between different feed sizes
        {
            std::lock_guard<std::mutex> lock(input_resampler_mutex_);
            if (input_resampler_ != nullptr) {
                esp_ae_rate_cvt_reset(input_resampler_);
            }
        }
        wake_word_->Start();
        xEventGroupSetBits(event_group_, AS_EVENT_WAKE_WORD_RUNNING);
    } else {
        // 先清 RUNNING 位停掉 AudioInputTask 喂入，再 Stop
        xEventGroupClearBits(event_group_, AS_EVENT_WAKE_WORD_RUNNING);
        wake_word_->Stop();
        // 通话期彻底释放 SR-AFE 引擎以回收内部 RAM（含 6KB 检测任务栈 + AEC/WakeNet 内部缓冲）；
        // 下次 EnableWakeWordDetection(true) 会重建。仅对 AFE 生效，Custom 唤醒词 Release 为 no-op 维持原状。
        if (IsAfeWakeWord()) {
            wake_word_->Release();
            wake_word_initialized_ = false;
        }
    }
}

void AudioService::EnableVoiceProcessing(bool enable) {
    ESP_LOGD(TAG, "%s voice processing", enable ? "Enabling" : "Disabling");
    if (enable) {
        if (!audio_processor_initialized_) {
            audio_processor_->Initialize(codec_, OPUS_FRAME_DURATION_MS, models_list_);
            audio_processor_initialized_ = true;
        }

        /* We should make sure no audio is playing */
        ResetDecoder();
        audio_input_need_warmup_ = true;
        // Reset input resampler to clear cached data from previous mode (e.g. WakeWord)
        // This prevents buffer overflow when switching between different feed sizes
        {
            std::lock_guard<std::mutex> lock(input_resampler_mutex_);
            if (input_resampler_ != nullptr) {
                esp_ae_rate_cvt_reset(input_resampler_);
            }
        }
        audio_processor_->Start();
        xEventGroupSetBits(event_group_, AS_EVENT_AUDIO_PROCESSOR_RUNNING);
    } else {
        audio_processor_->Stop();
        xEventGroupClearBits(event_group_, AS_EVENT_AUDIO_PROCESSOR_RUNNING);
    }
}

void AudioService::EnableAudioTesting(bool enable) {
    ESP_LOGI(TAG, "%s audio testing", enable ? "Enabling" : "Disabling");
    if (enable) {
        xEventGroupSetBits(event_group_, AS_EVENT_AUDIO_TESTING_RUNNING);
    } else {
        xEventGroupClearBits(event_group_, AS_EVENT_AUDIO_TESTING_RUNNING);
        /* Copy audio_testing_queue_ to audio_decode_queue_ */
        std::lock_guard<std::mutex> lock(audio_queue_mutex_);
        audio_decode_queue_ = std::move(audio_testing_queue_);
        audio_queue_cv_.notify_all();
    }
}

void AudioService::EnableDeviceAec(bool enable) {
    ESP_LOGI(TAG, "%s device AEC", enable ? "Enabling" : "Disabling");
    if (!audio_processor_initialized_) {
        audio_processor_->Initialize(codec_, OPUS_FRAME_DURATION_MS, models_list_);
        audio_processor_initialized_ = true;
    }

    audio_processor_->EnableDeviceAec(enable);
}

void AudioService::SetFrameDuration(int frame_duration_ms) {
    if (g_opus_frame_duration_ms == frame_duration_ms) return;
    ESP_LOGI(TAG, "SetFrameDuration: %d → %d ms", g_opus_frame_duration_ms, frame_duration_ms);

    {
        std::lock_guard<std::mutex> lock(encoder_mutex_);
        esp_opus_enc_close(opus_encoder_);
        g_opus_frame_duration_ms = frame_duration_ms;
        esp_opus_enc_config_t cfg = AS_OPUS_ENC_CONFIG_MS(frame_duration_ms);
        esp_opus_enc_open(&cfg, sizeof(cfg), &opus_encoder_);
        esp_opus_enc_get_frame_size(opus_encoder_, &encoder_frame_size_, &encoder_outbuf_size_);
        encoder_frame_size_ /= sizeof(int16_t);
        encoder_duration_ms_ = frame_duration_ms;
    }

    if (audio_processor_initialized_) {
        bool running = xEventGroupGetBits(event_group_) & AS_EVENT_AUDIO_PROCESSOR_RUNNING;
        if (running) audio_processor_->Stop();
        audio_processor_->Initialize(codec_, frame_duration_ms, models_list_);
        if (running) audio_processor_->Start();
    }
}

void AudioService::SetCallbacks(AudioServiceCallbacks& callbacks) {
    callbacks_ = callbacks;
}

void AudioService::PlaySound(const std::string_view& ogg) {
    if (codec_ == nullptr) {
        ESP_LOGW(TAG, "PlaySound called before Initialize, ignored");
        return;
    }
    if (!codec_->output_enabled()) {
        esp_timer_stop(audio_power_timer_);
        esp_timer_start_periodic(audio_power_timer_, AUDIO_POWER_CHECK_INTERVAL_MS * 1000);
        codec_->EnableOutput(true);
    }

    const auto* buf = reinterpret_cast<const uint8_t*>(ogg.data());
    size_t size = ogg.size();

    auto demuxer = std::make_unique<OggDemuxer>();
    demuxer->OnDemuxerFinished([this](const uint8_t* data, int sample_rate, size_t size){
        auto packet = std::make_unique<AudioStreamPacket>();
        packet->sample_rate = sample_rate;
        packet->frame_duration = 60;
        packet->payload.resize(size);
        std::memcpy(packet->payload.data(), data, size);
        PushPacketToDecodeQueue(std::move(packet), true);
    });
    demuxer->Reset();
    demuxer->Process(buf, size);
}

bool AudioService::IsIdle() {
    std::lock_guard<std::mutex> lock(audio_queue_mutex_);
    return audio_encode_queue_.empty() && audio_decode_queue_.empty() && audio_playback_queue_.empty() && audio_testing_queue_.empty();
}

void AudioService::WaitForPlaybackQueueEmpty() {
    std::unique_lock<std::mutex> lock(audio_queue_mutex_);
    audio_queue_cv_.wait(lock, [this]() { 
        return service_stopped_ || (audio_decode_queue_.empty() && audio_playback_queue_.empty()); 
    });
}

void AudioService::ResetDecoder() {
    std::lock_guard<std::mutex> lock(audio_queue_mutex_);
    std::unique_lock<std::mutex> decoder_lock(decoder_mutex_);
    if (opus_decoder_ != nullptr) {
        esp_opus_dec_reset(opus_decoder_);
    }
    decoder_lock.unlock();
    timestamp_queue_.clear();
    audio_decode_queue_.clear();
    audio_playback_queue_.clear();
    audio_testing_queue_.clear();
    consecutive_underrun_plc_ = 0;
    last_plc_tail_sample_ = 0;
    audio_queue_cv_.notify_all();
}

void AudioService::CheckAndUpdateAudioPowerState() {
    auto now = std::chrono::steady_clock::now();
    auto input_elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_input_time_).count();
    auto output_elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_output_time_).count();
    if (input_elapsed > AUDIO_POWER_TIMEOUT_MS && codec_->input_enabled()) {
        codec_->EnableInput(false);
    }
    if (output_elapsed > AUDIO_POWER_TIMEOUT_MS && codec_->output_enabled()) {
        // Keep TX clock when duplex RX is active; otherwise RX may stall on some boards.
        if (!(codec_->duplex() && codec_->input_enabled())) {
            codec_->EnableOutput(false);
        }
    }
    if (!codec_->input_enabled() && !codec_->output_enabled()) {
        esp_timer_stop(audio_power_timer_);
    }
}

void AudioService::SetModelsList(srmodel_list_t* models_list) {
    models_list_ = models_list;

#if CONFIG_IDF_TARGET_ESP32S3 || CONFIG_IDF_TARGET_ESP32P4
    std::string mode = Settings("wakeword", false).GetString("mode", "afe");
    bool has_mn = esp_srmodel_filter(models_list_, ESP_MN_PREFIX, NULL) != nullptr;
    bool has_wn = esp_srmodel_filter(models_list_, ESP_WN_PREFIX, NULL) != nullptr;
    if (mode == "custom" && has_mn) {
        wake_word_ = std::make_unique<CustomWakeWord>();
    } else if (has_wn) {
        wake_word_ = std::make_unique<AfeWakeWord>();
    } else {
        wake_word_ = nullptr;
    }
    ESP_LOGI(TAG, "wake_word: mode=%s → %s", mode.c_str(),
             wake_word_ ? (mode == "custom" && has_mn ? "Custom" : "AFE") : "null");
#else
    (void)models_list;
    wake_word_ = nullptr;
#endif

    if (wake_word_) {
        wake_word_->OnWakeWordDetected([this](const std::string& wake_word) {
            if (callbacks_.on_wake_word_detected) {
                callbacks_.on_wake_word_detected(wake_word);
            }
        });
    }
}

bool AudioService::IsAfeWakeWord() {
#if CONFIG_IDF_TARGET_ESP32S3 || CONFIG_IDF_TARGET_ESP32P4
    return wake_word_ != nullptr && dynamic_cast<AfeWakeWord*>(wake_word_.get()) != nullptr;
#else
    return false;
#endif
}
