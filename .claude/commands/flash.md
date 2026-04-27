# ESP32 编译烧录验证

自动执行编译→烧录→监控的完整流程。

## 执行步骤

### 1. 编译
运行以下命令编译固件：
```bash
zsh -ic 'idf55 && idf.py build' 2>&1
```

如果编译失败：
- 阅读错误信息，定位问题文件和行号
- 修复编译错误
- 重新编译（最多重试 2 次）
- 如果 3 次都失败，停止并报告错误

### 2. 烧录
编译成功后，执行烧录：
```bash
zsh -ic 'idf55 && idf.py -p /dev/cu.usbmodem2101 -b 2000000 app-flash' 2>&1
```

注意：
- 使用 `app-flash` 增量烧录（不刷 bootloader），速度更快
- 如果端口不存在，尝试 `ls /dev/cu.usb*` 查找可用端口
- 烧录失败时检查 USB 连接

### 3. 监控验证
烧录完成后，启动串口监控：
```bash
zsh -ic 'idf55 && idf.py -p /dev/cu.usbmodem2101 monitor' 2>&1
```

在后台运行监控，等待 10 秒，然后检查输出：
- 查找 `ESP_ERROR_CHECK failed` 或 `assert` 或 `Guru Meditation` → 启动崩溃
- 查找 `Es7111AudioCodec` 相关日志 → 音频初始化状态
- 查找 `heap` 或 `free_size` → 内存状态
- 查找 `初始化完成` → 系统就绪

### 4. 报告结果
汇总报告：
- 编译状态（成功/失败 + 用时）
- 固件大小和剩余空间
- 烧录状态
- 启动日志关键信息（内存/音频/网络状态）
- 是否有错误或警告需要关注
