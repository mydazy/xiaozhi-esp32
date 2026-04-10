/**
 * @file typec_headset.h
 * @brief Type-C 耳机动态插拔检测（5-GPIO 方案）
 *
 * 检测逻辑:
 *   USB_DET 高 → 充电器，开 PA（喇叭模式）
 *   USB_DET 低 → CC_VDD 拉低读 CC_ADC
 *   CC_ADC < 100mV → 耳机插入 → USB_SW 高切耳机，关 PA
 *   MIC_SELECT 翻转测 USB_MIC_ADC 确定正反插
 */

#pragma once

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <driver/gpio.h>
#include <esp_adc/adc_oneshot.h>
#include <esp_adc/adc_cali.h>
#include <esp_adc/adc_cali_scheme.h>
#include <atomic>
#include <functional>

struct TypecHeadsetConfig {
    gpio_num_t usb_det_pin;       // USB 检测（高=充电器）
    gpio_num_t cc_adc_pin;        // CC ADC 检测
    gpio_num_t usb_sw_pin;        // USB 音频开关
    gpio_num_t cc_vdd_pin;        // CC 电源控制
    gpio_num_t mic_select_pin;    // MIC 正反插选择
    gpio_num_t pa_pin;            // PA 功放控制
    gpio_num_t usb_mic_adc_pin;   // USB MIC ADC

    adc_unit_t cc_adc_unit;
    adc_channel_t cc_adc_channel;
    adc_unit_t mic_adc_unit;
    adc_channel_t mic_adc_channel;

    int cc_headset_mv;            // 耳机判定阈值 (mV)
};

using HeadsetCallback = std::function<void(bool inserted)>;

class TypecHeadset {
public:
    TypecHeadset(const TypecHeadsetConfig& cfg);
    ~TypecHeadset();

    /// 启动检测任务
    void Start(adc_oneshot_unit_handle_t shared_adc = nullptr);

    /// 停止检测任务
    void Stop();

    /// 当前是否有耳机
    bool IsInserted() const { return inserted_; }

    /// 设置插拔回调
    void SetCallback(HeadsetCallback cb) { callback_ = std::move(cb); }

private:
    TypecHeadsetConfig cfg_;
    std::atomic<bool> inserted_{false};
    std::atomic<bool> running_{false};
    HeadsetCallback callback_;
    TaskHandle_t task_ = nullptr;

    adc_oneshot_unit_handle_t adc_handle_ = nullptr;
    adc_cali_handle_t cc_cali_ = nullptr;
    adc_cali_handle_t mic_cali_ = nullptr;
    bool adc_owned_ = false;
    int mic_select_level_ = 0;

    void InitGpio();
    void InitAdc(adc_oneshot_unit_handle_t shared_adc);
    bool ReadMv(adc_channel_t channel, adc_cali_handle_t cali, int& out_mv);
    void DetectLoop();
    static void TaskFunc(void* arg);
};
