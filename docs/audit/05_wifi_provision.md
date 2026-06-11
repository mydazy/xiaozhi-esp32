# 05 WiFi & 配网子系统审计报告

> 审计范围：`main/wifi/`（dns_server / ssid_manager / wifi_ap / wifi_station / wifi_csi 及 assets 逻辑）
> 审计方法：三遍（广度遍历 → 红线深挖 → 反审自检）
> 审计日期：2026-05-20
> 关注点：NVS 频写(Flash 寿命)、配网状态机、并发、明文凭据、DNS/AP 输入校验

## 文件清单（共 13 个文件，逻辑代码 8 个 .cc/.h 对 + 2 个 html 资产）

| 文件 | 行数 |
|---|---|
| dns_server.h / dns_server.cc | 27 / 134 |
| ssid_manager.h / ssid_manager.cc | 37 / 137 |
| wifi_ap.h / wifi_ap.cc | 99 / 573 |
| wifi_station.h / wifi_station.cc | 217 / 1046 |
| wifi_csi.h / wifi_csi.cc | 106 / 387 |
| assets/wifi_configuration.html, wifi_configuration_done.html | （前端资产，不计入逻辑审计） |
| README.md | （文档） |

---

# 第一遍 · 广度遍历（显性缺陷）

### 05-P0-A `SaveToNvs` 用 `ESP_ERROR_CHECK(nvs_open(...))`，NVS 打开失败直接 abort → 砖机/重启循环
- **严重等级：P0**。判级理由：`ESP_ERROR_CHECK` 在 NVS 满 / 分区损坏 / 并发占用时会 `abort()` 触发重启；该函数在每次配网保存凭证、删除、设默认时都会走到，量产中 NVS 老化或写满会让设备开机配网即崩，且每次重启又走 `LoadFromNvs`/再保存，构成重启循环 = 砖机表现。属"必然崩溃"红线。
- **文件：`main/wifi/ssid_manager.cc:64`**
- 问题代码：
```cpp
void SsidManager::SaveToNvs() {
    nvs_handle_t nvs_handle;
    ESP_ERROR_CHECK(nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle));  // 失败即 abort
    ...
}
```
- 根因：对可恢复的运行时错误用了致命断言宏。注意构造函数 `LoadFromNvs`（第 31 行）已正确用 `if (ret != ESP_OK) return;` 软处理，写路径却没对齐。
- 触发条件/影响面：NVS 分区写满（凭证频繁改写本身会加速，见 05-P1-A）、Flash 坏块、低电压写入失败。一旦发生，配网保存/删除/SmartConfig 保存全部 abort。
- 修复建议：将第 64 行改为
```cpp
nvs_handle_t nvs_handle;
esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
if (err != ESP_OK) { ESP_LOGE(TAG, "nvs_open RW failed: %s", esp_err_to_name(err)); return; }
```
后续 `nvs_set_str` / `nvs_commit` 同样去掉隐式断言、检查返回值并 `nvs_close`。
- [发现于第一遍]

### 05-P2-A `LoadFromNvs` 遇到中间槽位缺失即 `continue`，可能少加载有效凭证
- **严重等级：P2**。判级理由：体验退化非崩溃。`AddSsid` 用 `pop_back` + `insert(begin)`，正常情况下 0..N 连续；但 `RemoveSsid` 删中间 index 后 `SaveToNvs` 会重排（写满前 size 个、erase 其余），所以一般连续。仅在历史固件遗留稀疏 key 时漏读。
- **文件：`main/wifi/ssid_manager.cc:50-55`**
- 问题代码：
```cpp
if (nvs_get_str(nvs_handle, ssid_key.c_str(), ssid, &length) != ESP_OK) {
    continue;   // 中间槽位缺失则跳过该 i，但循环继续
}
```
- 根因：用 `continue` 而非 `break`，对稀疏存储语义不清晰；同时 `nvs_get_str` 长度不足（ESP_ERR_NVS_INVALID_LENGTH）也被当作"无此 key"静默跳过。
- 触发条件/影响面：跨固件版本升级、历史稀疏槽位时少量凭证丢失，用户需重新配网。
- 修复建议：保持 `continue` 可接受，但对 `ESP_ERR_NVS_INVALID_LENGTH` 单独 `ESP_LOGW` 告警，避免静默丢凭证；长期建议改为单 key 存 JSON 数组（同时解决 05-P1-A 的频写）。
- [发现于第一遍]

### 05-P2-B `/saved/set_default` 与 `/saved/delete` 用 `sscanf("%d")` 解析 index，未校验上界且依赖下游
- **严重等级：P2**。判级理由：`SetDefaultSsid`/`RemoveSsid` 内部已有 `index < 0 || index >= size()` 守卫（ssid_manager.cc:113、123），不会越界崩溃；但 HTTP 层对超长 / 非数字 query 无校验，`sscanf` 失败时 `index` 保持初值 -1，被下游拒绝，属可控。仅边缘输入。
- **文件：`main/wifi/wifi_ap.cc:248`、`wifi_ap.cc:269`**
- 问题代码：
```cpp
int index = -1;
sscanf(&req->uri[pos+7], "%d", &index);   // 未检查 sscanf 返回值
SsidManager::GetInstance().SetDefaultSsid(index);
```
- 根因：未检查 `sscanf` 返回值；初值 -1 恰好被下游守卫挡住，属"偶然安全"。
- 触发条件/影响面：恶意/畸形 query（AP 开放无鉴权，任何连上热点的人可发）。当前不崩，但依赖下游守卫，脆弱。
- 修复建议：`if (sscanf(&req->uri[pos+7], "%d", &index) != 1) { 返回 400; }`，显式拒绝非法输入，不依赖下游。
- [发现于第一遍]

### 05-P3-A DnsServer 未校验 DNS 报文最小长度即改写 header 字节
- **严重等级：P3**。判级理由：`buffer[512]`，写 `buffer[2]/[3]/[7]` 与从 `buffer[len]` 起追加 16 字节，因 `len <= 512-16` 守卫存在（dns_server.cc:111）不会缓冲区溢出；但未校验 `len >= 12`（DNS header），对 0 字节空 UDP 包或畸形短包会生成无效应答。仅协议层畸形，无内存安全风险。
- **文件：`main/wifi/dns_server.cc:116-127`**
- 问题代码：
```cpp
buffer[2] |= 0x80;
buffer[7] = 1;
memcpy(&buffer[len], "\xc0\x0c", 2);  // 名指针指向偏移12，若 len<12 应答无意义
```
- 根因：把任意收到的包都当合法 DNS 查询处理。
- 触发条件/影响面：配网期手机发探测包；最坏只是手机重试，不影响设备稳定性。
- 修复建议：`recvfrom` 后增加 `if (len < 12) continue;`，并可选检查 QR 位（`buffer[2] & 0x80`）只回应查询。
- [发现于第一遍]

### 05-P3-B `esp_netif_get_ip_info` 返回值未检查
- **严重等级：P3**。判级理由：`ap_netif` 已判非空，正常返回 OK；失败时 `ip_info.gw` 为未初始化栈值，DNS 会回应错误网关。概率极低。
- **文件：`main/wifi/wifi_ap.cc:142-144`**
- 问题代码：
```cpp
esp_netif_ip_info_t ip_info;
esp_netif_get_ip_info(ap_netif, &ip_info);  // 未检查返回值，ip_info 可能未初始化
dns_server_.Start(ip_info.gw);
```
- 根因：未检查返回值 + `ip_info` 未清零。
- 触发条件/影响面：netif 状态异常时 DNS 劫持指向脏地址，captive portal 失效。
- 修复建议：`esp_netif_ip_info_t ip_info = {};` 并 `if (esp_netif_get_ip_info(...) != ESP_OK) return;`。
- [发现于第一遍]

---

# 第二遍 · 红线深挖（电源/内存/并发/OTA + 跨文件数据流 + NVS 频写 + 状态机 + 明文凭据）

### 05-P1-A 【NVS 频写·Flash 寿命】每次增删/设默认/连接成功都全量重写 10 组 ssid+password key
- **严重等级：P1**。判级理由：直接威胁 Flash 寿命（红线外但属高频体验退化）。`SaveToNvs` 每次循环 10 个槽位、对每个有效槽 `nvs_set_str(ssid)+nvs_set_str(password)`、对空槽 `nvs_erase_key×2`，共最多 20 次写操作 + 1 次 commit。即使只改 1 个 SSID 也全量重写。更糟的是配网成功路径 `TryConnectAndSave → AddSsid → SaveToNvs`，每次成功连接（含路由器重启后用户重连）都触发一次全量写。NVS 单页擦写寿命约 10 万次，频繁配网/多 SSID 切换场景会加速磨损，并放大 05-P0-A 的 abort 风险。
- **文件：`main/wifi/ssid_manager.cc:62-85`**（`SaveToNvs`），调用点 `87/111/121` 及 `wifi_station.cc:1033`
- 问题代码：
```cpp
for (int i = 0; i < MAX_WIFI_SSID_COUNT; i++) {   // 始终循环 10 次
    if (i < ssid_list_.size()) {
        nvs_set_str(nvs_handle, ssid_key.c_str(), ssid_list_[i].ssid.c_str());     // 全量重写
        nvs_set_str(nvs_handle, password_key.c_str(), ssid_list_[i].password.c_str());
    } else {
        nvs_erase_key(nvs_handle, ssid_key.c_str());      // 空槽每次都尝试擦除
        nvs_erase_key(nvs_handle, password_key.c_str());
    }
}
```
- 根因：用"删 key + 重写全部"代替"只写改动项"。`AddSsid` 中已存在且密码相同时已 `return`（第 92-93 行，正确），但密码变化、新增、删除、设默认都会全量重写。
- 触发条件/影响面：用户频繁切换 WiFi、多机房漫游、自动重连后再保存等场景，Flash 写放大 10-20 倍。
- 修复建议：(1) `SaveToNvs` 只对发生变化的槽位写；(2) 更彻底——改为单 key 存整个列表的 JSON/blob，一次 `nvs_set_blob` 替代 20 次 set/erase；(3) NVS 本身做值比对（写前 `nvs_get_str` 比对相同则跳过），避免无意义擦写。推荐方案 (2)。
- [发现于第二遍]

### 05-P1-B 【明文凭据】WiFi 密码以明文存入 NVS，且 NVS 加密未启用
- **严重等级：P1**。判级理由：安全红线（凭据保护）。`SsidItem.password` 以明文 `nvs_set_str` 写入 namespace `"wifi"`。ESP32-S3 默认 NVS 不加密，物理读取 Flash（拆机/JTAG/OTA dump）即可提取用户家庭 WiFi 密码。量产设备遗失/二手流通有泄露风险。判 P1 而非 P0：需物理接触，非远程必崩。
- **文件：`main/wifi/ssid_manager.cc:76-77`**、结构定义 `ssid_manager.h:8-11`
- 问题代码：
```cpp
nvs_set_str(nvs_handle, password_key.c_str(), ssid_list_[i].password.c_str());  // 明文
```
- 根因：未启用 NVS Encryption（需配 flash encryption + nvs_flash_secure_init），凭据未加密落盘。
- 触发条件/影响面：物理获取设备 → dump Flash → 提取所有保存的 WiFi 明文密码。
- 修复建议：量产开启 Flash Encryption + NVS Encryption（`CONFIG_NVS_ENCRYPTION`，配合 `nvs_flash_secure_init` + key partition）；这与 OTA Secure Boot 是同一组量产安全门禁，建议合并到出货门禁清单。
- [发现于第二遍]

### 05-P1-C 【并发·数据竞争】`/scan` HTTP 线程调用的 `IsCacheValid()` / `GetLastScanTime()` 无锁读 `scan_cache_`，与 WiFi event 任务 `UpdateScanCache` 并发
- **严重等级：P1**。判级理由：并发红线（共享热路径未加锁）。`IsCacheValid()`（wifi_station.cc:165）读 `scan_cache_.empty()` 与 `last_scan_time_ms_` 但**不持 `scan_mutex_`**；同一时刻 WiFi event task（Core 0）在 `UpdateScanCache`（已持锁）执行 `scan_cache_.clear()` + `reserve()` + `push_back()`，正在重分配 vector 内部指针。HTTP 任务读 `.empty()`（访问 size/begin 指针）可能读到撕裂的中间状态 → 偶发崩溃。`GetMatchedAccessPoints` 也调用 `IsCacheValid()`（第 116 行）同样无锁。
- **文件：`main/wifi/wifi_station.cc:165-172`**（`IsCacheValid` 无锁），对照 `186-204`（`UpdateScanCache` 持锁写）
- 问题代码：
```cpp
bool WifiStation::IsCacheValid() const {
    if (scan_cache_.empty()) {       // ← 无锁读，与持锁的 clear()/reserve() 竞争
        return false;
    }
    int64_t now = esp_timer_get_time() / 1000;
    return (now - last_scan_time_ms_) < scan_config_.cache_valid_ms;  // last_scan_time_ms_ 非原子 64bit 撕裂读
}
```
- 根因：读路径漏加 `scan_mutex_`；`last_scan_time_ms_` 是 `int64_t`，32 位核上非原子，撕裂读。
- 触发条件/影响面：配网期手机轮询 `/scan` 恰逢扫描完成回调写缓存，偶发但量产返修高发（并发崩溃最贵，符合本项目纪律第 3 条）。
- 修复建议：`IsCacheValid()` 内 `std::lock_guard<std::mutex> lock(scan_mutex_);` 后再访问 `scan_cache_`/`last_scan_time_ms_`；或将 `last_scan_time_ms_` 改 `std::atomic<int64_t>` 并对 `scan_cache_.empty()` 也走锁。`GetLastScanTime()`（header:73 内联）同步改原子读。
- [发现于第二遍]

### 05-P1-D 【状态机死锁】`is_scanning_` 置 true 后若 SCAN_DONE 事件不来，永久卡死，所有后续扫描被跳过
- **严重等级：P1**。判级理由：高频体验严重退化。`is_scanning_` 仅在 `HandleScanResult`（SCAN_DONE 事件）或 `esp_wifi_scan_start` 失败时复位。若扫描启动成功（`is_scanning_=true`）后，因 `SetMode` 切到 APSTA 调用了 `esp_wifi_scan_stop()`（wifi_station.cc:731）、或模式切换/断连打断了扫描，SCAN_DONE 可能不再投递 → `is_scanning_` 永久为 true。此后 `TriggerScan`（第 77 行 exchange）、`STA_START` 自动扫（第 562 行）、定时器扫（第 872 行）全部因 `is_scanning_` 已 true 被跳过，配网页 `/scan` 永远拿不到新结果。
- **文件：`main/wifi/wifi_station.cc:77-95`、`562-578`、`731`**
- 问题代码：
```cpp
if (is_scanning_.exchange(true, std::memory_order_acq_rel)) {
    ESP_LOGI(TAG, "Scan already in progress, skip");
    return;   // is_scanning_ 一旦卡 true，永久 skip
}
...
// SetMode 中：
esp_wifi_scan_stop();   // 取消扫描但未复位 is_scanning_
```
- 根因：`esp_wifi_scan_stop()` 调用点（wifi_station.cc:271、731、947）均未同步 `is_scanning_ = false`；缺少扫描超时看门狗。
- 触发条件/影响面：配网期扫描进行中用户提交凭证（触发 SetMode/disconnect）、或扫描中模式切换。表现为配网页 SSID 列表空白且刷新无效，用户无法配网。
- 修复建议：(1) 每处 `esp_wifi_scan_stop()` 后加 `is_scanning_.store(false)`；(2) 增加扫描看门狗——`TriggerScan` 时记时间戳，`/scan` 或定时器中若 `is_scanning_ && now - scan_start > 5s` 则强制复位。
- [发现于第二遍]

### 05-P2-C 【并发】`TryConnect` 修改 `try_connect_mode_/ssid_/password_/last_disconnect_reason_` 与 Core 0 事件任务无同步
- **严重等级：P2**。判级理由：偶发。`TryConnect`（主/httpd 任务）写 `try_connect_mode_`（第 936 行）、`ssid_`/`password_`（982-983）、读 `last_disconnect_reason_`（1019）；同时 WiFi event task 在 `WifiEventHandler` 读 `try_connect_mode_`（589、651、687）、写 `last_disconnect_reason_`（583）。这些非原子 bool/string 无锁跨核访问。`try_connect_mode_` 是普通 `bool`，存在可见性/时序窗口：disconnect 事件读到旧的 `try_connect_mode_` 值会误走自动重连退避路径，干扰配网验证。
- **文件：`main/wifi/wifi_station.cc:936、982-983、1003`** vs **`583、589、651、687`**；声明 `wifi_station.h:160-161、193、195`
- 问题代码：
```cpp
try_connect_mode_ = true;     // 主任务写（普通 bool，无内存屏障）
...
ssid_ = ssid; password_ = password;   // std::string 跨核写，event task 读 ssid_ 打印
```
- 根因：配网状态标志与连接上下文未加锁/未原子化，跨核共享。
- 触发条件/影响面：配网验证恰逢上一次连接的 disconnect 事件，偶发误进自动重连，验证器超时返回失败，用户重试。
- 修复建议：`try_connect_mode_`、`scan_only_mode_` 改 `std::atomic<bool>`；`ssid_`/`password_` 访问加 `connect_queue_mutex_` 或新增轻量锁；`last_disconnect_reason_` 改 `std::atomic<uint8_t>`。
- [发现于第二遍]

### 05-P2-D 【内存安全·外部输入】SmartConfig 回调 `memcpy(ssid, evt->ssid, sizeof(evt->ssid))` 后未保证 null 结尾
- **严重等级：P2**。判级理由：边缘场景内存安全。本地 `char ssid[33]={0}` / `password[65]={0}` 已零初始化，`memcpy` 拷 `sizeof(evt->ssid)`。若 IDF 的 `evt->ssid` 字段为 33 字节且被填满，会覆盖到 `ssid[32]`（最后的 0 字节），导致 `AddSsid(ssid,...)`（接收 `std::string`）按 C 串构造时读越界。`evt->ssid` 实际尺寸需对照 IDF 头确认。
- **文件：`main/wifi/wifi_ap.cc:556-561`**
- 问题代码：
```cpp
char ssid[33] = {0}, password[65] = {0};
memcpy(ssid, evt->ssid, sizeof(evt->ssid));      // 若 sizeof(evt->ssid)==33 则填满，无 null 余量
memcpy(password, evt->password, sizeof(evt->password));
...
SsidManager::GetInstance().AddSsid(ssid, password);  // std::string(const char*) 依赖 null 结尾
```
- 根因：依赖源结构体大小恰好留 null 余量，未显式截断终止。`evt->ssid` 具体长度 [待确认]（需查 `smartconfig_event_got_ssid_pswd_t` 定义；ESP-IDF 中 ssid 通常为 32+1 或 33）。
- 触发条件/影响面：SmartConfig 配满 32 字符 SSID + 边界尺寸时，`AddSsid` 读越界 → 脏数据写 NVS 或偶发崩溃。
- 修复建议：拷贝固定字节数并强制终止：`memcpy(ssid, evt->ssid, 32); ssid[32]='\0'; memcpy(password, evt->password, 64); password[64]='\0';`（按 IDF 实际字段长取 min）。
- [发现于第二遍]

### 05-P2-E SmartConfig 凭据未经连接验证直接保存并重启
- **严重等级：P2**。判级理由：体验退化。`SC_EVENT_GOT_SSID_PSWD` 直接 `AddSsid` 保存并 3s 后 `esp_restart()`，注释明示"SmartConfig 不经过验证"。若密码错误，重启后进入正常 STA 模式持续重连失败（走指数退避，05 状态机），且错误凭据已落盘污染 SSID 列表。
- **文件：`main/wifi/wifi_ap.cc:560-566`**
- 问题代码：
```cpp
// SmartConfig 不经过验证，直接保存凭证
SsidManager::GetInstance().AddSsid(ssid, password);
xTaskCreatePinnedToCore([](void *ctx){ vTaskDelay(pdMS_TO_TICKS(3000)); esp_restart(); }, ...);
```
- 根因：SmartConfig 协议本身在连接成功后才发 ACK，但此处未等待 GOT_IP 即保存。
- 触发条件/影响面：邻居/误触 SmartConfig 推送错误凭据，设备保存脏 SSID 并陷入重连。
- 修复建议：保存前可选做一次 `TryConnect` 验证（与 AP 配网路径一致）；至少在 `SC_EVENT_SEND_ACK_DONE`（已确认连接）后再保存，而非 GOT_SSID_PSWD 立即保存。
- [发现于第二遍]

### 05-P3-C `wifi_csi` PingCallback 每 500ms 建/销毁 UDP socket，频繁 syscall
- **严重等级：P3**。判级理由：远期效率/资源。每 500ms `socket()+sendto()+close()`，虽不泄漏（close 配对），但高频建销 socket 浪费 CPU/锁。CSI 默认关闭，影响面小。
- **文件：`main/wifi/wifi_csi.cc:167-190`**
- 根因：未复用 socket。
- 触发条件/影响面：仅 CSI 开启时；轻微功耗/CPU 浪费（与本项目电源域间接相关）。
- 修复建议：socket 在 `Start()` 创建一次、`Stop()` 关闭，PingCallback 复用 fd。
- [发现于第二遍]

---

# 第三遍 · 反审自检（复验 + 对抗视角反查漏报，删误报）

### 复验结论（行号/片段/判级核对）
- 05-P0-A：行号 `ssid_manager.cc:64` 核对无误，`ESP_ERROR_CHECK(nvs_open(...))` 真实存在；对照构造函数 31 行的软处理，判 P0（重启循环=砖机）成立，**不拔高也不压低**。
- 05-P1-A：`SaveToNvs` 全量重写经复读为真（62-85 行 for 循环固定 10 次）；判 P1（Flash 寿命+放大 P0 风险）合理，未拔到 P0（单次写不立即崩）。
- 05-P1-C：复核 `IsCacheValid` 确无 `lock_guard`，而 `GetScanCache/GetDeduplicatedCache/GetCacheCount/ClearScanCache/UpdateScanCache` 全部持锁——**唯独 `IsCacheValid` 和 `GetLastScanTime` 漏锁**，竞争窗口真实，判 P1 成立。
- 05-P1-D：`esp_wifi_scan_stop()` 三处（271/731/947）均未复位 `is_scanning_` 经复读确认；死锁路径成立。
- 误报排查：`DnsServer::Run` 的 header 改写最初疑似 OOB，复核发现 `len <= 512-16` 守卫（111 行）+ buffer[512]，header 索引 2/3/7 恒在界内，**降为 P3（仅协议畸形）**，未误报为内存安全。
- 误报排查：`CsiRxCallback` 的 `buf[idx+1]`，复算 max idx = start+2(n-1)+1 = start+2n-1 ≤ len-1，`zone_names[]`/`names[]` 下标 ≤3 数组长 4，**均安全，不计入问题**。
- 误报排查：`HandleScanResult` 的 `connect_queue_` 访问（419-465）确实持 `connect_queue_mutex_`，`IpEventHandler`/`Stop` 清队列也持锁，**并发已正确处理，不报**。

### 对抗视角新增发现

### 05-P2-F 【攻击配网链路】AP 热点 `WIFI_AUTH_OPEN` 无密码 + HTTP 接口无鉴权，任意人可读已存 SSID 列表 / 删凭据
- **严重等级：P2**。判级理由：安全但需物理邻近 + 配网窗口期。AP 以 `WIFI_AUTH_OPEN`（wifi_station.cc:749、851）开放，配网期任何附近设备连上热点即可：`GET /saved/list` 读出所有已保存 SSID 名（wifi_ap.cc:223-232，明文返回 SSID）、`GET /saved/delete?index=N` 删除凭据、`POST /submit` 注入凭据。无任何鉴权/PIN。判 P2：仅配网窗口期开放，非永久暴露。
- **文件：`main/wifi/wifi_ap.cc:219-279`、`wifi_station.cc:749`**
- 问题代码：
```cpp
ap_config.ap.authmode = WIFI_AUTH_OPEN;   // 配网热点无密码
// /saved/list 直接返回所有 SSID 名，无鉴权
for (const auto& ssid : ssid_list) { json_str += "\"" + ssid.ssid + "\","; }
```
- 根因：配网通道无认证；SSID 列表接口泄露用户已连过的网络名（隐私指纹）。
- 触发条件/影响面：攻击者在配网窗口期连入开放热点，枚举/删除用户凭据、注入恶意 AP 凭据（钓鱼）。
- 修复建议：(1) AP 改用带随机密码（PIN 显示在屏上/包装），`WIFI_AUTH_WPA2_PSK`；(2) `/saved/*` 与 `/submit` 增加简单 token（首页下发、提交校验）；(3) `/saved/list` 不回传完整 SSID 或仅返回数量。产品侧需 Jack 拍板配网便利性 vs 安全（推荐至少做 (1)）。
- [发现于第三遍]

### 05-P2-G 【攻击/资源】`/submit` 验证器在 httpd worker 内同步阻塞最长 10s，仅 3 个 socket，易拒绝服务
- **严重等级：P2**。判级理由：偶发体验退化。`form_submit` 处理器内同步调用 `credential_validator_`（即 `TryConnectAndSave`，阻塞 ≤10s，wifi_station.cc:1000）。`httpd` 仅 `max_open_sockets=3`（wifi_ap.cc:195）。配网期手机的 captive-portal 探测 + 用户连点提交，会占满 worker，其余请求（含 `/`、`/scan`）排队或超时（recv/send_wait 15s）。
- **文件：`main/wifi/wifi_ap.cc:384-385`、`wifi_ap.cc:195`**
- 问题代码：
```cpp
if (ap.credential_validator_) {
    success = ap.credential_validator_(ssid_str, password_str, error_message);  // 同步阻塞≤10s
}
```
- 根因：长耗时连接验证放在 HTTP 同步处理器，socket 池小。
- 触发条件/影响面：用户多次点提交 / 多设备同时连热点，页面卡顿、captive portal 探测失败导致手机判定"无网络"。
- 修复建议：验证改为异步——提交后立即返回"验证中"，后台任务跑 `TryConnect`，前端轮询 `/status`；或对并发提交加互斥，第二个请求直接返回"上一个验证进行中"。
- [发现于第三遍]

### 05-P3-D `last_scan_time_ms_` 64 位非原子读（与 05-P1-C 同源，单列记录）
- **严重等级：P3**。判级理由：32 位核上 `int64_t` 读写非原子，理论撕裂，但仅影响缓存有效期判断（最坏多扫一次或漏扫一次），不崩。随 05-P1-C 一并改为原子即可。
- **文件：`main/wifi/wifi_station.cc:171、199`，header:73、179**
- 修复建议：`std::atomic<int64_t> last_scan_time_ms_`。
- [发现于第三遍]

---

# 统计

| 等级 | 数量 | 编号 |
|---|---|---|
| P0 | 1 | 05-P0-A |
| P1 | 4 | 05-P1-A, 05-P1-B, 05-P1-C, 05-P1-D |
| P2 | 7 | 05-P2-A, 05-P2-B, 05-P2-C, 05-P2-D, 05-P2-E, 05-P2-F, 05-P2-G |
| P3 | 4 | 05-P3-A, 05-P3-B, 05-P3-C, 05-P3-D |
| **合计** | **16** | |

> 逐级核对：P0=1，P1=4，P2=7（05-P2-A~G），P3=4（05-P3-A/B/C/D）。**合计 1+4+7+4 = 16**。

### 三遍新增分布
- 第一遍（广度遍历）：**5** 个 — 05-P0-A、05-P2-A、05-P2-B、05-P3-A、05-P3-B
- 第二遍（红线深挖）：**8** 个 — 05-P1-A、05-P1-B、05-P1-C、05-P1-D、05-P2-C、05-P2-D、05-P2-E、05-P3-C
- 第三遍（反审自检对抗）：**3** 个 — 05-P2-F、05-P2-G、05-P3-D；外加复验阶段删除 3 个疑似误报（DNS header OOB→降级为 05-P3-A、CSI 下标越界→排除、connect_queue 并发→已正确加锁排除）
- **合计 5 + 8 + 3 = 16 条**。

### 出货门禁建议（按风险离量产近→远）
1. **05-P0-A** 一行级修复，出货前必须清零（abort→软返回）。
2. **05-P1-A/B/C/D** 量产前收敛：NVS 频写改 blob、明文凭据随 Flash Encryption 一并上、`IsCacheValid` 补锁、扫描看门狗。
3. P2 安全组（05-P2-F/G）需 Jack 产品侧拍板配网便利 vs 安全。
