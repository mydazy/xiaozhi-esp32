#include "at_uart.h"
#include <esp_log.h>
#include <esp_err.h>
#include <esp_pm.h>
#include <esp_sleep.h>
#include <algorithm>
#include <cstring>
#include <cstdlib>
#include <cstdint>
#include <sstream>
#include <chrono>

#define TAG "AtUart"

// AtUart 构造函数实现
AtUart::AtUart(gpio_num_t tx_pin, gpio_num_t rx_pin, gpio_num_t dtr_pin, gpio_num_t ri_pin)
    : tx_pin_(tx_pin), rx_pin_(rx_pin), dtr_pin_(dtr_pin), ri_pin_(ri_pin), uart_num_(UART_NUM),
      baud_rate_(115200), initialized_(false), dtr_pin_state_(false),
      pm_lock_(nullptr), ri_pm_lock_(nullptr), ri_pm_lock_acquired_(false) {
    // Create power management lock for DTR operations
    esp_pm_lock_create(ESP_PM_CPU_FREQ_MAX, 0, "at_uart_pm_lock", &pm_lock_);
    // Create power management lock for RI pin operations
    if (ri_pin_ != GPIO_NUM_NC) {
        esp_pm_lock_create(ESP_PM_CPU_FREQ_MAX, 0, "at_uart_ri_pm_lock", &ri_pm_lock_);
    }
}

AtUart::~AtUart() {
    if (receive_task_handle_) {
        vTaskDelete(receive_task_handle_);
    }
    if (event_task_handle_) {
        vTaskDelete(event_task_handle_);
    }
    if (event_group_handle_) {
        vEventGroupDelete(event_group_handle_);
    }
    if (initialized_) {
        // Remove RI pin ISR handler if configured
        if (ri_pin_ != GPIO_NUM_NC) {
            gpio_isr_handler_remove(ri_pin_);
        }
        // 标准 driver 卸载（内部释放 event_queue_handle_ + ring buffer + 中断）
        uart_driver_delete(uart_num_);
        event_queue_handle_ = nullptr;
    }
    if (ri_pm_lock_) {
        if (ri_pm_lock_acquired_) {
            esp_pm_lock_release(ri_pm_lock_);
        }
        esp_pm_lock_delete(ri_pm_lock_);
    }
    if (pm_lock_) {
        esp_pm_lock_delete(pm_lock_);
    }
}

void AtUart::Initialize() {
    if (initialized_) {
        return;
    }

    event_group_handle_ = xEventGroupCreate();
    if (!event_group_handle_) {
        ESP_LOGE(TAG, "创建事件组失败");
        return;
    }

    uart_config_t uart_config = {};
    uart_config.baud_rate = baud_rate_;
    uart_config.data_bits = UART_DATA_8_BITS;
    uart_config.parity = UART_PARITY_DISABLE;
    uart_config.stop_bits = UART_STOP_BITS_1;
    uart_config.source_clk = UART_SCLK_DEFAULT;

    ESP_ERROR_CHECK(uart_driver_install(uart_num_, AT_UART_RX_BUFFER_SIZE, 0, 16, &event_queue_handle_, ESP_INTR_FLAG_IRAM));
    ESP_ERROR_CHECK(uart_param_config(uart_num_, &uart_config));
    ESP_ERROR_CHECK(uart_set_pin(uart_num_, tx_pin_, rx_pin_, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    
    // Enable pull-up on RX pin
    gpio_set_pull_mode(rx_pin_, GPIO_PULLUP_ONLY);

    if (dtr_pin_ != GPIO_NUM_NC) {
        gpio_config_t config = {};
        config.pin_bit_mask = (1ULL << dtr_pin_);
        config.mode = GPIO_MODE_OUTPUT;
        config.pull_up_en = GPIO_PULLUP_DISABLE;
        config.pull_down_en = GPIO_PULLDOWN_DISABLE;
        config.intr_type = GPIO_INTR_DISABLE;
        gpio_config(&config);
        gpio_set_level(dtr_pin_, 0);
        dtr_pin_state_ = false;  // 记录初始状态为低电平
    }

    // Configure RI pin as input with interrupt
    if (ri_pin_ != GPIO_NUM_NC) {
        gpio_config_t ri_config = {};
        ri_config.pin_bit_mask = (1ULL << ri_pin_);
        ri_config.mode = GPIO_MODE_INPUT;
        ri_config.pull_up_en = GPIO_PULLUP_ENABLE;  // Enable pull-up for RI pin
        ri_config.pull_down_en = GPIO_PULLDOWN_DISABLE;
        ri_config.intr_type = GPIO_INTR_LOW_LEVEL;  // Trigger on falling edge (low level)
        gpio_config(&ri_config);

        gpio_wakeup_enable(ri_pin_, GPIO_INTR_LOW_LEVEL);

        // Add ISR handler for RI pin
        gpio_isr_handler_add(ri_pin_, RiPinIsrHandler, this);
    }

    // ReceiveTask: 消费 UART event_queue → 读 ring buffer → append rx_buffer_
    xTaskCreatePinnedToCore([](void* arg) {
        auto at_uart = (AtUart*)arg;
        at_uart->ReceiveTask();
        vTaskDelete(NULL);
    }, "modem_receive", 2048 * 3, this, 8, &receive_task_handle_, 0 /* Core 0 */);

    // EventTask: 解析 rx_buffer_ + 派发 URC，优先级 5（低于 ReceiveTask 让 RX 先消化）
    xTaskCreatePinnedToCore([](void* arg) {
        auto at_uart = (AtUart*)arg;
        at_uart->EventTask();
        vTaskDelete(NULL);
    }, "modem_event", 2048 * 3, this, 5, &event_task_handle_, 0 /* Core 0 */);

    initialized_ = true;
}

void AtUart::ReceiveTask() {
    // 标准 UART driver 模式：从 event_queue_handle_ 收 uart_event_t，按事件类型处理。
    uart_event_t event;
    while (true) {
        if (xQueueReceive(event_queue_handle_, &event, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        switch (event.type) {
        case UART_DATA: {
            size_t available = 0;
            uart_get_buffered_data_len(uart_num_, &available);
            if (available == 0) break;
            std::string chunk;
            chunk.resize(available);
            int read_len = uart_read_bytes(uart_num_, chunk.data(), available, pdMS_TO_TICKS(20));
            if (read_len <= 0) break;
            {
                std::lock_guard<std::mutex> lock(rx_buffer_mutex_);
                rx_buffer_.append(chunk.data(), read_len);
            }
            xEventGroupSetBits(event_group_handle_, AT_EVENT_PARSE_NEEDED);
            break;
        }
        case UART_FIFO_OVF:
        case UART_BUFFER_FULL: {
            // RX 路径数据丢失 → TCP/WS 流必然错位（实测 v32 日志：50s 卡死在脏数据上挣扎）。
            // 立即清 buffer + 触发上层"网络断开"事件让 Ml307AtModem 触发 Disconnected,
            // 走通 Application::HandleNetworkDisconnectedEvent → CloseAudioChannel → L6 重连。
            ESP_LOGW(TAG, "UART %s, notify upper layer to reconnect",
                     event.type == UART_FIFO_OVF ? "FIFO overflow" : "ring buffer full");
            uart_flush_input(uart_num_);
            xQueueReset(event_queue_handle_);
            {
                std::lock_guard<std::mutex> lock(rx_buffer_mutex_);
                rx_buffer_.clear();
            }
            {
                std::lock_guard<std::mutex> lock(urc_mutex_);
                for (auto& cb : urc_callbacks_) {
                    cb("__UART_OVERFLOW__", {});
                }
            }
            break;
        }
        case UART_BREAK:
        case UART_PARITY_ERR:
        case UART_FRAME_ERR:
            // 波特率切换瞬间常见，吞掉不报警
            break;
        default:
            break;
        }
    }
}

// HTTP Binary Receive Mode 引用计数（Patch B · 2026-04-29 修引用计数语义）
// 多 Ml307Http 实例并发时（如 MP3 下载 + OTA 上报），原单 bool 会被后退者关掉前进者，
void AtUart::SetHttpBinaryMode(bool enabled) {
    if (enabled) {
        http_binary_mode_count_.fetch_add(1, std::memory_order_acq_rel);
    } else {
        int prev = http_binary_mode_count_.fetch_sub(1, std::memory_order_acq_rel);
        if (prev <= 0) {
            // 防御：不应发生（所有 Ml307Http instance Open/Close 配对）
            http_binary_mode_count_.store(0, std::memory_order_release);
            ESP_LOGW(TAG, "SetHttpBinaryMode(false) underflow, clamped to 0");
        }
    }
}

void AtUart::EventTask() {
    // 解析任务（低优先级）：从 rx_buffer_ 提取 URC 并派发，与 ReceiveTask 解耦。
    while (true) {
        auto bits = xEventGroupWaitBits(event_group_handle_,
            AT_EVENT_PARSE_NEEDED | AT_EVENT_RI_PIN_INT,
            pdTRUE, pdFALSE, portMAX_DELAY);

        if (bits & AT_EVENT_PARSE_NEEDED) {
            // Parse all available responses
            while (ParseResponse()) {}
        }

        if (ri_pin_ != GPIO_NUM_NC) {
            if (bits & AT_EVENT_RI_PIN_INT) {
                // RI pin went low - acquire PM lock to prevent sleep
                if (!ri_pm_lock_acquired_) {
                    esp_pm_lock_acquire(ri_pm_lock_);
                    ri_pm_lock_acquired_ = true;
                    ESP_LOGD(TAG, "RI pin went low, PM lock acquired");
                }
            } else {
                // Release RI PM lock when data is available (modem has data to send)
                if (ri_pm_lock_acquired_) {
                    esp_pm_lock_release(ri_pm_lock_);
                    ri_pm_lock_acquired_ = false;
                    gpio_intr_enable(ri_pin_);
                    ESP_LOGD(TAG, "Data available, RI PM lock released");
                }
            }
        }
    }
}

static bool is_number(const std::string& s) {
    return !s.empty() && std::all_of(s.begin(), s.end(), ::isdigit) && s.length() < 10;
}

bool AtUart::ParseResponse() {
    std::string command, values;
    std::string::size_type end_pos;
    
    // Lock rx_buffer_ for the duration of parsing
    {
        std::lock_guard<std::mutex> lock(rx_buffer_mutex_);
        
        if (rx_buffer_.empty()) {
            return false;
        }
        
        if (wait_for_response_ && rx_buffer_[0] == '>') {
            rx_buffer_.erase(0, 1);
            xEventGroupSetBits(event_group_handle_, AT_EVENT_COMMAND_DONE);
            return true;
        }

        // Patch B · HTTP Binary Receive Mode（来自 189 v3.5.3 验证版）
        // 必须在 find("\r\n") 之前拦截：binary content 内可能包含 \r\n 字节，
        // 走默认分支会被截断。仅在引用计数 >0 时启用，默认 0 不改变 HEX 行为。
        // EC801E 不调 SetHttpBinaryMode，此分支为 dead code 不影响 +QHTTP*/+QIURC 解析。
        const bool binary_active = http_binary_mode_count_.load() > 0;
        if (binary_active && rx_buffer_.size() > 22 &&
            memcmp(rx_buffer_.c_str(), "+MHTTPURC: \"header\"", 19) == 0) {
            return ParseBinaryHttpHeader();
        }
        if (binary_active && rx_buffer_.size() > 24 &&
            memcmp(rx_buffer_.c_str(), "+MHTTPURC: \"content\"", 20) == 0) {
            return ParseBinaryHttpContent();
        }

        end_pos = rx_buffer_.find("\r\n");
        if (end_pos == std::string::npos) {
            // FIXME: for +MHTTPURC: "ind", missing newline
            if (rx_buffer_.size() >= 16 && memcmp(rx_buffer_.c_str(), "+MHTTPURC: \"ind\"", 16) == 0) {
                // Find the end of this line and add \r\n if missing
                auto next_plus = rx_buffer_.find("+", 1);
                if (next_plus != std::string::npos) {
                    // Insert \r\n before the next + command
                    rx_buffer_.insert(next_plus, "\r\n");
                } else {
                    // Append \r\n at the end
                    rx_buffer_.append("\r\n");
                }
                end_pos = rx_buffer_.find("\r\n");
            } else {
                return false;
            }
        }

        // Ignore empty lines
        if (end_pos == 0) {
            rx_buffer_.erase(0, 2);
            return true;
        }

        if (debug_) {
            ESP_LOGI(TAG, "<< %.64s (%u bytes) [%02x%02x%02x]", rx_buffer_.substr(0, end_pos).c_str(), end_pos,
                rx_buffer_[0], rx_buffer_[1], rx_buffer_[2]);
        }

        // Parse "+CME ERROR: 123,456,789"
        if (rx_buffer_[0] == '+') {
            auto pos = rx_buffer_.find(": ");
            if (pos == std::string::npos || pos > end_pos) {
                command = rx_buffer_.substr(1, end_pos - 1);
            } else {
                command = rx_buffer_.substr(1, pos - 1);
                values = rx_buffer_.substr(pos + 2, end_pos - pos - 2);
            }
            rx_buffer_.erase(0, end_pos + 2);
            // Will call HandleUrc after releasing lock
        } else if (rx_buffer_.size() >= 4 && rx_buffer_[0] == 'O' && rx_buffer_[1] == 'K' && rx_buffer_[2] == '\r' && rx_buffer_[3] == '\n') {
            rx_buffer_.erase(0, 4);
            xEventGroupSetBits(event_group_handle_, AT_EVENT_COMMAND_DONE);
            return true;
        } else if (rx_buffer_.size() >= 7 && rx_buffer_[0] == 'E' && rx_buffer_[1] == 'R' && rx_buffer_[2] == 'R' && rx_buffer_[3] == 'O' && rx_buffer_[4] == 'R' && rx_buffer_[5] == '\r' && rx_buffer_[6] == '\n') {
            rx_buffer_.erase(0, 7);
            xEventGroupSetBits(event_group_handle_, AT_EVENT_COMMAND_ERROR);
            return true;
        } else if (rx_buffer_[0] == 0xE0) { // 4G wake up MCU, just ignore
            rx_buffer_.erase(0, end_pos + 2);
            return true;
        } else {
            std::lock_guard<std::mutex> response_lock(mutex_);
            response_ = rx_buffer_.substr(0, end_pos);
            rx_buffer_.erase(0, end_pos + 2);
            return true;
        }
    }
    
    // Handle URC outside the rx_buffer_ lock to avoid blocking ReceiveTask
    if (!command.empty()) {
        // Parse "string", int, int, ... into AtArgumentValue
        std::vector<AtArgumentValue> arguments;
        std::istringstream iss(values);
        std::string item;
        while (std::getline(iss, item, ',')) {
            AtArgumentValue argument;
            // 🔴 修崩溃：std::stod / std::stoi 在解析非法字符串时会抛 std::invalid_argument，
            // 项目启用 CONFIG_COMPILER_CXX_EXCEPTIONS=y 但 caller 未 try-catch
            // → 异常上抛触发 std::terminate → __cxa_throw → abort() 重启循环。
            // 已知触发场景：
            //   - 空字段（连续两个逗号 ",,"，item="" → stod/stoi 都崩）
            //   - 含 "." 但不是合法浮点（IP 地址 "10.0.0.1" 第一段合法，但 "v.1" 类不合法）
            //   - URC 字段含特殊字符（如 GPS 模糊数据）
            // 修法：try-catch 兜底 + ESP_LOGW 输出原始 item / command，便于下次崩溃前定位。
            if (!item.empty() && item.front() == '"') {
                argument.type = AtArgumentValue::Type::String;
                argument.string_value = (item.size() >= 2) ? item.substr(1, item.size() - 2) : "";
            } else if (item.find(".") != std::string::npos) {
                try {
                    argument.double_value = std::stod(item);
                    argument.type = AtArgumentValue::Type::Double;
                } catch (const std::exception& e) {
                    ESP_LOGW(TAG, "stod failed for cmd=%s item='%s' (%s) → 降级字符串",
                             command.c_str(), item.c_str(), e.what());
                    argument.type = AtArgumentValue::Type::String;
                    argument.string_value = item;
                }
            } else if (is_number(item)) {
                try {
                    argument.int_value = std::stoi(item);
                    argument.string_value = item;
                    argument.type = AtArgumentValue::Type::Int;
                } catch (const std::exception& e) {
                    ESP_LOGW(TAG, "stoi failed for cmd=%s item='%s' (%s) → 降级字符串",
                             command.c_str(), item.c_str(), e.what());
                    argument.type = AtArgumentValue::Type::String;
                    argument.string_value = std::move(item);
                }
            } else {
                argument.type = AtArgumentValue::Type::String;
                argument.string_value = std::move(item);
            }
            arguments.push_back(argument);
        }

        HandleUrc(command, arguments);
        return true;
    }
    
    return false;
}

// Patch B · HTTP Binary Receive Mode parsers
// 来自 189 v3.5.3（小智 fork）已经线上验证。仅 ML307 模组生效（EC801E HTTP 走 TCP-socket 路径不触发）。
// 注意：本函数在 ParseResponse() 持 rx_buffer_mutex_ 期间调用，并在持锁期间调用 HandleUrc()。
// HandleUrc 内部用 urc_mutex_ 保护，与 rx_buffer_mutex_ 无锁序冲突；callback 不应反向调 ParseResponse。
//
// 二进制模式解析 +MHTTPURC: "header",<httpid>,<status_code>,<header_len>,<header_text>\r\n
// header_text 是 ASCII（HTTP 响应头），但作为整体走二进制读取避免被 \r\n 截断。
bool AtUart::ParseBinaryHttpHeader() {
    const char* buf = rx_buffer_.c_str();
    size_t buf_len = rx_buffer_.size();

    // 跳过 +MHTTPURC: "header" 前缀（19字节）
    size_t pos = 19;
    if (pos >= buf_len || buf[pos] != ',') {
        return false;
    }
    pos++;  // 跳过 "header" 后的逗号

    // 解析 httpid, status_code, header_len（3个整数字段）
    int fields[3] = {0, 0, 0};
    for (int i = 0; i < 3; i++) {
        int value = 0;
        bool found_digit = false;
        while (pos < buf_len && buf[pos] >= '0' && buf[pos] <= '9') {
            value = value * 10 + (buf[pos] - '0');
            found_digit = true;
            pos++;
        }
        if (!found_digit || pos >= buf_len) {
            return false;  // 数据不完整
        }
        fields[i] = value;

        if (buf[pos] != ',') {
            return false;
        }
        pos++;
    }

    int header_len = fields[2];

    // 检查是否有足够的数据: 前缀 + header_len + \r\n
    size_t total_needed = pos + header_len + 2;
    if (buf_len < total_needed) {
        return false;  // 数据不完整，等待更多数据
    }

    // 提取完整的 header 文本数据
    std::string header_data(buf + pos, header_len);

    // 构造参数（与 HEX 模式保持相同的参数结构）
    std::vector<AtArgumentValue> arguments;

    AtArgumentValue arg0;
    arg0.type = AtArgumentValue::Type::String;
    arg0.string_value = "header";
    arguments.push_back(arg0);

    AtArgumentValue arg1;
    arg1.type = AtArgumentValue::Type::Int;
    arg1.int_value = fields[0];  // httpid
    arguments.push_back(arg1);

    AtArgumentValue arg2;
    arg2.type = AtArgumentValue::Type::Int;
    arg2.int_value = fields[1];  // status_code
    arguments.push_back(arg2);

    AtArgumentValue arg3;
    arg3.type = AtArgumentValue::Type::Int;
    arg3.int_value = header_len;
    arguments.push_back(arg3);

    AtArgumentValue arg4;
    arg4.type = AtArgumentValue::Type::String;
    arg4.string_value = std::move(header_data);
    arguments.push_back(arg4);

    rx_buffer_.erase(0, total_needed);
    HandleUrc("MHTTPURC", arguments);
    return true;
}

// 二进制模式解析 +MHTTPURC: "content",<httpid>,<content_len>,<sum_len>,<cur_len>,<binary_data>\r\n
// 前缀部分是 ASCII，<binary_data> 是原始二进制（可能包含 \r\n、逗号等）
bool AtUart::ParseBinaryHttpContent() {
    const char* buf = rx_buffer_.c_str();
    size_t buf_len = rx_buffer_.size();

    // 跳过 +MHTTPURC: "content", 前缀（20字节）
    size_t pos = 20;
    if (pos >= buf_len || buf[pos] != ',') {
        return false;
    }
    pos++;  // 跳过 "content" 后的逗号

    // 解析 httpid, content_len, sum_len, cur_len（4个整数字段）
    int fields[4] = {0, 0, 0, 0};
    for (int i = 0; i < 4; i++) {
        int value = 0;
        bool found_digit = false;
        while (pos < buf_len && buf[pos] >= '0' && buf[pos] <= '9') {
            value = value * 10 + (buf[pos] - '0');
            found_digit = true;
            pos++;
        }
        if (!found_digit || pos >= buf_len) {
            return false;
        }
        fields[i] = value;

        if (i < 3) {
            if (buf[pos] != ',') {
                return false;
            }
            pos++;
        }
    }

    int cur_len = fields[3];

    // cur_len=0 时（chunked EOF），模组不发尾部逗号和数据，直接跟 \r\n
    if (cur_len == 0) {
        if (pos + 2 > buf_len) {
            return false;
        }
        std::vector<AtArgumentValue> arguments;

        AtArgumentValue arg0;
        arg0.type = AtArgumentValue::Type::String;
        arg0.string_value = "content";
        arguments.push_back(arg0);

        AtArgumentValue arg1; arg1.type = AtArgumentValue::Type::Int; arg1.int_value = fields[0]; arguments.push_back(arg1);
        AtArgumentValue arg2; arg2.type = AtArgumentValue::Type::Int; arg2.int_value = fields[1]; arguments.push_back(arg2);
        AtArgumentValue arg3; arg3.type = AtArgumentValue::Type::Int; arg3.int_value = fields[2]; arguments.push_back(arg3);
        AtArgumentValue arg4; arg4.type = AtArgumentValue::Type::Int; arg4.int_value = 0;         arguments.push_back(arg4);

        rx_buffer_.erase(0, pos + 2);
        HandleUrc("MHTTPURC", arguments);
        return true;
    }

    // cur_len > 0: 第4个字段后面是逗号 + 二进制数据
    if (pos >= buf_len || buf[pos] != ',') {
        return false;
    }
    pos++;

    size_t total_needed = pos + cur_len + 2;  // +2 for trailing \r\n
    if (buf_len < total_needed) {
        return false;
    }

    std::string binary_data(buf + pos, cur_len);

    std::vector<AtArgumentValue> arguments;

    AtArgumentValue arg0; arg0.type = AtArgumentValue::Type::String; arg0.string_value = "content"; arguments.push_back(arg0);
    AtArgumentValue arg1; arg1.type = AtArgumentValue::Type::Int;    arg1.int_value = fields[0];    arguments.push_back(arg1);
    AtArgumentValue arg2; arg2.type = AtArgumentValue::Type::Int;    arg2.int_value = fields[1];    arguments.push_back(arg2);
    AtArgumentValue arg3; arg3.type = AtArgumentValue::Type::Int;    arg3.int_value = fields[2];    arguments.push_back(arg3);
    AtArgumentValue arg4; arg4.type = AtArgumentValue::Type::Int;    arg4.int_value = cur_len;      arguments.push_back(arg4);
    AtArgumentValue arg5; arg5.type = AtArgumentValue::Type::String; arg5.string_value = std::move(binary_data); arguments.push_back(arg5);

    rx_buffer_.erase(0, total_needed);
    HandleUrc("MHTTPURC", arguments);
    return true;
}

void AtUart::HandleUrc(const std::string& command, const std::vector<AtArgumentValue>& arguments) {
    if (command == "CME ERROR") {
        // 09-P0-D: 模组下发 "+CME ERROR:"(冒号后无错误码,弱网/异常应答常见)时 arguments 为空,
        // 原裸读 arguments[0] 越界(UB)。先判空,空码记 0。
        cme_error_code_ = arguments.empty() ? 0 : arguments[0].int_value;
        xEventGroupSetBits(event_group_handle_, AT_EVENT_COMMAND_ERROR);
        return;
    }

    std::lock_guard<std::mutex> lock(urc_mutex_);
    for (auto& callback : urc_callbacks_) {
        callback(command, arguments);
    }
}

void AtUart::FlushRxBuffers() {
    if (initialized_) {
        uart_flush_input(uart_num_);   // 标准 driver: 真正清 RX FIFO + ring buffer
    }
    {
        std::lock_guard<std::mutex> lock(rx_buffer_mutex_);
        rx_buffer_.clear();
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        response_.clear();
    }
    xEventGroupClearBits(event_group_handle_,
        AT_EVENT_COMMAND_DONE | AT_EVENT_COMMAND_ERROR | AT_EVENT_PARSE_NEEDED);
}

bool AtUart::DetectBaudRate(int timeout_ms) {
    int baud_rates[] = {115200, 921600, 460800};   // 出厂默认 115200 优先（首启最快）
    TickType_t start_time = xTaskGetTickCount();
    TickType_t timeout_ticks = (timeout_ms == -1) ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);

    for (int round = 0; round < 3; ++round) {
        ESP_LOGI(TAG, "Detecting baud rate... (round %d)", round + 1);
        for (size_t i = 0; i < sizeof(baud_rates) / sizeof(baud_rates[0]); i++) {
            int rate = baud_rates[i];
            uart_set_baudrate(uart_num_, rate);
            vTaskDelay(pdMS_TO_TICKS(20));
            FlushRxBuffers();
            if (SendCommand("AT", 200)) {
                ESP_LOGI(TAG, "Detected baud rate: %d", rate);
                baud_rate_ = rate;
                return true;
            }
            // 失败时 dump 收到的前 32 字节，便于诊断"无数据 vs 脏数据 vs 解析 bug"
            {
                std::lock_guard<std::mutex> lock(rx_buffer_mutex_);
                if (!rx_buffer_.empty()) {
                    size_t dump_len = std::min(rx_buffer_.size(), (size_t)32);
                    ESP_LOG_BUFFER_HEXDUMP(TAG, rx_buffer_.data(), dump_len, ESP_LOG_WARN);
                } else {
                    ESP_LOGD(TAG, "No bytes at %d baud", rate);
                }
            }
        }

        if (timeout_ms != -1) {
            TickType_t elapsed = xTaskGetTickCount() - start_time;
            if (elapsed >= timeout_ticks) {
                ESP_LOGE(TAG, "Baud rate detection timeout");
                return false;
            }
        }

        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    return false;
}

bool AtUart::SetBaudRate(int new_baud_rate, int timeout_ms) {
    if (!DetectBaudRate(timeout_ms)) {
        ESP_LOGE(TAG, "Failed to detect baud rate");
        return false;
    }
    if (new_baud_rate == baud_rate_) {
        return true;
    }
    // Set new baud rate
    if (!SendCommand(std::string("AT+IPR=") + std::to_string(new_baud_rate))) {
        ESP_LOGI(TAG, "Failed to set baud rate to %d", new_baud_rate);
        return false;
    }
    uart_set_baudrate(uart_num_, new_baud_rate);
    baud_rate_ = new_baud_rate;
    ESP_LOGI(TAG, "Set baud rate to %d", new_baud_rate);
    return true;
}

bool AtUart::SendData(const char* data, size_t length) {
    if (!initialized_) {
        ESP_LOGE(TAG, "UART未初始化");
        return false;
    }
    int written = uart_write_bytes(uart_num_, data, length);
    if (written < 0 || static_cast<size_t>(written) != length) {
        ESP_LOGE(TAG, "uart_write_bytes failed: %d / %u", written, (unsigned)length);
        return false;
    }
    return true;
}

bool AtUart::SendCommandWithData(const std::string& command, size_t timeout_ms, bool add_crlf, const char* data, size_t data_length) {
    // HTTP binary mode 防御（2026-04-30 · v2 扩展白名单到 MQTT）
    // 根因：ML307 进入 HTTP binary receive mode 后 UART 下行 raw 透传 HTTP body。
    //       此时若发"查询型" AT 命令（如 GetCsq → AT+CSQ）每 5s 一次持续注入，
    //       回应字节会混入下行 binary 流，mp3 解码器连续 16 次报
    //       "Not supported format" 后退出。
    // 策略：白名单 = 自管理 + 业务上行
    //   ① AT+MHTTP*  HTTP 自管理（含 AT+MHTTPCLOSE 必须能发，否则 binary 卡死）
    //   ② AT+MQTT*   MQTT 业务上行（listen state / device-server 通知，业务必发）
    //   ③ AT+MMQTT*  MQTT 多 ID 命令族（部分固件版本前缀）
    //   ④ AT+MIPSEND TCP/UDP 上行（保留兜底）
    // 业务命令的回应字节（OK/ERROR）量级很小（<10 字节），mp3 解码器有重同步
    // 能力可吞掉一帧损坏；但低频查询命令（AT+CSQ 周期 5s）持续注入会致命。
    // 其他纯查询命令（AT+CSQ / AT+CREG / AT+CCLK / AT+CGPSINFO 等）直接 fail，
    // 调用方拿 false 自行兜底（缓存上次值）。
    if (http_binary_mode_count_.load(std::memory_order_acquire) > 0) {
        bool allowed =
            command.compare(0, 8, "AT+MHTTP")  == 0 ||
            command.compare(0, 7, "AT+MQTT")   == 0 ||
            command.compare(0, 8, "AT+MMQTT")  == 0 ||
            command.compare(0, 10, "AT+MIPSEND") == 0;
        if (!allowed) {
            ESP_LOGW(TAG, "Reject AT in HTTP binary mode: %.32s", command.data());
            return false;
        }
    }

    // 第一性根因修复：等锁加超时（原 lock_guard 无限等 = modem 卡死时全应用瘫痪）
    // 抢锁超时 = 等价"AT 命令失败"，调用方既有失败处理路径全部 work
    std::unique_lock<std::timed_mutex> lock(command_mutex_, std::defer_lock);
    if (!lock.try_lock_for(std::chrono::milliseconds(timeout_ms))) {
        ESP_LOGW(TAG, "AT cmd mutex timeout %ums: %.32s", (unsigned)timeout_ms, command.data());
        return false;
    }
    if (debug_) {
        ESP_LOGI(TAG, ">> %.64s (%u bytes)", command.data(), command.length());
    }

    xEventGroupClearBits(event_group_handle_, AT_EVENT_COMMAND_DONE | AT_EVENT_COMMAND_ERROR);
    wait_for_response_ = true;
    cme_error_code_ = 0;
    {
        std::lock_guard<std::mutex> response_lock(mutex_);
        response_.clear();
    }

    if (add_crlf) {
        if (!SendData((command + "\r\n").data(), command.length() + 2)) {
            return false;
        }
    } else {
        if (!SendData(command.data(), command.length())) {
            return false;
        }
    }
    if (timeout_ms > 0) {
        auto bits = xEventGroupWaitBits(event_group_handle_, AT_EVENT_COMMAND_DONE | AT_EVENT_COMMAND_ERROR, pdTRUE, pdFALSE, pdMS_TO_TICKS(timeout_ms));
        wait_for_response_ = false;
        if (!(bits & AT_EVENT_COMMAND_DONE)) {
            return false;
        }
    } else {
        wait_for_response_ = false;
    }

    if (data && data_length > 0) {
        wait_for_response_ = true;
        if (!SendData(data, data_length)) {
            return false;
        }
        auto bits = xEventGroupWaitBits(event_group_handle_, AT_EVENT_COMMAND_DONE | AT_EVENT_COMMAND_ERROR, pdTRUE, pdFALSE, pdMS_TO_TICKS(timeout_ms));
        wait_for_response_ = false;
        if (!(bits & AT_EVENT_COMMAND_DONE)) {
            return false;
        }
    }
    return true;
}

bool AtUart::SendCommand(const std::string& command, size_t timeout_ms, bool add_crlf) {
    return SendCommandWithData(command, timeout_ms, add_crlf, nullptr, 0);
}

std::string AtUart::GetResponse() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return response_;
}

std::list<UrcCallback>::iterator AtUart::RegisterUrcCallback(UrcCallback callback) {
    std::lock_guard<std::mutex> lock(urc_mutex_);
    return urc_callbacks_.insert(urc_callbacks_.end(), callback);
}

void AtUart::UnregisterUrcCallback(std::list<UrcCallback>::iterator iterator) {
    std::lock_guard<std::mutex> lock(urc_mutex_);
    urc_callbacks_.erase(iterator);
}

void AtUart::SetDtrPin(bool high) {
    if (dtr_pin_ != GPIO_NUM_NC) {
        if (debug_) {
            ESP_LOGI(TAG, "Set DTR pin %d to %d", dtr_pin_, high ? 1 : 0);
        }
        gpio_set_level(dtr_pin_, high ? 1 : 0);
        dtr_pin_state_ = high;  // 记录DTR pin的状态
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

static const char hex_chars[] = "0123456789ABCDEF";
// 辅助函数，将单个十六进制字符转换为对应的数值
inline uint8_t CharToHex(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return 0;  // 对于无效输入，返回0
}

void AtUart::EncodeHexAppend(std::string& dest, const char* data, size_t length) {
    dest.reserve(dest.size() + length * 2 + 4);  // 预分配空间，多分配4个字节用于\r\n\0
    for (size_t i = 0; i < length; i++) {
        dest.push_back(hex_chars[(data[i] & 0xF0) >> 4]);
        dest.push_back(hex_chars[data[i] & 0x0F]);
    }
}

void AtUart::DecodeHexAppend(std::string& dest, const char* data, size_t length) {
    dest.reserve(dest.size() + length / 2 + 4);  // 预分配空间，多分配4个字节用于\r\n\0
    for (size_t i = 0; i < length; i += 2) {
        char byte = (CharToHex(data[i]) << 4) | CharToHex(data[i + 1]);
        dest.push_back(byte);
    }
}

std::string AtUart::EncodeHex(const std::string& data) {
    std::string encoded;
    EncodeHexAppend(encoded, data.c_str(), data.size());
    return encoded;
}

std::string AtUart::DecodeHex(const std::string& data) {
    std::string decoded;
    DecodeHexAppend(decoded, data.c_str(), data.size());
    return decoded;
}

void AtUart::SetDebug(bool enable) {
    debug_ = enable;
}

// RI pin ISR handler (runs in IRAM)
void IRAM_ATTR AtUart::RiPinIsrHandler(void* arg) {
    AtUart* at_uart = static_cast<AtUart*>(arg);
    // Disable interrupt
    gpio_intr_disable(at_uart->ri_pin_);
    // Notify the task to handle the interrupt
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    xEventGroupSetBitsFromISR(at_uart->event_group_handle_, AT_EVENT_RI_PIN_INT, &xHigherPriorityTaskWoken);
    portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
}
