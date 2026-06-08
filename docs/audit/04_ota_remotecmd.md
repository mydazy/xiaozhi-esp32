# 04 OTA & 远程命令子系统审计报告（红线子系统）

> 审计范围（只读，未越界）：`main/ota.h`、`main/ota.cc`、`main/remote_cmd.h`、`main/remote_cmd.cc`
> 文件数：**4**
> 审计日期：2026-05-20
> 判级原则：OTA 安全为四条硬红线之一，从严判级。
> 旁证（非审计范围内，仅用于定级）：`sdkconfig` 第 425/495/3207 行确认 `CONFIG_SECURE_BOOT is not set`、`CONFIG_APP_ANTI_ROLLBACK is not set`；`main/application.cc:856-862` 确认远程命令分发处无任何鉴权。

---

## 第一遍 · 广度遍历（显性缺陷）

逐文件精读 4 个文件，抓空指针/越界/泄漏/未检返回/超时缺失/异常崩溃等显性缺陷。

### 04-P1-A　`ParseVersion` 用 `std::stoi` 解析服务器版本号，非数字段抛异常崩溃
- 等级理由：版本字符串完全来自服务器 `/check` 响应（`firmware.version`），攻击者或服务端配置失误下发 `"1.0.beta"` / `"v2"` 即触发 `std::invalid_argument`，函数无 try/catch，异常上抛到 `CheckVersion` → 整机 abort 重启。每次开机都查版本 → 高频崩溃，判 **P1**。
- 文件：`main/ota.cc:607-617`
```cpp
while (std::getline(ss, segment, '.')) {
    versionNumbers.push_back(std::stoi(segment));   // 非数字 → 抛异常
}
```
- 根因：未捕获 `std::stoi` 的 `invalid_argument` / `out_of_range`；外部输入未先做数字校验。
- 触发条件/影响面：服务端版本字段含非数字字符即崩溃；恶意服务器可远程让全网设备开机即崩。
- 修复建议：在 607 行循环体内改用安全解析——`char* end=nullptr; long v=strtol(segment.c_str(), &end, 10); if(end==segment.c_str()) v=0; versionNumbers.push_back((int)v);`，或整个 while 包 `try{...}catch(...){return {};}` 并让 `IsNewVersionAvailable` 在空 vector 时返回 false。
- [发现于第一遍]

### 04-P2-A　`Activate()` 对可能为空的 URL 调用 `url.back()`（空串 UB）
- 等级理由：`GetCheckVersionUrl()` 在 NVS 与 `CONFIG_OTA_URL` 都为空时返回空串；空 `std::string::back()` 是未定义行为，可能崩溃。激活仅首次/重置后走，触发面窄，判 **P2**。
- 文件：`main/ota.cc:677-682`
```cpp
std::string url = GetCheckVersionUrl();
if (url.back() != '/') {   // url 可能为空 → UB
    url += "/activate";
}
```
- 根因：未对 `url.empty()` 先行判空（对比 `CheckVersion` 用 `url.length()<10` 守了，`Activate` 没守）。
- 触发条件/影响面：OTA URL 未配置时激活崩溃。
- 修复建议：677 行后加 `if (url.empty()) { ESP_LOGE(TAG,"OTA url empty"); return ESP_ERR_INVALID_ARG; }`。
- [发现于第一遍]

### 04-P2-B　`PostToOta` 在 URL 过短分支泄漏 `payload`
- 等级理由：`url.length() < 10` 分支直接 `return` 未 `cJSON_Delete(payload)`，每次 `/switch`、`/status` 类调用泄漏一个 cJSON 树。低频（仅 URL 配置异常时），判 **P2**。
- 文件：`main/ota.cc:281`
```cpp
if (url.length() < 10) return ESP_ERR_INVALID_ARG;   // 漏 cJSON_Delete(payload)
```
- 根因：错误返回路径未释放调用方约定"调用后自动释放"的 payload。
- 触发条件/影响面：OTA URL 配置异常时，每次 NFC/iBeacon 切换或状态上报泄漏内存，长期堆碎片。
- 修复建议：281 行改为 `if (url.length() < 10) { cJSON_Delete(payload); return ESP_ERR_INVALID_ARG; }`。
- [发现于第一遍]

### 04-P2-C　`PostToOta` Open 失败分支未读 body 也未 Close（句柄/连接资源依赖析构）
- 等级理由：`http->Open` 失败时直接 `return ESP_FAIL`（304 行），`http` 为 unique_ptr 析构会回收，但未显式 `Close()`；与下方成功路径不一致。资源最终释放，判 **P3**（见第三遍复核，非泄漏，降级）。
- 文件：`main/ota.cc:303-306`
- [发现于第一遍，第三遍复核降为 P3]

### 04-P3-A　`OnDownload` 远程命令为空实现但仍 Alert"下载中"
- 等级理由：`OnDownload` 仅打印 "not yet implemented"，但向用户弹"同步文件/下载中..."误导提示；功能未做不影响安全，判 **P3**。
- 文件：`main/remote_cmd.cc:253-270`
- 根因：占位实现未与 UI 文案对齐。
- 修复建议：实现前移除 263 行的"下载中..."Alert 或改为"暂不支持"。
- [发现于第一遍]

### 04-P3-B　`Ota::Upgrade` 进度回调 `progress` 用 `%u` 打印 `size_t`，平台格式串不匹配
- 等级理由：`ESP_LOGI(... "%u%% (%u/%u)" ... progress, total_read, content_length ...)`，`size_t` 在 ESP32 为 32 位与 `%u` 恰好匹配，仅日志，判 **P3**。
- 文件：`main/ota.cc:463`
- [发现于第一遍]

---

## 第二遍 · 红线深挖（OTA 安全链路逐行核）

核查：固件验签 / 回滚保护 / Secure Boot / 分区写入边界 / 远程命令鉴权与越权 / 下载缓冲边界 / 外部输入校验。

### 04-P0-A　固件 OTA 全程无签名验证，且 Secure Boot 关闭 → 可刷入任意恶意固件
- 等级理由：`Ota::Upgrade` 全流程仅靠 `esp_ota_end()`（只做 SHA256 完整性 / 镜像格式校验，**不是密码学验签**）；旁证 `sdkconfig:495 CONFIG_SECURE_BOOT is not set`、`sdkconfig:494 CONFIG_SECURE_SIGNED_APPS_NO_SECURE_BOOT is not set` 确认未启用签名应用。固件 URL 来自服务器/远程命令明文 HTTP，攻击者只要 MITM 下载链路或攻陷下发服务器即可烧入任意固件 → 永久控制设备/砖机。OTA 红线头号缺口，判 **P0**。
- 文件：`main/ota.cc:515-529`（`esp_ota_end` + `esp_ota_set_boot_partition`，二者之间无验签）；URL 来源 `main/ota.cc:232-235`、`main/application.cc:1434`
```cpp
esp_err_t err = esp_ota_end(update_handle);   // 仅完整性校验，非验签
...
err = esp_ota_set_boot_partition(update_partition);  // 直接设为启动分区
```
- 根因：未启用 Secure Boot V2 + Signed App，固件信任链断裂；HTTP 下载未强制 TLS/未校验来源。
- 触发条件/影响面：弱网/公共 WiFi/4G AT 通道 MITM，或服务器被攻陷；影响全部出货设备，可批量刷恶意固件。
- 修复建议：①出货前打开 `CONFIG_SECURE_BOOT=y`、`CONFIG_SECURE_BOOT_V2_RSA` 并烧录公钥摘要 efuse；②`CONFIG_SECURE_SIGNED_ON_UPDATE_NO_SECURE_BOOT`（过渡期至少软件验签）；③`ota.cc:366` 强制 `firmware_url` 必须为 `https://` 且校验域名白名单，拒绝 `http://`。这是出货门禁必须清零项。
- [发现于第二遍]

### 04-P0-B　远程命令通道零鉴权，任意 `custom` 消息可远程触发重启/解绑休眠/OTA/任意 URL 拉流
- 等级理由：`application.cc:856-859` 收到 `type=="custom"` 即把 payload 交给 `RemoteCmd::Handle`，`Handle` 无任何 token/签名/权限校验（`remote_cmd.cc:62-113`），直接分发 `reboot`/`sleep`(弹"解绑设备"进入深睡)/`ota`/`reload`/`flow start url`/`music_play url`/`update_prompt` 等高危操作。任何能向 WS 通道注入 JSON 的一方（被劫持的服务器、可伪造的明文 WS、同信道中间人）即可远程让设备重启循环、强制休眠失联、刷新协议指向恶意平台、或拉取任意 URL。安全漏洞，判 **P0**。
- 文件：`main/remote_cmd.cc:62-113`（分发无鉴权）；`main/application.cc:856-862`（入口无鉴权）
```cpp
bool RemoteCmd::Handle(const cJSON* payload) {
    ...
    if (strcmp(type, "reboot") == 0) OnReboot();
    else if (strcmp(type, "ota") == 0) OnOta();
    else if (strcmp(type, "sleep") == 0) OnSleep(msg);      // 解绑休眠
    ...
```
- 根因：远程命令完全信任传输层，未做命令级鉴权；危险命令（reboot/sleep/ota/reload）与普通命令同级无差别处理。
- 触发条件/影响面：传输层一旦被穿透（明文 WS / 服务器被攻陷 / 凭证泄漏），可远程批量砖机或失联。
- 修复建议：①传输层强制 TLS + 服务器证书校验（协议层落实）；②在 `Handle` 入口对 `reboot/sleep/ota/reload/flow/update_prompt` 等高危命令要求附带服务端签名字段（HMAC over payload + nonce + 设备 SN），用 efuse HMAC_KEY 校验后才执行，校验失败 `return false`；③`flow start url` / `music_play url` 强制 https + 域名白名单。
- [发现于第二遍]

### 04-P1-B　无防回滚（anti-rollback），可远程刷入旧版含已知漏洞固件
- 等级理由：旁证 `sdkconfig:3207 CONFIG_APP_ANTI_ROLLBACK is not set`，仅启用了基础 `CONFIG_APP_ROLLBACK_ENABLE`（启动失败自动回滚，非安全防回滚）。`Ota::Upgrade` 设 boot 分区前未校验 `secure_version`，攻击者可推送已撤销的旧固件绕过后续修复。结合 04-P0-A 是攻击放大器；独立看是高危但需先有刷入能力，判 **P1**。
- 文件：`main/ota.cc:525`（`esp_ota_set_boot_partition` 前无版本下限校验）
- 根因：未启用 anti-rollback efuse 计数与 `CONFIG_BOOTLOADER_APP_ANTI_ROLLBACK`。
- 触发条件/影响面：漏洞修复后攻击者降级到旧版重新利用。
- 修复建议：出货前置 `CONFIG_BOOTLOADER_APP_ANTI_ROLLBACK=y` + `CONFIG_BOOTLOADER_APP_SECURE_VERSION`，并在 `MarkCurrentVersionValid`（329 行）成功后 `esp_efuse_update_secure_version`。
- [发现于第二遍]

### 04-P1-C　`Ota::Download` 缓冲边界：body 超 `max_size` 时静默截断当成功
- 等级理由：循环 `while (total_read < max_size)` 读满即停（577 行），随后完整性校验只判 `total_read < expected`（584 行）。当 `expected == 0`（无 Content-Length）或 `expected > max_size` 但缓冲已读满，截断后被当成"成功"返回截断数据。下游（动态图片/资源解码）拿到不完整 buffer 可能越界解析。外部 URL 来自远程命令，判 **P1**。
- 文件：`main/ota.cc:575-599`
```cpp
while (total_read < max_size) {
    int n = http->Read(...max_size - total_read);
    if (n <= 0) break;
    total_read += n;
}
...
if (expected > 0 && total_read < expected) { ...continue; }  // 不覆盖 expected>max_size 截断
```
- 根因：未对 `expected > max_size`（超出预算）与 `total_read==max_size 但 body 未尽` 做拒绝。
- 触发条件/影响面：服务器返回超大或无 Content-Length 资源时，下游拿到截断数据 → 解码越界/花屏。
- 修复建议：584 行后增 `if (expected > 0 && expected > max_size) { ESP_LOGW(...); continue; }`；并在读满 max_size 后再 `http->Read` 探测是否仍有数据，有则判失败 `continue`。
- [发现于第二遍]

### 04-P2-D　续传 `Range` 重连仅校验状态码 206，未校验 `Content-Range` 起始偏移
- 等级理由：`ota.cc:441-447` 续传只认 `GetStatusCode()==206`，未核对响应 `Content-Range` 起点等于 `total_read`。若服务器/中间人返回从 0 开始的 206，会把错位数据 `esp_ota_write` 到 partition，导致镜像污染（最终 `esp_ota_end` 完整性校验大概率拦下，但浪费整轮下载且理论上构造可绕过）。判 **P2**。
- 文件：`main/ota.cc:438-448`
- 根因：续传未验证服务端实际返回的字节区间。
- 修复建议：读取响应 `Content-Range` 头解析起始字节，与 `total_read` 不符则 `continue` 重试。
- [发现于第二遍]

### 04-P2-E　`image_header` 解析早于任何验签，`memcpy` 拷贝攻击者可控头部
- 等级理由：`ota.cc:471-486` 在累计到 `sizeof(esp_image_header_t)+...+sizeof(esp_app_desc_t)` 后 `memcpy(&new_app_info, ...)`，长度边界有校验（`image_header.size() >=` 才拷），不越界；但 `new_app_info` 解析出来后**未被使用**（无版本/项目名校验），且整段在 04-P0-A 缺验签前提下毫无防护意义。本身不崩，判 **P2**（应配合 P0-A 修）。
- 文件：`main/ota.cc:473-475`
- 修复建议：拷出 `new_app_info` 后校验 `project_name`/`secure_version` 是否匹配本机型，不符则 `esp_ota_abort` 并 return。
- [发现于第二遍]

---

## 第三遍 · 反审自检（复验 + 对抗视角反查漏报）

逐条复验行号与代码片段真实性，校正判级，并以"如何刷入恶意固件 / 如何越权下发命令"对抗视角反查漏报。

### 复验结论
- 04-P0-A：行号 515/525 属实，`esp_ota_end` 语义复核无误（非验签）。sdkconfig 旁证真实。**维持 P0**。
- 04-P0-B：`Handle` 62-113 与 application.cc:856-859 调用链属实，无鉴权。**维持 P0**。
- 04-P1-A：`std::stoi` 在 607-613 属实，确无 try/catch（整文件无 catch）。**维持 P1**。
- 04-P1-C：577/584 行边界逻辑复核属实。**维持 P1**。
- 04-P2-C：复核 `http` 为 `unique_ptr`，析构会释放连接，非真泄漏，**由 P2 降为 P3**（已在第一遍标注）。
- 04-P3-B：`size_t` 在 ESP32(xtensa lp32) 为 32 位，`%u` 实际匹配，仅理论不规范，**维持 P3**。
- `esp_ota_begin` 返回值判断（`ota.cc:477 if(esp_ota_begin(...))`）：复核 `esp_ota_begin` 成功返回 `ESP_OK(0)`，`if(0)` 为假 → 跳过 abort 走正常路径；失败返回非 0 → 进 abort。**逻辑正确，非缺陷，不计入。**

### 对抗视角新增漏报

### 04-P1-D　`OnSleep` 降级路径必然 `esp_restart`，远程一条 sleep 命令即让设备重启而非休眠
- 等级理由：`remote_cmd.cc:352-358` 中 `EnterDeepSleep` 后紧跟无条件 `ESP_LOGW("未实现，降级 esp_restart"); esp_restart();`。若 `EnterDeepSleep` 当前为空实现（注释明示"未实现"），则任何 `sleep` 远程命令实际效果是**重启**。结合 04-P0-B 无鉴权，攻击者循环下发 `sleep` 即可让设备无限重启（拒绝服务）。高频体验崩溃，判 **P1**。
- 文件：`main/remote_cmd.cc:352-358`
```cpp
Board::GetInstance().EnterDeepSleep(enable_gyro_wakeup);
ESP_LOGW(TAG, "EnterDeepSleep 未实现，降级 esp_restart");
esp_restart();   // 无条件执行 → sleep 命令实为 reboot
```
- 根因：`EnterDeepSleep` 若返回（未真正休眠）则无条件重启，无"已成功休眠则不应到达此行"的保护。
- 触发条件/影响面：远程 sleep 命令（或 NFC/解绑流程）触发重启循环。
- 修复建议：确认 `EnterDeepSleep` 真正进入休眠不返回；若仍为占位，356-357 行改为 `ESP_LOGW(...); return;`（不降级重启），由产品决定是否真重启。
- [发现于第三遍]

### 04-P2-F　`Handle` 顶层命令格式把 `payload` 直接当 `msg`，与双层格式共用同一无鉴权入口
- 等级理由：`remote_cmd.cc:74-77` 支持顶层 `{"type":"music_play",...}` 直接命令，`msg = const_cast<cJSON*>(payload)`，进一步扩大无鉴权命令面（任意顶层 type 都能触发）。本身不崩，是 04-P0-B 的攻击面放大，独立判 **P2**。
- 文件：`main/remote_cmd.cc:74-78`
- 修复建议：鉴权校验（见 04-P0-B）须覆盖顶层格式与双层格式两条路径。
- [发现于第三遍]

### 04-P3-C　`OnFlow` / `OnMusicPlay` 远程 URL 无 https/白名单校验，任意外链可被注入拉取
- 等级理由：`flow start` 的 `url`（remote_cmd.cc:293-297）与 `music_play` 的 `url`（362-372）直接交给 FlowEngine/MusicPlayer 拉取，无 scheme/域名校验。结合无鉴权可被指向任意服务器（SSRF/恶意脚本/恶意音频流）。功能性风险，远期判 **P3**（拉流解析的内存安全由各自子系统兜底）。
- 文件：`main/remote_cmd.cc:293-297`、`362-372`
- 修复建议：拉取前统一校验 `https://` + 域名白名单，非法则 Alert 拒绝。
- [发现于第三遍]

### 误报排查（已删除，不计入）
- `Ota::Upgrade` 中 `update_handle` 在 `image_header_checked==false` 时 `esp_ota_abort` 的使用：复核所有 abort 调用都在 `image_header_checked==true`（即 begin 已成功）或 begin 失败分支内，无"未 begin 即 abort"的误用，**非缺陷**。
- `WdtGuard` RAII 恢复看门狗：复核覆盖所有 break/return 路径，**正确**。

---

## 统计汇总

| 等级 | 数量 | 编号 |
|------|------|------|
| P0 | 2 | 04-P0-A、04-P0-B |
| P1 | 4 | 04-P1-A、04-P1-B、04-P1-C、04-P1-D |
| P2 | 6 | 04-P2-A、04-P2-B、04-P2-C(已降P3)→不计、04-P2-D、04-P2-E、04-P2-F |
| P3 | 4 | 04-P3-A、04-P3-B、04-P3-C、04-P2-C(降级后) |

> 说明：04-P2-C 经第三遍复核由 P2 降为 P3，下列合计按最终判级计。

### 最终计数（按复核后判级）
- **P0：2**（04-P0-A、04-P0-B）
- **P1：4**（04-P1-A、04-P1-B、04-P1-C、04-P1-D）
- **P2：4**（04-P2-A、04-P2-B、04-P2-D、04-P2-E、04-P2-F — 注：实为 5 条，见下方订正）
- **P3：4**（04-P3-A、04-P3-B、04-P3-C、04-P2-C降级）

### 计数订正（权威）
逐条点名，避免归并误差：
- P0（2）：04-P0-A、04-P0-B
- P1（4）：04-P1-A、04-P1-B、04-P1-C、04-P1-D
- P2（5）：04-P2-A、04-P2-B、04-P2-D、04-P2-E、04-P2-F
- P3（4）：04-P3-A、04-P3-B、04-P3-C、04-P2-C（复核降级）

**合计：15 个问题。**

### 三遍新增
- 第一遍（广度）：新增 6 —— 04-P1-A、04-P2-A、04-P2-B、04-P2-C、04-P3-A、04-P3-B
- 第二遍（红线深挖）：新增 6 —— 04-P0-A、04-P0-B、04-P1-B、04-P1-C、04-P2-D、04-P2-E
- 第三遍（反审自检）：新增 3 —— 04-P1-D、04-P2-F、04-P3-C（另复核降级 04-P2-C，未计新增）

**新增分布：6 + 6 + 3 = 15。**

---

## 出货门禁结论
本子系统 **2 个 P0 必须出货前清零**：
1. 04-P0-A：开启 Secure Boot V2 + 固件签名验证，下载强制 https/白名单。
2. 04-P0-B：远程高危命令加服务端签名鉴权（efuse HMAC），传输层强制 TLS。

P1 中 04-P1-A（版本号解析崩溃）与 04-P1-D（sleep 实为重启可被远程 DoS）建议同批修复——一行级改动、收益高。
