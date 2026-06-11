# 06 Blufi / BLE / iBeacon 子系统审计

> 审计范围：`main/blufi/`（blufi.cc/.h、blufi_init.c/.h、blufi_security.c、ibeacon.cc/.h）
> 关注点：BLE 回调内存、blufi security 校验、配网链路外部输入校验、并发。
> 审计方法：三遍递进（广度遍历 → 红线深挖 → 反审自检），严格判级。
> 文件数：7（blufi.cc, blufi.h, blufi_init.c, blufi_init.h, blufi_security.c, ibeacon.cc, ibeacon.h）

---

## 第一遍 · 广度遍历（逐文件精读，抓显性缺陷）

### 06-P0-A —— DH 协商帧未校验长度，畸形包导致堆溢出/越界读

- **严重等级：P0**。判级理由：外部 BLE 报文（DH 协商帧）直接驱动 `malloc` 大小与 `memcpy` 长度，无任何长度边界校验，攻击者一个畸形包即可造成堆缓冲区溢出 / 越界读，可砖机或被利用。属四条硬红线中的"内存安全"，必崩，出货前必须清零。
- **文件：`main/blufi/blufi_security.c:68-100`**
- **问题代码片段：**
```c
void blufi_dh_negotiate_data_handler(uint8_t *data, int len, ...)
{
    uint8_t type = data[0];                      // len 未校验，len==0 时越界读 data[0]
    ...
    case SEC_TYPE_DH_PARAM_LEN:
        blufi_sec->dh_param_len = ((data[1]<<8)|data[2]);   // 读 data[1]/data[2]，len<3 越界
        blufi_sec->dh_param = (uint8_t *)malloc(blufi_sec->dh_param_len);  // 长度全由对端控制
        ...
    case SEC_TYPE_DH_PARAM_DATA:
        memcpy(blufi_sec->dh_param, &data[1], blufi_sec->dh_param_len);  // 拷 dh_param_len 字节，
                                                                        // 不校验 len-1 是否够长
```
- **根因**：沿用 Espressif 官方 example 代码，未做产品化加固。`type=data[0]`、`dh_param_len=(data[1]<<8)|data[2]` 在 `len` 未知时即解引用；`SEC_TYPE_DH_PARAM_DATA` 分支用上一帧约定的 `dh_param_len` 从本帧 `&data[1]` 拷贝，但本帧实际 `len` 可能远小于 `dh_param_len`，造成对源缓冲区的越界读，并把垃圾写入堆。
- **触发条件/影响面**：手机端（或恶意 BLE 设备）连接后，在加密协商阶段发送：① 长度为 0/1/2 的协商帧 → 越界读 `data[0..2]`；② 先发 `DH_PARAM_LEN` 声明一个大 `dh_param_len`，再发一个短 `DH_PARAM_DATA` 帧 → `memcpy` 越界读源、写入 `malloc` 的堆块（若 PARAM_LEN 声明值小于 DATA 实际，则堆写溢出）。配网阶段任何用户都可触发，必崩或可利用。
- **修复建议**：函数入口加 `if (data == NULL || len < 1) { btc_blufi_report_error(ESP_BLUFI_DATA_FORMAT_ERROR); return; }`；`SEC_TYPE_DH_PARAM_LEN` 分支加 `if (len < 3) {report_error; return;}` 且对 `dh_param_len` 设上限（如 `> DH_SELF_PUB_KEY_LEN*4` 或 `> 1024` 即拒绝）；`SEC_TYPE_DH_PARAM_DATA` 分支 `memcpy` 前加 `if (len - 1 < blufi_sec->dh_param_len) {report_error; return;}`。
- [发现于第一遍]

### 06-P2-B —— DH `malloc(dh_param_len)` 当声明长度为 0 时返回 NULL 被误判

- **严重等级：P2**。判级理由：`dh_param_len=0` 时 `malloc(0)` 实现相关（可能返回非 NULL 的最小块或 NULL），后续 `DH_PARAM_DATA` 分支 `memcpy(...,0)` 不崩，但逻辑无意义；边缘场景，不必崩。
- **文件：`main/blufi/blufi_security.c:81-91`**
- **根因**：未对 `dh_param_len==0` 做语义校验。
- **触发条件/影响面**：对端发送 `DH_PARAM_LEN=0`。一般不导致崩溃，但浪费一次协商、可能让协商卡死等超时。
- **修复建议**：在 06-P0-A 的上限校验里一并加下限 `if (blufi_sec->dh_param_len == 0) {report_error; return;}`。
- [发现于第一遍]

### 06-P2-C —— `blufi_aes_encrypt/decrypt`、`blufi_crc_checksum` 未判 `blufi_sec==NULL`

- **严重等级：P2**。判级理由：这三个回调由 blufi 协议栈在加密通道建立后调用，正常流程 `blufi_sec` 已就绪；但若 `blufi_security_deinit()` 与协议栈回调存在时序竞态（断开瞬间仍有在途帧），会解引用 NULL 崩溃。偶发/边缘，判 P2。
- **文件：`main/blufi/blufi_security.c:155-193`**
- **问题代码片段：**
```c
int blufi_aes_encrypt(uint8_t iv8, uint8_t *crypt_data, int crypt_len) {
    ...
    memcpy(iv0, blufi_sec->iv, sizeof(blufi_sec->iv));  // blufi_sec 可能已被 deinit 置 NULL
```
- **根因**：`blufi_security_deinit()`（blufi.cc:500 BLE_DISCONNECT 调用）会 `free(blufi_sec); blufi_sec=NULL`，但加解密回调无空指针保护。
- **触发条件/影响面**：BLE 断开与加密帧处理并发的窄窗口。
- **修复建议**：三个函数入口加 `if (blufi_sec == NULL) return -1;`（checksum 返回 0）。
- [发现于第一遍]

### 06-P3-D —— `myrand` 忽略 `esp_fill_random` 无返回值假设 / 始终返回 0

- **严重等级：P3**。判级理由：`esp_fill_random` 无失败返回，`myrand` 恒返回 0 符合 mbedtls 约定，无功能问题；记录为远期技术债（若将来换 RNG 源需重审）。
- **文件：`main/blufi/blufi_security.c:60-64`**
- **根因**：example 代码遗留。
- **修复建议**：保留即可，加注释说明 `esp_fill_random` 不会失败。
- [发现于第一遍]

### 06-P3-E —— `ibeacon.cc` 单例回调 `on_detected_` 与 `Stop()` 无并发保护

- **严重等级：P3**。判级理由：`ScanCallback` 在 NimBLE host 任务上下文调用 `on_detected_`，`OnDetected()`/`Stop()` 可能在其他任务设置/清空；但 iBeacon 为量产 stub（默认 `CONFIG_BT_NIMBLE_ROLE_OBSERVER` 未启用，Start 直接 return false），实际不运行，故仅潜在风险。
- **文件：`main/blufi/ibeacon.cc:106-108`、`ibeacon.h:50-52`**
- **根因**：`scanning_`/`on_detected_` 为裸 `bool`/`std::function`，非 atomic、无锁。
- **触发条件/影响面**：仅当启用 observer 角色且多任务并发设置回调时。
- **修复建议**：启用 observer 前，将 `scanning_` 改 `std::atomic<bool>`，`on_detected_` 设置/读取加锁或在 host 任务内统一操作。
- [发现于第一遍]

---

## 第二遍 · 红线深挖（四条硬红线 + 跨文件数据流）

### 红线② 内存安全 —— BLE/配网外部输入入口全链路梳理

逐个追踪外部输入入口（blufi.cc `BlufiCallback`）：

| 入口事件 | 数据来源 | 长度处理 | 结论 |
|---|---|---|---|
| RECV_STA_SSID | `param->sta_ssid.ssid` + `ssid_len` | `std::string::assign(ptr,len)` | 用了带 len 的 assign，安全；但见 06-P2-F |
| RECV_STA_PASSWD | `param->sta_passwd.passwd` + `passwd_len` | `assign(ptr,len)` | 安全；见 06-P2-F |
| RECV_CUSTOM_DATA | `param->custom_data.data` + `data_len` | `assign(ptr,len)` | 安全；见 06-P1-G |
| DH 协商帧 | `data`+`len`（blufi_security.c） | **无校验** | 见 06-P0-A |
| iBeacon adv | `event->disc.data`+`length_data` | `len<30` 校验 | 见 06-P3-H |

### 06-P1-G —— RECV_CUSTOM_DATA 绑定码无长度/内容上限即落 NVS

- **严重等级：P1**。判级理由：`custom_data.data_len` 完全由对端控制，`binding_code_.assign` 本身用 len 安全，但随后无任何长度上限即 `settings.SetString("binding_code", ...)` 写入 NVS。攻击者发超大 custom_data（blufi 单帧+分片可达数百字节～KB）反复写 NVS，可撑爆 NVS 分区 / 触发写失败，且 `binding_code_` 无界增长。属外部输入未设上限，量产可被滥用，判 P1。
- **文件：`main/blufi/blufi.cc:616-625`**
- **问题代码片段：**
```c
case ESP_BLUFI_EVENT_RECV_CUSTOM_DATA:
    self.binding_code_.assign((char *)param->custom_data.data,
                              param->custom_data.data_len);  // data_len 无上限
    {
      Settings settings("blufi", true);
      settings.SetString("binding_code", self.binding_code_);  // 直接落 NVS
    }
```
- **根因**：把对端任意自定义数据当绑定码无条件持久化，缺长度/格式校验。
- **触发条件/影响面**：配网期任何 BLE 客户端发送畸形/超长 custom_data；可污染 NVS、撑爆 blufi 命名空间、影响 OTA 模块读取的绑定码。
- **修复建议**：在 assign 前加 `if (param->custom_data.data_len == 0 || param->custom_data.data_len > 64) { ESP_LOGW(...); break; }`（绑定码按业务定上限，如 ≤32/64）；可加字符白名单校验后再写 NVS。
- [发现于第二遍]

### 06-P2-F —— SSID/PASSWD 未做长度上限校验，依赖底层但建议防御性收口

- **严重等级：P2**。判级理由：`assign(ptr,len)` 不会溢出，但 `ssid_len`/`passwd_len` 理论可超过 WiFi 规范（SSID≤32、PSK≤64）。后续 `credential_validator_` 与 WiFi 驱动若直接使用超长值，可能在底层 `wifi_config_t`（定长数组）处溢出。本文件内安全，跨模块潜在，判 P2。
- **文件：`main/blufi/blufi.cc:600-609`**
- **根因**：未在入口对 SSID/PASSWD 做长度上限裁剪。
- **触发条件/影响面**：对端发送超长 SSID/PASSWD，风险落在下游 wifi_config 拷贝（不在本子系统）。
- **修复建议**：入口加 `if (param->sta_ssid.ssid_len > 32) break;`、`if (param->sta_passwd.passwd_len > 64) break;`，或 assign 后 `if (ssid_.size()>32) ssid_.resize(32)`，并由 validator 兜底。
- [发现于第二遍]

### 红线③ 并发 —— 回调与主任务共享数据

### 06-P1-H —— `ssid_`/`password_`/`binding_code_` 跨任务读写无锁，配网竞态可读到撕裂值

- **严重等级：P1**。判级理由：`RECV_STA_SSID`/`RECV_STA_PASSWD`（NimBLE host 任务）写 `self.ssid_`/`self.password_`（`std::string`），而 `REQ_CONNECT_TO_AP` 启动的 `blufi_wifi` 任务（Core 0，blufi.cc:522）读这两个 string 调 validator。`std::string` 非原子，跨任务并发读写会导致内存撕裂 / use-after-realloc，量产偶发崩溃且难复现，判 P1。
- **文件：`main/blufi/blufi.cc:525`（读）、`600-607`（写）**
- **问题代码片段：**
```c
// host 任务写：
self.ssid_.assign(...); self.password_.assign(...);
// blufi_wifi 任务读：
if (self.credential_validator_(self.ssid_, self.password_, error)) { ... }
```
- **根因**：协议正常时序是先收 SSID/PASSWD 再 REQ_CONNECT，但手机可乱序或在连接任务运行中再发 SSID 帧，触发并发读写同一 `std::string`。无 mutex / 无快照。
- **触发条件/影响面**：配网期帧乱序或重复发送；`std::string` 在写时若触发 realloc，读侧持有悬空指针 → 崩溃。
- **修复建议**：在 `REQ_CONNECT_TO_AP` 入口（host 任务内）把 `ssid_`/`password_` 拷贝为局部快照传给子任务（通过 heap 结构体 + arg 传参），子任务不再直接读 `self.ssid_`；或对三个 string 加 `std::mutex`，读写均 `lock_guard`。
- [发现于第二遍]

### 06-P2-I —— `[修复]` 分支在非连接态调用 `blufi_security_init` 与扫描，可与正常 BLE_CONNECT 重复初始化

- **严重等级：P2**。判级理由：blufi.cc:395-418 的"BLE_CONNECT 丢失修复"分支会在数据事件里补 `blufi_security_init()`、置 `ble_connected_=true`、触发扫描；若紧接着真正的 BLE_CONNECT 事件到来（433-487），会再次 `blufi_security_init`（幂等，OK）但重复 `esp_blufi_adv_stop`、重复注册扫描回调、可能重复触发扫描。逻辑冗余，偶发状态错乱，判 P2。
- **文件：`main/blufi/blufi.cc:395-418`**
- **根因**：用数据事件兜底丢失的 connect 事件，缺与真实 connect 路径的互斥/去重。
- **触发条件/影响面**：BLE_CONNECT 事件丢失后又补发的边缘场景。
- **修复建议**：在修复分支设一标志，真正 BLE_CONNECT 到来时若该标志已置则跳过重复的 adv_stop/扫描注册；或将 security_init/扫描触发抽成幂等的 `EnsureConnectedState()`。
- [发现于第二遍]

### 06-P2-J —— `init_done_sem_` 等待超时后不返回失败，主机栈半初始化继续广播

- **严重等级：P2**。判级理由：`Start()` 在等 `INIT_FINISH` 超时 3s 时只 `ESP_LOGW` 然后 `return true`（blufi.cc:177-182），调用方误以为成功。若 NimBLE 实际未就绪，后续广播/连接行为未定义。非必崩，判 P2。
- **文件：`main/blufi/blufi.cc:177-182`**
- **根因**：超时未当错误处理。
- **修复建议**：超时后视配网重要性决定 `return false` 并清理，或至少标记 `initialized_` 状态待 INIT_FINISH 真正置位；调用方据返回值决定是否重试/重启。
- [发现于第二遍]

### 红线④ OTA / ① 电源域

- 本子系统不含 OTA 验签、Secure Boot、回滚逻辑（绑定码仅供 OTA 模块读取，验签责任在 OTA 子系统），本轮无 OTA 缺陷。
- 本子系统不直接操作电源域（ADC/充电/休眠）。`ReleaseStaticMem()`（blufi.cc:42-61）释放 BT 静态 RAM 属内存管理，已用类静态 flag 幂等、释放后再 init 走 `Application::Reboot`，处理稳妥，无电源红线问题。

---

## 第三遍 · 反审自检（复验 + 对抗视角反查漏报）

### 复验结论（行号/代码/判级核对）

- 06-P0-A：复核 blufi_security.c:71 `uint8_t type = data[0]`、:81 `(data[1]<<8)|data[2]`、:100 `memcpy(blufi_sec->dh_param, &data[1], blufi_sec->dh_param_len)` 真实存在，`len` 形参全程未使用。判级 P0 成立（内存安全硬红线 + 外部可控）。**保留**。
- 06-P1-G：复核 blufi.cc:617-624，`data_len` 无上限即落 NVS 真实。**保留 P1**（NVS 滥用比单纯崩溃轻于 P0，但比偶发重，量产可被反复触发）。
- 06-P1-H：复核 host 任务写 / blufi_wifi 任务（:522 xTaskCreatePinnedToCore，:525 读 ssid_/password_）真实。`std::string` 非原子。判级 P1 成立。**保留**。
- 06-P2-C：复核加解密回调无 NULL 判，deinit 置 NULL 真实（:228-231）。竞态窗口窄，**维持 P2**（不拔高 P1：blufi 栈一般在 deinit 前已停止派发帧）。
- 06-P2-F：SSID/PASSWD 本文件 assign 安全，溢出风险在下游模块，**维持 P2 不拔高**（避免越界到 wifi 子系统重复计费）。
- 06-P3-E/H：iBeacon 为量产 stub（默认 observer 未启用），**维持 P3**，不拔高。

### 对抗视角反查（"如何用畸形 BLE 包让它崩 / 绕过 security"）

1. **绕过加密**：blufi_init.c:219 `ble_hs_cfg.sm_io_cap=4`、:229 `sm_sc=0`，且 blufi 协议允许明文配网（手机端可选是否加密，见 docs/blufi_zh.md:26）。攻击者可全程明文发送 SSID/PASSWD/custom_data 绕过 DH 协商 → 见新增 06-P2-K。
2. **空帧/超短帧打 security handler**：已被 06-P0-A 覆盖（len<3 越界）。
3. **PARAM_LEN 声明大值 + 短 DATA 帧**：已被 06-P0-A 覆盖（memcpy 越界）。
4. **custom_data 洪泛写 NVS**：已被 06-P1-G 覆盖。
5. **乱序帧触发 string 竞态**：已被 06-P1-H 覆盖。

### 06-P2-K —— Blufi 默认允许明文配网，凭据/绑定码可被旁听者嗅探

- **严重等级：P2**。判级理由：`sm_sc=0` 且 blufi 协议本身允许不加密通道；若手机端不主动协商加密，SSID/PASSWD/绑定码以明文经 BLE 传输，可被附近设备嗅探。属信息泄露而非崩溃/砖机，量产隐私风险，判 P2（不判 P0：非必崩、需物理邻近且依赖客户端选择）。
- **文件：`main/blufi/blufi_init.c:219, 226-234`；`main/blufi/blufi.cc:600-624`**
- **根因**：未强制要求加密协商完成后才接收凭据。
- **触发条件/影响面**：用户用不加密的 blufi 客户端配网；WiFi 密码 / 绑定码明文泄露。
- **修复建议**：业务上要求客户端强制走加密协商；固件侧可在 `REQ_CONNECT_TO_AP` 前校验"本次会话是否已完成 DH 协商"（如检查 `blufi_sec->share_len>0`），未加密则拒绝并报错。需产品侧拍板（影响兼容性）。
- [发现于第三遍]

### 06-P2-L —— `blufi_security_init` 在 BLE_CONNECT 与 `[修复]` 分支多处调用，deinit 仅在 DISCONNECT，异常断开可能泄漏

- **严重等级：P2**。判级理由：`blufi_security_init` 幂等（:198 已初始化直接返回），但 `Stop()`（blufi.cc:185-231）路径未调用 `blufi_security_deinit()`，仅 `host_deinit` 间接停栈；若通过 `Stop()` 而非 BLE_DISCONNECT 退出配网，`blufi_sec` 堆块（含 dhm/aes 上下文）泄漏。偶发，判 P2。
- **文件：`main/blufi/blufi.cc:185-231`（Stop 无 security_deinit）vs `:500`（仅 DISCONNECT 调）**
- **根因**：security 生命周期绑定在 BLE_DISCONNECT 事件，未覆盖主动 Stop 路径。
- **触发条件/影响面**：业务主动 `Stop()`（如配网成功后停 BLE）且此前未收到 DISCONNECT 事件 → `blufi_sec` 内存泄漏（约 sizeof(blufi_security)+dhm/aes 内部分配）。
- **修复建议**：在 `Stop()` 的清理段（如 blufi.cc:213 控制器清理后）加 `blufi_security_deinit();`（幂等，安全）。
- [发现于第三遍]

### 误报排除

- blufi.cc:346-352 `OnScanDone` 的 `memcpy(ap_list[i].ssid, records[i].ssid, sizeof(records[i].ssid))`：源与目标均为定长 `ssid` 数组、`count` 经 `std::min(WIFI_LIST_NUM, size)` 收口，无溢出，**非缺陷**。
- ibeacon.cc:121-138 `ParseIBeacon`：已有 `len<30` 前置校验，`UuidToString` 读 16 字节、major/minor/tx_power 共 21 字节，30≥9(prefix)+21，边界足够，**非缺陷**。
- retry_timer_ 用 `std::atomic<esp_timer_handle_t>` + `exchange` 防并发双 delete（:216, :306, :315, :496），处理正确，**非缺陷**。

---

## 统计汇总

| 等级 | 数量 | 编号 |
|---|---|---|
| P0 | 1 | 06-P0-A |
| P1 | 2 | 06-P1-G, 06-P1-H |
| P2 | 7 | 06-P2-B, 06-P2-C, 06-P2-F, 06-P2-I, 06-P2-J, 06-P2-K, 06-P2-L |
| P3 | 2 | 06-P3-D, 06-P3-E |
| **合计** | **12** | — |

> 说明：原第一遍曾列出 06-P3-H（iBeacon adv 解析），第三遍反审复核为已含 `len<30` 前置校验、边界充足，归为误报排除，不计入。

**三遍各自新增：**
- 第一遍（广度）新增 5：06-P0-A、06-P2-B、06-P2-C、06-P3-D、06-P3-E
- 第二遍（红线）新增 5：06-P1-G、06-P2-F、06-P1-H、06-P2-I、06-P2-J
- 第三遍（反审）新增 2：06-P2-K、06-P2-L

**三遍新增合计：5 + 5 + 2 = 12（= 合计）。**

**最严重 P0 一句话**：`blufi_security.c` 的 DH 协商帧处理器对外部 BLE 报文不校验长度，畸形/短包即可越界读 `data[0..2]` 或借 `dh_param_len` 触发 `memcpy` 堆越界，配网期任意客户端可触发，必崩且可被利用——一组入口长度校验即可修。
