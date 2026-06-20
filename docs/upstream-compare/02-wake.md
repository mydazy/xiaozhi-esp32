# 模块 02 · wake 唤醒词链路

> 对比基线：`v2.2.4`。模式：A 对比。
> 范围：`main/audio/wake_words/*`、`main/audio/wake_word.h`、`audio_service.cc` 唤醒词管理层。

---

## 一、核心结论：已恢复对齐官方（正面案例）

本模块是"**恢复过度优化已执行**"的成果。commit `11a932fa9 对比官网版本恢复过度优化` 大幅回退了自研唤醒词改动：

| 文件 | 恢复 commit 变化 | 当前 vs v2.2.4 |
|------|----------------|----------------|
| `afe_wake_word.cc` | **−209 行** | **零差异** ✅ |
| `afe_wake_word.h` | −18→对齐 | 零差异 ✅ |
| `custom_wake_word.cc` | **−186 行** | 零差异 ✅ |
| `custom_wake_word.h` | 对齐 | 零差异 ✅ |
| `wake_word.h` | −2 | 零差异 ✅ |
| `srmodel_vfs.cc/.h` | **整文件删除（−126/−13）** | 已移除，不在工作树 |

**唤醒词检测核心（afe wakenet / multinet custom）现与官方 v2.2.4 字节一致**，无任何过度优化残留。memory `custom-wakeword-chain-2026-06` 记录的"multinet 四硬坑修复 / VFS 桥"已随恢复 commit 移除——官方 v2.2.4 的 custom_wake_word 实现本身已覆盖该能力，自研桥层冗余被正确清除。

---

## 二、相对官方的保留差异

| 改动 | 官方 v2.2.4 | 判定 |
|------|-----------|------|
| 删 `esp_wake_word.cc/.h`（−155 行）| 非 S3/P4 平台用的旧唤醒词后端 | 🟢 正常裁剪（本项目只跑 ESP32-S3）|
| `SetModelsList` 读 NVS `wakeword.mode` 选 custom/afe（`audio_service.cc:864`）| 仅按模型存在性自动选 | ⚪ 自定义唤醒词运行时切换配套（业务功能，登记）|
| `EnableWakeWordDetection` 切换时 reset input_resampler（`audio_service.cc:697`）| 无 | 🟢 防 feed size 切换缓存溢出（并发/缓冲安全，模块01已计入 audio）|

---

## 三、🔴 残留项（恢复不彻底）

### 🔴-02-A · 3 个死接口声明（恢复清理遗漏）

- **位置**：`main/audio/audio_service.h:131/134/135`
  ```cpp
  bool HasMultinetModel();              // :131
  void SetWakeWordThreshold(float);     // :134
  void ReleaseWakeWord();               // :135
  ```
- **性质**：恢复 commit 删除了它们的实现（随自研 wake_word / srmodel_vfs 一起移除），但 `.h` 声明未同步删除。
- **取证**：全仓库（main/ + components/）grep 这 3 个符号，**无任何 `.cc`/`.c` 实现或调用方**（仅 `.h` 声明）。C++ 中未被 ODR-use 的纯声明不报链接错误，故编译通过、无功能影响。
- **维护成本/风险**：极轻微。属头文件噪音 / 技术债——读 `.h` 会误以为这些能力存在，实际是空壳。**不碰任何红线**。
- **处置建议**：直接删除 `audio_service.h` 这 3 行声明。（注意：`HasMultinetModel` 注释说"custom 唤醒词前提"，删前确认无人计划接线；当前确认零调用。）

---

## 四、判定汇总

- wake 检测核心：**已对齐 v2.2.4，无过度优化**（恢复成功）。
- 🟢：删 esp_wake_word（裁剪）、resampler reset（防溢出）。
- ⚪：NVS mode 运行时选 custom/afe（自定义唤醒词业务）。
- 🔴：仅 1 项 = 3 个死接口声明（清理遗漏，建议删 3 行，无红线风险）。

> 本阶段只分析不改代码。🔴 已登记 PROGRESS.md。
