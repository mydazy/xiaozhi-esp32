# WiFi / Blufi / 配网子系统缺陷审计报告

> 审计范围：`main/wifi/`（10 源文件）+ `main/blufi/`（7 源文件）
> 审计日期：2026-05-20 · 目标平台：ESP32-S3 / ESP-IDF 5.5 / FreeRTOS / NimBLE
> 审计人：嵌入式固件审计（只读审计，未改动源码）

## 子系统概述

本子系统承担三件事：

1. **WiFi STA 联网与重连**（`wifi_station.cc`）：扫描 → 凭证匹配 → 连接队列 → 指数退避重连（20 次后转 5 分钟稀疏重试），事件回调跑在 ESP-IDF 的 WiFi event task（默认 Core 0）。
2. **两条配网通道**：
   - **AP 配网**（`wifi_ap.cc` + `dns_server.cc`）：APSTA 模式 + Captive Portal + HTTP 表单 + SmartConfig，凭证走明文 HTTP。
   - **BLE Blufi 配网**（`blufi.cc` + `blufi_init.c` + `blufi_security.c`）：NimBLE 主机栈 + Blufi 协议 + DH/AES 应用层加密，凭证走 BLE。
3. **凭证持久化**（`ssid_manager.cc`）：最多 10 组 SSID/密码明文存入 NVS `wifi` 命名空间。
4. 附属功能：`wifi_csi.cc`（人体接近检测，默认关闭）、`ibeacon.cc`（iBeacon 观察者，量产 stub）。

整体架构成熟、对竞态/双重释放/重连风暴等问题已有大量针对性修复（atomic exchange 释放 timer、连接队列加锁、稀疏退避封顶）。但仍存在若干**线程安全**、**凭证泄漏**、**NVS 寿命**与**BLE 配网鉴权**层面的缺陷，按严重等级分组如下。

---

## P0 — 必崩 / 砖机 / 安全漏洞 / 必然崩溃

### P0-1 SsidManager 全局单例无任何锁，跨任务并发读写 std::vector → use-after-free 崩溃

- **判级理由**：`SsidManager` 的 `ssid_list_`（std::vector）被 **WiFi event task**（`HandleScanResult` 调 `GetSsidList`）、**HTTP httpd task**（`/saved/list`、`/saved/delete`、`/saved/set_default` 调 `GetSsidList`/`RemoveSsid`/`SetDefaultSsid`）、**Blufi 配网任务**（`TryConnectAndSave` → `AddSsid`）、**SmartConfig event task**（`AddSsid`）多任务并发访问，且 `RemoveSsid`/`AddSsid` 会 `erase`/`insert`/`pop_back` 导致 vector 重新分配。一边遍历返回的 `const&` 列表、另一边 erase，是确定的并发 UAF/迭代器失效，触发即 LoadProhibited 崩溃。判 P0。
- **文件**：`main/wifi/ssid_manager.h:23,32`（`GetSsidList` 返回 `const&`、`ssid_list_` 无 mutex）；`main/wifi/ssid_manager.cc:86-113`（AddSsid/RemoveSsid 无锁修改）
- **问题代码**：
  ```cpp
  // ssid_manager.h
  const std::vector<SsidItem>& GetSsidList() const { return ssid_list_; }  // 无锁返回引用
  std::vector<SsidItem> ssid_list_;                                        // 无 mutex 保护
  // ssid_manager.cc:111
  ssid_list_.erase(ssid_list_.begin() + index);  // HTTP task 调用，可能正被其它任务遍历
  ```
- **根因**：单例被多任务共享但完全没有同步原语；`GetSsidList` 返回内部容器引用，调用方在锁外长时间持有/遍历。
- **触发条件与影响面**：配网期手机端轮询 `/saved/list` 的同时点击删除，或扫描完成回调正在遍历 `GetSsidList` 时 Blufi 任务 `AddSsid`。配网高频操作场景，必现 race，崩溃概率随并发上升。影响所有配网用户。
- **修复建议**：在 `SsidManager` 内加 `std::mutex mutex_`，所有 public 方法持 `lock_guard`；`GetSsidList` 改为**按值返回拷贝** `std::vector<SsidItem> GetSsidList() const`（调用方已是临时拷贝使用，改动小），消除引用逃逸。

### P0-2 Blufi 全部状态标志为非 atomic bool，NimBLE host task / esp_timer task / 业务任务跨核竞争

- **判级理由**：`ble_connected_` `scanning_` `initialized_` `advertising_` `stopping_` `scan_retry_count_` `conn_handle_` 均为裸 `bool/int`（`blufi.h:67-83`）。`BlufiCallback`（NimBLE host task）、`OnScanDone`（esp_timer task）、`Stop`（主任务/UI 任务）、`blufi_wifi` 连接任务（Core 0）并发读写这些标志，且 `Stop()` 会在 `BlufiCallback` 仍可能运行时 `esp_blufi_host_deinit` + 释放 `init_done_sem_`/`retry_timer_`。非原子标志在双核上既有可见性问题（一个核改了另一个核读到旧值），也无法保证 `Stop` 与回调之间的 happens-before。`init_done_sem_` 在 `Stop` 中 `vSemaphoreDelete` 后，若 `INIT_FINISH` 回调晚到会 `xSemaphoreGive` 已删除句柄 → 崩溃。结合 BLE 配网随时可被用户取消，判 P0。
- **文件**：`main/blufi/blufi.h:67-83`（状态字段定义）；`main/blufi/blufi.cc:194-242`（Stop 与回调并发）；`main/blufi/blufi.cc:451-453`（give 信号量）
- **问题代码**：
  ```cpp
  // blufi.h —— 跨任务共享却非 atomic
  bool ble_connected_ = false;  bool scanning_ = false;  bool stopping_ = false;
  // blufi.cc:238  Stop() 删除信号量
  if (init_done_sem_) { vSemaphoreDelete(init_done_sem_); init_done_sem_ = nullptr; }
  // blufi.cc:451  晚到的 INIT_FINISH 回调仍 give
  if (self.init_done_sem_) { xSemaphoreGive(self.init_done_sem_); }
  ```
- **根因**：违反「多核共享变量用 std::atomic」规范；`Stop` 与异步 NimBLE 回调缺乏关闭握手（先停广播/置 stopping 再 deinit，但回调可能已进入函数体）。
- **触发条件与影响面**：用户在配网中途退出 / 配网成功触发 `Stop` 的同时 NimBLE 仍在派发 `INIT_FINISH` 或 `BLE_DISCONNECT`。BLE 配网是首次开机必经路径，偶发但后果是硬崩。
- **修复建议**：① 全部状态标志改 `std::atomic<bool>` / `std::atomic<int>`；② `conn_handle_` 改 `std::atomic<uint16_t>`；③ `Stop()` 先 `stopping_=true` 并 `esp_blufi_register_callbacks(nullptr)`（或确认 host_deinit 已停回调）再删信号量；④ 给信号量 give 前再判一次 `stopping_`。

### P0-3 SaveToNvs 每次增删改都重写全部 10×2 个键，频繁配网磨损 Flash

- **判级理由**：`AddSsid`/`RemoveSsid`/`SetDefaultSsid`/`Clear` 任一调用都进 `SaveToNvs`，后者无条件对 20 个 key 逐个 `nvs_set_str` 或 `nvs_erase_key` 再 `nvs_commit`（`ssid_manager.cc:61-84`）。即使只改 1 组凭证也会重写 20 个 entry。配网验证流程 `TryConnectAndSave` 每次成功都写一遍；用户多次试错、SmartConfig 重连、`SetDefaultSsid` 拖动排序都会触发整表重写。NVS 底层 flash 扇区擦写有寿命上限，长期高频写入是 Flash 磨损/砖机风险（CLAUDE.md 明列「NVS 频繁写损耗 Flash」为审计点）。因属累积型硬件损伤，判 P0（远期但确定性硬件后果）。
- **文件**：`main/wifi/ssid_manager.cc:61-84,103`
- **问题代码**：
  ```cpp
  void SsidManager::SaveToNvs() {
      for (int i = 0; i < MAX_WIFI_SSID_COUNT; i++) {  // 每次全表 20 个 key
          if (i < ssid_list_.size()) { nvs_set_str(...ssid...); nvs_set_str(...password...); }
          else { nvs_erase_key(...); nvs_erase_key(...); }
      }
      nvs_commit(nvs_handle);
  }
  ```
- **根因**：未做差异写入；NVS 库本身对「值未变」的 set 会跳过物理写，但 `erase_key` 对不存在的 key、以及排序导致的全表 set 仍产生大量写。
- **触发条件与影响面**：演示机/门店反复配网、用户频繁切换默认网络。影响 Flash 寿命与长期可靠性。
- **修复建议**：① `AddSsid` 命中已存在且密码未变时直接 `return`，不写 NVS；② 改为「单条键」写入而非全表重写（只 set 受影响的索引，删除时只 erase 末位）；③ 或将整个 list 序列化为单个 blob（`nvs_set_blob`）一次写入，避免 20 次 entry 操作。

---

## P1 — 高频崩溃 / 体验严重退化

### P1-1 配网 SSID / 密码 / 绑定码以明文写入日志

- **判级理由**：`RECV_STA_SSID` 用 `ESP_LOGI` 打印 SSID（`blufi.cc:636`）、`RECV_CUSTOM_DATA` 打印绑定码（`blufi.cc:653`）、`HandleScanResult` 打印匹配 AP 的 SSID（`wifi_station.cc:154,426`）、SmartConfig 打印 SSID（`wifi_ap.cc:559`）。`ssid_manager.cc:88` 还在 INFO 级打印对比 SSID。虽然密码本身在 Blufi 路径未直接打印（`[5/8] 收到密码` 不带值），但 SSID + 绑定码属敏感凭据，量产固件日志可经 UART/USB 抓取，构成凭据泄漏。默认 `CONFIG_LOG_DEFAULT_LEVEL>=INFO` 时全部生效。判 P1（安全/隐私退化，非崩溃）。
- **文件**：`main/blufi/blufi.cc:636,653`；`main/wifi/wifi_ap.cc:559`；`main/wifi/wifi_station.cc:154,426`；`main/wifi/ssid_manager.cc:88`
- **问题代码**：
  ```cpp
  // blufi.cc:636
  ESP_LOGI(TAG, "[4/8] 收到SSID: %s", self.ssid_.c_str());
  // blufi.cc:653
  ESP_LOGI(TAG, "[6/8] 收到绑定码: %s", self.binding_code_.c_str());
  // wifi_ap.cc:559  SmartConfig
  ESP_LOGI(TAG, "SmartConfig: %s", ssid);
  ```
- **根因**：调试期日志未在量产构建降级/脱敏。
- **触发条件与影响面**：每次配网必触发；任何能读串口日志的人可获取用户 WiFi SSID 与设备绑定码。
- **修复建议**：① SSID/绑定码改 `ESP_LOGD`（量产 INFO 级不输出），或脱敏打印（仅长度/前 2 后 2 字符）；② 密码相关行确认永不打印值；③ 在量产构建把 `CONFIG_LOG_DEFAULT_LEVEL` 设为 WARN。

### P1-2 BLE Blufi 无配对/绑定/MITM，且未强制应用层加密，凭证可被嗅探/中间人

- **判级理由**：`blufi_init.c:219-234` 设置 `sm_io_cap=4`(NoInputNoOutput)、`sm_sc=0`、未定义 `CONFIG_EXAMPLE_BONDING`/`CONFIG_EXAMPLE_MITM`，即 BLE 链路层无 Secure Connections、无 MITM 保护、无绑定。配网安全完全依赖 Blufi 应用层 DH+AES（`blufi_security.c`），但 DH 用裸 MD5(shared)→AES-128-CFB 无身份认证（无 PSK 预共享、无证书），**对主动中间人无防护**——攻击者可与设备和手机分别协商各自的 DH，转发并解密 WiFi 密码。同时代码层面**未校验**手机是否真的启用了加密/校验位（Blufi `frame_ctrl` 的 enc/checksum 标志由对端声明），固件照单全收。判 P1（配网期短窗口、需邻近攻击，非必崩，但属真实可利用的凭证泄漏）。
- **文件**：`main/blufi/blufi_init.c:219-234`；`main/blufi/blufi_security.c:118-137`
- **问题代码**：
  ```cpp
  // blufi_init.c
  ble_hs_cfg.sm_io_cap = 4;       // NoInputNoOutput
  ble_hs_cfg.sm_sc = 0;           // 无 Secure Connections
  // CONFIG_EXAMPLE_BONDING / CONFIG_EXAMPLE_MITM 均未定义 → 无绑定无 MITM
  // blufi_security.c —— DH 无身份认证
  ret = mbedtls_md5(blufi_sec->share_key, blufi_sec->share_len, blufi_sec->psk);
  mbedtls_aes_setkey_enc(&blufi_sec->aes, blufi_sec->psk, 128);
  ```
- **根因**：沿用 Espressif Blufi 示例的默认安全级别，未针对量产做加固；无应用层「必须加密」强制门禁。
- **触发条件与影响面**：配网期间（首次开机/重配）邻近攻击者发起 MITM 或被动嗅探。影响所有走 BLE 配网且处于敌意 RF 环境的用户。
- **修复建议**：① 在 `REQ_CONNECT_TO_AP` 处理前校验本次会话已完成 DH 协商且数据帧 enc 标志为真，否则拒绝并 `report_error`；② 引入带外校验（设备屏显/小程序展示一次性 PIN，作为 DH 后的 PSK 混入），抵御 MITM；③ 评估开启 BLE SC + 数字比较（`sm_sc=1`, `sm_io_cap` 配 DisplayYesNo）。

### P1-3 BLE_CONNECT 事件回调内执行同步 WiFi 阻塞操作，阻塞 NimBLE host task

- **判级理由**：`BlufiCallback` 的 `BLE_CONNECT` 分支（`blufi.cc:497-511`）在 WiFi 未初始化时直接调 `wifi.Start()`（同步走 `InitWifiDriver`：`esp_netif_init`/`esp_wifi_init`/`esp_wifi_start` 一连串带 `ESP_ERROR_CHECK` 的同步调用）。该回调运行在 NimBLE host task 上下文，长时间阻塞会拖慢 BLE 事件派发，可能撞 BLE supervision timeout（虽随后改到 6s）。同理 `Start()`（`blufi.cc:140-146`）在调用线程内 `vTaskDelay` 自旋等待扫描完成最多 3s。配网通道初始化属高频路径，体验退化明显。注：`REQ_CONNECT_TO_AP` 已正确地把 `TryConnectAndSave`（最长 10s 阻塞）丢到独立任务（`blufi.cc:554`），但 `BLE_CONNECT` 的 `wifi.Start()` 未做同样处理。判 P1。
- **文件**：`main/blufi/blufi.cc:497-511`；`main/wifi/wifi_station.cc:780-890`（InitWifiDriver 同步链）
- **问题代码**：
  ```cpp
  // blufi.cc BLE_CONNECT 分支，运行在 NimBLE host task
  if (!wifi.IsInitialized()) {
      wifi.SetScanOnlyMode(true);
      self.scanning_ = true;
      wifi.Start();   // 同步初始化整个 WiFi 驱动，阻塞 host task
  }
  ```
- **根因**：在异步 BLE 回调里做重量级同步初始化，未异步化。
- **触发条件与影响面**：BLE 先连、WiFi 未初始化的配网首连场景。WiFi 驱动初始化期间 BLE 事件积压，可能导致连接参数协商/列表下发延迟甚至断连。
- **修复建议**：把 `wifi.Start()` 改为投递到独立任务（参照 `REQ_CONNECT_TO_AP` 的 `xTaskCreatePinnedToCore` 模式，Core 0），回调内只置标志立即返回。

### P1-4 GetMatchedAccessPoints 在调用线程内自旋等待扫描最长 3s（潜在阻塞 UI/HTTP）

- **判级理由**：`GetMatchedAccessPoints(force_refresh)` 触发扫描后用 `for (i<30) vTaskDelay(100ms)` 同步轮询 `is_scanning_`（`wifi_station.cc:111-119`），最长阻塞 3s。该方法是 public API，若被 HTTP `/scan` 之外的 UI 线程或主循环调用会造成卡顿。当前 `wifi_ap.cc` 的 `/scan` 走的是非阻塞 `GetDeduplicatedCache`，但该阻塞 API 仍暴露且 `Blufi::Start` 类似自旋。判 P1（体验退化）。
- **文件**：`main/wifi/wifi_station.cc:111-119`
- **问题代码**：
  ```cpp
  if (force_refresh || !IsCacheValid()) {
      TriggerScan();
      for (int i = 0; i < 30 && is_scanning_; i++) { vTaskDelay(pdMS_TO_TICKS(100)); }  // 阻塞 3s
  }
  ```
- **根因**：同步等待异步扫描结果。
- **触发条件与影响面**：任意调用方传 `force_refresh=true` 或缓存失效。阻塞 3s。
- **修复建议**：文档标注「阻塞，禁止在 UI/event 上下文调用」，或提供纯回调式异步变体；上层统一走 `OnScanComplete` + 缓存读取。

---

## P2 — 偶发 / 边缘场景

### P2-1 DnsServer::Stop 对已自删除的任务句柄调用 eTaskGetState，潜在访问已释放 TCB

- **判级理由**：DNS task 体内 `Run()` 返回后 `vTaskDelete(NULL)` 自删；`Stop()` 用 `eTaskGetState(task_handle_)==eDeleted` 轮询（`dns_server.cc:74-82`）。任务自删后其 TCB 可能已被 idle task 回收，此时对 `task_handle_` 调 `eTaskGetState` 是对已释放内存的访问，行为未定义。多数情况下因 1s socket 超时窗口 task 尚未退出而侥幸通过，属偶发。判 P2。
- **文件**：`main/wifi/dns_server.cc:74-82`
- **问题代码**：
  ```cpp
  for (int i = 0; i < 20; i++) {
      if (eTaskGetState(task_handle_) == eDeleted) break;  // 句柄可能已失效
      vTaskDelay(pdMS_TO_TICKS(100));
  }
  ```
- **根因**：用任务句柄状态轮询代替显式退出握手。
- **触发条件与影响面**：配网频繁 Start/Stop DNS。偶发崩溃或误判。
- **修复建议**：task 退出前 give 一个 `done_sem_`，`Stop()` 改 `xSemaphoreTake(done_sem_, timeout)`；或让 task 退出前自置 `task_handle_=nullptr`（仍有 TOCTOU，信号量更稳）。

### P2-2 blufi_dh_negotiate_data_handler 用对端长度字段直接 memcpy，越界读 / data[0] 无长度校验

- **判级理由**：`blufi_security.c:81` 用对端发来的 `data[1]<<8|data[2]` 作为 `dh_param_len` 并 `malloc`，随后 `SEC_TYPE_DH_PARAM_DATA` 分支 `memcpy(blufi_sec->dh_param, &data[1], dh_param_len)`（`:100`）——若 `dh_param_len` 大于本帧实际 `len-1`，则从 `data` 越界读。另 `:71` `uint8_t type = data[0]` 在 `len==0` 时越界读。这是 Espressif 参考实现的已知薄弱点，Blufi 库通常已对帧长做上层校验，但本地未加防护。需对端恶意构造帧触发，判 P2。
- **文件**：`main/blufi/blufi_security.c:68-101`
- **问题代码**：
  ```cpp
  uint8_t type = data[0];                              // len==0 越界
  ...
  blufi_sec->dh_param_len = ((data[1]<<8)|data[2]);    // 对端可控
  memcpy(blufi_sec->dh_param, &data[1], blufi_sec->dh_param_len);  // 可越界读 data
  ```
- **根因**：未用入参 `len` 校验 `dh_param_len <= len-1` 与 `len>=3`。
- **触发条件与影响面**：恶意 BLE 对端在 DH 协商阶段发畸形帧。配网期、需主动攻击。
- **修复建议**：函数开头 `if (len < 3) return;`；`SEC_TYPE_DH_PARAM_LEN` 分支限制 `dh_param_len` 上界（如 ≤512）；`PARAM_DATA` 分支校验 `len-1 >= dh_param_len` 否则报错返回。

### P2-3 SmartConfig 凭证不经验证直接保存并重启，且未释放 SmartConfig 资源

- **判级理由**：`SmartConfigEventHandler` 的 `GOT_SSID_PSWD` 分支（`wifi_ap.cc:554-567`）直接 `AddSsid` 保存凭证并起 `restart` 任务 `esp_restart`，**未经 `TryConnect` 验证**，错误密码也会被保存→重启后陷入重连失败。且重启前未 `esp_smartconfig_stop()`（仅 `SEND_ACK_DONE` 分支停）。与主配网通道「先验证后保存」策略不一致。SmartConfig 是否默认启用未确认 [待确认 `StartSmartConfig` 调用点]。判 P2。
- **文件**：`main/wifi/wifi_ap.cc:554-567`
- **问题代码**：
  ```cpp
  case SC_EVENT_GOT_SSID_PSWD: {
      ...
      SsidManager::GetInstance().AddSsid(ssid, password);   // 不验证直接保存
      xTaskCreatePinnedToCore([](void*){ vTaskDelay(3000); esp_restart(); }, ...);  // 裸 esp_restart
  }
  ```
- **根因**：SmartConfig 路径未对齐主流程的验证器机制；用裸 `esp_restart` 而非 `Application::Reboot`（其它路径已强调要走安全 Reboot 序列，见 `wifi_ap.cc:473`）。
- **触发条件与影响面**：用户用 SmartConfig（一键配网）输错密码。保存坏凭证 + 不安全重启（可能 I2S/codec 未停）。
- **修复建议**：保存前调 `WifiStation::TryConnect` 验证；改用 `Application::GetInstance().Reboot()`；重启前 `esp_smartconfig_stop()`。

### P2-4 StartConnect / TryConnect 在 WiFi event 上下文用 vTaskDelay + WaitBits 引入固定延时

- **判级理由**：`StartConnect`（`wifi_station.cc:485-489`）和 `TryConnect`（`:950-956`）在配置前 `esp_wifi_disconnect()` + `xEventGroupWaitBits(...1500ms)` + `vTaskDelay(50ms)`。`StartConnect` 由 `HandleScanResult` 在 WiFi event task 内调用，这条路径会在 event task 上阻塞最长 ~1.55s，期间无法处理其它 WiFi 事件。属为兼容 IDF 5.5「sta is connecting」态而引入的必要等待，但放在 event task 内是次优。判 P2。
- **文件**：`main/wifi/wifi_station.cc:485-489`
- **问题代码**：
  ```cpp
  esp_err_t dret = esp_wifi_disconnect();
  if (dret == ESP_OK) xEventGroupWaitBits(event_group_, WIFI_EVENT_DISCONNECTED, pdTRUE, pdFALSE, pdMS_TO_TICKS(1500));
  vTaskDelay(pdMS_TO_TICKS(50));
  ```
- **根因**：连接前置同步等待放在了 event task 调用链上。
- **触发条件与影响面**：每次扫描后自动连接。event task 短时阻塞，弱网下可能错过事件。
- **修复建议**：将 `StartConnect` 的实际连接动作投递到独立任务执行，event task 只触发不阻塞。

### P2-5 AP 配网 HTTP 全程明文，密码经未加密 HTTP POST 传输

- **判级理由**：`/submit` 接口（`wifi_ap.cc:332-415`）通过明文 HTTP（192.168.4.1）接收 JSON 中的 `password`。AP 为 `WIFI_AUTH_OPEN`（`wifi_station.cc:746,848`），任何人可连入该开放热点并嗅探同网段明文流量。属配网通道固有弱点，影响面受限于配网短窗口 + 物理邻近，判 P2。
- **文件**：`main/wifi/wifi_ap.cc:332-376`；`main/wifi/wifi_station.cc:746,848`（`authmode = WIFI_AUTH_OPEN`）
- **问题代码**：
  ```cpp
  ap_config.ap.authmode = WIFI_AUTH_OPEN;   // 开放热点
  // /submit 明文接收密码
  password_str = password_item->valuestring;
  ```
- **根因**：配网 AP 开放 + HTTP 明文，无 TLS/无 AP 密码。
- **触发条件与影响面**：用户用 AP 网页配网，邻近攻击者连入开放 AP 嗅探。
- **修复建议**：① 给配网 AP 设 WPA2 密码（设备屏显/印刷）；② 评估在网页端用预置公钥对密码做端到端加密后再 POST。

### P2-6 config_callback_fired_ 一旦置位永不复位，单次启动内二次配网回调失效

- **判级理由**：`/submit` 与 `/reboot` 用类静态 `config_callback_fired_.exchange(true)` 防重入（`wifi_ap.cc:396,471`），仅在 `WifiAp::Start` 时 `store(false)`（`:60`）。若同一次启动内 `Stop` 后再次 `Start`，标志会被重置 OK；但若配网成功 `/submit` 已置位、用户又点 `/reboot`，`/reboot` 的回调会被吞掉（已 fired），可能导致预期的「reboot」动作不触发而停在成功页。属边缘交互。判 P2。
- **文件**：`main/wifi/wifi_ap.cc:396,471`
- **根因**：`/submit` 成功回调与 `/reboot` 回调共用同一防重入标志，语义重叠。
- **触发条件与影响面**：用户先触发 submit 成功回调、再点 reboot 按钮。reboot 不执行。
- **修复建议**：`/submit` 与 `/reboot` 用各自独立的防重入标志，或明确二者只允许其一。

---

## P3 — 潜在远期风险

### P3-1 WiFi 事件 handler 用 ESP_EVENT_ANY_ID 注册却仅在 unregister 时按 ANY_ID 注销，AP 子事件无 handler

- **判级理由**：`WifiEventHandler` 注册了 `WIFI_EVENT/ANY_ID`，但只处理 STA 子事件，AP 的 `AP_STACONNECTED/AP_STADISCONNECTED` 在 `WifiAp` 中有静态处理函数（`OnApStaConnected/OnApStaDisconnected`）却**从未注册**到事件循环（grep 无注册点）。仅日志缺失，无功能影响。判 P3。
- **文件**：`main/wifi/wifi_ap.cc:178-188`（定义未注册）；`main/wifi/wifi_station.cc:818-822`
- **修复建议**：若需 AP 客户端日志，在 APSTA 模式注册对应 handler；否则删除死代码。

### P3-2 wifi_station.cc 中 ssid_/password_ 成员明文常驻 RAM，且 Stop 后才 clear

- **判级理由**：`ssid_` `password_` 作为 `std::string` 成员长期持有当前网络明文密码，连接成功后未清零，仅 `Stop()` 时 clear（`wifi_station.cc:312-313`）。RAM dump/调试探针可读取。常驻明文属轻度信息泄漏面，判 P3。
- **文件**：`main/wifi/wifi_station.h:160-161`；`main/wifi/wifi_station.cc:477-478,979-980`
- **修复建议**：连接成功（GOT_IP）后清空 `password_`（重连可从 SsidManager 重取），减少明文驻留时间。

### P3-3 reconnect 退避稀疏期固定 5 分钟，密码改后设备最长 5 分钟才重试，且日志/电流可控但 SSID 永久失配无上报

- **判级理由**：20 次后转 5 分钟稀疏重试（`wifi_station.cc:619-631`）已是良好实践，但稀疏期无限持续、不上报「疑似密码已改」给业务层/用户，设备可能长期处于「连不上也不告警」状态。属体验/可观测性远期问题，判 P3。
- **文件**：`main/wifi/wifi_station.cc:619-635`
- **修复建议**：进入稀疏期时触发一次 `on_disconnected_` 携带特殊原因码，让上层提示用户重新配网。

### P3-4 DnsServer::Run 未校验 DNS 请求最小长度（<12 字节头）即改写 buffer[7]

- **判级理由**：`dns_server.cc:117-119` 对任意 `len>=0`（实际 recvfrom 返回 >0）的包都写 `buffer[2]/[3]/[7]`，未校验 `len>=12`（DNS 头长度）。因 buffer 为 512 字节本地数组，写固定低偏移不会越界，仅会对畸形短包产生无意义应答。无安全/崩溃后果，判 P3。
- **文件**：`main/wifi/dns_server.cc:110-130`
- **修复建议**：开头加 `if (len < 12) continue;`。

---

## 统计

| 等级 | 数量 | 编号 |
|------|------|------|
| P0   | 3    | P0-1, P0-2, P0-3 |
| P1   | 4    | P1-1, P1-2, P1-3, P1-4 |
| P2   | 6    | P2-1 ~ P2-6 |
| P3   | 4    | P3-1 ~ P3-4 |
| **合计** | **17** | |

> 待确认项：SmartConfig（`StartSmartConfig`）在产品中是否默认启用（影响 P2-3 实际触发概率）；Blufi `frame_ctrl` 加密/校验强制策略是否在 esp_blufi 库层另有门禁（影响 P1-2 判级）。
