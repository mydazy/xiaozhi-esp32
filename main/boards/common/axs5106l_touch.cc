/*
 * AXS5106L 触摸屏驱动
 * 功能: 单击、双击、长按、上滑、下滑、左滑、右滑
 * 特性: 自动检测并升级触摸芯片固件
 */

#include "axs5106l_touch.h"
#include "axs5106l_upgrade.h"
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <cmath>
#include <cstring>
#include <algorithm>

static const char* TAG = "Axs5106lTouch";

// AXS5106L 寄存器
#define AXS5106L_I2C_ADDR      0x63
#define AXS5106L_REG_DATA      0x01
#define AXS5106L_REG_FW_VER    0x05
#define AXS5106L_REG_CHIP_ID   0x08

// 触摸数据解析宏
#define AXS_GET_POINT_NUM(buf)  (buf[1])
#define AXS_GET_POINT_X(buf)    (((uint16_t)(buf[2] & 0x0F) << 8) | buf[3])
#define AXS_GET_POINT_Y(buf)    (((uint16_t)(buf[4] & 0x0F) << 8) | buf[5])

// 触摸芯片坐标范围（284x240 屏幕）
#define TOUCH_MAX_X   240
#define TOUCH_MAX_Y   284

// 手势识别参数（优化灵敏度）
#define SWIPE_THRESHOLD      40       // 滑动距离阈值(像素)
#define CLICK_MAX_TIME       400000   // 点击最大时长(us) - 400ms（放宽）
#define CLICK_MIN_TIME       30000    // 点击最小时长(us) - 20ms（降低，提高灵敏度）
#define CLICK_MAX_MOVE       30       // 点击时允许的最大移动距离(像素)（放宽）
#define LONG_PRESS_TIME      600000   // 长按阈值(us) - 600ms（缩短，响应更快）
#define DOUBLE_CLICK_TIME    500000   // 双击间隔(us) - 500ms（放宽，更容易双击）
#define DOUBLE_CLICK_DIST    60       // 双击位置容差(像素)（放宽）

// 抗干扰参数（优化灵敏度 vs 抗干扰平衡）
#define MIN_TOUCH_INTERVAL   50000    // 最小触摸间隔(us) - 50ms（缩短，提高响应）
#define STABLE_TOUCH_COUNT   2        // 需要连续稳定读取2次（约60ms，减少等待）
#define STABLE_TOUCH_DIST    30       // 稳定触摸的位置容差(像素)（放宽，提高灵敏度）
#define MAX_SPEED_THRESHOLD  8000     // 速度过滤阈值(px/s)（放宽）

// I2C 参数
#define I2C_TIMEOUT_MS    50          // I2C超时
#define I2C_MAX_RETRIES   3           // 重试次数

Axs5106lTouch::Axs5106lTouch(i2c_master_bus_handle_t i2c_bus,
                             gpio_num_t rst_gpio,
                             gpio_num_t int_gpio,
                             uint16_t width,
                             uint16_t height,
                             bool swap_xy,
                             bool mirror_x,
                             bool mirror_y)
    : i2c_bus_(i2c_bus),
      i2c_handle_(nullptr),
      rst_gpio_(rst_gpio),
      int_gpio_(int_gpio),
      width_(width),
      height_(height),
      swap_xy_(swap_xy),
      mirror_x_(mirror_x),
      mirror_y_(mirror_y),
      lvgl_indev_(nullptr),
      touch_callback_(nullptr),
      gesture_callback_(nullptr),
      interrupt_installed_(false),
      hardware_initialized_(false),
      reset_gpio_configured_(false),
      int_gpio_configured_(false) {
    // 初始化手势状态
    gesture_ = {false, false, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    // 初始化滤波器
    filter_ = {{0, 0, 0}, {0, 0, 0}, 0, 0, 0, 0, 0};
}

Axs5106lTouch::~Axs5106lTouch() {
    Cleanup();
}

bool Axs5106lTouch::Initialize() {
    // 兼容旧调用：仍然提供一次性初始化入口，
    // 实际上内部已经拆成“硬件初始化”和“LVGL 输入初始化”两个阶段。
    if (!InitializeHardware()) {
        return false;
    }
    return InitializeInput();
}

bool Axs5106lTouch::InitializeHardware() {
    // 这一阶段允许拉共享复位线，因此必须在 LCD 初始化之前执行。
    ESP_LOGI(TAG, "初始化触摸屏: RST=GPIO%d, INT=GPIO%d, 分辨率=%dx%d",
             rst_gpio_, int_gpio_, width_, height_);

    if (!reset_gpio_configured_) {
        gpio_config_t rst_cfg = {
            .pin_bit_mask = (1ULL << rst_gpio_),
            .mode = GPIO_MODE_OUTPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE
        };
        ESP_ERROR_CHECK(gpio_config(&rst_cfg));
        reset_gpio_configured_ = true;
        gpio_set_level(rst_gpio_, 1);
    }

    if (i2c_handle_ == nullptr) {
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
    }

    if (!ResetChip()) {
        return false;
    }

    if (CheckAndUpgradeFirmware()) {
        ESP_LOGI(TAG, "固件已升级，重新复位芯片");
        ResetChip();
    }

    uint8_t chip_id[3] = {0};
    for (int i = 0; i < 5; i++) {
        if (ReadRegister(AXS5106L_REG_CHIP_ID, chip_id, 3)) {
            if ((chip_id[0] | chip_id[1] | chip_id[2]) != 0 &&
                (chip_id[0] & chip_id[1] & chip_id[2]) != 0xFF) {
                ESP_LOGI(TAG, "芯片ID: 0x%02X%02X%02X", chip_id[0], chip_id[1], chip_id[2]);
                break;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(100));
        if (i == 4) {
            ESP_LOGE(TAG, "无法读取有效芯片ID");
            return false;
        }
    }

    uint8_t fw_ver[2] = {0};
    if (ReadRegister(AXS5106L_REG_FW_VER, fw_ver, 2)) {
        ESP_LOGI(TAG, "固件版本: %u", (fw_ver[0] << 8) | fw_ver[1]);
    }

    hardware_initialized_ = true;
    ESP_LOGI(TAG, "触摸芯片硬件初始化完成");
    return true;
}

bool Axs5106lTouch::InitializeInput() {
    // 这里只做中断和 LVGL 输入设备注册，避免在 LCD 点亮后再次触发共享复位线。
    if (!hardware_initialized_) {
        ESP_LOGE(TAG, "触摸芯片尚未完成硬件初始化，无法注册 LVGL 输入");
        return false;
    }

    if (!int_gpio_configured_) {
        gpio_config_t int_cfg = {
            .pin_bit_mask = (1ULL << int_gpio_),
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = GPIO_PULLUP_ENABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_NEGEDGE
        };
        ESP_ERROR_CHECK(gpio_config(&int_cfg));
        int_gpio_configured_ = true;
    }

    if (lvgl_indev_ != nullptr) {
        return true;
    }

    esp_err_t ret = gpio_install_isr_service(0);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "GPIO ISR 服务安装失败");
        return false;
    }

    if (!interrupt_installed_) {
        ret = gpio_isr_handler_add(int_gpio_, GpioIsrHandler, this);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "GPIO ISR 处理函数添加失败");
            return false;
        }
        interrupt_installed_ = true;
    }

    lvgl_indev_ = lv_indev_create();
    if (lvgl_indev_ == nullptr) {
        ESP_LOGE(TAG, "LVGL 输入设备创建失败");
        if (interrupt_installed_) {
            gpio_isr_handler_remove(int_gpio_);
            interrupt_installed_ = false;
        }
        return false;
    }

    lv_indev_set_type(lvgl_indev_, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(lvgl_indev_, LvglReadCallback);
    lv_indev_set_user_data(lvgl_indev_, this);

    ESP_LOGI(TAG, "触摸屏已接入 LVGL");
    return true;
}

bool Axs5106lTouch::ResetChip() {
    // 这根 RST 线和 LCD 共用时，下面的硬复位会把 LCD 也一起复位，
    // 所以只能放在 LCD 初始化之前，或者在明确知道可以重建显示状态时调用。
    // 1. 软件复位命令（原始驱动 axs_reset 必须的初始化步骤）
    const uint8_t rst_cmd[5] = {0xB3, 0x55, 0xAA, 0x34, 0x01};
    WriteRegister(0xF0, rst_cmd, 5);

    // 2. 硬件复位时序
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
    const uint8_t sleep_cmd[1] = {0x03};
    WriteRegister(0x19, sleep_cmd, 1);
    gpio_set_level(rst_gpio_, 1);
}

void Axs5106lTouch::Resume() {
    ResetChip();
    vTaskDelay(pdMS_TO_TICKS(10));
}

bool Axs5106lTouch::WriteRegister(uint8_t reg, const uint8_t* data, size_t len) {
    if (i2c_handle_ == nullptr) return false;

    uint8_t buf[16];
    if (len > sizeof(buf) - 1) return false;

    buf[0] = reg;
    memcpy(&buf[1], data, len);

    for (int retry = 0; retry < I2C_MAX_RETRIES; retry++) {
        esp_err_t ret = i2c_master_transmit(i2c_handle_, buf, len + 1, I2C_TIMEOUT_MS);
        if (ret == ESP_OK) return true;
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    return false;
}

bool Axs5106lTouch::ReadRegister(uint8_t reg, uint8_t* data, size_t len) {
    if (i2c_handle_ == nullptr) return false;

    for (int retry = 0; retry < I2C_MAX_RETRIES; retry++) {
        esp_err_t ret = i2c_master_transmit(i2c_handle_, &reg, 1, I2C_TIMEOUT_MS);
        if (ret != ESP_OK) {
            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }

        ret = i2c_master_receive(i2c_handle_, data, len, I2C_TIMEOUT_MS);
        if (ret == ESP_OK) return true;

        vTaskDelay(pdMS_TO_TICKS(5));
    }
    return false;
}

void IRAM_ATTR Axs5106lTouch::GpioIsrHandler(void* arg) {
    // ISR 中不应调用 std::function 或任何可能分配内存的操作
    // 触摸数据由 LVGL 定期轮询读取，不需要 ISR 回调
    // 保留此函数只是为了满足 GPIO ISR 注册要求
    (void)arg;  // 避免未使用参数警告
}

void Axs5106lTouch::HandleTouchInterrupt() {
    if (touch_callback_) {
        touch_callback_();
    }
}

void Axs5106lTouch::LvglReadCallback(lv_indev_t* indev, lv_indev_data_t* data) {
    auto* self = static_cast<Axs5106lTouch*>(lv_indev_get_user_data(indev));
    if (self == nullptr) {
        data->state = LV_INDEV_STATE_RELEASED;
        return;
    }

    uint16_t x, y;
    bool pressed;

    if (self->ParseTouchData(x, y, pressed)) {
        data->point.x = x;
        data->point.y = y;
        data->state = pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
        self->RecognizeGesture(x, y, pressed);
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
        if (self->gesture_.is_pressed) {
            self->RecognizeGesture(0, 0, false);
        }
    }
}

bool Axs5106lTouch::ParseTouchData(uint16_t& x, uint16_t& y, bool& pressed) {
    uint8_t buf[8] = {0};

    if (!ReadRegister(AXS5106L_REG_DATA, buf, 8)) {
        return false;
    }

    uint8_t points = AXS_GET_POINT_NUM(buf);
    if (points == 0 || points > 1) {
        pressed = false;
        // 释放时重置滤波器
        if (filter_.count > 0) {
            filter_.count = 0;
            filter_.idx = 0;
            ESP_LOGD(TAG, "触摸释放，重置滤波器");
        }
        return false;
    }

    // 获取原始坐标
    uint16_t raw_x = AXS_GET_POINT_X(buf);
    uint16_t raw_y = AXS_GET_POINT_Y(buf);

    // 验证坐标范围（防止 4G 电磁干扰导致的 I2C 噪声数据）
    if (raw_x == 0xFFF && raw_y == 0xFFF) {
        return false;
    }

    // 超出物理范围的坐标
    if (raw_x > TOUCH_MAX_Y + 50 || raw_y > TOUCH_MAX_X + 50) {
        return false;
    }

    // 坐标转换
    uint16_t screen_x = (raw_x >= TOUCH_MAX_Y) ? (TOUCH_MAX_Y - 1) : raw_x;
    uint16_t screen_y = (raw_y > TOUCH_MAX_X) ? TOUCH_MAX_X : raw_y;

    // 应用坐标变换（与显示配置独立）
    if (swap_xy_) std::swap(screen_x, screen_y);
    if (mirror_x_) screen_x = width_ - 1 - screen_x;
    if (mirror_y_) screen_y = height_ - 1 - screen_y;

    // 边界检查
    if (screen_x >= width_) screen_x = width_ - 1;
    if (screen_y >= height_) screen_y = height_ - 1;

    // ✅ 速度异常检测（抗4G干扰核心）
    uint64_t now = esp_timer_get_time();
    if (filter_.last_time > 0) {
        uint32_t dt = (now - filter_.last_time) / 1000;  // 转换为ms

        if (dt > 0 && dt < 500) {  // 只检测500ms内的移动
            int16_t dx = screen_x - filter_.last_x;
            int16_t dy = screen_y - filter_.last_y;
            uint32_t dist = std::abs(dx) + std::abs(dy);  // 曼哈顿距离
            uint32_t speed = dist * 1000 / dt;  // px/s

            if (speed > MAX_SPEED_THRESHOLD) {
                return false;  // 丢弃异常速度触摸
            }
        }
    }

    // ✅ 滑动平均滤波（3点滤波，平滑坐标抖动）
    filter_.x_buf[filter_.idx] = screen_x;
    filter_.y_buf[filter_.idx] = screen_y;
    filter_.idx = (filter_.idx + 1) % 3;
    if (filter_.count < 3) filter_.count++;

    // 计算平均值
    int32_t sum_x = 0, sum_y = 0;
    for (int i = 0; i < filter_.count; i++) {
        sum_x += filter_.x_buf[i];
        sum_y += filter_.y_buf[i];
    }
    int16_t filtered_x = sum_x / filter_.count;
    int16_t filtered_y = sum_y / filter_.count;

    // 更新滤波器状态（用于下次速度计算）
    filter_.last_x = filtered_x;
    filter_.last_y = filtered_y;
    filter_.last_time = now;

    x = filtered_x;
    y = filtered_y;
    pressed = true;

    return true;
}

void Axs5106lTouch::RecognizeGesture(int16_t x, int16_t y, bool pressed) {
    uint64_t now = esp_timer_get_time();

    if (pressed && !gesture_.is_pressed) {
        // 抗干扰：检查最小触摸间隔
        if (gesture_.last_touch_time > 0) {
            uint64_t interval = now - gesture_.last_touch_time;
            if (interval < MIN_TOUCH_INTERVAL) {
                return;  // 忽略间隔过短的触摸
            }
        }

        // ✅ 抗干扰：触摸点稳定性检查（需要连续稳定读取）
        if (gesture_.stable_count > 0) {
            int16_t dx = x - gesture_.stable_x;
            int16_t dy = y - gesture_.stable_y;
            int16_t dist = std::sqrt(dx * dx + dy * dy);

            if (dist < STABLE_TOUCH_DIST) {
                gesture_.stable_count++;
                if (gesture_.stable_count >= STABLE_TOUCH_COUNT) {
                    // 触摸点稳定，接受此次触摸
                    ESP_LOGD(TAG, "触摸点稳定 (%d, %d), 稳定次数: %d", x, y, gesture_.stable_count);
                } else {
                    // 尚未达到稳定次数要求，继续等待
                    return;
                }
            } else {
                // 位置不稳定，重置计数器
                gesture_.stable_x = x;
                gesture_.stable_y = y;
                gesture_.stable_count = 1;
                return;
            }
        } else {
            // 首次触摸，开始稳定性检查
            gesture_.stable_x = x;
            gesture_.stable_y = y;
            gesture_.stable_count = 1;
            return;
        }

        // 按下
        gesture_.is_pressed = true;
        gesture_.long_press_triggered = false;
        gesture_.start_x = x;
        gesture_.start_y = y;
        gesture_.last_x = x;
        gesture_.last_y = y;
        gesture_.press_time = now;
        gesture_.last_touch_time = now;  // ✅ 记录此次触摸时间
        gesture_.stable_count = 0;        // ✅ 重置稳定计数器

    } else if (pressed && gesture_.is_pressed) {
        // 持续按住
        gesture_.last_x = x;
        gesture_.last_y = y;

        // 检测长按（按住期间检测，只触发一次）
        if (!gesture_.long_press_triggered) {
            uint64_t duration = now - gesture_.press_time;
            int16_t dx = gesture_.last_x - gesture_.start_x;
            int16_t dy = gesture_.last_y - gesture_.start_y;
            int16_t distance = std::sqrt(dx * dx + dy * dy);

            if (duration >= LONG_PRESS_TIME && distance < CLICK_MAX_MOVE) {
                gesture_.long_press_triggered = true;
                gesture_.last_click_time = 0;  // 清除双击记录，防止松开被误判为双击
                ESP_LOGI(TAG, "长按 (%d, %d)", gesture_.start_x, gesture_.start_y);
                if (gesture_callback_) {
                    gesture_callback_(TouchGesture::LongPress, gesture_.start_x, gesture_.start_y);
                }
            }
        }

    } else if (!pressed && gesture_.is_pressed) {
        // 释放 - 识别手势
        gesture_.is_pressed = false;

        // 如果长按已触发，不再识别其他手势
        if (gesture_.long_press_triggered) {
            gesture_.long_press_triggered = false;
            return;
        }

        int16_t dx = gesture_.last_x - gesture_.start_x;
        int16_t dy = gesture_.last_y - gesture_.start_y;
        int16_t abs_dx = std::abs(dx);
        int16_t abs_dy = std::abs(dy);
        int16_t distance = std::sqrt(dx * dx + dy * dy);
        uint64_t duration = now - gesture_.press_time;

        TouchGesture gesture = TouchGesture::None;

        if (distance < CLICK_MAX_MOVE && duration >= CLICK_MIN_TIME && duration < CLICK_MAX_TIME) {
            // 可能是单击或双击（时长在50-300ms之间）
            // 检查是否为双击（与上次单击间隔短且位置接近）
            int16_t click_dx = gesture_.start_x - gesture_.last_click_x;
            int16_t click_dy = gesture_.start_y - gesture_.last_click_y;
            int16_t click_dist = std::sqrt(click_dx * click_dx + click_dy * click_dy);
            // 双击间隔：从第一次释放到第二次按下（不包括第二次按住的时长）
            uint64_t click_interval = gesture_.press_time - gesture_.last_click_time;

            if (gesture_.last_click_time > 0 &&
                click_interval < DOUBLE_CLICK_TIME &&
                click_dist < DOUBLE_CLICK_DIST) {
                // 双击
                gesture = TouchGesture::DoubleClick;
                ESP_LOGI(TAG, "双击 (%d, %d)", gesture_.start_x, gesture_.start_y);
                // 重置上次点击记录，避免三击误判
                gesture_.last_click_time = 0;
            } else {
                // 单击
                gesture = TouchGesture::SingleClick;
                ESP_LOGI(TAG, "单击 (%d, %d)", gesture_.start_x, gesture_.start_y);
                // 记录本次点击，用于下次双击检测
                gesture_.last_click_time = now;
                gesture_.last_click_x = gesture_.start_x;
                gesture_.last_click_y = gesture_.start_y;
            }

        } else if (distance >= SWIPE_THRESHOLD) {
            // 滑动（清除双击记录）
            gesture_.last_click_time = 0;

            if (abs_dx > abs_dy) {
                gesture = (dx > 0) ? TouchGesture::SwipeRight : TouchGesture::SwipeLeft;
                ESP_LOGI(TAG, "%s (%d,%d)->(%d,%d)",
                         (dx > 0) ? "右滑" : "左滑",
                         gesture_.start_x, gesture_.start_y,
                         gesture_.last_x, gesture_.last_y);
            } else {
                gesture = (dy > 0) ? TouchGesture::SwipeDown : TouchGesture::SwipeUp;
                ESP_LOGI(TAG, "%s (%d,%d)->(%d,%d)",
                         (dy > 0) ? "下滑" : "上滑",
                         gesture_.start_x, gesture_.start_y,
                         gesture_.last_x, gesture_.last_y);
            }
        }

        if (gesture != TouchGesture::None && gesture_callback_) {
            gesture_callback_(gesture, gesture_.start_x, gesture_.start_y);
        }
    }
}

void Axs5106lTouch::SetTouchCallback(std::function<void()> callback) {
    touch_callback_ = callback;
}

void Axs5106lTouch::SetGestureCallback(TouchGestureCallback callback) {
    gesture_callback_ = callback;
}

void Axs5106lTouch::Cleanup() {
    ESP_LOGI(TAG, "[touch cleanup] enter");
    if (interrupt_installed_) {
        ESP_LOGI(TAG, "[touch cleanup] before remove isr");
        gpio_isr_handler_remove(int_gpio_);
        interrupt_installed_ = false;
    }

    if (lvgl_indev_) {
        ESP_LOGI(TAG, "[touch cleanup] before lv_indev_delete");
        lv_indev_delete(lvgl_indev_);
        lvgl_indev_ = nullptr;
    }

    if (i2c_handle_) {
        ESP_LOGI(TAG, "[touch cleanup] before rm i2c device");
        i2c_master_bus_rm_device(i2c_handle_);
        i2c_handle_ = nullptr;
    }

    hardware_initialized_ = false;
    if (reset_gpio_configured_) {
        // 共享复位线不能在清理时拉低，否则已经初始化好的 LCD 会被一起按在复位态。
        gpio_set_level(rst_gpio_, 1);
    }
    ESP_LOGI(TAG, "[touch cleanup] exit");
}

bool Axs5106lTouch::CheckAndUpgradeFirmware() {
    // 创建升级模块实例
    Axs5106lUpgrade upgrader(i2c_handle_, rst_gpio_);

    // 执行固件检查和升级
    Axs5106lUpgradeResult result = upgrader.CheckAndUpgrade();

    switch (result) {
        case Axs5106lUpgradeResult::Success:
            ESP_LOGI(TAG, "触摸芯片固件升级成功");
            return true;  // 需要重新复位

        case Axs5106lUpgradeResult::NotNeeded:
            ESP_LOGI(TAG, "触摸芯片固件已是最新版本");
            return false;  // 不需要重新复位

        case Axs5106lUpgradeResult::Failed:
            ESP_LOGW(TAG, "触摸芯片固件升级失败，继续使用当前固件");
            return false;

        case Axs5106lUpgradeResult::I2cError:
            ESP_LOGW(TAG, "I2C 通信错误，跳过固件升级");
            return false;

        default:
            return false;
    }
}
