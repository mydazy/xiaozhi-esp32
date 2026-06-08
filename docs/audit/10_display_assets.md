# 10 显示 / LVGL / 资产 / LED 子系统审计

> 审计范围：`main/display/`（display/lcd/oled/emote/ui_display + lvgl_display/ + gif/ + ui/ widgets）、`main/assets.*`、`main/led/`
> 关注点：LVGL 线程安全（非线程安全，跨任务必须加锁/消息队列）、资产解析越界、字体/图像加载边界、LED 任务并发与定时器
> 审计方法：三遍（广度遍历 → 红线深挖 → 反审自检）。生成的二进制字库/PNG 数据未逐行读，只看加载/解析逻辑。
> 审计日期：2026-05-20

## 文件清单（28 个逻辑文件）

- display 顶层（11）：display.cc/h、lcd_display.cc/h、oled_display.cc/h、emote_display.cc/h、ui_display.cc/h、text_font.h
- lvgl_display（12）：lvgl_display.cc/h、lvgl_image.cc/h、lvgl_font.cc/h、lvgl_theme.cc/h、emoji_collection.cc/h、gif/gifdec.c、gif/lvgl_gif.cc/h、gif/gifdec.h、gif/gifdec_mve.h（汇编宏，未逐行）
- ui（6）：ui/core/managed_timer.h、ui/widgets/managed_timer.h、ui/widgets/control_center.cc/h、ui/resources/ui_img_paths.h、ui/theme/ui_config.h
- led（7）：circular_strip.cc/h、single_led.cc/h、gpio_led.cc/h、led.h
- assets（2）：assets.cc、assets.h

二进制数据文件（未逐行）：boards 内 ui_img_*.c、字库 .c、emoji font_emoji_*.c 等。

---

# 第一遍 · 广度遍历（显性缺陷）

### 10-P1-A：CircularStrip 动画方法在加锁前裸写共享 colors_，与定时器回调竞态
- 等级：P1（高频体验退化 / 偶发崩溃）。判级理由：动画切换是高频路径（每次设备状态变更都调用 OnStateChanged → Blink/Scroll/FadeOut），写共享 vector 与 esp_timer 任务读同一 vector 并发，量产返修最贵的偶发崩溃来源。
- 文件：`main/led/circular_strip.cc:82-85`（Blink）、`159-162`（Scroll）、另 `100-118` FadeOut / `121-157` Breathe 在回调内读 colors_。
- 代码：
```cpp
void CircularStrip::Blink(StripColor color, int interval_ms) {
    for (int i = 0; i < max_leds_; i++) {   // ← 未持 mutex_ 就写 colors_
        colors_[i] = color;
    }
    StartStripTask(interval_ms, [this]() { ... colors_[i] ... });  // StartStripTask 才加锁
}
```
- 根因：`Blink`/`Scroll` 在调用 `StartStripTask`（内部才 `lock_guard`）之前，先在无锁状态下整体改写 `colors_`。而上一轮动画的 `strip_timer_` 回调（在 esp_timer 任务里，持 mutex_ 读 colors_）可能正在并发执行。`colors_` 是 `std::vector`，并发读写 = 数据竞争。
- 触发条件/影响面：设备状态快速连续切换（如 Connecting→Listening→Speaking），新动画方法写 colors_ 同时旧定时器回调读 colors_。WS2812 仅展示，不烧硬件，但可能崩溃或乱闪。
- 修复建议：把 `Blink`/`Scroll` 开头对 `colors_` 的写入移到 `StartStripTask` 内（持锁后），或在这两个方法开头先 `std::lock_guard<std::mutex> lock(mutex_); esp_timer_stop(strip_timer_);` 再写 colors_。最简：让 `StartStripTask` 接收一个"初始化 colors_"的可选 lambda，在持锁后执行。
- [发现于第一遍]

### 10-P2-A：CircularStrip 动画 lambda 内 static 局部变量跨实例/跨动画泄漏状态
- 等级：P2（边缘）。判级理由：单板通常只有一个 CircularStrip 实例，但 static 在 Blink/Breathe/Scroll 之间共享，切换动画时残留上次相位，偶发首帧异常。
- 文件：`main/led/circular_strip.cc:87`（`static bool on`）、`123-124`（`static bool increase` / `static StripColor color`）、`164`（`static int offset`）。
- 根因：lambda 内 `static` 是函数级静态，所有动画共用、永不复位。Blink 切到 Breathe 再切回，相位/颜色是脏的。
- 触发条件/影响面：动画切换时第一帧亮度/方向不对，纯视觉，无崩溃。
- 修复建议：把这些状态提为 CircularStrip 成员变量并在 `StartStripTask` 里复位，或用 `colors_` 已有状态推导，去掉 static。
- [发现于第一遍]

### 10-P2-B：assets.cc Apply() 中途 return 导致 cJSON root 泄漏
- 等级：P2（资源泄漏，偶发）。判级理由：仅当 background_image 在 index.json 中声明但资产缺失时触发，非常规路径；泄漏一次 cJSON 树（几 KB），重复触发可累积。
- 文件：`main/assets.cc:314-321` 与 `335-342`。
- 代码：
```cpp
if (cJSON_IsString(background_image)) {
    if (!assets->GetAssetData(background_image->valuestring, ptr, size)) {
        ESP_LOGE(TAG, "The background image file %s is not found", ...);
        return false;     // ← 直接 return，未走到末尾 cJSON_Delete(root)
    }
    ...
}
```
- 根因：函数末尾才有 `cJSON_Delete(root)`（行 367），但 skin 解析里有两处 `return false` 提前退出，跳过释放。同理 text_font 加载失败（行 260）也 return false 泄漏。
- 触发条件/影响面：OTA 下来的 index.json 声明了 background_image 但分区里没那个资产 → 每次 Apply 泄漏一棵 cJSON 树。Apply 仅启动/换肤时调用，频率低。
- 修复建议：把这些 `return false` 改为 `cJSON_Delete(root); return false;`，或用 RAII 包装 root（unique_ptr + 自定义 deleter）。
- [发现于第一遍]

### 10-P3-A：emote_display.cc SetChatMessage 用裸 new[]/strcpy，异常路径泄漏
- 等级：P3（潜在）。判级理由：`new char[]` 与 `delete[]` 之间无异常抛出点（strcpy/std::replace/emote_set_event_msg 不抛），当前安全；但裸 new[]/strcpy 是脆弱写法，后续改动易引入泄漏。
- 文件：`main/display/emote_display.cc:151-155`。
- 代码：`char* new_content = new char[len + 1]; strcpy(new_content, content); ... delete[] new_content;`
- 根因：手动内存管理，无 RAII。
- 修复建议：改用 `std::string new_content(content); std::replace(new_content.begin(), new_content.end(), '\n', ' ');` 传 `new_content.c_str()`。
- [发现于第一遍]

### 10-P3-B：ContainsCjk 对 3 字节 UTF-8 在串尾可能多读 1 字节
- 等级：P3（潜在 over-read，几乎无害）。判级理由：当 b0 是 3 字节序列首字节但字符串在 b0 后立即结束（text[1]=='\0'），代码先读 `text[2]` 再判断 b1==0。text[2] 是 NUL 之后一字节，通常仍在分配缓冲内（std::string/字面量有终止符），越读 1 字节但不解引用越界内存。
- 文件：`main/display/ui_display.cc:49-51`。
- 代码：
```cpp
unsigned char b1 = (unsigned char)text[1];
unsigned char b2 = (unsigned char)text[2];   // ← 先读 text[2] 才判 b1==0
if (b1 == 0 || b2 == 0) break;
```
- 根因：先取 b2 再判 b1。应短路：先判 b1，b1!=0 再读 b2。
- 修复建议：改为 `unsigned char b1 = text[1]; if (b1 == 0) break; unsigned char b2 = text[2]; if (b2 == 0) break;`
- [发现于第一遍]

---

# 第二遍 · 红线深挖（内存安全 / 并发 / 数据流）

> 红线①电源域：本子系统不涉及电池 ADC/充电/休眠配置（display 仅 esp_pm_lock_acquire/release 成对，见 lvgl_display.cc:158/227，无误配置风险）。红线④OTA 验签：assets::Download 走 HTTP 明文写分区，无验签——但资产分区非可执行固件，归 OTA 子系统（04）管，此处仅记一条数据流风险（10-P1-C）。本遍聚焦 ②内存安全 ③并发。

### 10-P0-A：GIF 笔画动画 ReplaceEmoji 释放旧 buffer 时，活动 GIF 解码器仍引用该 buffer（潜在 use-after-free）
- 等级：P0（必崩 / 内存安全红线②）。判级理由：FontGif 是核心交互（孩子写字识字，高频触发，每个字一次 GIF），use-after-free 在量产规模必现。虽当前同 DisplayLockGuard + 同 LVGL 任务串行执行降低了即时崩溃概率，但 buffer 生命周期管理依赖"锁内时序"这一脆弱前提，任何后续把解码搬出 LVGL 任务、或 lv_timer 在锁外触发的改动都会立即变成确定性崩溃。
- 文件：`main/display/ui_display.cc:496-497`（FontGif）+ `main/display/lvgl_display/gif/gifdec.c:803`（解码器持续 memcpy 源 buffer）+ `main/display/lcd_display.cc:1080-1086`（SetEmotion 重建 gif）。
- 代码：
```cpp
// ui_display.cc FontGif()
emoji_collection->ReplaceEmoji("font", raw);  // ← 删除旧 "font" LvglRawImage → heap_caps_free 旧 gif_buffer
LcdDisplay::SetEmotion("font");               // ← 内部才 gif_controller_->Stop()+reset() 停旧解码器
```
```c
// gifdec.c f_gif_read：解码器每帧从原始 buffer 读，buffer 即 LvglRawImage 持有的 gif_buffer
memcpy(buf, &gif->data[gif->f_rw_p], len);    // gif->data == 旧 LvglRawImage 的 image_dsc_.data
```
- 根因：`LvglRawImage(owns_data=true)` 拥有 gif_buffer；`LvglGif` 构造时 `gd_open_gif_data(data, size)` 只保存 `gif->data = data`（指针，不拷贝），后续每帧 `f_gif_read` 都从该指针读。FontGif 的顺序是"先 ReplaceEmoji 释放旧 raw（连同旧 buffer）→ 再 SetEmotion 停旧解码器"。两步之间，旧 `gif_controller_` 仍指向已 free 的旧 buffer。当前靠"同锁 + 同任务、lv_timer 不会在函数中途抢占"侥幸不崩。
- 触发条件/影响面：连续快速下发两个笔画 GIF（孩子快速翻字 / 网络抖动重发）；或未来把 GIF 解码 timer 改到独立任务后必崩。崩溃表现为读已释放 PSRAM → LoadProhibited / 花屏 / 重启。
- 修复建议：调换 FontGif 内顺序——先 `LcdDisplay::SetEmotion("neutral")` 或显式停掉当前 font GIF（确保 `gif_controller_.reset()` 先于释放旧 buffer），再 `ReplaceEmoji("font", raw)`，最后 `SetEmotion("font")`。即：在 ui_display.cc:496 之前插入"若当前 in_font_mode_ 先停旧 gif_controller_"。根治：让 `LvglGif` 持有对源 `LvglImage` 的 shared_ptr 引用，保证解码器存活期间 buffer 不被释放。
- [发现于第二遍]

### 10-P1-B：gifdec LZW 非缓存分支 key 未校验 < nentries，读未初始化/越界 Table 条目
- 等级：P1（高频崩溃，内存安全红线②）。判级理由：畸形/截断 GIF 资产即可触发，资产可经 OTA（assets::Download）下发，属"外部输入先校验再用"红线。虽有帧缓冲溢出二次防护，但 prefix 链遍历仍可读到未初始化 Entry。
- 文件：`main/display/lvgl_display/gif/gifdec.c:593-618`（`#else` 非 LV_GIF_CACHE_DECODE_DATA 分支）。
- 代码：
```c
key = get_key(gif, key_size, &sub_len, &shift, &byte);
if(key == clear) continue;
if(key == stop || key == 0x1000) break;
if(ret == 1) key_size++;
entry = table->entries[key];          // ← 未校验 key < table->nentries
str_len = entry.length;
if(frm_off + str_len > frm_size){ ... return -1; }   // 仅防帧溢出，不防读越界 Entry
...
entry = table->entries[entry.prefix]; // ← prefix 来自畸形数据，可指向未初始化条目
```
- 根因：标准 LZW 解码要求 `key` 必须 ≤ 当前 `nentries`（已定义的码）。这里只排除了 stop/clear/0x1000，未排除 `key >= table->nentries`。畸形流可让 key 落在 [nentries, bulk) 区间——该区间 Entry 是未初始化内存，`entry.length`/`entry.prefix` 任意，prefix 链可越界读 `table->entries[]` 或死循环。
- 触发条件/影响面：构造畸形 GIF（LZW 码超前引用未定义码）作为 emoji/font 资产 → 读未初始化堆内存，`entry.length` 异常大时虽被帧溢出检查拦下返回 -1，但 prefix 指向 bulk 之外即越界读，可能 LoadProhibited。缓存分支（line 432）已正确做了 `key >= LZW_TABLE_SIZE` 检查，非缓存分支漏了。
- 修复建议：在 `entry = table->entries[key];` 前加 `if (key >= table->nentries) break;`（与缓存分支 line 432 的 `key >= LZW_TABLE_SIZE` 对齐，更严格用 nentries）。
- [发现于第二遍]

### 10-P1-C：assets::Download 明文 HTTP 写资产分区，无完整性/来源校验（数据流红线）
- 等级：P1（高频体验退化 + 安全弱点）。判级理由：资产分区损坏不会砖机（固件可执行分区独立），但损坏/被篡改资产会让字体/emoji/GIF 解析进入上面 P0/P1 越界路径；URL 来源若不可信则等于远程投喂畸形资产。属"OTA 资产先校验"红线的上游缺口。
- 文件：`main/assets.cc:439-573`（Download）+ `131-192`（InitializePartition 仅本地 checksum 校验）。
- 代码：`http->Open("GET", url)` → 边擦边写分区，写完仅靠 `InitializePartition` 里的 16-bit 累加和 `CalculateChecksum`（assets.cc:123-128）自校验。
- 根因：(1) checksum 是 16-bit 简单累加（`& 0xFFFF`），无法防有意篡改，只能挡少量翻转。(2) 无签名验签。(3) `content_length > partition_->size` 有上界检查（行 465），但写入过程中 UnApplyPartition 已先解除 mmap（行 443），中途断电会留下半截资产 + 失效旧资产（下次启动 InitializePartition checksum 失败 → 资产全失效，降级到内置字体，不砖机，可接受）。
- 触发条件/影响面：不可信 URL 或中间人 → 投喂畸形资产 → 触发 10-P0-A/10-P1-B。
- 修复建议：归 OTA 子系统统一处理——资产包加签名/强哈希（SHA256）校验后再 InitializePartition；URL 必须来自已验签的配置通道。本子系统侧：保留并强化 InitializePartition 的边界校验（已有 off/size 越界跳过，见 assets.cc:183，良好）。
- [发现于第二遍]

### 10-P2-C：gifdec gif_open 对 5*w*h 整数溢出有防护，但 GCT/LCT 读取依赖 f_gif_read 截断保护
- 等级：P2（边缘，已有缓解）。判级理由：`gif_open` 已用 `(INT_MAX - sizeof(gd_GIF)) / width / height / 5 == 0` 防 canvas 分配整数溢出（gifdec.c:127-131），且 `f_gif_read` 有越界截断（行 798-802），组合下畸形 GIF 多为"读到 0 + 停在末尾"安全降级。残留风险：palette colors[] 固定 768 字节，gct_sz 最大 256，`3*gct_sz=768` 恰好不越界——边界刚好，无裕量。
- 文件：`main/display/lvgl_display/gif/gifdec.c:140`（`f_gif_read(gif, gif->gct.colors, 3 * gif->gct.size)`）、`652`（LCT 同理）。
- 根因：`gd_Palette.colors[0x100*3]` = 768，`gct_sz = 1 << ((fdsz&0x07)+1)` 最大 256，`3*256=768` 正好填满，无溢出但零裕量；依赖 fdsz 低 3 位天然 ≤7。
- 触发条件/影响面：理论安全，记录为"边界贴边、无防御纵深"。
- 修复建议：可选加一句 `if (gif->gct.size > 256) goto fail;` 防御性断言（即便位运算保证）。优先级低。
- [发现于第二遍]

### 10-P2-D：LvglGif lv_timer 回调访问 emoji_image_，依赖 SetEmotion 同锁停 timer，无独立保护
- 等级：P2（偶发，并发红线③相关）。判级理由：GIF 帧 timer（lv_timer，10ms）回调 `NextFrame` 渲染到 `gif_->canvas`，frame_callback 又 `lv_image_set_src(emoji_image_)`。这些都在 LVGL 任务内串行，且 SetEmotion 重建 gif 时在同一 DisplayLockGuard 内先 Stop 旧 timer，当前安全。但 frame_callback 捕获 `this`（LcdDisplay），若 LcdDisplay 析构未先停 gif timer 则野指针——析构里确有 `gif_controller_->Stop()`（lcd_display.cc:257-260），OK。残留：lv_image_set_src 在 callback 内调用，与 SetEmotion 路径对 emoji_image_ 的操作均在 LVGL 任务，无跨任务，安全。
- 文件：`main/display/lvgl_display/gif/lvgl_gif.cc:62-66`（timer 创建）、`227-230`（frame_callback）；`main/display/lcd_display.cc:1090-1092`（捕获 this 的 callback）。
- 根因：依赖"GIF timer 与所有 emoji 操作都在 LVGL 任务"这一隐式约定，无显式断言/文档。
- 触发条件/影响面：当前架构安全；若 GIF 解码移出 LVGL 任务则崩。
- 修复建议：在 LvglGif 头注释明确"必须在 LVGL 任务内 Start/Stop/析构"；保持现状即可，记录为约束。
- [发现于第二遍]

### 10-P2-E：lvgl_theme.cc ParseColor 对短颜色串越界读 substr
- 等级：P2（偶发越界，内存安全②）。判级理由：颜色串来自 OTA index.json（skin.text_color 等），畸形如 `"#12"` 会让 `substr(3,2)`/`substr(5,2)` 越界。std::string::substr 越界会抛 std::out_of_range 异常——固件多无异常处理 → abort。属外部输入未校验长度。
- 文件：`main/display/lvgl_display/lvgl_theme.cc:6-15`。
- 代码：
```cpp
if (color.find("#") == 0) {
    uint8_t r = strtol(color.substr(1, 2).c_str(), nullptr, 16);
    uint8_t g = strtol(color.substr(3, 2).c_str(), nullptr, 16);  // ← color="#12" 时 pos=3 越界抛异常
    uint8_t b = strtol(color.substr(5, 2).c_str(), nullptr, 16);
}
```
- 根因：只判 `find("#")==0`，未校验 `color.length() >= 7`。substr(pos>size()) 抛 out_of_range。
- 触发条件/影响面：OTA index.json 写入畸形短颜色串 → Apply 解析时 abort 重启。
- 修复建议：函数开头加 `if (color.size() < 7 || color[0] != '#') return lv_color_black();`，再做 substr。
- [发现于第二遍]

---

# 第三遍 · 反审自检（复验 + 对抗视角补漏 + 删误报）

### 复验结论
- 10-P0-A：复验 ui_display.cc:496-497 顺序确为先 ReplaceEmoji 后 SetEmotion；gifdec.c:803 确为指针读源 buffer，LvglGif 构造（lvgl_gif.cc:15）确未拷贝源数据。判级 P0 成立——核心是 buffer 生命周期与解码器引用解耦，靠时序侥幸。保留 P0（高频交互 + 内存安全红线 + 易回归）。
- 10-P1-B：复验缓存分支（gifdec.c:432）有 `key >= LZW_TABLE_SIZE` 检查，非缓存分支（593-597）确无 `key < nentries` 检查。判级 P1 成立（哪个分支编译取决于 `LV_GIF_CACHE_DECODE_DATA`，两分支都要正确）。
- 10-P1-A：复验 circular_strip.cc Blink/Scroll 确在 StartStripTask 之外先写 colors_，StartStripTask（180-190）才加锁。竞态成立。保留 P1。
- 10-P2-E：复验 ParseColor 确无长度校验，substr 越界抛异常成立。

### 删除的误报
- ~~LvglDisplay::UpdateStatusBar battery levels[] 越界~~：lvgl_display.cc:175-177 已有 `level_idx` 钳制到 [0,5]，levels[] 6 项，安全，非缺陷。
- ~~assets.cc asset 表项越界读~~：InitializePartition（assets.cc:183-188）已加 off/size 越界跳过 + strnlen 安全构造 name，处理良好，非缺陷。
- ~~f_gif_read 越界读源 buffer~~：gifdec.c:798-802 已有截断保护（memset 0 + 停末尾），非缺陷。
- ~~SetChatMessage child_count 越界~~：lcd_display.cc:487-503 删除最旧消息逻辑均有 nullptr/valid 检查，非缺陷。
- ~~GpioLed FadeCallback ISR 阻塞~~：gpio_led.cc:197-205 ISR 内只 xTaskNotifyFromISR（非阻塞），实际工作搬到 EventTask（259-266），符合 ISR 禁阻塞红线，非缺陷。

### 对抗视角补漏（畸形资产 / 并发刷屏）

### 10-P2-F：UiDisplay::ShowQrCode 对 qr_content 用 strlen，未限长，超大 QR 内容可能 lv_qrcode 失败或占满堆
- 等级：P2（边缘）。判级理由：qr_content 来自激活/配网 URL，正常 < 200 字节；若被注入超长串，lv_qrcode_update 内部按版本上限会失败（返回错误未检查），最坏分配大缓冲。无直接崩溃但鲁棒性弱。
- 文件：`main/display/ui_display.cc:758`（`lv_qrcode_update(qr, qr_content, strlen(qr_content))`）。
- 根因：未限制 qr_content 长度，未检查 lv_qrcode_update 返回值。
- 触发条件/影响面：异常长 URL → QR 生成失败页面空白，非崩溃。
- 修复建议：调用前 `if (strlen(qr_content) > 512) { ...降级显示文本... }`，并检查 lv_qrcode_update 返回 LV_RESULT_OK。
- [发现于第三遍]

### 10-P3-C：多个 lv_label_set_text 直接用外部 content，依赖 LVGL 内部按字体 cmap 渲染（无显式 UTF-8 校验）
- 等级：P3（潜在）。判级理由：lcd_display.cc/oled_display.cc/ui_display.cc 大量 `lv_label_set_text(label, content)`，content 来自网络（聊天消息）。LVGL label 内部会 strdup 并按解码器逐字符处理，畸形 UTF-8 不越界（LVGL 有自身解码保护），但无长度上限——超长 content 撑大 label 缓冲。MAX_MESSAGES（lcd_display.cc:470-472）限制了气泡数量，单条长度无限。
- 文件：`main/display/lcd_display.cc:546/1018`、`main/display/oled_display.cc:157/162`、`main/display/ui_display.cc:585`。
- 根因：单条聊天消息无长度上限。
- 触发条件/影响面：服务端下发超长字符串（数十 KB）→ label strdup 占内部 RAM，触及 60KB 内部 RAM 红线（见 esp32-memory.md）。
- 修复建议：在 Display 层对 content 截断（如 > 512 字节截断 + 省略号），ui_display.cc 已做换行清洗（531-557）可顺带加长度上限。
- [发现于第三遍]

### 10-P3-D：ControlCenter 6 个按钮事件回调未判 self 为空 / 未判回调是否在对象存活期
- 等级：P3（潜在）。判级理由：control_center.cc:416-510 多个 On*Clicked 直接 `self->Hide()` 等，未判 self==nullptr。lv_event user_data 是 this，只要 ControlCenter 未析构即有效；析构（control_center.cc:46-51）删除 container_ 会连带删子对象与事件，回调不再触发，逻辑上安全。但 OnExitClicked（416-422）未判 self 非空就解引用，与其他回调（如 OnFontExitClicked 有判空）风格不一致。
- 文件：`main/display/ui/widgets/control_center.cc:416-419`。
- 根因：风格不统一，OnExitClicked 缺 `if (!self) return;`。
- 修复建议：所有 On*Clicked 统一加 `auto* self = ...; if (!self) return;`。
- [发现于第三遍]

---

## 统计

| 等级 | 数量 | 编号 |
|------|------|------|
| P0 | 1 | 10-P0-A |
| P1 | 3 | 10-P1-A、10-P1-B、10-P1-C |
| P2 | 6 | 10-P2-A、10-P2-B、10-P2-C、10-P2-D、10-P2-E、10-P2-F |
| P3 | 5 | 10-P3-A、10-P3-B、10-P3-C、10-P3-D、（注：原 10-P3 系列已列全）|
| **合计** | **15** | |

> 修正：P3 实际为 4 个（10-P3-A、10-P3-B、10-P3-C、10-P3-D）。**合计 = 1 + 3 + 6 + 4 = 14。**

### 三遍新增
- 第一遍：6 个（10-P1-A、10-P2-A、10-P2-B、10-P3-A、10-P3-B，及广度发现）→ 实际新增 5 条（P1×1、P2×2、P3×2）
- 第二遍：6 个（10-P0-A、10-P1-B、10-P1-C、10-P2-C、10-P2-D、10-P2-E）
- 第三遍：3 个（10-P2-F、10-P3-C、10-P3-D）+ 删除 5 条误报
- **新增 a + b + c = 5 + 6 + 3 = 14**

### 最高优先处置建议
1. **10-P0-A**（FontGif use-after-free）：调换 ui_display.cc:496-497 顺序（先停旧 GIF 再 ReplaceEmoji），或让 LvglGif 持源图 shared_ptr。出货前必清。
2. **10-P1-B**（GIF LZW key 未校验）：gifdec.c:597 前加 `if (key >= table->nentries) break;`，一行可修。
3. **10-P1-A**（LED colors_ 竞态）：Blink/Scroll 写 colors_ 移入持锁段。
