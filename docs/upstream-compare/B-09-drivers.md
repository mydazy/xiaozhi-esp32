# B-09 自研驱动深审（sc7a20h + i2c_bus_worker） — 对比官网 v2.2.4

> 官方对这两个驱动 0 引用，纯自研；标尺=量产稳定；🟢必要/🔴过度/⚪扩展/🛡️红线保留；只分析不改码。

## 取证范围

| 文件 | 行数 | 角色 |
|---|---|---|
| `components/mydazy__esp_sc7a20h/sc7a20h.c` | 373 | SC7A20H 加速度计：拿起唤醒 + 摇一摇 + 桌面双击 + 死值兜底 |
| `components/mydazy__esp_sc7a20h/include/sc7a20h.h` | 100 | 四 API 声明 |
| `components/mydazy__i2c_bus_worker/i2c_bus_worker.c` | 490 | I2C 总线单线程串行化调度器（codec/touch/sensor 共线） |
| `components/mydazy__i2c_bus_worker/include/i2c_bus_worker.h` | 208 | worker API + 默认配置宏 |
| 调用方 `main/boards/mydazy-p30-4g/mydazy_p30_board.cc` | :175/:300/:531 | worker 创建、sc7a20h init/shake、休眠 arm |
| 调用方 `main/boards/mydazy-p30-wifi/mydazy_p30_board.cc` | :173/:298/:530 | 同上（wifi 板） |
| 间接用户 `main/audio/codecs/box_audio_codec.cc` | :37/:67 | ES8311/ES7210 经 worker 串行 |

**串行化全覆盖已核实**：`grep i2c_master_*` 仅命中 worker 内部（`i2c_bus_worker.c:110/118/126/135/194`），sc7a20h / codec(`mydazy__codec_ctrl_i2c`) / touch(`axs5106l_touch.c`+`axs5106l_upgrade.c`) 全部经 `i2c_worker_*`，**无任何 driver 绕过 worker 裸调 i2c_master**。worker 上实际挂载 4 类设备：codec×2 + touch + sc7a20h。

---

## 🟢 必要（偏离官方但服务量产稳定）

| 项 | 实现 | 为何必要（并发/内存/稳定） | 证据 file:line |
|---|---|---|---|
| **I2C 单线程串行化** | 单 task + 单队列 + 每 op 携带 caller 栈 binary sem，caller 阻塞等结果，worker 内唯一调 `i2c_master_*` | 4G RF 共线真实存在：codec/touch/sensor 多 task 并发访问同一 I2C bus；IDF bus mutex 只保护单 transaction，无法阻止 `i2c_master_bus_reset`（发 9 个 SCL 脉冲）砸正在传输的其它设备状态机。去掉它 → 弱网下 bus_reset 与 codec/touch 事务交错 → 量产偶发卡死/乱跳。**红线·并发** | `i2c_bus_worker.c:147` worker_task / `:103` worker_execute_op；理由见 `README.md:21` | 
| **死值兜底（数据卡死检测）** | motion_task 内连续 ≥20 帧 xyz 完全不变 → 判数据卡死，暂停摇一摇/双击判定，恢复变化后自动解除 | 项目记忆 `sc7a20h-boot-deadvalue`：缺 BOOT 上电重载致 ADC 死值 → 静止误触发，三台真机验证。死值兜底是运行期第二道防线（BOOT 是第一道，见下条），防 ADC 卡死值持续喂入判定算法误触发 | `sc7a20h.c:189-206`（same_streak 计数 + data_dead 状态机 + continue 跳过判定） |
| **BOOT 上电重载** | init 序列首步 `CTRL_REG5=0x80`（BOOT=1 reboot memory）+ 延时 20ms | 同上记忆根因直接修复：上电先 reboot sensor 内部寄存器，根治 ADC 死值来源 | `sc7a20h.c:260-261` |
| **10 寄存器原子配置序列** | init 用 `lock_session(200)` 包裹 10 条寄存器写，结束 unlock | 防音频/触摸 ISR 中途插入打断配置序列，导致 sensor 半配置态。配置期间是真实多 driver 并发窗口 | `sc7a20h.c:257-277` |
| **LIR_INT1=0 不锁存** | `CTRL_REG5=0x00`（不锁存中断） | 注释记 v4.0.1 量产修复"秒醒"：codec 失电短路 SDA/SCL 致 INT1_SRC 清不掉钉死中断；改动态电平后瞬态运动不再钉死 INT1 | `sc7a20h.c:267-269` |
| **整数定点运算（无浮点/无 sqrt）** | 模长平方 − 重力平方比较，阈值预平方；mg/ms→寄存器 LSB 编译期整除 | motion_task 100ms 周期热路径，避免 FPU 上下文与精度漂移；ESP32-S3 浮点虽硬件支持但任务切换需保存 FPU 上下文 | `sc7a20h.c:184-187`、`:324-325`、`:252-255` |
| **bus_reset 节流（防风暴）** | 连续 reset ≥3 次（500ms 窗内）→ 冷却 5s 不再 reset，期间仍清 err_streak 让 driver 继续 retry | 设备层 NACK（chip 未上电/空白片/RF 干扰）反复触发 reset 会刷屏并加剧总线扰动；节流是稳定优化。逻辑自洽无未初始化 bug（首次 last_reset_us=0，now−0 远超窗口，走正常 reset） | `i2c_bus_worker.c:172-208` |
| **submit 超时诊断 + 兜底再等** | sem take 超时打印队列深度/累计错误/total_ops，再兜底等 200ms | 量产现场可区分"worker 被饿"还是"i2c_master 硬件超时卡住"，可观测性 | `i2c_bus_worker.c:259-276` |
| **lock-free 诊断统计** | total_ops/errors/bus_reset/queue_depth/timeout 全 atomic，worker 写外部读 | 任意 task 可安全读诊断上报后台，无需持锁。worker 私有计数器(err_streak 等)严格只 worker 访问，分层正确 | `i2c_bus_worker.c:83-88` / `:467` get_stats |

---

## 🔴 过度（不服务稳定 / 治标 / 堆复杂度 / 死代码）

| 项 | 实现 | 为何判过度 | 维护成本/风险 | 证据 file:line |
|---|---|---|---|---|
| **桌面双击 strike 全链路（死代码）** | `sc7a20h_strike()` API + `update_strike()` 状态机 + `strike_state_t` 结构 + `STRIKE_IDLE/WAIT_GAP` 枚举，约 80 行 | **从未被调用**：4g/wifi 两板 `InitializeSc7a20h()` 只调 `sc7a20h_shake`（:303/:301），`OnStrike` 回调虽定义但无人注册。`update_strike` 每帧被 motion_task 调用(`:210`)但 `t->enabled` 永远=false（仅 `sc7a20h_strike` 会置 true），整条逻辑跑不到 | 死代码占 motion_task 每帧一次空判 + 维护者误以为双击在用；改 shake 时易牵连 | `sc7a20h.c:344-372`(API 无调用方) / `:136-165`(update_strike) / `:65-78`(枚举+结构) / 板级 `:314 OnStrike` 定义但 `:300-304` 不注册 |

> 说明：strike 这 80 行**不触及电源域/内存安全/4G脏帧守卫/并发**，不属红线保留，可判 🔴。它确实"已写未启用·无调用方"，符合死代码定义。建议产品决策：要么板级接上双击唤醒（代码已就绪），要么删除。

---

## ⚪ 扩展（纯业务功能，登记）

| 项 | 说明 | 证据 |
|---|---|---|
| 摇一摇闹钟摇停 | `OnShake` → `AlarmRinger::ShakeStop(6)`，6 次累计才停闹铃（防误关）；日常摇→AI 互动已砍 | 板级 `mydazy_p30_board.cc:307-311`，驱动 `sc7a20h.c:309 sc7a20h_shake` |
| 诊断统计读取/重置 API | `i2c_worker_get_stats` / `reset_stats`，供后台上报；reset 保留 bus_reset_count | `i2c_bus_worker.c:467-489` |

---

## 🛡️ 红线保留（触及休眠/掉电/并发，即便像过度也只标不动）

| 项 | 说明 | 证据 |
|---|---|---|
| **EXT1 拿起唤醒 sc7a20h_wakeup** | 深睡前 arm EXT1(ANY_LOW) + RTC pullup + 兜底清 INT1 latch。属**掉电/休眠路径**。运行期默认行为两板不一致：4g 板 `pickupWake` 默认 0（关），wifi 板默认 1（开）。项目记忆 `gyro-sleep-wake`：息屏 arm 陀螺仪→桌面震动整机重启扰民，故 4g 默认关是规避。**休眠路径红线，保留不动**，仅登记两板默认值差异供产品确认 | 驱动 `sc7a20h.c:285-307`；调用 4g `:531-535`(默认0) / wifi `:530-532`(默认1) |
| **I2C worker 自动 bus_reset 调度** | err_streak 达阈值 worker 统一 `i2c_master_bus_reset`，50ms settling。属**并发/总线异常恢复路径** | `i2c_bus_worker.c:172-208` / `:134-139` |
| **整个 i2c_bus_worker 串行化层** | 见 🟢 首条。**并发红线**，去掉会崩（4G RF 共线 + reset 砸状态机真实存在），保留 | `i2c_bus_worker.c` 全文 |

---

## 深审发现·并发与内存（逐点，file:line + 风险级 · 本模块重头）

### 🔴 P1（高频隐患）— submit_and_wait 超时放弃路径：栈上 Static 信号量 UAF

**`i2c_bus_worker.c:241-280`**
```
241  StaticSemaphore_t sem_buf;                       // caller 栈上
242  SemaphoreHandle_t sem = xSemaphoreCreateBinaryStatic(&sem_buf);
...
270  if (xSemaphoreTake(sem, pdMS_TO_TICKS(200)) != pdTRUE) {
272      ESP_LOGE(TAG, "worker stuck > 200ms after queue timeout");
273      return ESP_ERR_TIMEOUT;                       // ← 放弃 delete sem 直接返回
274  }
```
- 信号量存储 `sem_buf` 在 **caller 栈帧**上。`submit_and_wait` 从 273 行 return 后，调用链一路返回（`i2c_worker_write/read/write_read` → driver → 业务），**caller 栈帧回退，`sem_buf` 失效**。
- 而 worker 此刻正卡在 `i2c_master_*`（>200ms 硬件超时未返回）。worker 一旦恢复并执行到 `:226-227`：`*op.result_out = ret; xSemaphoreGive(op.result_sem)` —— `op.result_sem` 仍指向已失效的栈 `sem_buf`，`xSemaphoreGive` 会**写已被其它栈帧复用的内存 = 栈 UAF**。同理 `op.result_out`(`&op_result`，:247)也指向失效栈。
- 注释（:272）自称"接受 leak 换 UAF 安全"，但**对栈上 static 信号量根本换不到安全**：不 delete 只能避免"提前释放堆"，而这里存储在栈上、随函数返回必失效，放弃 delete 不改变栈回退的事实。**注释与实现矛盾**。
- 风险级 **P1**：触发条件=单次 I2C 事务硬件超时 >（队列等待 + 200ms 兜底）。弱网/RF 干扰/codec 失电短路 SDA 时 I2C 卡死真实存在（驱动自己多处注释提到 codec 失电短路 SDA/SCL）。一旦命中，worker 回写踩栈 → 随机 corruption，量产返修最难定位的一类。
- 正确方向（不在本任务改）：result_sem/result_out 改为**堆分配 + 引用计数/worker 负责释放**，或超时后置 op 失效标志让 worker 跳过回写（需 op 生命周期与 caller 解耦）。

### 🟢 已正确处理 — 诊断私有计数器无 data race

**`i2c_bus_worker.c:75/79/80`**：`err_streak` / `consecutive_resets` / `last_reset_us` 为裸 `uint32_t`/`uint64_t`，注释标"仅 worker task 访问"。已核实全部读写点(`:137/138/175/177/179/182/184/185/189/193/196/197/200/220/223`)均在 worker_task / worker_execute_op 上下文，create(`:296`) 仅初始化。**单写者单读者同 task，无并发，正确**。对外暴露的统计(total_ops 等)该 atomic 的都 atomic 了(`:83-88`)。

### 🟢 已正确处理 — 自动 reset 节流无未初始化 bug

**`i2c_bus_worker.c:175-208`**：曾疑首次进入 `last_reset_us=0` 导致 burst 窗口误判，核实=首次 `now−0` 远超 500ms 窗口 → `in_burst_window=false` → `consecutive_resets` 归零 → 正常走 reset。逻辑自洽。

### 🟡 P3（潜在 · 语义文档缺口）— lock_session 的"原子"有前提

**`i2c_bus_worker.c:438-450` + `sc7a20h.c:257-277`**
- recursive mutex 让同一 caller 串行提交多 op，挡住"其它 caller 在会话期间提交新 op"。但 mutex **不清空队列**：若别的 caller 在 `lock_session` 调用**之前**已把 op 塞进队列，那个 op 仍会先于本会话的 op 被 worker 执行。即 lock_session 保证"会话内 op 之间不被插入"，**不保证"会话起点队列为空"**。
- sc7a20h init 的 10 寄存器序列(`:258-277`)依赖此"原子"。实际安全：init 在系统早期、其它 driver 尚未起 task，队列空。但**语义上有前提未在头文件说明**。
- 风险级 **P3**：当前调用时序下不触发；若未来有 driver 在 init 前并发提交则序列可能被前置 op 插队。属文档/契约缺口，非当前 bug。

### ✅ 内存边界检查（无裸越界）

- **sc7a20h 读缓冲**：`read_burst` 固定读 6 字节进 `buf[6]`(`:104-105`)；`read_xyz_mg` 按固定下标 buf[0..5] 解析(`:108-113`)，长度=类型固定，**无裸索引越界**。WHO_AM_I 读 1 字节进 1 字节缓冲(`:242-243`)。
- **worker 读写 buffer**：长度由 caller 显式传 `len`，直接透传 `i2c_master_*`，无 worker 内 memcpy/拼接，**无越界面**。
- **滑窗 ring buffer**：`ring[SHAKE_WINDOW_MAX=16]`，`win` 入参被 clamp 到 [1,16](`sc7a20h.c:320-322`)，`idx=(idx+1)%window`(`:123`) 且遍历 `i<window`(`:126`)，**无越界**。
- **结构体内存**：sc7a20h `calloc` 一次(`:235`)，失败/WHO_AM_I 不符均正确 free + remove_device(`:240/246-248`)，无泄漏。worker `heap_caps_calloc` 内部 RAM，create 失败 goto fail 逐项释放(`:320-324`)，destroy 投 QUIT op 优雅停 + 释放队列/mutex(`:327-345`)，**无泄漏**。

### ⚠️ 长跑/资源观察（非 bug，登记）

- **submit_and_wait 每 op 创建+删除栈 static 信号量**(`:241/278`)：正常路径成对，无泄漏。但叠加上面 P1，超时放弃路径不 delete（虽然真正问题是 UAF 而非 leak）。
- **motion_task 永不退出**(`sc7a20h.c:178 for(;;)`)：无 stop API，shake/strike 一旦 ensure 创建即常驻（栈 2560 / P1 / Core1）。设计如此（常驻传感器），非泄漏；但 sc7a20h 无 deinit，task 与 sensor 句柄生命周期=整机，登记。

---

## 小结

🟢 必要 **9** ｜ 🔴 过度 **1** ｜ ⚪ 扩展 **2** ｜ 🛡️ 红线保留 **3**

**一句话结论**：i2c_bus_worker 串行化层是服务 4G 共线稳定的真·并发红线（去掉会崩，保留），sc7a20h 的 BOOT/死值兜底/原子配置/不锁存均有真机根因背书属必要；**唯一过度是 strike 桌面双击 80 行死代码**（无任何调用方）；并发深审查出**一处 P1 真实隐患——submit_and_wait 超时放弃路径对 caller 栈上 static 信号量放弃 delete，注释自称换 UAF 安全实则栈回退后 worker 回写仍踩失效栈，量产难定位**，强烈建议修（不在本任务范围）。
