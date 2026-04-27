---
name: esp32-s3-debug
description: ESP32-S3 崩溃诊断知识包。场景触发词：死机、重启、崩溃、Guru Meditation、LoadProhibited、StoreProhibited、backtrace、coredump、double exception、栈溢出、内存碎片。自动被 firmware-debugger / rtos-task-auditor 加载。
---

# ESP32-S3 崩溃诊断手册

本 skill 由 Claude Code 在遇到 ESP32-S3 崩溃 / 重启 / 栈溢出相关问题时自动加载。

---

## 一、常见崩溃码识别

### 1. Guru Meditation Error: Core <n> panic'ed (LoadProhibited)
**含义**：从一个不可读的地址读数据。
**常见原因**：
- 野指针（对象已被 delete / free，还在访问）
- 未初始化的指针
- 数组越界
- DMA 缓冲跨区（PSRAM 的 DMA 地址访问）

**排查**：
1. 看 `EXCVADDR` 寄存器值（访问的地址）
   - `0x00000000` 附近 → NULL 解引用
   - `0xA5A5A5A5` / `0xCDCDCDCD` → 未初始化内存
   - `0x3Cxxxxxx` → PSRAM 地址，可能 DMA 配错
2. backtrace 找到哪个函数访问的

### 2. LoadStoreAlignment
**含义**：未对齐访问（如 4 字节读取地址不是 4 的倍数）。
**常见**：结构体 cast、`memcpy` 后的裸指针访问。

### 3. StoreProhibited
**含义**：往只读内存写。
**常见**：`const char*` 被 cast 成 `char*` 后改写；字符串常量尝试修改。

### 4. InstrFetchProhibited
**含义**：PC 跳到不可执行地址。
**常见**：函数指针被踩坏；栈溢出覆盖返回地址。

### 5. StackCanaryWatchpointTriggered（栈溢出）
**含义**：FreeRTOS 的 stack overflow check 触发。
**处理**：
- 确定哪个任务溢出了
- 用 `uxTaskGetStackHighWaterMark` 常态监控
- 加大该任务栈（典型 +2KB）

### 6. Double Exception（最棘手）
**SP 值异常**（如 `0x60100000`）或 `|<-CORRUPTED`。
**🔴 第一怀疑**：PSRAM 栈 + flash op 碰撞。
**根因**：
- NVS `Settings::Set*` / OTA 会 `spi_flash_op_lock`，禁用 cache + PSRAM
- IPC 通知另一核 spin 等待
- 此时若 PSRAM 栈任务被调度 → 栈访问失败
**解决**：
- 持续循环 + Core0 的任务**必须内部 RAM 栈**
- 参考 `~/GitHub/.claude/CLAUDE.md` PSRAM 栈分层规则

---

## 二、Backtrace 解析

### 2.1 拿到 backtrace
```
Backtrace: 0x40xxxxxx:0x3fcfxxxx 0x40xxxxxx:0x3fcfxxxx ...
```
- 第一对：PC:SP（崩溃位置）
- 之后依次是调用链的返回地址

### 2.2 解析（用 addr2line）
```bash
# 项目根目录下
xtensa-esp32s3-elf-addr2line -e build/xiaozhi.elf -fCi 0x40xxxxxx
```
输出：`<函数名> at <文件>:<行>`

### 2.3 批量解析（推荐）
ESP-IDF 提供 `idf.py monitor` 会自动解析（启动 monitor 时捕获的 backtrace）。
离线：
```bash
xtensa-esp32s3-elf-addr2line -e build/xiaozhi.elf -fCi \
  0x40xxxxxx 0x40xxxxxx 0x40xxxxxx
```

---

## 三、Coredump 提取

### 3.1 开启 coredump
`sdkconfig`:
```
CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH=y
CONFIG_ESP_COREDUMP_DATA_FORMAT_ELF=y
CONFIG_ESP_COREDUMP_CHECKSUM_CRC32=y
```
分区表需要 coredump 分区（见 `partitions/v2/`）。

### 3.2 提取
```bash
idf.py coredump-info -p /dev/cu.usbmodem2101    # 读取 flash 上的 coredump
idf.py coredump-debug -p /dev/cu.usbmodem2101   # 进入 gdb 调试
```

### 3.3 关键信息
- 崩溃任务名、PC、SP
- 寄存器 dump
- 栈区内容
- 所有任务状态（运行中 / blocked / suspended）

---

## 四、内存碎片诊断

### 4.1 症状
- 剩余内存充足但 `malloc` 返回 NULL
- 长时间运行后性能下降
- 大块分配失败但小块还能成

### 4.2 诊断
```cpp
// 查看最大可分配块
size_t max_block = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
size_t free_size = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
ESP_LOGI(TAG, "internal: free=%d, largest=%d, frag=%.1f%%",
         free_size, max_block,
         100.0 * (1.0 - (double)max_block / free_size));
// 碎片率 > 50% → 严重碎片
```

### 4.3 预防
- 大对象池化（如图片解码缓冲用 static buffer）
- 启动阶段一次性分配长生命周期对象
- 频繁创建/销毁小对象 → 用 memory pool
- 避免长生命周期对象被短生命周期对象"夹"在中间

---

## 五、Task Watchdog 超时

### 5.1 报错
```
Task watchdog got triggered. The following tasks did not reset the watchdog in time:
 - IDLE (CPU 0)
 - <your_task> (CPU 0)
```

### 5.2 根因
- 任务长时间占用 CPU 不让出
- 死循环 / 阻塞在无超时的 API
- 优先级过高饥饿 IDLE

### 5.3 处理
- 长阻塞必须有超时
- 循环中 `vTaskDelay(1)` 或 `esp_task_wdt_reset()`
- 预期长耗时任务：`esp_task_wdt_delete(NULL)` 退出监控（谨慎）

---

## 六、典型 backtrace 模式快查

| 模式 | 含义 |
|---|---|
| `... → ipc_task → spi_flash_op_block_func` | 🔴 flash op 碰撞（另一核 PSRAM 访问） |
| `... → xQueueReceive → portMAX_DELAY` | 队列死等（违反 CLAUDE.md） |
| `... → lv_obj_del → 悬空对象` | LVGL 对象生命周期问题 |
| `... → tcp_input → recv` 崩溃 | 网络栈内存问题或栈溢出 |
| `... → esp_ota_write → flash_op` | OTA 过程崩溃，疑 PSRAM 栈陷阱 |

---

## 七、给 Jack 打出的调试命令模板

让 Jack 烧录后收集：

```bash
# 1. 编译并烧录
source idf54 && idf.py build
idf.py -p /dev/cu.usbmodem2101 -b 2000000 flash monitor

# 2. 触发 bug 后，保存完整 monitor 输出到文件
# 3. 如果有 coredump
idf.py coredump-info -p /dev/cu.usbmodem2101 > coredump_info.txt

# 4. 如果是任务栈/调度问题
# 在代码中加：vTaskList(buf); 或 vTaskGetRunTimeStats(buf)
# 编译时启用 CONFIG_FREERTOS_USE_TRACE_FACILITY=y
```

---

## 参考链接

- ESP-IDF 官方：[Fatal Errors](https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/api-guides/fatal-errors.html)
- ESP-IDF 官方：[Core Dump](https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/api-guides/core_dump.html)
- 项目经验：`~/GitHub/.claude/CLAUDE.md` PSRAM 栈章节（Double Exception 陷阱）
