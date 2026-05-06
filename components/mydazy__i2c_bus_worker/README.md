# I2C Bus Worker

单线程串行化 I2C 总线访问的轻量调度器。专为 4G RF 共线场景设计——多 driver 共享 I2C 总线时杜绝协议层 / 会话层污染。

## 解决什么问题

ESP32 + 4G modem 项目里，I2C 总线常挂多个设备：

```
I2C bus ── ES8311 (codec)
        ── ES7210 (ADC)
        ── 触摸屏
        ── 加速度
        ── NFC
        ── ...
```

IDF `i2c_master` 内置 bus mutex，**单 transaction 已经原子**。但：

- 多 transaction 序列（如 sensor 配置 3 个寄存器）**之间不原子**——可能被音频写打断
- `i2c_master_bus_reset()` 发 9 个 SCL 脉冲——**会砸正在传输的其他设备状态机**
- ISR 路径与 polling 路径并发——单 transaction 微秒级冲突
- 4G LTE 突发 RF 干扰下，多 driver 错误恢复策略相互冲突

工业级方案是 Linux i2c-core 风格——**所有 I2C 访问汇聚到单一 worker，物理上串行**。本组件实现了这个模式。

## 用法

### 创建 worker

```c
i2c_master_bus_handle_t bus = ...;   // 已有 IDF bus

i2c_worker_config_t cfg = I2C_WORKER_DEFAULT_CONFIG(bus);
i2c_worker_handle_t worker;
ESP_ERROR_CHECK(i2c_worker_create(&cfg, &worker));
```

### 注册设备

```c
i2c_worker_dev_t* es8311_dev = i2c_worker_add_device(worker, 0x18, 100000);
i2c_worker_dev_t* sc7a20_dev = i2c_worker_add_device(worker, 0x19, 400000);
```

### 单条事务（同步阻塞）

```c
uint8_t cmd[] = { 0x01, 0x80 };
esp_err_t ret = i2c_worker_write(es8311_dev, cmd, sizeof(cmd), 100);

uint8_t reg = 0x05;
uint8_t val;
ret = i2c_worker_write_read(sc7a20_dev, &reg, 1, &val, 1, 100);
```

### 批量序列（防被打断）

```c
i2c_worker_lock_session(worker, 100);
{
    write_reg_a(es8311_dev);
    write_reg_b(es8311_dev);
    write_reg_c(es8311_dev);
}
i2c_worker_unlock_session(worker);
```

期间其他 driver 的 op 排队等待 unlock。

## 架构

```
caller A ──submit──┐
caller B ──submit──┤    ┌──────────┐
caller C ──submit──┼──► │ FIFO 32  ├──► worker task ──► i2c_master_*
caller D ──submit──┤    └──────────┘     (Pin Core 0)    (唯一调用方)
caller E ──submit──┘                      P10
   ▲                                      │
   └────────── result_sem ◄────────────────┘
```

- 单 task：`i2c_worker`（推荐 Core 0 P10）
- Queue 容量：默认 32，溢出阻塞 caller
- 同步语义：caller 阻塞直到 worker 完成
- 错误连击 ≥ N 次：worker 自动 `i2c_master_bus_reset`
- session lock：recursive，可重入

## 推荐配置

| 项 | 值 | 说明 |
|---|---|---|
| `task_priority` | 10 | 与 audio_output 同位，高于 LVGL/网络 |
| `task_core` | 0 | 与网络/codec 同核 |
| `queue_depth` | 32 | 5 driver 各 6 op 突发能容 |
| `stack_size` | 4096 | i2c_master_* 栈占用 ~1KB |
| `err_streak_for_reset` | 3 | 连续 3 次错误自动 bus_reset |

## 诊断

```c
i2c_worker_stats_t stats;
i2c_worker_get_stats(worker, &stats);
ESP_LOGI(TAG, "ops=%u err=%u resets=%u q_max=%u",
         stats.total_ops, stats.total_errors,
         stats.bus_reset_count, stats.max_queue_depth);
```

可定期上报后台，监控 RF 干扰强度。

## 性能开销

每次 op 多一次 task 切换（~10 µs）+ Queue / Semaphore 操作（~5 µs）。
对触摸 30 ms LVGL 节拍 / codec 启动序列等场景几乎无感。

## License

Apache-2.0
