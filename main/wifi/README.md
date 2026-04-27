# WiFi 模块

WiFi Station、AP 配网和 SSID 管理模块。

## 目录

- [文件说明](#文件说明)
- [核心类](#核心类)
- [架构流程图](#架构流程图)
- [工作模式](#工作模式)
- [智能联网流程](#智能联网流程)
- [热点配网流程](#热点配网流程)
- [sdkconfig 优化配置](#sdkconfig-优化配置)
- [常见问题](#常见问题)

---

## 文件说明

| 文件 | 说明 |
|------|------|
| `wifi_station.cc/h` | WiFi Station 管理（扫描、连接、重连） |
| `wifi_ap.cc/h` | WiFi AP 热点配网（HTTP 服务、DNS 劫持） |
| `dns_server.cc/h` | DNS 服务器（Captive Portal 支持） |
| `ssid_manager.cc/h` | SSID 凭证管理（NVS 存储） |
| `assets/` | 配网页面 HTML 资源 |

---

## 核心类

### WifiStation

WiFi Station 单例，负责 WiFi 连接和扫描管理。

```cpp
auto& wifi = WifiStation::GetInstance();

// 启动 WiFi（STA 模式）
wifi.Start();

// 启动 WiFi（APSTA 模式，用于配网）
wifi.Start(WifiMode::APSTA, "MyDevice-XXXX");

// 触发扫描
wifi.TriggerScan();

// 获取扫描缓存
auto cache = wifi.GetDeduplicatedCache();

// 配网专用：单次连接尝试
auto result = wifi.TryConnect("SSID", "password", 15000);

// 配网专用：连接并保存凭证
auto result = wifi.TryConnectAndSave("SSID", "password", 15000);
```

### WifiAp

WiFi AP 配网模块，提供 HTTP 配网界面。

```cpp
auto& ap = WifiAp::GetInstance();

// 设置凭证验证器
ap.SetCredentialValidator([](const std::string& ssid,
                              const std::string& password,
                              std::string& error) {
    return WifiStation::GetInstance().TryConnectAndSave(ssid, password).success;
});

// 设置成功回调
ap.OnConfigSuccess([]() {
    // 配网成功处理
});

// 启动 AP 配网
ap.Start("MyDevice-XXXX", "zh-CN");

// 停止配网
ap.Stop();
```

### SsidManager

SSID 凭证管理，支持多个 WiFi 凭证存储。

```cpp
auto& mgr = SsidManager::GetInstance();

// 添加凭证
mgr.AddSsid("SSID", "password");

// 获取凭证列表
auto list = mgr.GetSsidList();

// 删除凭证
mgr.RemoveSsid(0);

// 设置默认 SSID
mgr.SetDefaultSsid(0);
```

## 工作模式

### STA 模式（正常联网）

```
WifiStation::Start() → 自动扫描 → 匹配凭证 → 连接 → 获取 IP
```

### APSTA 模式（配网）

```
WifiStation::Start(APSTA) → 开启热点 + STA → DNS 劫持 → HTTP 配网
```

### 仅扫描模式（scan_only）

配网时使用，只缓存扫描结果，不自动连接：

```cpp
wifi.SetScanOnlyMode(true);   // 启用
wifi.SetScanOnlyMode(false);  // 恢复正常
```

## sdkconfig 优化配置

### WiFi 核心配置

```ini
# WiFi 缓冲区配置
CONFIG_ESP_WIFI_STATIC_RX_BUFFER_NUM=6
CONFIG_ESP_WIFI_DYNAMIC_RX_BUFFER_NUM=16
CONFIG_ESP_WIFI_DYNAMIC_TX_BUFFER_NUM=16
CONFIG_ESP_WIFI_RX_BA_WIN=6
CONFIG_ESP_WIFI_TX_BA_WIN=6

# WiFi 功耗优化
CONFIG_ESP_WIFI_IRAM_OPT=y
CONFIG_ESP_WIFI_RX_IRAM_OPT=y
CONFIG_ESP_WIFI_SLP_IRAM_OPT=y

# WiFi NVS 配置
CONFIG_ESP_WIFI_NVS_ENABLED=n
```

### LWIP 网络栈优化

```ini
# TCP/IP 缓冲区
CONFIG_LWIP_TCP_SND_BUF_DEFAULT=8192
CONFIG_LWIP_TCP_WND_DEFAULT=8192
CONFIG_LWIP_TCP_RECVMBOX_SIZE=12
CONFIG_LWIP_UDP_RECVMBOX_SIZE=12

# DNS 配置
CONFIG_LWIP_DNS_MAX_SERVERS=3
CONFIG_LWIP_DNS_SUPPORT_MDNS_QUERIES=y

# DHCP 优化
CONFIG_LWIP_DHCP_OPTIONS_LEN=128
CONFIG_LWIP_DHCP_DISABLE_VENDOR_CLASS_ID=y
```

### 内存优化配置

```ini
# 减少 WiFi 内存占用
CONFIG_ESP_WIFI_STATIC_RX_BUFFER_NUM=4      # 最小值
CONFIG_ESP_WIFI_DYNAMIC_RX_BUFFER_NUM=8     # 降低动态缓冲
CONFIG_ESP_WIFI_RX_BA_WIN=4                 # 降低接收窗口

# LWIP 内存优化
CONFIG_LWIP_TCPIP_RECVMBOX_SIZE=16
CONFIG_LWIP_TCP_SND_BUF_DEFAULT=4096
CONFIG_LWIP_TCP_WND_DEFAULT=4096
```

### 稳定性配置

```ini
# WiFi 重连配置
CONFIG_ESP_WIFI_SOFTAP_BEACON_MAX_LEN=752

# TCP Keepalive
CONFIG_LWIP_TCP_KEEPALIVE_DEFAULT=y
CONFIG_LWIP_TCP_KEEPIDLE_DEFAULT=60
CONFIG_LWIP_TCP_KEEPINTVL_DEFAULT=10
CONFIG_LWIP_TCP_KEEPCNT_DEFAULT=5
```

### SmartConfig 配置（可选）

```ini
CONFIG_ESP_WIFI_ENABLE_ESPNOW=n
CONFIG_ESP_WIFI_ENABLE_WPA3_SAE=y
```

## 扫描配置参数

```cpp
WifiScanConfig config;
config.interval_ms = 10000;      // 定时扫描间隔 (毫秒)
config.cache_valid_ms = 15000;   // 缓存有效期 (毫秒)
config.max_ap_count = 20;        // 最大缓存 AP 数量
config.auto_deduplicate = true;  // 自动去重同名 SSID

wifi.SetScanConfig(config);
```

## 连接状态回调

```cpp
// 开始扫描
wifi.OnScanBegin([]() {
    ESP_LOGI(TAG, "开始扫描...");
});

// 开始连接
wifi.OnConnect([](const std::string& ssid) {
    ESP_LOGI(TAG, "连接 %s...", ssid.c_str());
});

// 连接成功
wifi.OnConnected([](const std::string& ssid) {
    ESP_LOGI(TAG, "已连接 %s", ssid.c_str());
});

// 断开连接
wifi.OnDisconnected([](uint8_t reason) {
    ESP_LOGW(TAG, "断开连接，原因: %d", reason);
});

// 扫描完成
wifi.OnScanComplete([](const std::vector<wifi_ap_record_t>& records) {
    ESP_LOGI(TAG, "扫描到 %d 个热点", records.size());
});
```

## 常见问题

### 1. 配网时搜不到热点

确保启用了 `scan_only` 模式：
```cpp
wifi.SetScanOnlyMode(true);
wifi.TriggerScan();
```

### 2. 连接超时

调整超时时间：
```cpp
auto result = wifi.TryConnect(ssid, password, 20000);  // 20秒
```

### 3. 配网页面无法访问

检查 DNS 服务是否正常启动，确保设备连接到 AP 热点。

### 4. 多个同名热点

系统会自动选择信号最强的热点连接。

## 迁移说明

从 `components/78__esp-wifi-connect` 迁移到 `main/wifi`：

1. 头文件引用从 `<wifi_station.h>` 改为 `"wifi_station.h"`
2. 所有功能保持不变
3. CMakeLists.txt 已自动配置

---

## 架构流程图

### 整体架构

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                            WifiBoard (wifi_board.cc)                         │
│                               配网管理层                                     │
│  ┌──────────────────────────────────────────────────────────────────────┐   │
│  │ StartNetwork()  →  SmartConnect() 失败  →  EnterWifiConfigMode()     │   │
│  └──────────────────────────────────────────────────────────────────────┘   │
│                              │                                              │
│            ┌─────────────────┴─────────────────┐                           │
│            ▼                                   ▼                           │
│  ┌───────────────────┐             ┌───────────────────┐                   │
│  │ Blufi 蓝牙配网     │             │ WifiAp 热点配网    │                   │
│  │ (blufi.cc)        │             │ (wifi_ap.cc)      │                   │
│  │ • BLE 广播/连接   │             │ • HTTP 服务器     │                   │
│  │ • BluFi 协议帧   │             │ • DNS 劫持        │                   │
│  │ • WiFi 列表发送  │             │ • WiFi 列表接口   │                   │
│  └─────────┬─────────┘             └─────────┬─────────┘                   │
│            │                                   │                           │
│            └─────────────────┬─────────────────┘                           │
│                              ▼                                              │
│  ┌───────────────────────────────────────────────────────────────────────┐ │
│  │                    WifiStation (wifi_station.cc)                       │ │
│  │  • WiFi 扫描管理 (scan_cache_, TriggerScan)                            │ │
│  │  • 连接验证 (TryConnectAndSave)                                        │ │
│  │  • 模式切换 (STA/APSTA)                                                │ │
│  └───────────────────────────────────────────────────────────────────────┘ │
│                              │                                              │
│                              ▼                                              │
│  ┌───────────────────────────────────────────────────────────────────────┐ │
│  │                    SsidManager (ssid_manager.cc)                       │ │
│  │  • NVS 存储 (ssid, password)                                           │ │
│  │  • 凭证管理 (AddSsid, RemoveSsid, SetDefaultSsid)                      │ │
│  └───────────────────────────────────────────────────────────────────────┘ │
└─────────────────────────────────────────────────────────────────────────────┘
```

### WifiStation 连接流程

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                   WifiStation 自动连接流程 (wifi_station.cc)                 │
└─────────────────────────────────────────────────────────────────────────────┘

WifiStation::Start()
    │
    ├──▶ InitWifiDriver(STA)
    │        ├──▶ esp_netif_create_default_wifi_sta()
    │        ├──▶ esp_wifi_init()
    │        ├──▶ 注册事件处理器 (WIFI_EVENT, IP_EVENT)
    │        ├──▶ esp_wifi_set_mode(WIFI_MODE_STA)
    │        └──▶ esp_wifi_start()
    │
    └──▶ 事件: WIFI_EVENT_STA_START
             │
             ▼
         ┌───────────────────────────────────────────────────────────────┐
         │ WifiEventHandler: WIFI_EVENT_STA_START                        │
         │   1. 自动触发首次扫描 esp_wifi_scan_start()                    │
         │   2. 调用 on_scan_begin_() 回调                               │
         └───────────────────────────────────────────────────────────────┘
             │
             ▼
         事件: WIFI_EVENT_SCAN_DONE
             │
             ▼
         ┌───────────────────────────────────────────────────────────────┐
         │ HandleScanResult()                                             │
         │   1. esp_wifi_scan_get_ap_num() 获取数量                       │
         │   2. esp_wifi_scan_get_ap_records() 获取详情                   │
         │   3. UpdateScanCache() 更新缓存                                │
         │   4. 触发 on_scan_complete_() 回调                             │
         │   5. 如果 scan_only_mode_=true，直接返回                       │
         │   6. 构建 connect_queue_ (匹配凭证的热点队列)                  │
         │   7. 调用 StartConnect()                                       │
         └───────────────────────────────────────────────────────────────┘
             │
             ▼
         ┌───────────────────────────────────────────────────────────────┐
         │ StartConnect()                                                 │
         │   1. 从 connect_queue_ 取第一个热点                            │
         │   2. 配置 wifi_config_t (SSID, Password, BSSID)               │
         │   3. esp_wifi_set_config()                                     │
         │   4. esp_wifi_connect()                                        │
         │   5. 调用 on_connect_() 回调                                   │
         └───────────────────────────────────────────────────────────────┘
             │
             ▼
         ┌──────────────────────────────────┬──────────────────────────────┐
         │ WIFI_EVENT_STA_CONNECTED         │ WIFI_EVENT_STA_DISCONNECTED  │
         │ WiFi 层连接成功                  │ 连接失败                      │
         └──────────────────────────────────┴──────────────────────────────┘
             │                                           │
             ▼                                           ▼
         IP_EVENT_STA_GOT_IP                 ┌───────────────────────────────┐
             │                               │ 重连逻辑:                      │
             ▼                               │ 1. reconnect_count_++ < 5?    │
         ┌───────────────────────────────┐   │    → esp_wifi_connect()       │
         │ IpEventHandler:               │   │ 2. connect_queue_ 还有?       │
         │   1. 保存 ip_address_          │   │    → StartConnect() 下一个   │
         │   2. 设置 WIFI_EVENT_CONNECTED │   │ 3. 都没有 → 放弃             │
         │   3. 调用 on_connected_()      │   └───────────────────────────────┘
         │   4. 清空 connect_queue_       │
         └───────────────────────────────┘
```

---

## 智能联网流程

### SmartConnect 流程图

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                     智能联网流程 (wifi_board.cc:SmartConnect)                │
└─────────────────────────────────────────────────────────────────────────────┘

SmartConnect()
    │
    ├──▶ 设置显示回调
    │        OnScanBegin  → "正在扫描WiFi..."
    │        OnConnect    → "正在连接 xxx..."
    │        OnConnected  → "已连接到 xxx"
    │
    ├──▶ WifiStation.Start()  // 启动 WiFi，自动触发首次扫描
    │
    └──▶ 重试循环 (最多 2 次):
             │
             ├──▶ 等待 3 秒 (扫描 + 尝试连接)
             │        └── 成功 → return true
             │
             ├──▶ 检查匹配热点 GetMatchedAccessPoints()
             │        │
             │        ├── 空: 立即 TriggerScan() + continue (不等待)
             │        │
             │        └── 有匹配: 再等待 10 秒连接
             │                    └── 成功 → return true
             │
             └──▶ 2 次都失败 → return false → 进入配网模式
```

### 参数配置

```cpp
// wifi_board.cc 智能联网参数
static constexpr int MAX_SCAN_RETRY = 2;         // 最大扫描重试次数
static constexpr int SCAN_WAIT_MS = 3000;        // 单次扫描等待时间 (3秒)
static constexpr int CONNECT_TIMEOUT_MS = 10000; // 连接超时时间 (10秒)
```

### 时间对比

| 场景 | 优化前 | 优化后 |
|------|--------|--------|
| 无匹配热点，进入配网 | 3×20s + 2×5s = **70秒** | 2×3s = **6秒** |
| 有匹配但连接失败 | 3×20s + 2×5s = **70秒** | 2×(3s+10s) = **26秒** |
| 正常连接成功 | ~5-10秒 | ~3-10秒 |

---

## 热点配网流程

### WifiAp 流程图

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                         热点配网流程 (wifi_ap.cc)                            │
└─────────────────────────────────────────────────────────────────────────────┘

WifiBoard::StartConfigMode(AP)
    │
    └──▶ WifiAp::Start(ssid_prefix, language)
             │
             ├──▶ WifiStation::SetMode(APSTA)  // 切换到 AP+STA 模式
             │
             ├──▶ 注册扫描回调
             │        wifi.OnScanComplete(WifiAp::OnScanDone)
             │
             ├──▶ ConfigureApInterface()
             │        └──▶ dns_server_.Start()  // DNS 劫持 (强制门户)
             │
             ├──▶ StartWebServer()
             │        │
             │        ├──▶ /              → index.html (配网页面)
             │        ├──▶ /scan          → JSON 热点列表
             │        ├──▶ /submit        → 提交 WiFi 凭证
             │        ├──▶ /saved/list    → 已保存凭证列表
             │        ├──▶ /reboot        → 重启设备
             │        └──▶ /status        → 当前连接状态
             │
             ├──▶ 创建定时扫描器 (每10秒)
             │
             └──▶ WifiStation::TriggerScan()  // 首次扫描

HTTP /submit 请求处理:
    ├──▶ 解析 JSON (ssid, password)
    │
    └──▶ credential_validator_(ssid, password, error)
             │
             └──▶ WifiStation::TryConnectAndSave()
                      │
                      ├──▶ 成功: {"success": true}
                      │         + 延迟调用 on_config_success_()
                      │
                      └──▶ 失败: {"success": false, "error": "..."}
```

### 用户端流程

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                         热点配网用户端流程                                   │
└─────────────────────────────────────────────────────────────────────────────┘

1. 手机连接热点 "Ai-品牌名-XXXX"
       │
       ▼
2. 自动弹出强制门户 (DNS 劫持)
       │  或手动访问 http://192.168.4.1
       ▼
3. 显示配网页面 (index.html)
       │
       ├──▶ 轮询 /scan 获取热点列表
       │
       ├──▶ 选择热点 + 输入密码
       │
       └──▶ POST /submit 提交配置
                │
                ├──▶ 成功 → 跳转 /done.html → 点击 /reboot
                │
                └──▶ 失败 → 显示错误信息
```

---