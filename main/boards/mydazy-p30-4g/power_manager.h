#pragma once
#include <vector>
#include <functional>

#include <esp_timer.h>
#include <driver/gpio.h>
#include <esp_adc/adc_oneshot.h>

#define ADC_CHANNEL    ADC_CHANNEL_7
#define POWER_MANAGER_TAG "PowerManager"

typedef struct {
    uint16_t adc;    // 电压值
    uint8_t level;
} bat_level_t;

class PowerManager {
private:
    esp_timer_handle_t timer_handle_;
    inline static PowerManager* instance_ = nullptr; //hsf
    std::function<void(bool)> on_charging_status_changed_;
    std::function<void(bool)> on_low_battery_status_changed_;

    gpio_num_t charging_pin_ = GPIO_NUM_NC;
    std::vector<uint16_t> adc_values_;
    uint32_t battery_level_ = 0;
    bool is_charging_ = false;
    bool is_low_battery_ = false;
    bool is_off_battery_ = false;  // 达到无法开机的标准
    int ticks_ = 0;
    const int kBatteryAdcInterval = 60;
    const int kBatteryAdcDataCount = 3;
    const int kLowBatteryLevel = 0;

    adc_oneshot_unit_handle_t adc_handle_;

    bool do_calibration1_chan0_ = false;
    adc_cali_handle_t adc1_cali_chan0_handle_ = NULL;

    /*---------------------------------------------------------------
        ADC校准
    ---------------------------------------------------------------*/
    bool AdcCalibrationInit(adc_unit_t unit, adc_channel_t channel, adc_atten_t atten, adc_cali_handle_t *out_handle)
    {
        adc_cali_handle_t handle = NULL;
        esp_err_t ret = ESP_FAIL;
        bool calibrated = false;

    #if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
        if (!calibrated) {
            ESP_LOGI(POWER_MANAGER_TAG, "calibration scheme version is %s", "Curve Fitting");
            adc_cali_curve_fitting_config_t cali_config = {
                .unit_id = unit,
                .chan = channel,
                .atten = atten,
                .bitwidth = ADC_BITWIDTH_DEFAULT,
            };
            ret = adc_cali_create_scheme_curve_fitting(&cali_config, &handle);
            if (ret == ESP_OK) {
                calibrated = true;
            }
        }
    #endif

    #if ADC_CALI_SCHEME_LINE_FITTING_SUPPORTED
        if (!calibrated) {
            ESP_LOGI(POWER_MANAGER_TAG, "calibration scheme version is %s", "Line Fitting");
            adc_cali_line_fitting_config_t cali_config = {
                .unit_id = unit,
                .atten = atten,
                .bitwidth = ADC_BITWIDTH_DEFAULT,
            };
            ret = adc_cali_create_scheme_line_fitting(&cali_config, &handle);
            if (ret == ESP_OK) {
                calibrated = true;
            }
        }
    #endif

        *out_handle = handle;
        if (ret == ESP_OK) {
            ESP_LOGI(POWER_MANAGER_TAG, "Calibration Success");
        } else if (ret == ESP_ERR_NOT_SUPPORTED || !calibrated) {
            ESP_LOGW(POWER_MANAGER_TAG, "eFuse not burnt, skip software calibration");
        } else {
            ESP_LOGE(POWER_MANAGER_TAG, "Invalid arg or no memory");
        }

        return calibrated;
    }

    void CheckBatteryStatus() {
        // Get charging status
        bool new_charging_status = gpio_get_level(charging_pin_) == 0;
        if (new_charging_status != is_charging_) {
            is_charging_ = new_charging_status;
            if (on_charging_status_changed_) {
                on_charging_status_changed_(is_charging_);
            }
            adc_values_.clear();
            ReadBatteryAdcData();
            return;
        }

        // 如果电池电量数据不足，则读取电池电量数据
        if (adc_values_.size() < kBatteryAdcDataCount) {
            ReadBatteryAdcData();
            return;
        }

        // 如果电池电量数据充足，则每 kBatteryAdcInterval 个 tick 读取一次电池电量数据
        ticks_++;
        if (ticks_ % kBatteryAdcInterval == 0) {
            ReadBatteryAdcData();
        }
    }

    void ReadBatteryAdcData() {
        int adc_value;
        int voltage;
        
        ESP_ERROR_CHECK(adc_oneshot_read(adc_handle_, ADC_CHANNEL, &adc_value));
        
        // 将 ADC 值添加到队列中
        adc_values_.push_back(adc_value);
        if (adc_values_.size() > kBatteryAdcDataCount) {
            adc_values_.erase(adc_values_.begin());
        }
        uint32_t average_adc = 0;
        for (auto value : adc_values_) {
            average_adc += value;
        }
        average_adc /= adc_values_.size();

        // 定义电池电量区间  放电
        const bat_level_t levels_fd[] = {
            {3400, 0},
            {3500, 20},
            {3600, 40},
            {3700, 60},
            {3800, 80},
            {4000, 100}
        };

        const bat_level_t levels_cd[] = {
            {3550, 0},
            {3650, 20},
            {3750, 40},
            {3850, 60},
            {3950, 80},
            {4150, 100}
        };

        const bat_level_t* levels;
        if (is_charging_) {
            levels = levels_cd;
        } else {
            levels = levels_fd;
        }

        // 计算电压值
        if (do_calibration1_chan0_) {
            ESP_ERROR_CHECK(adc_cali_raw_to_voltage(adc1_cali_chan0_handle_, average_adc, &voltage));
            voltage = voltage * 2;  // 2个1M电阻分压
        }
        else {
            voltage = 3600 * 1000 / 4096 * average_adc / 1000;
        }

        // 低于最低值时
        if (voltage < levels[0].adc) {
            battery_level_ = 0;
        }
        // 高于最高值时
        else if (voltage >= levels[5].adc) {
            battery_level_ = 100;
        } else {
            // 线性插值计算中间值
            for (int i = 0; i < 5; i++) {
                if (voltage >= levels[i].adc && voltage < levels[i+1].adc) {
                    float ratio = static_cast<float>(voltage - levels[i].adc) / (levels[i+1].adc - levels[i].adc);
                    battery_level_ = levels[i].level + ratio * (levels[i+1].level - levels[i].level);
                    break;
                }
            }
        }

        // Check low battery status
        if (adc_values_.size() >= kBatteryAdcDataCount) {
            bool new_low_battery_status = battery_level_ <= kLowBatteryLevel;
            if(is_charging_) new_low_battery_status = false;
            if (new_low_battery_status != is_low_battery_) {
                is_low_battery_ = new_low_battery_status;
                if (on_low_battery_status_changed_) {
                    on_low_battery_status_changed_(is_low_battery_);
                }
            }
        }

        if(voltage < levels_fd[0].adc && is_charging_ == false){
            is_off_battery_ = true;
        }
        else{
            is_off_battery_ = false;
        }

        ESP_LOGI("PowerManager", "charging: %u, ADC value: %d average: %ld level: %ld, Cali Voltage: %d mV", is_charging_, adc_value, average_adc, battery_level_, voltage);

    }

public:
    PowerManager(gpio_num_t pin) : charging_pin_(pin) {
        instance_ = this; //hsf
        // 初始化充电引脚
        gpio_config_t io_conf = {};
        io_conf.intr_type = GPIO_INTR_DISABLE;
        io_conf.mode = GPIO_MODE_INPUT;
        io_conf.pin_bit_mask = (1ULL << charging_pin_);
        io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE; 
        io_conf.pull_up_en = GPIO_PULLUP_ENABLE;     
        gpio_config(&io_conf);

        // 创建电池电量检查定时器
        esp_timer_create_args_t timer_args = {
            .callback = [](void* arg) {
                PowerManager* self = static_cast<PowerManager*>(arg);
                self->CheckBatteryStatus();
            },
            .arg = this,
            .dispatch_method = ESP_TIMER_TASK,
            .name = "battery_check_timer",
            .skip_unhandled_events = true,
        };
        ESP_ERROR_CHECK(esp_timer_create(&timer_args, &timer_handle_));
        ESP_ERROR_CHECK(esp_timer_start_periodic(timer_handle_, 1000000));

        // 初始化 ADC
        adc_oneshot_unit_init_cfg_t init_config = {
            .unit_id = ADC_UNIT_1,
            .ulp_mode = ADC_ULP_MODE_DISABLE,
        };
        ESP_ERROR_CHECK(adc_oneshot_new_unit(&init_config, &adc_handle_));
        
        adc_oneshot_chan_cfg_t chan_config = {
            .atten = ADC_ATTEN_DB_12,
            .bitwidth = ADC_BITWIDTH_12,
        };
        ESP_ERROR_CHECK(adc_oneshot_config_channel(adc_handle_, ADC_CHANNEL, &chan_config));

        do_calibration1_chan0_ = AdcCalibrationInit(ADC_UNIT_1, ADC_CHANNEL, ADC_ATTEN_DB_12, &adc1_cali_chan0_handle_);

        CheckBatteryStatus();
        if(is_off_battery_){
            for (size_t i = 0; i < 3; i++)
            {
                CheckBatteryStatus();
            }            
        }
    }

    // ~PowerManager() {
    //     if (timer_handle_) {
    //         esp_timer_stop(timer_handle_);
    //         esp_timer_delete(timer_handle_);
    //     }
    //     if (adc_handle_) {
    //         adc_oneshot_del_unit(adc_handle_);
    //     }
    // }
    
    // hsf
       ~PowerManager() {
        if (timer_handle_) {
            esp_timer_stop(timer_handle_);
            esp_timer_delete(timer_handle_);
        }
        if (adc_handle_) {
            adc_oneshot_del_unit(adc_handle_);
        }
    }

    static adc_oneshot_unit_handle_t GetSharedAdcHandle() {
        return instance_ ? instance_->adc_handle_ : nullptr;
    }

    static adc_cali_handle_t GetSharedAdcCaliHandle() {
        return instance_ ? instance_->adc1_cali_chan0_handle_ : nullptr;
    }

    static bool IsSharedAdcCalibrated() {
        return instance_ ? instance_->do_calibration1_chan0_ : false;
    }

    bool IsCharging() {
        // // 如果电量已经满了，则不再显示充电中
        // if (battery_level_ == 100) {
        //     return false;
        // }
        return is_charging_;
    }

    bool IsDischarging() {
        // 没有区分充电和放电，所以直接返回相反状态
        return !is_charging_;
    }

    uint8_t GetBatteryLevel() {
        return battery_level_;
    }

    void OnLowBatteryStatusChanged(std::function<void(bool)> callback) {
        on_low_battery_status_changed_ = callback;
    }

    void OnChargingStatusChanged(std::function<void(bool)> callback) {
        on_charging_status_changed_ = callback;
    }

    bool IsOffBatteryLevel(){
        return is_off_battery_;
    }
};
