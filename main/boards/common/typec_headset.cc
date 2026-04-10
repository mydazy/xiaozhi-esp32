#include "typec_headset.h"
#include <esp_log.h>
#include <cstring>

#define TAG "TypecHeadset"

TypecHeadset::TypecHeadset(const TypecHeadsetConfig& cfg) : cfg_(cfg) {}

TypecHeadset::~TypecHeadset() { Stop(); }

void TypecHeadset::InitGpio() {
    // 输入: USB_DET
    gpio_config_t in_cfg = {
        .pin_bit_mask = (1ULL << cfg_.usb_det_pin),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&in_cfg);

    // 输出: USB_SW, CC_VDD, MIC_SELECT, PA
    gpio_config_t out_cfg = {
        .pin_bit_mask = (1ULL << cfg_.usb_sw_pin) | (1ULL << cfg_.cc_vdd_pin)
                      | (1ULL << cfg_.mic_select_pin) | (1ULL << cfg_.pa_pin),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&out_cfg);

    // ADC 引脚
    gpio_reset_pin(cfg_.usb_mic_adc_pin);
    gpio_reset_pin(cfg_.cc_adc_pin);

    mic_select_level_ = 0;
    gpio_set_level(cfg_.mic_select_pin, 0);
}

void TypecHeadset::InitAdc(adc_oneshot_unit_handle_t shared_adc) {
    if (shared_adc) {
        adc_handle_ = shared_adc;
        adc_owned_ = false;
    } else {
        adc_oneshot_unit_init_cfg_t init_cfg = {
            .unit_id = cfg_.cc_adc_unit,
            .ulp_mode = ADC_ULP_MODE_DISABLE,
        };
        adc_oneshot_new_unit(&init_cfg, &adc_handle_);
        adc_owned_ = true;
    }

    adc_oneshot_chan_cfg_t chan_cfg = {
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_12,
    };
    adc_oneshot_config_channel(adc_handle_, cfg_.cc_adc_channel, &chan_cfg);
    adc_oneshot_config_channel(adc_handle_, cfg_.mic_adc_channel, &chan_cfg);

    // 校准
#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
    adc_cali_curve_fitting_config_t cc_cal = {
        .unit_id = cfg_.cc_adc_unit, .chan = cfg_.cc_adc_channel,
        .atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    adc_cali_create_scheme_curve_fitting(&cc_cal, &cc_cali_);

    adc_cali_curve_fitting_config_t mic_cal = {
        .unit_id = cfg_.mic_adc_unit, .chan = cfg_.mic_adc_channel,
        .atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    adc_cali_create_scheme_curve_fitting(&mic_cal, &mic_cali_);
#endif
}

bool TypecHeadset::ReadMv(adc_channel_t channel, adc_cali_handle_t cali, int& out_mv) {
    int raw = 0;
    if (adc_oneshot_read(adc_handle_, channel, &raw) != ESP_OK) return false;
    if (cali && adc_cali_raw_to_voltage(cali, raw, &out_mv) == ESP_OK) return true;
    out_mv = raw * 3300 / 4095;
    return true;
}

void TypecHeadset::DetectLoop() {
    bool last_state = false;

    while (running_) {
        int usb_det = gpio_get_level(cfg_.usb_det_pin);

        if (usb_det) {
            // USB_DET 高 = 充电器，非耳机
            if (inserted_) {
                inserted_ = false;
                gpio_set_level(cfg_.usb_sw_pin, 0);
                gpio_set_level(cfg_.pa_pin, 1);
                ESP_LOGW(TAG, "🔌 USB_DET=HIGH → charger/debug, USB_SW=0 (speaker mode)");
                if (callback_) callback_(false);
            }
            gpio_set_level(cfg_.cc_vdd_pin, 1);
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        // USB_DET 低，检测耳机
        gpio_set_level(cfg_.cc_vdd_pin, 0);
        vTaskDelay(pdMS_TO_TICKS(2));

        int cc_mv = 0;
        if (!ReadMv(cfg_.cc_adc_channel, cc_cali_, cc_mv)) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        static int insert_cnt = 0, remove_cnt = 0;
        // 每 50 轮（~1秒）打印一次检测状态
        static int dbg_cnt = 0;
        if (++dbg_cnt % 50 == 0) {
            ESP_LOGW(TAG, "🎧 USB_DET=LOW cc_mv=%d (thr=%d) inserted=%d insert_cnt=%d remove_cnt=%d",
                     cc_mv, cfg_.cc_headset_mv, inserted_, insert_cnt, remove_cnt);
        }

        if (cc_mv < cfg_.cc_headset_mv) {
            insert_cnt++;
            remove_cnt = 0;
        } else {
            remove_cnt++;
            insert_cnt = 0;
        }

        // 插入确认（防抖 2 次）
        if (!inserted_ && insert_cnt >= 2) {
            insert_cnt = 0;
            ESP_LOGW(TAG, "🎧🎧🎧 HEADSET INSERTING: cc_mv=%d, PA=OFF, USB_SW=1", cc_mv);
            gpio_set_level(cfg_.pa_pin, 0);   // 关 PA
            vTaskDelay(pdMS_TO_TICKS(20));
            gpio_set_level(cfg_.usb_sw_pin, 1); // 切耳机

            // 测 MIC 正反插
            gpio_set_level(cfg_.mic_select_pin, 0);
            vTaskDelay(pdMS_TO_TICKS(2));
            int mv0 = 0;
            ReadMv(cfg_.mic_adc_channel, mic_cali_, mv0);

            gpio_set_level(cfg_.mic_select_pin, 1);
            vTaskDelay(pdMS_TO_TICKS(2));
            int mv1 = 0;
            ReadMv(cfg_.mic_adc_channel, mic_cali_, mv1);

            mic_select_level_ = (mv0 > mv1) ? 0 : 1;
            gpio_set_level(cfg_.mic_select_pin, mic_select_level_);

            inserted_ = true;
            ESP_LOGW(TAG, "🎧🎧🎧 HEADSET INSERTED: MIC_SEL=%d mv0=%d mv1=%d USB_SW=1 PA=OFF",
                     mic_select_level_, mv0, mv1);
            if (callback_) callback_(true);
        }

        // 拔出确认（防抖 3 次）
        if (inserted_ && remove_cnt >= 3) {
            remove_cnt = 0;
            ESP_LOGW(TAG, "🔊🔊🔊 HEADSET REMOVING: cc_mv=%d, USB_SW=0, PA=ON", cc_mv);
            gpio_set_level(cfg_.pa_pin, 0);
            gpio_set_level(cfg_.usb_sw_pin, 0);
            vTaskDelay(pdMS_TO_TICKS(10));
            gpio_set_level(cfg_.pa_pin, 1);   // 开 PA

            inserted_ = false;
            mic_select_level_ = 0;
            gpio_set_level(cfg_.mic_select_pin, 0);
            ESP_LOGW(TAG, "🔊🔊🔊 HEADSET REMOVED: speaker mode, USB_SW=0 PA=ON");
            if (callback_) callback_(false);
        }

        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

void TypecHeadset::TaskFunc(void* arg) {
    static_cast<TypecHeadset*>(arg)->DetectLoop();
    vTaskDelete(nullptr);
}

void TypecHeadset::Start(adc_oneshot_unit_handle_t shared_adc) {
    if (running_) return;

    InitGpio();
    InitAdc(shared_adc);

    // 初始状态：喇叭模式
    gpio_set_level(cfg_.pa_pin, 1);
    gpio_set_level(cfg_.usb_sw_pin, 0);

    running_ = true;
    xTaskCreatePinnedToCore(TaskFunc, "headset", 3072, this, 3, &task_, 0);
    ESP_LOGW(TAG, "🔊 Detection started: USB_DET=%d USB_SW=0 PA=ON (speaker mode)",
             gpio_get_level(cfg_.usb_det_pin));
}

void TypecHeadset::Stop() {
    running_ = false;
    if (task_) {
        vTaskDelay(pdMS_TO_TICKS(100));
        task_ = nullptr;
    }
    if (adc_owned_ && adc_handle_) {
        adc_oneshot_del_unit(adc_handle_);
        adc_handle_ = nullptr;
    }
}
