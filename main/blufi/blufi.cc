#include "blufi.h"
#include "blufi_init.h"

#include <cstring>
#include <vector>

#include "application.h"
#include "settings.h"
#include "esp_blufi.h"
#include "esp_bt.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#ifdef CONFIG_BT_NIMBLE_ENABLED
#include "services/gap/ble_svc_gap.h"
#include "host/ble_gap.h"
#endif

#include "settings.h"
#include "wifi_station.h"

#define TAG "Blufi"

static constexpr int WIFI_LIST_NUM = 20; // 发送全部扫描到的热点

// 类静态：BT 静态 RAM 是否已永久释放
bool Blufi::static_mem_released_ = false;

// ============ 单例 ============

Blufi &Blufi::GetInstance() {
  static Blufi instance;
  return instance;
}

// ============ 永久释放 BT 静态 RAM ============

void Blufi::ReleaseStaticMem() {
  if (static_mem_released_) {
    ESP_LOGD(TAG, "BT 静态 RAM 已释放过，跳过");
    return;
  }

  // 安全前提：调用方保证 BT controller 已 deinit 或从未 init
  // 本项目调用时机：WifiBoard::StartNetwork SmartConnect 成功路径
  //   —— 此时 BT 从未被 InitializeController，即 .bss 区原封不动
  //   esp_bt_controller_mem_release 内部允许 STATUS_IDLE 状态调用

  size_t free_before = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);

  // ESP_BT_MODE_BTDM: 释放 BT 双模（Classic + BLE）所有 .bss/.data
  // 本项目只用 BLE 但 sdkconfig 未裁剪 Classic，用 BTDM 一次性释放最大
  esp_err_t ret = esp_bt_mem_release(ESP_BT_MODE_BTDM);

  size_t free_after = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);

  if (ret == ESP_OK) {
    static_mem_released_ = true;
    ESP_LOGW(TAG, "✅ BT 静态 RAM 已永久释放: 内部 free %u → %u (+%d KB)",
             (unsigned)free_before, (unsigned)free_after,
             (int)((free_after - free_before) / 1024));
    ESP_LOGW(TAG, "⚠️ 本次启动不可再启动 BT（进配网必须先 Application::Reboot）");
  } else {
    ESP_LOGE(TAG, "esp_bt_mem_release(BTDM) 失败: %s", esp_err_to_name(ret));
  }
}

// ============ 生命周期 ============

bool Blufi::InitializeController() {
  ESP_LOGI(TAG, "【阶段1/2】初始化 BLE 控制器");

  if (static_mem_released_) {
    Settings settings("wifi", true);
    settings.SetInt("force_ap", 1);
    Application::GetInstance().Reboot();  // 内部安全序列 + esp_restart，不返回
    return false;  // 理论上到不了这里
  }

  // 检查是否已经初始化
  if (controller_initialized_) {
    ESP_LOGI(TAG, "BLE 控制器已初始化，跳过");
    return true;
  }

  // 初始化 BLE 控制器（不启动主机栈）
  if (esp_blufi_controller_init() != ESP_OK) {
    ESP_LOGE(TAG, "BLE 控制器初始化失败");
    // 【P1改进】清理部分初始化的资源
    esp_blufi_controller_deinit();
    return false;
  }

  controller_initialized_ = true;
  ESP_LOGI(TAG, "✅ BLE 控制器初始化成功");
  return true;
}

bool Blufi::Start(const std::string &device_name) {
  ESP_LOGI(TAG, "【阶段2/2】启动 BLE 主机栈和广播: %s", device_name.c_str());

  // 【P1改进】验证设备名称长度
  if (device_name.empty()) {
    ESP_LOGE(TAG, "错误：设备名称不能为空");
    return false;
  }
  if (device_name.length() > 248) {
    ESP_LOGE(TAG, "错误：设备名称过长 (%u bytes)，最大 248 字节",
             (unsigned int)device_name.length());
    return false;
  }

  // 检查控制器是否已初始化
  if (!controller_initialized_) {
    ESP_LOGE(TAG, "错误：必须先调用 InitializeController()");
    return false;
  }

  // ⭐ 修复：检查是否已经在初始化过程中或已初始化
  if (initialized_) {
    if (!advertising_) {
      stopping_ = false;
      if (StartAdvertising()) {
        advertising_ = true;
      }
    }
    ESP_LOGI(TAG, "Blufi已初始化，跳过重复初始化");
    return true;
  }

  stopping_ = false;
  device_name_ = device_name;

  // 优化: 先扫描WiFi，再启动BLE，避免RF冲突
  auto &wifi = WifiStation::GetInstance();
  if (wifi.IsInitialized()) {
    // 如果正在扫描，等待完成
    if (wifi.IsScanning()) {
      int wait_count = 0;
      while (wifi.IsScanning() && wait_count < 30) {
        vTaskDelay(pdMS_TO_TICKS(100));
        wait_count++;
      }
    }

    // 注册扫描回调
    wifi.OnScanComplete(
        [](const std::vector<wifi_ap_record_t> &) { Blufi::OnScanDone(); });
  }

  // 设备名 set 移到 INIT_FINISH 回调（NimBLE 主机栈 init 会重置 GAP service buffer，
  // 在 host_init 之前 set 会被覆盖 → 广播打出乱码）

  // 初始化主机栈和回调（控制器已在 InitializeController() 中初始化）
  static esp_blufi_callbacks_t callbacks = {
      .event_cb = BlufiCallback,
      .negotiate_data_handler = blufi_dh_negotiate_data_handler,
      .encrypt_func = blufi_aes_encrypt,
      .decrypt_func = blufi_aes_decrypt,
      .checksum_func = blufi_crc_checksum,
  };

  // ⚠️ 创建 INIT_FINISH 同步信号（BlufiCallback 触发时 give）
  // 用途：调用方持 lvgl_port_lock 期间，等 NimBLE 异步完成 GATT profile init
  // 避免释放锁时 NimBLE 还在 flash op，导致 LVGL flush 入队失败 → WDT
  if (init_done_sem_ == nullptr) {
    init_done_sem_ = xSemaphoreCreateBinary();
  } else {
    xSemaphoreTake(init_done_sem_, 0);  // 清除旧信号
  }

  if (esp_blufi_host_and_cb_init(&callbacks) != ESP_OK) {
    ESP_LOGE(TAG, "Host init failed");
    // 【P0改进】清理控制器资源
    esp_blufi_controller_deinit();
    controller_initialized_ = false;
    ESP_LOGE(TAG, "已清理 BLE 控制器资源");
    return false;
  }

  // 修复 BLUFI↔AP 切换 bug：原代码仅在 INIT_FINISH callback 里设 true，
  initialized_ = true;

  // ⚠️ 同步等待 INIT_FINISH（NimBLE 异步初始化 GATT profile，flash op 密集）
  // 超时 3s：正常 < 100ms，超时说明 NimBLE 异常但至少能释放调用方的 LVGL 锁
  if (init_done_sem_ &&
      xSemaphoreTake(init_done_sem_, pdMS_TO_TICKS(3000)) != pdTRUE) {
    ESP_LOGW(TAG, "等待 INIT_FINISH 超时 3s（NimBLE 可能异常）");
  }
  ESP_LOGI(TAG, "✅ BLE 主机栈启动成功");
  return true;
}

void Blufi::Stop() {
  ESP_LOGI(TAG, "停止Blufi");
  stopping_ = true;

  // 【P0改进】分别检查和清理，避免资源泄漏

  // 1. 清理广播和连接
  if (advertising_) {
    esp_blufi_adv_stop();
    advertising_ = false;
  }

  if (ble_connected_) {
    esp_blufi_disconnect();
    ble_connected_ = false;
    vTaskDelay(pdMS_TO_TICKS(100));
  }

  // 2. 清理主机栈（如果已初始化）
  if (initialized_) {
    esp_blufi_host_deinit();
    initialized_ = false;
    ESP_LOGI(TAG, "✅ BLE 主机栈已清理");
  }

  // 3. 清理控制器（如果已初始化）
  if (controller_initialized_) {
    esp_blufi_controller_deinit();
    controller_initialized_ = false;
    ESP_LOGI(TAG, "✅ BLE 控制器已清理");
  }

  // 4. 清理定时器和回调
  if (retry_timer_) {
    esp_timer_stop(retry_timer_);
    esp_timer_delete(retry_timer_);
    retry_timer_ = nullptr;
  }
  WifiStation::GetInstance().OnScanComplete(nullptr);
  on_success_ = nullptr;
  credential_validator_ = nullptr;
  scanning_ = false;
  scan_retry_count_ = 0;

  // 5. 清理 INIT_FINISH 同步信号
  if (init_done_sem_) {
    vSemaphoreDelete(init_done_sem_);
    init_done_sem_ = nullptr;
  }
}

// ============ 接口 ============

void Blufi::SetCredentialValidator(
    std::function<bool(const std::string &ssid, const std::string &password,
                       std::string &error_message)>
        validator) {
  credential_validator_ = validator;
}

void Blufi::OnConfigSuccess(std::function<void()> callback) {
  on_success_ = callback;
}

void Blufi::SendData(const char *data, int len) {
  // 【P1改进】验证数据长度
  if (!data || len <= 0) {
    ESP_LOGE(TAG, "错误：数据为空或长度无效 (len=%d)", len);
    return;
  }

  if (len > 512) {
    ESP_LOGE(TAG, "错误：数据过长 (%d bytes)，Blufi 最大支持 512 字节", len);
    return;
  }

  if (!ble_connected_) {
    ESP_LOGW(TAG, "警告：BLE 未连接，无法发送数据");
    return;
  }

  esp_blufi_send_custom_data((uint8_t *)data, len);
  ESP_LOGD(TAG, "发送自定义数据: %d bytes", len);
}

bool Blufi::StartAdvertising() {
#ifdef CONFIG_BT_NIMBLE_ENABLED
  const char *name = ble_svc_gap_device_name();
  if (!name || name[0] == '\0') {
    ESP_LOGW(TAG, "BLE设备名为空，跳过广播");
    return false;
  }

  // ⚠️ 修复: 使用ESP-IDF提供的esp_blufi_adv_start_with_name()
  // 原因: 直接调用ble_gap_adv_start()时没有传入GAP事件回调(esp_blufi_gap_event)
  //      导致BLE_CONNECT事件无法被处理,进而导致ESP_BLUFI_EVENT_BLE_CONNECT事件丢失
  // 参考:
  // esp-idf/components/bt/common/btc/profile/esp/blufi/nimble_host/esp_blufi.c:464
  esp_blufi_adv_start_with_name(name);

  ESP_LOGI(TAG, "[1/8] BLE广播已启动: %s", name);
  return true;
#else
  esp_blufi_adv_start();
  return true;
#endif
}

// ============ 扫描回调 ============

void Blufi::OnScanDone() {
  auto &self = GetInstance();
  self.scanning_ = false;

  if (!self.ble_connected_) {
    ESP_LOGW(TAG, "[3/8] BLE已断开，跳过发送WiFi列表");
    self.scan_retry_count_ = 0;
    return;
  }

  auto &wifi = WifiStation::GetInstance();
  auto records = wifi.GetDeduplicatedCache();

  if (records.empty()) {
    // 空结果自动重试（最多2次，覆盖RF时分不稳定场景）
    if (self.scan_retry_count_ < 2) {
      self.scan_retry_count_++;
      ESP_LOGW(TAG, "[3/8] 未扫描到热点，第%d次重试", self.scan_retry_count_);
      self.scanning_ = true;

      // 清理上次定时器
      if (self.retry_timer_) {
        esp_timer_delete(self.retry_timer_);
        self.retry_timer_ = nullptr;
      }

      esp_timer_create_args_t timer_args = {
          .callback = [](void *) {
            auto &s = Blufi::GetInstance();
            // 回调后立即删除定时器
            if (s.retry_timer_) {
              esp_timer_delete(s.retry_timer_);
              s.retry_timer_ = nullptr;
            }
            if (s.ble_connected_ && s.scanning_) {
              WifiStation::GetInstance().TriggerScan();
            } else {
              s.scanning_ = false;
            }
          },
          .arg = nullptr,
          .dispatch_method = ESP_TIMER_TASK,
          .name = "blufi_retry",
          .skip_unhandled_events = true,
      };
      if (esp_timer_create(&timer_args, &self.retry_timer_) == ESP_OK) {
        esp_timer_start_once(self.retry_timer_, 500 * 1000);  // 500ms
      } else {
        wifi.TriggerScan();
      }
      return;
    }
    self.scan_retry_count_ = 0;
    ESP_LOGW(TAG, "[3/8] %d次重试后仍未扫描到热点，发送空列表", 2);
    esp_blufi_send_wifi_list(0, nullptr);
    return;
  }

  self.scan_retry_count_ = 0;

  uint16_t count = std::min((size_t)WIFI_LIST_NUM, records.size());
  std::vector<esp_blufi_ap_record_t> ap_list(count);

  ESP_LOGI(TAG, "[3/8] 发送 %d 个热点到手机", count);
  for (int i = 0; i < count; i++) {
    ap_list[i].rssi = records[i].rssi;
    memcpy(ap_list[i].ssid, records[i].ssid, sizeof(records[i].ssid));
  }

  // 发送前添加小延迟，确保安全层完全就绪
  ESP_LOGI(TAG, "[3/8] 调用 esp_blufi_send_wifi_list，count=%d", count);
  esp_blufi_send_wifi_list(count, ap_list.data());
  ESP_LOGI(TAG, "[3/8] ✅ WiFi列表已发送");
}

// ============ BLE 回调 ============
// 配网流程步骤序号说明:
// [步骤1] Blufi初始化完成
// [步骤2] 手机BLE连接
// [步骤3] WiFi扫描/获取列表
// [步骤4] 接收SSID
// [步骤5] 接收密码
// [步骤6] 接收绑定码
// [步骤7] 验证凭据并连接
// [步骤8] 配网完成/断开

// 事件名称映射（仅调试构建时有效，节省 Flash）
#if CONFIG_LOG_DEFAULT_LEVEL >= 4  // ESP_LOG_DEBUG
static const char *BlufiEventName(esp_blufi_cb_event_t event) {
  switch (event) {
  case ESP_BLUFI_EVENT_INIT_FINISH:       return "INIT_FINISH";
  case ESP_BLUFI_EVENT_BLE_CONNECT:       return "BLE_CONNECT";
  case ESP_BLUFI_EVENT_BLE_DISCONNECT:    return "BLE_DISCONNECT";
  case ESP_BLUFI_EVENT_GET_WIFI_LIST:     return "GET_WIFI_LIST";
  case ESP_BLUFI_EVENT_RECV_STA_SSID:     return "RECV_STA_SSID";
  case ESP_BLUFI_EVENT_RECV_STA_PASSWD:   return "RECV_STA_PASSWD";
  case ESP_BLUFI_EVENT_REQ_CONNECT_TO_AP: return "REQ_CONNECT_TO_AP";
  case ESP_BLUFI_EVENT_REPORT_ERROR:      return "REPORT_ERROR";
  case ESP_BLUFI_EVENT_RECV_CUSTOM_DATA:  return "RECV_CUSTOM_DATA";
  default:                                return "OTHER";
  }
}
#endif

void Blufi::BlufiCallback(esp_blufi_cb_event_t event,
                          esp_blufi_cb_param_t *param) {
  auto &self = Blufi::GetInstance();

  // 修复：如果收到数据类事件但BLE未标记连接，说明BLE_CONNECT事件丢失
  if (!self.ble_connected_ &&
      (event == ESP_BLUFI_EVENT_GET_WIFI_LIST ||
       event == ESP_BLUFI_EVENT_RECV_STA_SSID ||
       event == ESP_BLUFI_EVENT_RECV_STA_PASSWD ||
       event == ESP_BLUFI_EVENT_RECV_CUSTOM_DATA ||
       event == ESP_BLUFI_EVENT_REQ_CONNECT_TO_AP ||
       event == ESP_BLUFI_EVENT_GET_WIFI_STATUS ||
       event == ESP_BLUFI_EVENT_RECV_SLAVE_DISCONNECT_BLE)) {
    ESP_LOGW(TAG, "[修复] BLE_CONNECT事件丢失，修正连接状态 (event=%d)", event);
    self.ble_connected_ = true;

    // 停止广播（BLE已连接时不应继续广播）
    esp_blufi_adv_stop();

    // 初始化安全层（blufi_security_init已支持多次调用，不会重复初始化）
    blufi_security_init();

    // 补偿 BLE_CONNECT 中的 WiFi 预扫描逻辑
    auto &wifi = WifiStation::GetInstance();
    if (wifi.IsInitialized() && !wifi.IsCacheValid() && !self.scanning_) {
      ESP_LOGW(TAG, "[修复]   → 补偿WiFi预扫描");
      self.scanning_ = true;
      wifi.TriggerScan();
    }

    ESP_LOGW(TAG, "[修复]   → 连接状态修正完成");
  }

  switch (event) {
  case ESP_BLUFI_EVENT_INIT_FINISH:
    ESP_LOGI(TAG, "[1/8] Blufi初始化完成");
    // ⭐ 关键修复：在profile真正初始化完成后才设置状态
    self.initialized_ = true;
#ifdef CONFIG_BT_NIMBLE_ENABLED
    // NimBLE 主机栈已就绪（GAP service buffer 已分配），此时 set 才生效
    ble_svc_gap_device_name_set(self.device_name_.c_str());
#endif
    self.advertising_ = self.StartAdvertising();
    if (!self.advertising_) {
      ESP_LOGW(TAG, "[1/8] 广播启动失败，advertising_=false");
    }
    // ⚠️ 唤醒 Start() 同步等待：GATT profile 和广播已就绪，flash op 结束
    if (self.init_done_sem_) {
      xSemaphoreGive(self.init_done_sem_);
    }
    break;

  case ESP_BLUFI_EVENT_BLE_CONNECT:
    self.ble_connected_ = true;
    self.conn_handle_ = param->connect.conn_id;
    ESP_LOGI(TAG, "[2/8] ✅ 手机BLE已连接 (conn_handle=%d)", self.conn_handle_);
    ESP_LOGI(TAG, "[2/8]   → 停止广播");
    esp_blufi_adv_stop();
    ESP_LOGI(TAG, "[2/8]   → 初始化安全层");
    blufi_security_init();

    // ⭐ 优化：更新 BLE 连接参数，延长监管超时
    // 用户浏览 WiFi 列表期间可能 5-60 秒无 BLE 数据交互，
    // 默认监管超时（iOS ~720ms, Android 5-20s）容易导致断连。
    // 参考：小米/涂鸦等头部品牌配网均设置 6s 以上监管超时。
#ifdef CONFIG_BT_NIMBLE_ENABLED
    {
      struct ble_gap_upd_params conn_params = {};
      conn_params.itvl_min = 24;              // 30ms  (24 * 1.25ms)
      conn_params.itvl_max = 40;              // 50ms  (40 * 1.25ms)
      conn_params.latency = 0;                // 不跳过连接事件
      conn_params.supervision_timeout = 600;  // 6000ms (600 * 10ms)
      conn_params.min_ce_len = 0;
      conn_params.max_ce_len = 0;
      int rc = ble_gap_update_params(self.conn_handle_, &conn_params);
      if (rc == 0) {
        ESP_LOGI(TAG, "[2/8]   → 连接参数已更新: interval=30-50ms, timeout=6s");
      } else {
        ESP_LOGW(TAG, "[2/8]   → 连接参数更新失败: rc=%d (不影响配网)", rc);
      }
    }
#endif

    ESP_LOGI(TAG, "[2/8]   → BLE连接建立完成");

    // ⭐ BLE连接后初始化WiFi并预扫描
    {
      auto &wifi = WifiStation::GetInstance();

      // 注册扫描回调（无论WiFi是否已初始化都需要）
      wifi.OnScanComplete(
          [](const std::vector<wifi_ap_record_t> &) { Blufi::OnScanDone(); });

      if (!wifi.IsInitialized()) {
        // WiFi未初始化：Start()内部同步初始化，
        // WIFI_EVENT_STA_START 事件会自动触发首次扫描，无需手动 TriggerScan
        ESP_LOGI(TAG, "[2/8] 初始化WiFi（自动触发首次扫描）");
        wifi.SetScanOnlyMode(true);
        self.scanning_ = true;
        wifi.Start();
      } else {
        // WiFi已初始化，手动触发扫描
        ESP_LOGI(TAG, "[2/8] WiFi已就绪，触发扫描");
        if (!self.scanning_ && !wifi.IsScanning()) {
          self.scanning_ = true;
          wifi.TriggerScan();
        }
      }
    }

    break;

  case ESP_BLUFI_EVENT_BLE_DISCONNECT:
    ESP_LOGW(TAG, "[8/8] ⚠️ BLE连接已断开");
    ESP_LOGW(TAG, "[8/8]   → 当前状态: ble_connected=%d, stopping=%d",
             self.ble_connected_, self.stopping_);
    self.ble_connected_ = false;
    self.scanning_ = false;
    self.scan_retry_count_ = 0;
    if (self.retry_timer_) {
      esp_timer_stop(self.retry_timer_);
      esp_timer_delete(self.retry_timer_);
      self.retry_timer_ = nullptr;
    }
    blufi_security_deinit();

    if (!self.stopping_) {
      ESP_LOGI(TAG, "[8/8]   → 重新启动广播");
      self.StartAdvertising();
    }
    break;

  case ESP_BLUFI_EVENT_REQ_CONNECT_TO_AP: {
    ESP_LOGI(TAG, "[7/8] 收到WiFi连接请求: %s", self.ssid_.c_str());

    if (!self.credential_validator_) {
      ESP_LOGE(TAG, "[7/8] 错误: 未设置凭据验证器");
      esp_blufi_send_wifi_conn_report(WIFI_MODE_STA, ESP_BLUFI_STA_CONN_FAIL, 0,
                                      nullptr);
      break;
    }

    // ⭐ 立即回复 CONNECTING 状态，让小程序及时更新进度
    esp_blufi_send_wifi_conn_report(WIFI_MODE_STA, ESP_BLUFI_STA_CONNECTING, 0,
                                    nullptr);

    // ⭐ 异步执行WiFi连接，避免阻塞NimBLE host task
    // TryConnectAndSave 最多阻塞10s，超过BLE supervision timeout(6s)会导致
    // BLE断连，小程序收不到配网结果→显示"配网超时"而实际WiFi已连接成功
    // P1 修：Pin Core 1（BLE/WiFi 协议任务 · 与 wifi_ap 同核 · 避 Core 0 NVS 死锁）
    xTaskCreatePinnedToCore([](void *arg) {
      auto &self = Blufi::GetInstance();
      std::string error;
      if (self.credential_validator_(self.ssid_, self.password_, error)) {
        // ⭐ 联动优化：WiFi连接成功后立即发送设备信息JSON
        // 小程序 chooseWifi.vue 的 handleResult 实际解析 JSON（非 wifi_conn_report）
        // 先发 JSON 让小程序尽快进入绑定流程，再发冗余 conn_report 作为兼容
        if (self.on_success_) {
          ESP_LOGI(TAG, "[7/8] WiFi连接成功，立即发送设备信息JSON");
          self.on_success_();
        }

        // 兼容发送 wifi_conn_report（小程序主要靠 JSON 判断成功，这里仅兼容旧流程）
        esp_blufi_extra_info_t info = {};
        info.sta_ssid = (uint8_t *)self.ssid_.c_str();
        info.sta_ssid_len = self.ssid_.length();
        esp_blufi_send_wifi_conn_report(WIFI_MODE_STA, ESP_BLUFI_STA_CONN_SUCCESS,
                                        0, &info);
        ESP_LOGI(TAG, "[7/8] 配网成功: %s", self.ssid_.c_str());
      } else {
        // ⭐ 先发 JSON 错误信息，小程序可直接解析显示具体原因
        char err_json[128];
        snprintf(err_json, sizeof(err_json), "{\"error\":\"%s\"}", error.c_str());
        esp_blufi_send_custom_data((uint8_t *)err_json, strlen(err_json));
        ESP_LOGE(TAG, "[7/8] 配网失败: %s, 已发送错误JSON", error.c_str());

        // 兼容发送 conn_report(FAIL)
        esp_blufi_send_wifi_conn_report(WIFI_MODE_STA, ESP_BLUFI_STA_CONN_FAIL, 0,
                                        nullptr);
      }
      vTaskDelete(NULL);
    }, "blufi_wifi", 4096, NULL, 5, NULL, 1);
    break;
  }

  case ESP_BLUFI_EVENT_REPORT_ERROR:
    ESP_LOGE(TAG, "[错误] Blufi协议错误: state=%d", param->report_error.state);
    esp_blufi_send_error_info(param->report_error.state);
    break;

  case ESP_BLUFI_EVENT_GET_WIFI_STATUS: {
    auto &wifi = WifiStation::GetInstance();
    auto status = wifi.IsConnected() ? ESP_BLUFI_STA_CONN_SUCCESS
                                     : ESP_BLUFI_STA_CONN_FAIL;
    ESP_LOGI(TAG, "[查询] WiFi状态: %s",
             wifi.IsConnected() ? "已连接" : "未连接");
    esp_blufi_send_wifi_conn_report(WIFI_MODE_STA, status, 0, nullptr);
    break;
  }

  case ESP_BLUFI_EVENT_GET_WIFI_LIST:
    ESP_LOGI(TAG, "[3/8] 手机请求WiFi列表");
    {
      auto &wifi = WifiStation::GetInstance();

      // WiFi未初始化时先初始化（BLE先连接场景）
      if (!wifi.IsInitialized()) {
        ESP_LOGI(TAG, "[3/8] 初始化WiFi并扫描");
        wifi.SetScanOnlyMode(true);
        wifi.OnScanComplete(
            [](const std::vector<wifi_ap_record_t> &) { Blufi::OnScanDone(); });
        self.scanning_ = true;
        wifi.Start();  // STA_START事件会自动触发首次扫描
        break;
      }

      // 已在扫描中，等待回调
      if (self.scanning_ || wifi.IsScanning()) {
        self.scanning_ = true;
        ESP_LOGI(TAG, "[3/8] WiFi扫描进行中，等待完成");
        break;
      }

      // 配网场景始终触发新扫描，确保用户看到最新热点
      ESP_LOGI(TAG, "[3/8] 触发WiFi扫描");
      self.scanning_ = true;
      wifi.TriggerScan();
    }
    break;

  case ESP_BLUFI_EVENT_RECV_STA_SSID:
    self.ssid_.assign((char *)param->sta_ssid.ssid, param->sta_ssid.ssid_len);
    ESP_LOGI(TAG, "[4/8] 收到SSID: %s", self.ssid_.c_str());
    break;

  case ESP_BLUFI_EVENT_RECV_STA_PASSWD:
    self.password_.assign((char *)param->sta_passwd.passwd,
                          param->sta_passwd.passwd_len);
    ESP_LOGI(TAG, "[5/8] 收到密码");
    break;

  case ESP_BLUFI_EVENT_RECV_SLAVE_DISCONNECT_BLE:
    ESP_LOGI(TAG, "[通知] 收到手机断开BLE请求");
    esp_blufi_disconnect();
    break;

  case ESP_BLUFI_EVENT_RECV_CUSTOM_DATA:
    self.binding_code_.assign((char *)param->custom_data.data,
                              param->custom_data.data_len);
    ESP_LOGI(TAG, "[6/8] 收到绑定码: %s", self.binding_code_.c_str());
    // 保存到 Settings，供 OTA 等模块读取（解耦依赖）
    {
      Settings settings("blufi", true);
      settings.SetString("binding_code", self.binding_code_);
    }
    break;

  default:
    ESP_LOGD(TAG, "[事件] 未处理的Blufi事件: %d", event);
    break;
  }
}
