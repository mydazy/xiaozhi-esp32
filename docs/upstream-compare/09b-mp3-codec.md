# 模块 09b · 自研驱动深审补充（mp3_player + codec_ctrl_i2c）

> 窗口1 补审：窗口2 的 [B-09-drivers.md](B-09-drivers.md) 已覆盖 sc7a20h + i2c_bus_worker，标注"mp3/codec 不在窗口2范围"。本文补完模块 9 剩余两个自研驱动。
> 模式：🔶深审（官方无）。审查维度：边界校验 / 并发加锁 / 内存安全 / 弱网·掉电 / 长跑泄漏。

---

## 一、codec_ctrl_i2c（`mydazy__codec_ctrl_i2c.c` 161 行）

派生自 esp_codec_dev 的 `audio_codec_ctrl_i2c`，把 codec I2C 访问转发到 i2c_bus_worker 串行化（1:1 接口兼容）。

| 审查项 | 结论 |
|--------|------|
| **必要性** | 🟢 ES8311/ES7210 经 worker 串行，与 touch/sensor 共线不冲突（B-09 已证串行化全覆盖）|
| 写边界 | ✅ `_write_reg` 有 `len > 4` 校验 + `buf[4]` 固定，与上游一致 |
| 资源 | ✅ calloc + _open 失败 free；_open 重入保护（dev!=NULL 直接返回）；_close remove+置 NULL |
| 并发 | ✅ 无额外锁，正确依赖 worker 串行 |
| ⚠️ 轻微观察 | `_read_reg` 的 `addr_len` 无上限校验（`addr_data[2]`，若调用方传 >2 越界读 1-2 字节）。**实际不触发**（codec 寄存器地址固定 1-2 字节），属信任调用方的防御缺口，可加一行 `if (addr_len>2) return INVALID_ARG`。非过度优化。|

**判定：🟢 必要转发层，无过度优化。**

---

## 二、mp3_player（`mp3_player.cc` 833 行）

三任务流水线（Download → Decode → Output）+ 双 ringbuffer（compressed/pcm），流式 MP3。质量**高**。

### 内存安全 ✅

| 点 | 实现 | 结论 |
|----|------|------|
| DecodeLoop memcpy | `want = kInBufSize - in_len`；ReceiveUpTo 保证 `got <= want`；`memcpy(in_buf+in_len, recv, got)` → `in_len+got <= kInBufSize` | ✅ 无越界 |
| heap 配对 | download buf、decode in_buf/out_buf 均 malloc↔free 配对，失败路径也 free | ✅ 无泄漏 |
| ringbuffer | FreeRTOS 原语自带边界 | ✅ |

### 并发 / UAF 防护 ✅（优于 i2c_worker）

- **AbortAndJoin**：`abort_` 置位 → 轮询 `active_tasks_` 最多 2s → **任务未退出则不删 ring**（注释"Don't free rings — tasks may still be writing"）→ 仅全部退出才删。
- 这是**正确的 UAF 防护**——对比 B-09 在 i2c_worker 发现的栈 UAF（P1），mp3 这里"宁可泄漏不 UAF"的保守 join 是正面范例。
- 全程 `std::atomic` + memory_order（running_/abort_/paused_/active_tasks_/download_done_/prebuffered_），符合 esp32-memory 规范。
- 起播兜底：DownloadLoop 退出无论成败都放行 `prebuffered_`，防 OutputLoop 永久阻塞。

### 4G 弱网 / 掉电路径 ✅（有针对性）

Range 断点续传 / 主动周期断流（2MB Close+Range，治 NAT 老化·OSS LB 切节点）/ Pause 关 socket Resume 续传 / 指数退避（500ms·1s·2s + 上限）/ Premature EOF 检测 / 速率日志（DL 速度·comp/pcm 占用，4G 弱网调试命脉）。

**每个机制对应真实 4G 流媒体问题，非盲目堆叠。**

| ⚠️ 唯一可评估点 | 主动周期断流（每 2MB 重连）略激进，好网络下增加重连开销。但治 4G NAT 老化（真实问题）。倾向 🟢 保留，可 A/B 实测好网络下是否多余。|

**判定：🟢 高质量自研驱动，无过度优化。UAF 防护是正面范例。**

---

## 三、判定汇总

- **🔴 过度优化：0 项。** codec_ctrl_i2c 是必要转发层，mp3_player 质量高（UAF 防护/memcpy 边界/泄漏/弱网处理全部稳健）。
- ⚠️ 2 个非过度观察：codec `_read_reg` addr_len 防御缺口（实际不触发）、mp3 主动 2MB 重连可 A/B 评估。
- 模块 9 完整结论 = B-09（sc7a20h+i2c_worker，含 P1 栈 UAF）+ 本文（mp3+codec，无 🔴）。

> 本阶段只分析不改代码。本补充无 🔴 追加。
