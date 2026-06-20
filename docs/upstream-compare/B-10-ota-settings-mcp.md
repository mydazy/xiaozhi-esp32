# B-10 OTA/settings/mcp_server — 对比官网 v2.2.4 识别过度优化
> 基线 v2.2.4；标尺=量产稳定；🟢必要/🔴过度/⚪扩展/🔒安全/🛡️红线保留；只分析不改码。

## 取证范围
- 改动官方（git diff v2.2.4 HEAD）：`main/ota.cc`（764 行，官方 492，+308/-? ）、`main/ota.h`（+9）、`main/settings.cc`（+12/-0）、`main/settings.h`（+2）、`main/mcp_server.cc`（372 行变更，半重写 +187/-185）、`main/mcp_server.h`（+2）
- 自研新增（v2.2.4 无）：`main/remote_cmd.cc`（489 行）、`main/remote_cmd.h`（83 行）
- 入口取证：`application.cc:865-866`（custom 消息 → remote_cmd_->Handle）、`application.cc:518`（CheckAssetsVersion，官方原生，不在本模块）
- 官方对照：`git show v2.2.4:main/ota.cc|settings.cc|mcp_server.cc`（settings GetFloat/SetFloat 官方 grep 空；ota RequestSwitch/ReportStatus/Download 官方 grep 空；mcp self.upgrade_firmware v2.2.4:152 存在）

---

## 🟢 必要（服务量产稳定）

| 项 | 我们怎么改 | 官方 v2.2.4 原实现 | 为何必要 | 证据 file:line |
|---|---|---|---|---|
| MCP DoToolCall 迭代器→裸指针 | Schedule 异步回调捕获 `tool`（McpTool*）而非 `tool_iter`（迭代器） | `app.Schedule([this,id,tool_iter,...]){ (*tool_iter)->Call() }` 捕获迭代器 | 异步执行时若 tools_ 被 AddTool 改动，捕获的迭代器失效 → use-after-free 崩溃 | 我们 mcp_server.cc:478-506；官方 v2.2.4:512-557 |
| OTA 下载 WDT 守卫 | 下载前 `esp_task_wdt_delete` + RAII WdtGuard 恢复 | 官方无显式 WDT 处理 | 弱网大固件下载（4G AT 通道）耗时长，不喂狗会触发看门狗复位中断刷机 | ota.cc:397-402 |
| OTA 下载断点重试 | content_length 校验 + 5 次指数退避（3/6/12/24/48s） | 官方读断即失败 | 4G 弱网现场下载频繁中断，无重试 = OTA 几乎刷不成 | ota.cc:408-424、589 |
| 删除 self.upgrade_firmware MCP 工具 | 移除官方"服务器下发 url 直接刷固件+重启"工具 | `AddUserOnlyTool("self.upgrade_firmware", ...UpgradeFirmware(url))` | 收紧攻击面：少一个可被 url 注入刷任意固件的入口（详见 🔒/🛡️ 节） | 我们删除；官方 v2.2.4:151-164 |
| Ota::Download 外部资产边界 | PSRAM 分配 max_size 上限 + 流式读 `total_read<max_size` 防溢出 + Content-Length 完整性校验 | 官方无此函数 | 教育卡 GIF 下载（外部资产）先限长再用，调用方再加魔数校验，防截断/HTML 错误页崩 LVGL | ota.cc:545-610；调用方 education_mcp_tools.cc:152-167（512KB 上限 + GIF89a/87a 魔数 + 尾字节 0x3B + 最小长度） |

---

## 🔴 过度（不服务稳定 / 死代码 / 治标）

| 项 | 我们怎么改 | 官方 v2.2.4 原实现 | 为何判过度 | 维护成本/风险 | 证据 file:line |
|---|---|---|---|---|---|
| **Ota::RequestSwitch 死代码** | 定义完整的 /switch 切换通道接口（NFC/iBeacon 用） | 官方无 | 全仓库无任何调用方（board 层从未调用），纯已写未启用 | 死代码：误导后人以为有 NFC 切换能力；占维护注意力；编译进固件但永不执行 | ota.cc:328-331（定义）+ ota.h:19（声明）；grep RequestSwitch 仅这 2 处 |
| **OnSleep 降级死分支** | EnterDeepSleep 后紧跟 `esp_restart()`，注释"未实现，降级" | 官方无 remote sleep | EnterDeepSleep 若真进休眠则永不返回，后面 esp_restart 是死代码；若没进则注释自承认"未实现" = 该功能实为半成品 | 远程 sleep 命令名义存在实为重启；用户期望休眠却重启，行为误导 | remote_cmd.cc:343-346 |
| OnDownload 空实现 | download 命令解析 files/emoji 后只打 Log "not yet implemented in V2" | 官方无 | 命令登记了但核心逻辑（文件同步）未实现，只剩弹 emoji | 死功能挂在命令表，远程下发 download 无实际效果 | remote_cmd.cc:252-268（"File sync not yet implemented in V2"） |
| remote_cmd.h 文档与代码不符 | 头文件命令表写 `update/ota`、`reload`、`audio_debug`，实际 Handle 分支是 `ota`、无 reload/audio_debug | — | 注释陈旧，列了不存在的命令、漏了 edu_pool/mic_calibrate | 协议文档误导对接方（云端按陈旧表下发 reload 会被当未知命令丢弃） | 文档 remote_cmd.h:23/32/31 vs 实现 remote_cmd.cc:86-104 |

---

## ⚪ 扩展（官方没有的纯业务功能，只登记）

| 项 | 能力 | 证据 file:line |
|---|---|---|
| remote_cmd 远程遥控通道 | 复用对话通道（type=custom）下发 ~19 命令做远程运维/调试 | remote_cmd.cc:62-112 |
| ota /status 上报 | 设备 idle 时 POST 状态+板信息到 mydazy 云（仅 idle/非音乐时上报） | ota.cc:714-763 |
| settings GetFloat/SetFloat | float ×1000 存 int32（NVS 无原生 float）；**实际用于音频增益持久化**（input_gain/aec_gain），非 Vref（订正任务假设） | settings.cc:91-99；调用方 box_audio_codec.cc:23、audio_codec.cc:38/65/74 |
| mcp self.get_mac_address | 返回设备 MAC | mcp_server.cc:51 |
| mcp self.audio.set_aec / set_stt_popup / get_stt_popup | AEC 开关 / STT 弹窗开关 | mcp_server.cc:67/79/91 |
| mcp self.power.set_sleep_mode / get_sleep_mode | 休眠模式设置 | mcp_server.cc:112/124 |
| mcp self.music.play / stop | 云端控制 MP3 播放（带 url http(s) 前缀校验） | mcp_server.cc:142/194 |
| remote_cmd flow/music_*/edu_pool/update_prompt/wakeword/mic_calibrate/ttai | 直播伴侣脚本、音乐控制、摇一摇场景池、远程改 prompt/唤醒词、麦克风校准、文本转 AI | remote_cmd.cc:97-104 |

---

## 🔒 安全项（攻击者能做什么）

| 项 | 风险（攻击者能做什么） | 信任边界缺口 | 证据 file:line | 风险级 |
|---|---|---|---|---|
| **remote_cmd 零鉴权远程遥控** | 劫持/伪造对话通道服务器后，无需任何 token/签名/设备身份即可下发 `reboot`（远程重启砖用户）、`ota`（触发固件检查更新）、`update_prompt`（篡改设备人格/越权 prompt 注入）、`wakeword`（改唤醒词使设备"装聋"）、`sleep`（强制休眠=远程关机）、`tts/ttai`（让设备说任意话/向 AI 注入任意指令）、`music_play`（播放任意 url 音频）、`gain/volume`（爆音或静音） | 信任边界 = "已建立的对话连接"。命令分发 `application.cc:865` 不校验来源身份，凡能往该连接写 JSON 即可执行全部命令 | remote_cmd.cc:62（Handle 入口无鉴权）、86-104（命令表）；入口 application.cc:865-866 | **P1**（劫持需先攻破/MITM 对话连接；一旦得手可远程砖机/越权改 prompt） |
| update_prompt 无确认/无白名单 | 远程把设备 system prompt 替换为任意内容（含越狱/诱导/不当内容），prompt 为空还能清空恢复默认；无内容审查、无长度上限校验（仅按 model_type 槽写入） | 危险写操作无二次确认、无内容白名单 | remote_cmd.cc:436-449（UpdateSystemPrompt 直写） | P1 |
| wakeword 远程改写无确认 | 远程改唤醒词模式/文本写 NVS，重启生效；可把唤醒词改成用户永远不会喊的词 → 设备"装聋"无法语音唤醒（远程 DoS 语音功能） | 改持久化配置无确认/无回滚 | remote_cmd.cc:470-489（写 Settings("wakeword")） | P2 |
| ota 命令无版本/来源约束 | 远程触发 OTA 检查流程（CheckVersion）；OTA 源 url 取自本机 NVS ota_url，攻击者改不了源（除非同时控制了 ota_url 配置），但可反复触发打断用户 | 触发无限流 | remote_cmd.cc:124-143 | P3 |

**注**：所有危险命令（reboot/ota/sleep/update_prompt/wakeword/download/edu_pool）**均无二次确认、无频率限流**（remote_cmd.cc:86-104 直接分发到对应 On* 处理函数）。命令分发本身的健壮性 OK（见深审），风险全部集中在「零鉴权信任边界」这一处。

---

## 🛡️ 红线保留（OTA 安全 / 触碰红线，只标不动）

| 项 | 说明 | 证据 |
|---|---|---|
| OTA 验签/回滚链路未削弱 | Upgrade 走官方原生 esp_ota_begin/write/end + set_boot_partition；MarkCurrentVersionValid 用 esp_ota_mark_app_valid_cancel_rollback 保留回滚保护；我们的改动只加了 WDT 守卫+重试，未绕过验签 | ota.cc:357-541（esp_ota_* 原生）、337-355（回滚标记） |
| Upgrade image header memcpy 非外部越界 | 478 行 memcpy 拷贝固定大小 esp_app_desc_t，源为已读满的 image_header（≥固定头长才解析），目标固定 struct，非外部可控越界 | ota.cc:478 |
| 删 self.upgrade_firmware = 收紧 OTA 攻击面 | 官方该 MCP 工具（AddUserOnlyTool，本地用户级）允许下发任意 url 刷固件+重启；删除属正确收紧。**保留登记**：未来若需远程 OTA，应带验签+来源白名单重新设计，勿裸恢复官方版 | 官方 v2.2.4:151-164；我们已删 |
| settings GetFloat/SetFloat（精度核实） | ×1000 存 int32，float 范围 ±2.1M 内无溢出；用于增益（个位数 dB），精度 0.001 足够；**非 Vref 校准**（订正：Vref 校准不经此 API） | settings.cc:91-99 |

---

## 深审发现（逐点，file:line + 风险级）

1. **remote_cmd 命令分发健壮性 OK，无裸索引/裸 memcpy**（P3 仅文档债）。Handle 用 strcmp 链分发（remote_cmd.cc:86-104），每个 On* 都先做 cJSON 类型校验：OnVolume 校验 `cJSON_IsNumber`（:202）、OnTts/OnTtai 校验 string 非空（:166/184）、OnMusicPlay 校验 url 非空（:355）、OnFlow status 分支对 GetState 越界做了 `state_idx<0||>=4` 防护（:315）。未发现裸索引越界或裸 memcpy。**唯一隐患**：顶层命令格式 `msg = const_cast<cJSON*>(payload)`（:77）后 `need_delete=false`，生命周期正确（不重复 delete）。

2. **OnDownload 内存正确**：cJSON_Duplicate 后在 Schedule lambda 末尾 cJSON_Delete（:258/267），无泄漏；但功能空实现（🔴 已记）。

3. **ScheduleDelayedAction 单 timer 复用无并发问题**：reboot/ota/sleep 互斥下发，start_once 前先 stop（:48），pending_action_ 用 std::move（:49）；回调切回主线程 Schedule 执行（:58）。注释自述"互斥下发不会并发"——成立（同一连接串行收命令）。P3：若云端并发下两条延迟命令，后者覆盖前者 pending_action_（概率极低，非崩溃）。

4. **PostToOta 是设备→云出站，非入站命令**（P3）：url 取本机 NVS ota_url（:287）或 CONFIG_OTA_URL，带 Device-Id/Client-Id header（:301-302）。即便 RequestSwitch 被调用也只上报自己 MAC，无被劫持执行危险操作的风险——故死代码 RequestSwitch 的安全风险≈0，问题仅是维护成本。

5. **Ota::Download 流式读边界严谨**（无 P）：`while(total_read<max_size)`（:582）防缓冲溢出；Content-Length 不足判 incomplete 重试（:590）；realloc 缩容（:600）。调用方 education_mcp_tools.cc:163-167 再叠加 GIF 魔数+尾字节+最小长度校验，外部资产"先校验长度边界再用"达标。

6. **ReportStatus 仅 idle 上报，payload 拼接安全**（无 P）：state!=idle 或音乐播放时跳过（:716-722），避免热路径争抢；payload 用 GetDeviceStatusJson/GetBoardJson 拼接，空则补 "{}"（:741-742），无注入风险（自产 JSON）。

7. **MCP 框架核心未被破坏**（无 P）：AddTool 两个重载（:296/309）、tools_mutex_ 加锁 insert original_tools（:245-247）均保留；半重写（-185）主要是工具集替换（删 camera.take_photo / screen.snapshot / screen.preview_image / upgrade_firmware，加 16 个业务工具）+ DoToolCall 迭代器修复，非框架能力删除。

8. **mcp 工具入参校验达标**（无 P）：set_brightness 用 Property min/max(0,100) 框架级 clamp（:213）；music.play 校验 url 非空+http(s) 前缀（:155-160）；set_theme 查表失败返回 false（:234-238）。无裸取值越界。

---

## 小结
🟢 5　🔴 4　⚪ 9　🔒 4　🛡️ 4

**一句话结论**：本模块技术实现普遍扎实（OTA 重试/WDT 守卫、Download 边界、MCP 迭代器修复均必要，且删 upgrade_firmware 主动收紧了攻击面），真问题是 **remote_cmd 这条 ~19 命令的远程遥控通道零鉴权**——信任边界仅"已建连接"，劫持后可远程 reboot/改 prompt/改唤醒词，是本模块最高优先安全项（P1）；另有 RequestSwitch/OnSleep 降级/OnDownload 三处死代码/半成品需清理。
