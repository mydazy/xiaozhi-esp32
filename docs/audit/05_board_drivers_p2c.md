# 第二轮补充-外设/组件驱动

> 范围：ml307_gnss · education_mcp_tools · lamp_controller · i2c_device · camera ·
> i2c_bus_worker · esp_sc7a20h · esp_nfc_ws1850s · esp_lcd_touch_axs5106l
> 审计日期：2026-05-20 · 目标：ESP32-S3 / ESP-IDF 5.5
> 原则：仅记录第一轮（05_board_drivers.md）未覆盖的新发现。已报告项（共享 ADC 并发、
> lock_session 失效、NFC/codec 绕开 worker、背光 29%、触摸坐标钳位、worker sem 泄漏等）不重复。

---

## P0（必崩 / 烧硬件 / 砖机 / 安全）

### P0-1 NFC `JewelTransceive` 等待 TxIRQ 用无超时死循环，芯片卡死即整任务挂死（NFC）
- **判级理由**：后台 `nfc_detect` 任务（常驻 Core 0，300ms 轮询）每次检测都会进入
  `do{...}while((temp&0x40)==0)` 自旋等 TxIRQ，**无任何超时/退出条件**。WS1850S 在 4G RF 干扰、
  I2C 读 0xFF（`ws_read_reg` 错误返回 0xFF，bit6=1 会"碰巧"退出，但若返回 0x00 或挂死则永不退出）
  或天线异常时不上报 IRQ → 该 do/while 永久自旋，nfc_detect 任务卡死且不再 vTaskDelay，
  占满 Core 0 一个调度槽。属"必然挂死单任务+疑似看门狗复位"，定 P0。
- **文件**：`components/esp_nfc_ws1850s/src/nfc.c:646-649`
- **代码**：
  ```c
  do {
      temp = ReadRawRC(ComIrqReg);      //wait for TxIRQ
  } while((temp&0x40)==0);
  ```
- **根因**：移植自裸机示例，未加超时；I2C 错误时 `ReadRawRC` 返回 0xFF 才偶然脱困，
  返回 0x00（另一种 I2C 失败模式或上电未就绪）则死循环。
- **触发条件与影响面**：NFC 上电常驻轮询，4G TDD 突发期/读卡瞬间掉卡/天线接触不良时高频可触发。
  Core 0 同时跑网络/modem/主循环，一旦卡死会拖累整机响应并可能触发 Task WDT。
- **修复建议**：加自旋上限计数 + 超时退出：
  ```c
  for (int i = 0; i < 1000; i++) {
      temp = ReadRawRC(ComIrqReg);
      if (temp & 0x40) break;
      if (temp == 0xFF) { /* I2C 失败 */ return MI_ERR; }
  }
  if (!(temp & 0x40)) return MI_ERR;
  ```

### P0-2 NFC `PcdJewelCommand` 用上层小栈缓冲收任意长度响应 → 栈溢出（NFC）
- **判级理由**：`*replen = mf_com_data.mf_length / 8` 直接来自芯片回报的位长，最大可达
  `MAX_TRX_BUF_SIZE=255`（mf_data 数组容量）。随后 `memcpy(resp, mf_data, *replen)` 写入调用方
  栈缓冲——`PcdRidA`/`PcdReadA` 的 `resp[8]`。一张异常/恶意卡或 RF 噪声使 `mf_length` > 64 即写穿
  8 字节栈缓冲，破坏返回地址 → 崩溃/可能可控。读卡是常态路径，定 P0。
- **文件**：`components/esp_nfc_ws1850s/src/nfc.c:608-615`（产出）、:721 与 :738（`resp[8]` 受害方）
- **代码**：
  ```c
  *replen = mf_com_data.mf_length /8;
  if (*replen != 0) {
      memcpy(resp, &mf_com_data.mf_data[0], *replen);   // resp 仅 8B，*replen 可达 31
      Crc_Jewel(resp,*replen-2,crc);                    // *replen==1 时 -2 下溢为巨值
  ```
- **根因**：`memcpy` 长度未对调用方 `resp` 缓冲容量做上限钳制；`*replen-2` 在 `*replen==1` 时
  下溢（`if (*replen != 0)` 未排除 1）。
- **触发条件与影响面**：每次刷卡走 `JewelTransceive`→`PcdJewelCommand` 路径（Topaz/Jewel 卡）。
  虽然 `JewelTransceive` 末尾对 FIFO 读取已 `if (n > MAXRLEN) n = MAXRLEN(36)` 限了 mf_data 写入，
  但 `mf_length=(n-1)*8+lastBits` 仍可达 ~280，`/8` 后 *replen 可达 35 > resp[8]。
- **修复建议**：`PcdJewelCommand` 增加 `resp_cap` 入参并 `if (*replen > resp_cap) return MI_ERR;`；
  CRC 前显式 `if (*replen < 2) return MI_LENERR;`。调用方传 `sizeof(resp)`。

---

## P1（高频崩溃 / 体验严重退化）

### P1-1 触摸 `read_register` 把 reg-write 与 data-read 拆成两条独立 worker op，可被插队读错寄存器（I2C / 触摸）
- **判级理由**：worker 的串行保证是"每条 op 之间不交错"，但 `read_register` 提交的是
  **两条**独立 op（先 write reg 指针，再 read N 字节）。worker 队列在两条 op 之间可被 SC7A20H 的
  `read_burst`/`write_reg` 插入执行，使触摸读到的是被 SC7A20H 改写后的寄存器内部指针位置的数据
  → 触摸坐标错乱、误触。这正是 worker 想消灭的"跨事务竞争"，却因拆成两 op 复活。LVGL 读回调
  ~16ms + 加速度计 100ms 周期长期共存，高频，定 P1。
- **文件**：`components/esp_lcd_touch_axs5106l/axs5106l_touch.c:485-490`
- **代码**：
  ```c
  if (i2c_worker_write(self->dev, &reg, 1,   I2C_TIMEOUT_MS) == ESP_OK &&
      i2c_worker_read (self->dev, data, len, I2C_TIMEOUT_MS) == ESP_OK) {
  ```
- **根因**：用 write+read 两次提交模拟"先写后读"，而 worker 仅保证单 op 原子，不保证两 op 间不被
  插入。`i2c_worker_write_read`（OP_WRITE_READ，底层 `i2c_master_transmit_receive` 带 repeated-start）
  才是原子的，driver 却没用它。
- **触发条件与影响面**：P31 触摸 + 加速度计同 worker 常态共存。AXS5106L 多数寄存器有自动地址指针，
  中途被插入的事务会移动/复位指针，导致下一拍 read 偏移。表现为偶发坐标跳变/误点。
- **修复建议**：改用单条原子事务：
  ```c
  if (i2c_worker_write_read(self->dev, &reg, 1, data, len, I2C_TIMEOUT_MS) == ESP_OK) { ... }
  ```

### P1-2 `i2c_worker_write/read` 把 caller 给的 total_timeout 砍半再传底层，长事务下硬件超时被腰斩（I2C）
- **判级理由**：单事务接口把 caller 传入的 `timeout_ms` 用 `(timeout_ms>50)?(timeout_ms/2):50`
  当作底层 `i2c_master_*` 超时，再把**完整** `timeout_ms` 作为 `submit_and_wait` 的总等待。这意味着
  实际 I2C 硬件超时永远只有 caller 期望值的一半（如 100ms→50ms）。多字节 burst（NFC FIFO、触摸
  6 字节、固件升级）在 400kHz 下虽快，但总线被 reset/拥塞时 50ms 内未必完成 → 误判超时、触发
  err_streak→bus_reset 风暴，间接放大不稳定。高频路径上系统性偏短，定 P1。
- **文件**：`components/mydazy__i2c_bus_worker/i2c_bus_worker.c:396`、:410、:429
- **代码**：
  ```c
  .timeout_ms = (timeout_ms > 50) ? (timeout_ms / 2) : 50,   // 底层只拿到一半
  ...
  return submit_and_wait(dev->worker, &op, timeout_ms);       // 总等待是全额
  ```
- **根因**：作者意图"留一半给排队"，但排队时间应额外加而非从硬件超时里扣；且砍半逻辑无注释、
  无文档，caller（touch/sc7a20h 都传 100）实际只得 50ms 硬件超时。
- **触发条件与影响面**：所有走 worker 的事务。正常无负载时无感；bus_reset 后 50ms settling +
  设备重新 ACK 期间，50ms 硬件超时偏紧 → 连续 NACK → 触发 P1 级 reset 节流逻辑提前介入。
- **修复建议**：底层 op 超时直接用 caller 的 `timeout_ms`（或显式 `min(timeout_ms, 100)`），
  把"排队余量"加在 `submit_and_wait` 的总等待上：`total = timeout_ms + queue_margin`。

### P1-3 SC7A20H 摇一摇阈值 `deviation_mg²` 用 int32 计算，大阈值溢出为负 → 永远触发或永不触发（加速度计）
- **判级理由**：`thresh_sq = (int32_t)deviation_mg * deviation_mg`，`deviation_mg` 为 uint16
  最大 65535，平方 ≈ 4.29e9 远超 int32 上限（2.147e9），溢出为负数。比较 `s->ring[i] > s->thresh_sq`
  中 thresh_sq 为负 → 任意 `dev≥0` 都判强动帧 → 摇一摇被任意微动触发（误唤醒）。即使典型阈值
  ~1500mg（=2.25e6）不溢出，但接口允许的高值无防护。属"接口可配出错误状态"，且 dev 本身物理含义
  与 thresh_sq 量纲不一致（dev 是 mag²-grav² 差值 ~1e6 量级，thresh 却是 deviation²），判定基准本就
  存疑。误唤醒影响儿童玩具体验，定 P1。
- **文件**：`components/mydazy__esp_sc7a20h/sc7a20h.c:298`、比较点 :126-129
- **代码**：
  ```c
  h->shake.thresh_sq = (int32_t)deviation_mg * deviation_mg;   // 65535² 溢出 int32
  ...
  if (s->ring[i] > s->thresh_sq) strong++;   // thresh_sq 负 → 恒真
  ```
- **根因**：未对 deviation_mg 上限做钳制，int32 不足以容纳 uint16 平方；且 thresh 与被比较量
  （`dev = |mag²-gravity²|`）量纲设计上未对齐（注释也仅模糊称"deviation²"）。
- **触发条件与影响面**：调用方传 deviation_mg > ~46340（√2^31）即溢出。当前固件实际传值需核查
  （[待确认] 调用点 deviation_mg 取值）；即便未溢出，量纲不一致使阈值难调。
- **修复建议**：用 int64 计算并钳制：`int64_t t=(int64_t)deviation_mg*deviation_mg;` 比较量也升 int64；
  或将比较量统一为"加速度模长偏差"而非"模长平方差"，文档化量纲。同时入口 `if(deviation_mg>46340)`
  限幅或显式拒绝。

### P1-4 GNSS `last_fix_` 跨任务读写无同步，`GetLastFix()` 可读到撕裂的坐标（GNSS）
- **判级理由**：`ParseGga`/`ParseRmc` 在 AtUart 的 URC 回调任务里写 `last_fix_`（double lat/lon +
  char[16] utc），而 `GetLastFix()` 在调用方任务里读整个结构体引用，无锁无原子。double 在 32 位
  目标上非原子写，UI/上报任务可读到"新纬度+旧经度"或半写的 utc 字符串。位置显示错乱，4G 场景
  GNSS 常驻输出，高频，定 P1。
- **文件**：`main/boards/common/ml307_gnss.cc:59-64`、:86-89；`ml307_gnss.h:54`（`GetLastFix` 返回引用）
- **代码**：
  ```cpp
  last_fix_.latitude = lat; last_fix_.longitude = lon;     // URC 任务写
  ...
  const GnssFix& GetLastFix() const { return last_fix_; }   // 其它任务读，无同步
  ```
- **根因**：仅 `running_` 用了 `std::atomic`，`last_fix_` 这个真正的共享数据无任何保护。
- **触发条件与影响面**：任何在主循环/UI 任务调用 `GetLastFix()` 与 NMEA 解析并发的时刻。
- **修复建议**：用 `portMUX`/mutex 包裹写与读的整体拷贝，或 `GetLastFix()` 改为返回值拷贝并在
  拷贝期持锁；最低限度让 `valid` 作为发布屏障（写完所有字段后最后置 valid，读先查 valid）。

---

## P2（偶发 / 边缘场景）

### P2-1 NFC `pcd_fast_read` 长度算术可下溢，memcpy 越界（NFC）
- **判级理由**：`memcpy(rdata, mf_data, 4*(endaddr-startaddr))` 与 `user_print_hex(...,4*(endaddr-startaddr+1))`
  两处长度不一致，且若 `endaddr < startaddr`（参数误用）则 `endaddr-startaddr` 为负，提升为大正数
  → memcpy 巨量拷贝越界崩溃。该函数仅在 Ntag 读写示例路径调用（非主刷卡路径），定 P2。
- **文件**：`components/esp_nfc_ws1850s/src/nfc.c:2094-2097`
- **代码**：
  ```c
  user_print_hex("pcd_fast_read : ", mf_com_data.mf_data, 4 * (endaddr - startaddr + 1));
  memcpy(&rdata[0], mf_com_data.mf_data, 4 * (endaddr - startaddr));   // 与上行差 4 字节，且可下溢
  ```
- **根因**：长度未校验 `endaddr>=startaddr`，也未对 `rdata`/`mf_data` 容量设上限。
- **修复建议**：`if (endaddr < startaddr) return MI_PARAMERR;` 并统一长度公式、加 `MAX_TRX_BUF_SIZE`
  上限钳制。

### P2-2 `ws_read_reg` 错误哨兵 0xFF 与有效寄存器值混淆，污染 RMW 与重试判定（NFC）
- **判级理由**：`ws_read_reg` 失败返回 0xFF（:143），但 0xFF 是众多寄存器的合法值。`SetBitMask`/
  `ClearBitMask`（:161-185）读到 0xFF 错误值后仍照常 `tmp|mask` 写回，把错误读结果固化进寄存器；
  且 `SetBitMask` 用 `char tmp`（有符号）接收，0xFF→-1 符号扩展在 `tmp|mask` 中虽结果同值但属隐患。
  仅在 I2C 已出错时叠加，偶发，定 P2。
- **文件**：`components/esp_nfc_ws1850s/src/ws1850_iic.c:127-146`、:163-167
- **代码**：
  ```c
  uint8_t ws_read_reg(uint8_t reg) { ... return 0xFF; }   // 错误与合法 0xFF 不可区分
  void SetBitMask(unsigned char reg, unsigned char mask) {
      char tmp = 0x0; tmp = ws_read_reg(reg);             // signed char 接收
      ws_write_reg(reg, tmp | mask);                       // 读失败也照写
  ```
- **根因**：用带内哨兵表达错误；RMW 不检查读是否成功。
- **修复建议**：`ws_read_reg` 改为 `esp_err_t ws_read_reg(uint8_t reg, uint8_t *out)`；`SetBitMask`
  读失败则跳过写并返回错误；`tmp` 用 `uint8_t`。

### P2-3 `I2cDevice` 全部 I²C 操作用 `ESP_ERROR_CHECK`，单次总线错误即 abort 重启（I2C / codec）
- **判级理由**：`WriteReg`/`ReadReg`/`ReadRegs` 对 `i2c_master_transmit*` 用 `ESP_ERROR_CHECK`，
  与第一轮 P0-1（电池 ADC）同模式。codec 等走 I2cDevice 直连的外设在 4G RF 干扰/NACK 瞬态时
  任意一次失败即 panic 重启。但 I2cDevice 使用面比 power_manager 窄（codec 配置多在初始化期），
  运行期高频读写少，故 P2 而非 P0。
- **文件**：`main/boards/common/i2c_device.cc:24`、:29、:34；构造 :18
- **代码**：
  ```cpp
  ESP_ERROR_CHECK(i2c_master_transmit(i2c_device_, buffer, 2, 100));
  ESP_ERROR_CHECK(i2c_master_transmit_receive(i2c_device_, &reg, 1, buffer, 1, 100));
  ```
- **根因**：把可恢复运行时错误当启动断言；同时 codec 直连绕开 worker（第一轮 P1-3 已记），
  与 worker 路径并发时出错概率更高。
- **修复建议**：改返回 `esp_err_t` 由调用方处理；至少把运行期读写从 abort 改为日志+返回错误码。

### P2-4 `education_mcp_tools` UTF-8 解码对 4 字节字符越出 `0x4E00~0x9FFF` 后返回，但截断前已读到无效尾字节边界（教育）
- **判级理由**：`show_stroke` 解析 UTF-8 时，分支按 `character.length() >= N` 才读 `p[N-1]`，边界
  检查正确；但当字符串恰好 = 期望长度而内容是非法续字节（如 `0xE0 0x20 0x20`）时，仍会算出错误
  unicode 再被 `0x4E00~0x9FFF` 拦下，逻辑上安全。真正问题是 `g_inflight_*` 三个全局 atomic 在
  失败兜底路径 (:177) 只清 `url_hash` 不清 `start_us`/`req_id`，下次同字 dedup 判定依赖 hash==0
  短路尚可，但若 BeginFontPending 已翻位而下载任务创建失败（xTaskCreate 返回非 pdPASS 未检查，:207）
  则 pending 永不释放 → 后续 ShowEduCard 被屏蔽。属偶发资源未复位，定 P2。
- **文件**：`main/boards/common/education_mcp_tools.cc:146-207`（建任务无返回值检查）、:129（BeginFontPending）
- **代码**：
  ```cpp
  uint32_t req_id = ui->BeginFontPending();          // 先翻 pending 守护位
  ...
  xTaskCreatePinnedToCoreWithCaps([](void* arg){...}, "stroke_dl", 8192, ctx, 1, nullptr, 0, MALLOC_CAP_SPIRAM);
  // 返回值未检查；若建任务失败 → ctx 泄漏 + pending 永不 Cancel + g_inflight 残留
  return std::string("OK: stroke loading");
  ```
- **根因**：`xTaskCreatePinnedToCoreWithCaps` 返回值未判；失败时 ctx(new)、pending、inflight 全悬空。
- **触发条件与影响面**：仅在 PSRAM 紧张/任务表满导致建任务失败时（弱网+多次连发同时下载更易），
  之后教育卡再也弹不出。
- **修复建议**：检查建任务返回值，失败则 `delete ctx; ui->CancelFontPending(req_id);
  g_inflight_url_hash_.store(0);` 并返回 ERR。

### P2-5 GNSS `urc_handle_` 迭代器默认未初始化，Start 早失败时 Stop 可对野迭代器 Unregister（GNSS）
- **判级理由**：`std::list<UrcCallback>::iterator urc_handle_;` 默认构造为奇异（singular）迭代器，
  仅 `urc_registered_` 标志守护。当前 Start 流程是先 Register 再置 registered=true，路径上一致；
  但 `urc_handle_` 在 Register 成功后赋值、失败分支已正确 Unregister+清标志，故实际安全。风险点在
  Stop()/析构若被并发调用（如 Start 进行中外部调 Stop），`urc_registered_` 非原子读改可能让两条
  路径都进入 Unregister。极边缘，定 P2。
- **文件**：`main/boards/common/ml307_gnss.h:66`、`ml307_gnss.cc:189-192`、:11（析构调 Stop）
- **根因**：`urc_registered_` 为普通 bool，Start/Stop 跨任务无同步；迭代器作为句柄无"空值"语义。
- **修复建议**：`urc_registered_` 改 `std::atomic<bool>` 并用 CAS 保证 Unregister 只执行一次；
  或给 urc_handle_ 一个明确的 invalid 哨兵。

---

## P3（潜在远期风险）

### P3-1 `mydazy_nfc_resume` 的 running 检查与 store 存在 TOCTOU，可重复建检测任务（NFC）
- **判级理由**：`if (s_nfc.running.load()) return;` 与稍后 `s_nfc.running.store(true)` 之间无锁，
  两个任务并发 resume 可都通过检查 → 创建两个 `nfc_detect` 任务、覆盖 `s_nfc.task` 句柄 →
  pause 只能停一个，另一个泄漏常驻。pause/resume 通常由单一电源管理路径串行调用，故 P3。
- **文件**：`components/esp_nfc_ws1850s/src/esp_nfc_ws1850s.cc:218-231`、:160-166（init 同款）
- **修复建议**：用 `compare_exchange_strong(expected=false, true)` 原子占位再建任务。

### P3-2 `lamp_controller` 在 GPIO_NUM_NC 时静默返回但仍注册了"半个对象"语义（板级）
- **判级理由**：构造函数对 `GPIO_NUM_NC` 提前 return，不注册任何 MCP 工具——行为正确；但
  对象仍存在且 `power_` 状态字段无意义，外部若持有指针并误以为可控会无声失败。纯设计气味，
  无崩溃，定 P3。
- **文件**：`main/boards/common/lamp_controller.h:14-16`
- **修复建议**：用工厂函数返回 `optional`/nullptr，或在头注释明确 NC 即 no-op。

### P3-3 NFC `nfc_detect` 任务读卡回调 `s_nfc.cb` 在后台任务上下文执行，回调内若做重活会拖住轮询（NFC）
- **判级理由**：`detect_task_fn` 直接在检测循环里同步调用 `s_nfc.cb(type,&uid,ctx)`（:133），
  若回调内做 UI/网络/长阻塞，会推迟下一拍检测并占住 Core 0。当前回调约定为轻量（投事件），
  属契约依赖，定 P3。
- **文件**：`components/esp_nfc_ws1850s/src/esp_nfc_ws1850s.cc:133`
- **修复建议**：文档明确"回调必须非阻塞、≤数 ms"，或在回调外层 Schedule 到应用线程。

### P3-4 SC7A20H 初始化期 `write_reg` 返回值全部忽略，半配置态不可检测（加速度计）
- **判级理由**：`sc7a20h_init` 的 10 条配置 `write_reg(...)` 无一检查返回值（:237-249）。即便走了
  lock_session（第一轮 P1-2 已指其失效），单条 write 失败也被吞，传感器进入半配置态而 init 仍返回
  成功句柄。WHO_AM_I 已先校验过通信，配置期再失败概率低，定 P3。
- **文件**：`components/mydazy__esp_sc7a20h/sc7a20h.c:237-249`
- **修复建议**：累计写失败计数，>0 则 init 返回 NULL（或回读关键寄存器 CTRL_REG1/REG4 校验）。

---

## 统计

| 等级 | 数量 | 条目 |
|------|------|------|
| P0   | 2 | P0-1 NFC TxIRQ 无超时死循环挂任务 · P0-2 NFC 响应 memcpy 越界栈溢出 |
| P1   | 4 | P1-1 触摸 read 拆两 op 可被插队 · P1-2 worker 硬件超时被腰斩 · P1-3 摇一摇阈值平方溢出 int32 · P1-4 GNSS last_fix 跨任务无同步 |
| P2   | 5 | P2-1 fast_read 长度下溢越界 · P2-2 ws_read_reg 0xFF 哨兵混淆 · P2-3 I2cDevice ESP_ERROR_CHECK abort · P2-4 stroke 建任务无返回值检查泄漏 · P2-5 GNSS urc 句柄/标志非原子 |
| P3   | 4 | P3-1 NFC resume TOCTOU · P3-2 lamp NC 半对象 · P3-3 NFC 回调在后台任务执行 · P3-4 SC7A20H 配置写吞错 |
| **合计** | **15** | |

> 重点联动：P0-1（NFC 死循环）+ P2-2（0xFF 哨兵）共因——I2C 读失败的错误传播链全程不可靠，
> 建议先把 `ws_read_reg`/`JewelTransceive` 的错误路径打通再谈其余。P1-1（触摸两 op）与第一轮
> P1-2/P1-3 同属"worker 串行承诺被各种方式破坏"，根因一致，宜统一整改：所有读改用
> `i2c_worker_write_read` 原子事务，NFC/codec 全量并入 worker。
> [待确认] P1-3 实际 deviation_mg 调用取值；P2-4 BeginFontPending 内部实现是否对未匹配 req_id 的
> Cancel 做幂等。
