/*
 * AXS5106L 触摸屏驱动（精简版）
 * 硬件: MyDazy P30 (ESP32-S3 + JD9853 284×240 + AXS5106L @0x63)
 */

#include "axs5106l_touch.h"
#include "axs5106l_upgrade.h"

#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <algorithm>
#include <cstring>

static const char* TAG = "Axs5106lTouch";

// ===== AXS5106L I2C 寄存器（全部已知） =====
#define AXS5106L_I2C_ADDR      0x63
#define AXS5106L_REG_DATA      0x01   // 读触摸数据（6 字节：gesture/num/XH/XL/YH/YL）
#define AXS5106L_REG_FW_VER    0x05   // 读固件版本（2 字节 BE）
#define AXS5106L_REG_CHIP_ID   0x08   // 读 chip ID（3 字节）
#define AXS5106L_REG_SLEEP     0x19   // 写 0x03 进入 sleep
#define AXS5106L_REG_RESET     0xF0   // 写 {B3,55,AA,34,01} 软复位

// 触摸数据字段
#define AXS_POINT_NUM(buf)  (buf[1])
#define AXS_POINT_X(buf)    (((uint16_t)(buf[2] & 0x0F) << 8) | buf[3])
#define AXS_POINT_Y(buf)    (((uint16_t)(buf[4] & 0x0F) << 8) | buf[5])

// chip V2905 固件已做 rotation，直接输出 landscape 像素（见 README.md）
// AA 硬编码 [21..273]×[1..236]，无寄存器可配；超出此 + 50 余量即视为噪声帧
#define TOUCH_MAX_X   284
#define TOUCH_MAX_Y   240

// 手势参数
#define SWIPE_THRESHOLD      40       // 滑动距离阈值（像素）
#define CLICK_MAX_TIME_US    400000   // 点击最长时长 400ms
#define CLICK_MIN_TIME_US    30000    // 点击最短时长 30ms（过滤 RF 尖峰）
#define CLICK_MAX_MOVE       30       // 点击时允许最大移动
#define LONG_PRESS_TIME_US   600000   // 长按 600ms
#define DOUBLE_CLICK_TIME_US 500000   // 双击间隔 500ms
#define DOUBLE_CLICK_DIST    60       // 双击位置容差

// 4G RF 抗干扰
#define INT_DEBOUNCE_US       3000    // INT 持续低电平 ≥3ms 才算按下
#define RELEASE_DEBOUNCE      2       // 连续 2 帧无触摸才算释放
#define MAX_SPEED_PX_S        2000    // 速度阈值（正常滑动 ~1400，RF 跳变 >3000）

// I2C
#define I2C_TIMEOUT_MS  100
#define I2C_RETRIES     3

// ============================================================
// 构造 / 析构
// ============================================================
Axs5106lTouch::Axs5106lTouch(i2c_master_bus_handle_t i2c_bus,
                             gpio_num_t rst_gpio, gpio_num_t int_gpio,
                             uint16_t width, uint16_t height,
                             bool swap_xy, bool mirror_x, bool mirror_y)
    : i2c_bus_(i2c_bus),
      rst_gpio_(rst_gpio), int_gpio_(int_gpio),
      width_(width), height_(height),
      swap_xy_(swap_xy), mirror_x_(mirror_x), mirror_y_(mirror_y) {}

Axs5106lTouch::~Axs5106lTouch() { Cleanup(); }

// ============================================================
// 初始化（两阶段：硬件层 → LVGL 输入层）
// ============================================================
bool Axs5106lTouch::InitializeHardware() {
    ESP_LOGI(TAG, "初始化触摸屏: RST=GPIO%d, INT=GPIO%d, 分辨率=%dx%d",
             rst_gpio_, int_gpio_, width_, height_);

    // 配置 reset 引脚（init 默认拉高避免 chip 误复位）
    gpio_config_t rst_cfg = {
        .pin_bit_mask = (1ULL << rst_gpio_),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&rst_cfg));
    gpio_set_level(rst_gpio_, 1);

    // 注册 I2C 设备（400kHz，7 位地址 0x63）
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = AXS5106L_I2C_ADDR,
        .scl_speed_hz = 400000,
        .scl_wait_us = 0,
        .flags = {.disable_ack_check = 0},
    };
    esp_err_t ret = i2c_master_bus_add_device(i2c_bus_, &dev_cfg, &i2c_handle_);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "I2C 设备创建失败: %s", esp_err_to_name(ret));
        return false;
    }

    if (!ResetChip()) return false;

    // 固件版本对比，不一致则升级并再次复位
    if (CheckAndUpgradeFirmware()) {
        ESP_LOGI(TAG, "固件已升级，重新复位");
        ResetChip();
    }

    // 读 chip ID 验活（最多 5 次重试）
    uint8_t chip_id[3] = {0};
    for (int i = 0; i < 5; i++) {
        if (ReadRegister(AXS5106L_REG_CHIP_ID, chip_id, 3)) {
            uint8_t mix_or  = chip_id[0] | chip_id[1] | chip_id[2];
            uint8_t mix_and = chip_id[0] & chip_id[1] & chip_id[2];
            if (mix_or != 0 && mix_and != 0xFF) {
                ESP_LOGI(TAG, "芯片ID: 0x%02X%02X%02X", chip_id[0], chip_id[1], chip_id[2]);
                break;
            }
        }
        if (i == 4) {
            ESP_LOGE(TAG, "无法读取有效芯片ID");
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    uint8_t fw_ver[2] = {0};
    if (ReadRegister(AXS5106L_REG_FW_VER, fw_ver, 2)) {
        ESP_LOGI(TAG, "固件版本: %u", (fw_ver[0] << 8) | fw_ver[1]);
    }
    return true;
}

bool Axs5106lTouch::InitializeInput() {
    // INT 引脚用作轮询输入（不注册 GPIO 中断，LVGL 周期轮询读）
    gpio_config_t int_cfg = {
        .pin_bit_mask = (1ULL << int_gpio_),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&int_cfg));

    if (lvgl_indev_) return true;  // 幂等

    lvgl_indev_ = lv_indev_create();
    if (!lvgl_indev_) {
        ESP_LOGE(TAG, "LVGL 输入设备创建失败");
        return false;
    }
    lv_indev_set_type(lvgl_indev_, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(lvgl_indev_, LvglReadCallback);
    lv_indev_set_user_data(lvgl_indev_, this);

    ESP_LOGI(TAG, "触摸屏已接入 LVGL");
    return true;
}

// ============================================================
// 硬复位 + 软复位（顺序：软复位 I2C 帧 → 硬复位时序）
// ============================================================
bool Axs5106lTouch::ResetChip() {
    const uint8_t rst_cmd[5] = {0xB3, 0x55, 0xAA, 0x34, 0x01};
    WriteRegister(AXS5106L_REG_RESET, rst_cmd, 5);

    gpio_set_level(rst_gpio_, 1);
    esp_rom_delay_us(50);
    gpio_set_level(rst_gpio_, 0);
    esp_rom_delay_us(50);
    vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(rst_gpio_, 1);
    vTaskDelay(pdMS_TO_TICKS(50));
    return true;
}

void Axs5106lTouch::Sleep() {
    sleeping_ = true;
    const uint8_t cmd = 0x03;
    WriteRegister(AXS5106L_REG_SLEEP, &cmd, 1);
    gpio_set_level(rst_gpio_, 1);
}

void Axs5106lTouch::Resume() {
    sleeping_ = false;
    ResetChip();
    vTaskDelay(pdMS_TO_TICKS(10));
}

// ============================================================
// I2C 读写（每次最多重试 3 次，间隔 5ms）
// ============================================================
bool Axs5106lTouch::WriteRegister(uint8_t reg, const uint8_t* data, size_t len) {
    if (!i2c_handle_ || len > 15) return false;
    uint8_t buf[16];
    buf[0] = reg;
    memcpy(&buf[1], data, len);
    for (int i = 0; i < I2C_RETRIES; i++) {
        if (i2c_master_transmit(i2c_handle_, buf, len + 1, I2C_TIMEOUT_MS) == ESP_OK) return true;
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    return false;
}

bool Axs5106lTouch::ReadRegister(uint8_t reg, uint8_t* data, size_t len) {
    if (!i2c_handle_) return false;
    for (int i = 0; i < I2C_RETRIES; i++) {
        if (i2c_master_transmit(i2c_handle_, &reg, 1, I2C_TIMEOUT_MS) == ESP_OK &&
            i2c_master_receive(i2c_handle_, data, len, I2C_TIMEOUT_MS) == ESP_OK) {
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    return false;
}

// ============================================================
// LVGL 周期回调（驱动主循环入口）
// ============================================================
void Axs5106lTouch::LvglReadCallback(lv_indev_t* indev, lv_indev_data_t* data) {
    auto* self = static_cast<Axs5106lTouch*>(lv_indev_get_user_data(indev));
    if (!self || self->sleeping_) {
        data->state = LV_INDEV_STATE_RELEASED;
        return;
    }

#if AXS5106L_TOUCH_DEBUG_OVERLAY
    // 首次回调创建调试 overlay：跟手红圆点
    if (!self->debug_dot_) {
        lv_obj_t* scr = lv_screen_active();
        if (scr) {
            self->debug_dot_ = lv_obj_create(scr);
            lv_obj_remove_style_all(self->debug_dot_);
            lv_obj_set_size(self->debug_dot_, 12, 12);
            lv_obj_set_style_bg_color(self->debug_dot_, lv_color_make(0xFF, 0x30, 0x30), 0);
            lv_obj_set_style_bg_opa(self->debug_dot_, LV_OPA_COVER, 0);
            lv_obj_set_style_radius(self->debug_dot_, LV_RADIUS_CIRCLE, 0);
            lv_obj_add_flag(self->debug_dot_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(self->debug_dot_, LV_OBJ_FLAG_IGNORE_LAYOUT);
            lv_obj_move_foreground(self->debug_dot_);
        }
    }
#endif

    uint16_t x, y;
    if (self->ReadTouch(x, y)) {
        // 新按下边沿：触发唤醒回调
        if (!self->touch_.pressed && self->wake_callback_) {
            self->wake_callback_();
        }
        self->touch_.pressed = true;
        self->touch_.release_count = 0;
        self->touch_.last_x = x;
        self->touch_.last_y = y;
        data->point.x = x;
        data->point.y = y;
        data->state = LV_INDEV_STATE_PRESSED;
        self->RecognizeGesture(x, y, true);

#if AXS5106L_TOUCH_DEBUG_OVERLAY
        if (self->debug_dot_) {
            lv_obj_remove_flag(self->debug_dot_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_pos(self->debug_dot_, (int16_t)x - 6, (int16_t)y - 6);
            lv_obj_move_foreground(self->debug_dot_);
        }
#endif
        return;
    }

    // ReadTouch 返回 false 时的释放去抖：连续 2 帧无触摸才上报 RELEASED
    if (self->touch_.pressed) {
        self->touch_.release_count++;
        if (self->touch_.release_count >= RELEASE_DEBOUNCE) {
            self->touch_.pressed = false;
            self->touch_.release_count = 0;
            data->point.x = self->touch_.last_x;
            data->point.y = self->touch_.last_y;
            data->state = LV_INDEV_STATE_RELEASED;
            self->RecognizeGesture(0, 0, false);
#if AXS5106L_TOUCH_DEBUG_OVERLAY
            if (self->debug_dot_) lv_obj_add_flag(self->debug_dot_, LV_OBJ_FLAG_HIDDEN);
#endif
        } else {
            // 去抖窗口内保持按下（用最后有效坐标）
            data->point.x = self->touch_.last_x;
            data->point.y = self->touch_.last_y;
            data->state = LV_INDEV_STATE_PRESSED;
        }
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
    }
}

// ============================================================
// 读一帧触摸数据，返回 true 表示本帧有效按下
// ============================================================
bool Axs5106lTouch::ReadTouch(uint16_t& x, uint16_t& y) {
    uint64_t now = esp_timer_get_time();

    // Gate 1: INT 电平过滤（AXS5106L: 触摸时持续低电平）
    if (gpio_get_level(int_gpio_) != 0) {
        touch_.int_low_since = 0;
        touch_.last_time = 0;
        return false;
    }
    // Gate 2: INT 持续低电平 ≥ INT_DEBOUNCE_US 才算真按下（新按下才去抖，已按下跳过）
    if (!touch_.pressed) {
        if (touch_.int_low_since == 0) touch_.int_low_since = now;
        if (now - touch_.int_low_since < INT_DEBOUNCE_US) return false;
    }

    // 读 6 字节触摸数据
    uint8_t buf[6] = {0};
    if (!ReadRegister(AXS5106L_REG_DATA, buf, 6)) return false;

    uint8_t n = AXS_POINT_NUM(buf);
    if (n == 0 || n > 1) return false;  // 单点触摸，其他值视为噪声

    uint16_t raw_x = AXS_POINT_X(buf);
    uint16_t raw_y = AXS_POINT_Y(buf);

    // 坐标健壮性：全 1 无效帧 / 超范围帧丢弃
    if (raw_x == 0xFFF && raw_y == 0xFFF) return false;
    if (raw_x > TOUCH_MAX_X + 50 || raw_y > TOUCH_MAX_Y + 50) return false;

#if AXS5106L_TOUCH_DEBUG_OVERLAY
    if (raw_x < raw_stats_.raw_min_x) raw_stats_.raw_min_x = raw_x;
    if (raw_x > raw_stats_.raw_max_x) raw_stats_.raw_max_x = raw_x;
    if (raw_y < raw_stats_.raw_min_y) raw_stats_.raw_min_y = raw_y;
    if (raw_y > raw_stats_.raw_max_y) raw_stats_.raw_max_y = raw_y;
    if (++raw_stats_.sample_count % 20 == 0) {
        ESP_LOGI(TAG, "[calib] chip raw X=[%u..%u] Y=[%u..%u] (n=%lu) 期望 [0..%d][0..%d]",
                 raw_stats_.raw_min_x, raw_stats_.raw_max_x,
                 raw_stats_.raw_min_y, raw_stats_.raw_max_y,
                 (unsigned long)raw_stats_.sample_count,
                 TOUCH_MAX_X - 1, TOUCH_MAX_Y - 1);
    }
#endif

    // chip V2905 已输出 landscape，swap/mirror 默认全 false；保留参数供特殊贴片方向兼容
    uint16_t sx = raw_x, sy = raw_y;
    if (swap_xy_) std::swap(sx, sy);
    if (sx >= width_)  sx = width_  - 1;
    if (sy >= height_) sy = height_ - 1;
    if (mirror_x_) sx = width_  - 1 - sx;
    if (mirror_y_) sy = height_ - 1 - sy;

    // Gate 3: 速度滤波（过滤 4G RF 诱发的跳变）
    if (touch_.last_time > 0) {
        uint32_t dt_ms = (now - touch_.last_time) / 1000;
        if (dt_ms > 0 && dt_ms < 500) {
            uint32_t dist = std::abs((int)sx - (int)touch_.last_x) +
                            std::abs((int)sy - (int)touch_.last_y);
            if (dist * 1000 / dt_ms > MAX_SPEED_PX_S) return false;
        }
    }

    touch_.last_time = now;
    x = sx;
    y = sy;
    return true;
}

// ============================================================
// 手势识别（host 软件层）
// ============================================================
void Axs5106lTouch::RecognizeGesture(int16_t x, int16_t y, bool pressed) {
    uint64_t now = esp_timer_get_time();

    if (pressed && !gesture_.pressed) {
        // 按下边沿
        gesture_.pressed = true;
        gesture_.long_fired = false;
        gesture_.start_x = x;
        gesture_.start_y = y;
        gesture_.last_x = x;
        gesture_.last_y = y;
        gesture_.press_time = now;
        return;
    }

    if (pressed && gesture_.pressed) {
        // 持续按住：更新末位坐标，检查长按
        gesture_.last_x = x;
        gesture_.last_y = y;
        if (!gesture_.long_fired) {
            uint64_t dur = now - gesture_.press_time;
            int dx = x - gesture_.start_x;
            int dy = y - gesture_.start_y;
            if (dur >= LONG_PRESS_TIME_US && std::abs(dx) + std::abs(dy) < CLICK_MAX_MOVE) {
                gesture_.long_fired = true;
                gesture_.last_click_time = 0;  // 长按禁用双击判定
                ESP_LOGI(TAG, "长按 (%d, %d)", gesture_.start_x, gesture_.start_y);
                if (gesture_callback_) gesture_callback_(TouchGesture::LongPress, gesture_.start_x, gesture_.start_y);
            }
        }
        return;
    }

    if (!pressed && gesture_.pressed) {
        // 释放边沿：识别具体手势
        gesture_.pressed = false;

        if (gesture_.long_fired) {
            gesture_.long_fired = false;
            ESP_LOGI(TAG, "长按释放 (%d, %d)", gesture_.start_x, gesture_.start_y);
            if (gesture_callback_) gesture_callback_(TouchGesture::LongPressRelease, gesture_.start_x, gesture_.start_y);
            return;
        }

        int dx = gesture_.last_x - gesture_.start_x;
        int dy = gesture_.last_y - gesture_.start_y;
        int abs_dx = std::abs(dx);
        int abs_dy = std::abs(dy);
        int manhattan = abs_dx + abs_dy;
        uint64_t dur = now - gesture_.press_time;

        // 点击：位移小 + 时长在合理区间
        if (manhattan < CLICK_MAX_MOVE && dur >= CLICK_MIN_TIME_US && dur < CLICK_MAX_TIME_US) {
            int click_dx = gesture_.start_x - gesture_.last_click_x;
            int click_dy = gesture_.start_y - gesture_.last_click_y;
            int click_dist = std::abs(click_dx) + std::abs(click_dy);
            uint64_t interval = gesture_.press_time - gesture_.last_click_time;

            if (gesture_.last_click_time > 0 &&
                interval < DOUBLE_CLICK_TIME_US && click_dist < DOUBLE_CLICK_DIST) {
                ESP_LOGI(TAG, "双击 (%d, %d)", gesture_.start_x, gesture_.start_y);
                gesture_.last_click_time = 0;  // 清记录，防三击
                if (gesture_callback_) gesture_callback_(TouchGesture::DoubleClick, gesture_.start_x, gesture_.start_y);
            } else {
                ESP_LOGI(TAG, "单击 (%d, %d)", gesture_.start_x, gesture_.start_y);
                gesture_.last_click_time = now;
                gesture_.last_click_x = gesture_.start_x;
                gesture_.last_click_y = gesture_.start_y;
                if (gesture_callback_) gesture_callback_(TouchGesture::SingleClick, gesture_.start_x, gesture_.start_y);
            }
            return;
        }

        // 滑动：位移大
        if (manhattan >= SWIPE_THRESHOLD) {
            gesture_.last_click_time = 0;
            TouchGesture g;
            const char* name;
            if (abs_dx > abs_dy) {
                g    = (dx > 0) ? TouchGesture::SwipeRight : TouchGesture::SwipeLeft;
                name = (dx > 0) ? "右滑" : "左滑";
            } else {
                g    = (dy > 0) ? TouchGesture::SwipeDown  : TouchGesture::SwipeUp;
                name = (dy > 0) ? "下滑" : "上滑";
            }
            ESP_LOGI(TAG, "%s (%d,%d)->(%d,%d)", name,
                     gesture_.start_x, gesture_.start_y,
                     gesture_.last_x, gesture_.last_y);
            if (gesture_callback_) gesture_callback_(g, gesture_.start_x, gesture_.start_y);
        }
    }
}

// ============================================================
// 清理（多次安全：幂等）
// ============================================================
void Axs5106lTouch::Cleanup() {
    if (lvgl_indev_) {
        lv_indev_delete(lvgl_indev_);
        lvgl_indev_ = nullptr;
    }
    if (i2c_handle_) {
        i2c_master_bus_rm_device(i2c_handle_);
        i2c_handle_ = nullptr;
    }
    // RST 保持高，不拉低（复位线和 LCD 电源同属 AUD_VDD，拉低会影响显示）
    gpio_set_level(rst_gpio_, 1);
}

// ============================================================
// 固件升级（封装到 Axs5106lUpgrade 类）
// ============================================================
bool Axs5106lTouch::CheckAndUpgradeFirmware() {
    Axs5106lUpgrade upgrader(i2c_handle_, rst_gpio_);
    switch (upgrader.CheckAndUpgrade()) {
        case Axs5106lUpgradeResult::Success:
            ESP_LOGI(TAG, "触摸芯片固件升级成功");
            return true;   // 调用方会重新 ResetChip
        case Axs5106lUpgradeResult::NotNeeded:
            ESP_LOGI(TAG, "触摸芯片固件已是最新版本");
            return false;
        case Axs5106lUpgradeResult::Failed:
            ESP_LOGW(TAG, "触摸芯片固件升级失败");
            return false;
        case Axs5106lUpgradeResult::I2cError:
            ESP_LOGW(TAG, "I2C 错误，跳过固件升级");
            return false;
    }
    return false;
}
