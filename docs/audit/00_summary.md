---
type: dashboard
created: 2026-05-20
updated: 2026-05-20
tags: [固件审计, P30, P31, 量产, 缺陷汇总]
---

# 固件全量审计汇总（mydazy-p30-v31）

> 审计方式：6 子系统 × 2 轮（首轮各 1 agent，重型子系统第二轮按文件分片加派 6 agent），共 12 个 agent 并行排查。
> 范围：main/ 全部源码 + components/ 自研驱动；不含 main/assets/ 生成的字库图像二进制。

## 一、严重等级分布

| 严重等级 | 数量 | 占比 |
|---|---|---|
| P0（必崩 / 烧硬件 / 砖机 / 安全漏洞） | 20 | 10.9% |
| P1（高频崩溃或体验严重退化） | 48 | 26.1% |
| P2（偶发或边缘） | 64 | 34.8% |
| P3（潜在远期风险） | 52 | 28.3% |
| **合计** | **184** | **100%** |

## 二、子系统分布

| 子系统 | 报告 | 问题数 |
|---|---|---|
| 音频（Codec/AFE/OGG/Wake） | 01_audio_subsystem.md + 01_audio_p2.md | 34 |
| 网络协议（WS/MQTT/OTA） | 02_network_protocols.md | 18 |
| WiFi / Blufi / 配网 | 03_wifi_blufi.md | 17 |
| 应用主流程 / 状态机 / MCP | 04_application_core.md | 17 |
| 板级驱动 / 电源 / I2C | 05_board_drivers.md + p2a/p2b/p2c | **52（最多）** |
| 显示 / LVGL / 资产 | 06_display_assets.md + p2a/p2b | 46 |
| **合计** | | **184** |

## 三、P0 清单（量产前必须清零，20 项）

### 电源域（烧硬件 / 砖机 / 误关机 — 最高优先）
1. **05-P0-1** 电池 ADC 读失败直接 `ESP_ERROR_CHECK` → abort 重启
2. **05-P0-2** P31 充电检测引脚极性与驱动逻辑相反
3. **05b-P0-A** 电池电压非校准回退路径漏乘 2（分压）→ eFuse 未烧的整批样机开机即误判低电强制关机（砖机级，一行可修）
4. **05a-P0-A1** 过放关机门限被 `kLowBatteryLevel=0` 与电压双重失效，过放保护形同虚设

### 内存安全 / 受控输入（OTA 资产、相机、网络帧可触发）
5. **06-P0-1** GIF 内存解码无源缓冲上界 → 损坏/截断 GIF 越界读
6. **06-P0-2** `CircularStrip::SetSingleColor` 索引越界写（堆破坏）
7. **06a-P0-1** image_to_jpeg 尺寸 `int` 溢出 + 无 src_len 校验 → 相机编码堆溢出
8. **05c-P0-2** NFC 响应用小栈缓冲收任意长度 → 栈溢出
9. **02-P0-1** WebSocket 二进制音频帧无长度校验 → 越界读 + 巨量分配
10. **04-P0-1** OnIncomingJson 对 type/state/command 未判 null 即解引用 → 畸形 JSON 远端必崩

### 并发无锁（高频日常触发）
11. **01-P0-1** codec_ctrl_i2c `_open` 永不置 `is_open` → codec 二次 open 后读写被拒 → 构造 assert 砖机
12. **01-P0-2** AfeWakeWord `wake_word_pcm_` 检测/编码双线程无锁 → 结构损坏
13. **01p2-P0-A1** BoxAudioCodec `Read/Write` 不持锁，与省电定时器 close 并发 → 读写半释放设备崩溃
14. **03-P0-1** SsidManager 全局单例无锁，跨任务读写 std::vector → UAF
15. **03-P0-2** Blufi 状态标志非 atomic，NimBLE/timer/业务跨核竞争

### 挂死 / Flash / 安全
16. **05c-P0-1** NFC 等待 TxIRQ 用无超时死循环 → 芯片卡死即整任务挂死
17. **03-P0-3** SaveToNvs 每次增删改重写全部键 → 频繁配网磨损 Flash
18. **02-P0-2** MQTT ParseServerHello 对 udp 子字段空指针解引用
19. **02-P0-3** OTA 固件无应用层验签 + Secure Boot 未开 → 可刷入任意固件
20. **02-P0-4** 远程命令通道无鉴权 → 云端/链路可远程重启/休眠/刷机/改 STT 回传

## 四、跨子系统主题（修复时合并处理更高效）

1. **电源域是量产头号风险**：4 个 P0 集中在电池 ADC / 充电 / 过放，其中 05b-P0-A（漏乘 ×2）会让未校准整批样机开机即关机——量产前第一件事。
2. **"OTA 半受控资产 → 内存安全"族**：06 与多个二轮 agent 都指向同一根因（GIF/JPEG/资产表无边界与溢出校验）。建议统一补一层资产解析边界/溢出防护，而非逐点打补丁。
3. **并发无锁是隐性高发区**：codec、SsidManager、Blufi、wake word 多处热路径无锁，偶发崩溃难复现，量产返修成本高。
4. **安全短板**：OTA 无验签 + 远程命令无鉴权，量产联网设备的合规与品牌风险。
