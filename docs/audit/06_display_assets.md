# 06 显示 / LVGL / 资产子系统 缺陷审计报告

## 子系统概述

审计范围覆盖 `main/display/`（39 文件）、`main/led/`（7 文件）以及 `main/assets.cc` / `main/assets.h` 的资产加载逻辑（不含 `main/assets/` 下的生成数据）。

该子系统职责：
- **LCD/OLED 显示驱动**（`lcd_display.cc` / `oled_display.cc`）：自建 LVGL display、PSRAM 双 framebuffer、SPI DMA 刷屏、TE 硬件同步。
- **LVGL UI 层**（`ui_display.cc` 1442 行，时钟/聊天/播放器/番茄钟/二维码/控制中心多场景互斥）。
- **资产加载**（`assets.cc`）：mmap flash 分区 → 校验 → 字库/图像/emoji 索引。
- **解码器**：GIF（`gifdec.c` + `lvgl_gif.cc`）、JPEG 编/解码、cbin 字库、bin 图像（`ui_image_manager.cc`）。
- **emoji 集合**（`emoji_collection.cc`）、主题（`lvgl_theme.cc`）。
- **LED 指示**（`single_led` / `circular_strip` / `gpio_led`）。

整体架构成熟，已注意到 LVGL 锁（`DisplayLockGuard`）、并发 token 去重（FontGif）等。但 GIF/资产解析对**不可信/损坏数据**的边界检查薄弱，LED 存在越界与任务泄漏，部分内存释放路径有泄漏/竞争隐患。

判级口径：固件资产在出厂烧录，正常不可信外部输入；但 `Assets::Download()` 支持 **OTA 远程下载新资产分区**，因此 GIF/字库/图像解析须按"可能处理半受控数据"对待 —— 这是多个 P0/P1 判级的关键依据。

---

## P0 必崩 / 安全漏洞

### P0-1 GIF 内存解码无源缓冲区上界，损坏/截断 GIF 触发越界读
- **等级**：P0。判级理由：`gd_open_gif_data` 走的内存路径完全不记录源数据总长度，解析中任何 `f_gif_read`/`f_gif_seek` 都不做上界检查。损坏或截断的 GIF（OTA 下载的 emoji 资产、FontGif 推送的 GIF）会越过 buffer 持续 `memcpy`，读到无效地址 → LoadProhibited 崩溃，且属可被资产内容触发的内存安全漏洞。
- **文件**：`main/display/lvgl_display/gif/gifdec.c:789-798`
- **代码**：
```c
static void f_gif_read(gd_GIF * gif, void * buf, size_t len) {
    if(gif->is_file) {
        lv_fs_read(&gif->fd, buf, len, NULL);
    } else {
        memcpy(buf, &gif->data[gif->f_rw_p], len);  // 无任何 f_rw_p+len 上界校验
        gif->f_rw_p += len;
    }
}
```
- **根因**：内存模式 `gd_GIF` 结构里没有 `data_size` 字段（见 `gifdec.h:27-52`），`f_gif_open(is_file=false)` 只存 `gif->data = path`。`gct.colors` 读 `3*gct_sz`、LZW 子块、帧数据等全部基于文件内声明的长度直接 `memcpy`，攻击者/坏数据可声明任意大小。
- **触发条件与影响面**：任何经 `LvglRawImage`→`LvglGif` 解码的 GIF（emoji "font" GIF 经 `UiDisplay::FontGif`、`assets.cc` emoji_collection），只要数据损坏/截断/被篡改即崩溃。OTA 下载场景下风险最高。
- **修复建议**：在 `gd_GIF` 增加 `size_t data_len`，`gd_open_gif_data` 改签名接收长度（调用方 `LvglGif` 已持 `image_dsc_.data_size`，可传入）；`f_gif_read`/`f_gif_seek` 内存分支对 `f_rw_p + len > data_len` 直接置错误标志并中止解码（返回全 0 或安全失败）。

### P0-2 `CircularStrip::SetSingleColor` 索引越界写
- **等级**：P0。判级理由：`index` 为外部传入的 `uint8_t`，函数内既写 `colors_[index]`（std::vector）又调 `led_strip_set_pixel(led_strip_, index, ...)`，两处都无 `index < max_leds_` 检查。`max_leds_` 可能仅为个位数；越界写 vector 是堆破坏 → 必崩或随机损坏。
- **文件**：`main/led/circular_strip.cc:62-68`
- **代码**：
```c
void CircularStrip::SetSingleColor(uint8_t index, StripColor color) {
    std::lock_guard<std::mutex> lock(mutex_);
    esp_timer_stop(strip_timer_);
    colors_[index] = color;                      // 越界堆写
    led_strip_set_pixel(led_strip_, index, ...); // 驱动层越界
    led_strip_refresh(led_strip_);
}
```
- **根因**：缺少入参边界校验；`colors_` 大小为 `max_leds_`。
- **触发条件与影响面**：任何调用方传入 `index >= max_leds_`（如灯效逻辑误用或配置不一致）即触发。
- **修复建议**：函数开头加 `if (index >= max_leds_) return;`，同样保护 vector 与驱动调用。

---

## P1 高频崩溃 / 体验严重退化

### P1-1 `GpioLed` 析构未删除常驻 EventTask → 悬挂任务 use-after-free
- **等级**：P1。判级理由：构造函数 `xTaskCreate(EventTask,...)` 创建一个 `while(1) ulTaskNotifyTake(portMAX_DELAY)` 常驻任务，持有 `this`。析构函数只 `esp_timer_stop` + `ledc_fade_*`，**从不 vTaskDelete `event_task_handle_`**。对象销毁后该任务仍在，且 fade 完成 ISR `FadeCallback` 仍可 `xTaskNotifyFromISR(led->event_task_handle_,...)` → 任务被唤醒访问已释放的 `this->OnFadeEnd()`。
- **文件**：`main/led/gpio_led.cc:91-97`（析构），`gpio_led.cc:85-86`、`259-266`（任务）
- **代码**：
```c
GpioLed::~GpioLed() {
    esp_timer_stop(blink_timer_);
    if (ledc_initialized_) { ledc_fade_stop(...); ledc_fade_func_uninstall(); }
    // ❌ 未 vTaskDelete(event_task_handle_)，未 esp_timer_delete(blink_timer_)
}
```
- **根因**：任务/定时器生命周期未与对象绑定。
- **触发条件与影响面**：LED 对象析构（板级重配/热插拔/测试路径）后 fade 仍在跑即 UAF。也存在 `blink_timer_` 未 `esp_timer_delete` 的句柄泄漏（`CircularStrip::~` 同样只 stop 不 delete，见 `circular_strip.cc:44-49`）。
- **修复建议**：析构中先停 fade，再 `vTaskDelete(event_task_handle_)`（或用退出通知让任务自行 return 后 join），并 `esp_timer_delete(blink_timer_)`；`ledc_cb_register` 注销回调。CircularStrip 同补 `esp_timer_delete(strip_timer_)`。

### P1-2 `Assets::GetAssetData` 读 magic 前未校验 size，且资产表项偏移/大小无分区边界校验
- **等级**：P1。判级理由：`GetAssetData` 取 `data[0]/data[1]` 校验 'Z''Z' 前，未确认 `asset.size >= 2` 且 `offset+2` 在 mmap 范围内；`InitializePartition` 构建 `assets_` 时对每个 `item->asset_offset` / `asset_size` 不做分区上界校验。损坏/恶意（OTA）资产表可让 `offset` 指向 mmap 区外 → 越界读崩溃；后续把越界 `ptr/size` 交给字库/图像/GIF 解码进一步放大。
- **文件**：`main/assets.cc:198-212`（GetAssetData），`assets.cc:176-183`（表项构建）
- **代码**：
```c
auto data = (const char*)(mmap_root_ + asset->second.offset);
if (data[0] != 'Z' || data[1] != 'Z') { ... }     // offset 未校验是否落在 mmap 内
ptr = (void*)(data + 2);
size = asset->second.size;                          // size 未校验是否 offset+size<=partition
```
```c
auto item = (const mmap_assets_table*)(mmap_root_ + 12 + i * sizeof(mmap_assets_table));
auto asset = Asset{ .size = item->asset_size,
    .offset = 12 + sizeof(mmap_assets_table)*stored_files + item->asset_offset };
assets_[item->asset_name] = asset;                  // 无 offset/size 越界过滤
```
- **根因**：仅做了整体 checksum，但单条目偏移/长度未对 `partition_->size` 做范围验证；`asset_name[32]` 也未保证 NUL 结尾（`std::map<std::string,...>` 用 `item->asset_name` 构造可能读越界，若 name 字段填满 32 字节无终止符）。
- **触发条件与影响面**：损坏的 assets 分区 / OTA 下载未对齐数据 / 字段被篡改。checksum 通过但表项指针错误时即触发。
- **修复建议**：构建表项时校验 `offset + size <= partition_->size` 否则跳过该条；`GetAssetData` 中加 `if (asset->second.size < 2) return false;` 并校验 `offset+size` 上界；`asset_name` 用 `strnlen(item->asset_name,32)` 安全构造 string。

### P1-3 `UpdateStatusBar` 电量图标数组索引可越界
- **等级**：P1。判级理由：`levels[]` 6 元素（下标 0–5），索引 `battery_level / 20`。`battery_level` 为 board 返回的 `int`，若 >100（异常 ADC/校准）则 `>=6` 越界读 `const char*` → 解引用野指针给 `lv_label_set_text` → 崩溃。在状态栏每秒/每 tick 调用，命中即高频。
- **文件**：`main/display/lvgl_display/lvgl_display.cc:168-176`
- **代码**：
```c
const char* levels[] = { ...6 项... };
icon = levels[battery_level / 20];   // battery_level>100 → 越界
```
- **根因**：未 clamp `battery_level` 到 [0,100]。
- **触发条件与影响面**：电量读数异常（充电瞬态、ADC 抖动、未初始化）即可 >100。
- **修复建议**：`int idx = battery_level/20; if(idx<0)idx=0; if(idx>5)idx=5; icon=levels[idx];` 或先 `battery_level = std::clamp(battery_level,0,100)`。

### P1-4 `LvglRawImage::IsGif()` 不校验数据指针/长度即读前 3 字节
- **等级**：P1。判级理由：`IsGif` 无条件读 `ptr[0..2]`，未判 `image_dsc_.data == nullptr` 或 `data_size >= 3`。`SetEmotion` 路径对每个 emoji image 调用，若资产 size<3 或 data 为空（损坏资产、分配失败构造的 raw image）→ 越界/空指针读。
- **文件**：`main/display/lvgl_display/lvgl_image.cc:30-33`
- **代码**：
```c
bool LvglRawImage::IsGif() const {
    auto ptr = (const uint8_t*)image_dsc_.data;
    return ptr[0]=='G' && ptr[1]=='I' && ptr[2]=='F';   // 无 ptr/ size 校验
}
```
- **根因**：缺少防御性校验。
- **触发条件与影响面**：与 P1-2 资产越界叠加触发；OTA 资产 size<3 时直接命中。
- **修复建议**：`if (!image_dsc_.data || image_dsc_.data_size < 3) return false;` 再比较。

### P1-5 JPEG 输出缓冲区尺寸计算整数溢出
- **等级**：P1。判级理由：软件解码 `out_buf = jpeg_calloc_align(out_info.width * out_info.height * 2, 16)` 中 `width`/`height` 来自 JPEG 头，乘法在 `int` 域进行后才隐式转 size_t；大尺寸头（如 0xFFFF×0xFFFF）溢出为小值 → 分配过小缓冲，`jpeg_dec_process` 写入时堆溢出。`*out_len`/`*stride` 同样溢出。
- **文件**：`main/display/lvgl_display/jpg/jpeg_to_image.c:56,75-78`（软件路径）；硬件路径 `:158-163` 同类风险。
- **代码**：
```c
out_buf = jpeg_calloc_align(out_info.width * out_info.height * 2, 16);  // int 溢出
...
*out_len = (size_t)(out_info.width * out_info.height * 2);
```
- **根因**：未在乘法前对 width/height 上界校验，未用 size_t 运算并检查溢出。
- **触发条件与影响面**：相机/远程图片解码（`SnapshotToJpeg` 反向、预览图）遇到异常尺寸 JPEG。`image_to_jpeg.cpp` 的 `width*height*N` 系列（如 `:135,139,149`）也以 int 计算，同类隐患。
- **修复建议**：解析头后校验 `width<=MAX && height<=MAX`（按屏分辨率/PSRAM 设上限），用 `(size_t)w*(size_t)h*2` 运算并判溢出后再分配。

### P1-6 `ui_image_manager.cc` 解析 bin 图像时 RGB 数据未校验源长度即越界读
- **等级**：P1。判级理由：ARGB 转换循环中 alpha 读有 `i+rgb_size < src_size` 保护，但 RGB565 读 `rgb_data[i*2]/[i*2+1]` 完全无保护。若资产被截断（`src_size < w*h*2`）即越界读 mmap 区外。
- **文件**：`main/display/ui/resources/ui_image_manager.cc:145-147`
- **代码**：
```c
for (uint32_t i=0;i<pixel_count;i++){
    uint16_t rgb565 = rgb_data[i*2] | (rgb_data[i*2+1]<<8);  // 无 src_size 校验
    uint8_t alpha = (i+rgb_size < src_size) ? alpha_data[i] : 0xFF;  // 仅 alpha 有保护
```
- **根因**：未先校验 `rgb_size + pixel_count <= src_size`（即 `w*h*3 <= src_size`）。
- **触发条件与影响面**：损坏/截断的 UI 图标资产（OTA）。
- **修复建议**：转换前 `if ((size_t)w*h*3 > src_size) { lv_free(dsc); return nullptr; }`。

---

## P2 偶发 / 边缘场景

### P2-1 LED 灯效 lambda 使用 `static` 局部变量，跨实例/跨状态污染
- **等级**：P2。判级理由：`Blink`/`Breathe`/`Scroll` 的回调里用 `static bool on`/`static StripColor color`/`static int offset`。这些是函数级 static，被所有 CircularStrip 实例及历次状态切换共享，状态不随新灯效复位 → 颜色/相位错乱（如 Breathe 残留上次 increase 方向、Scroll offset 不归零）。多实例时还有数据竞争。
- **文件**：`main/led/circular_strip.cc:86,122-123,163`
- **代码**：`static bool on = true;` / `static StripColor color = low;` / `static int offset = 0;`
- **根因**：用 static 当成员状态。
- **触发条件与影响面**：连续状态切换或多 LED 实例时灯效不正确（非崩溃）。
- **修复建议**：改为类成员变量并在 `StartStripTask` 启动时复位；或用 capture-by-value 的可变 lambda 状态封进结构体。

### P2-2 GIF 帧回调与显示刷新对 canvas 的并发读写
- **等级**：P2。判级理由：`LvglGif::NextFrame`（LVGL timer，P5 任务）`gd_render_frame` 写 `gif_->canvas`，随后 `frame_callback_` 把 `emoji_image_` src 指向同一 canvas，LVGL flush 任务异步读取该 canvas 渲染。渲染期间下一帧又开始写 canvas → 撕裂/读到半帧。`LvglGif` 与 SetEmotion 都在 LVGL 锁/同任务内多数情况下安全，但 PARTIAL+双 buf 异步 flush 路径下 canvas 单缓冲无双缓冲保护。
- **文件**：`main/display/lvgl_display/gif/lvgl_gif.cc:223-231`，`gifdec.c:142`（canvas 单缓冲）
- **根因**：GIF canvas 单缓冲，渲染与下一帧解码无同步。
- **触发条件与影响面**：高帧率 GIF + 慢 SPI flush 时偶发画面撕裂（体验问题，非崩溃）。
- **修复建议**：帧回调改为把渲染好的帧拷贝到独立 buffer 后再 set src，或在 set src 与 NextFrame 之间复用 LVGL 的 invalidate 完成同步。

### P2-3 `emote_display.cc` 多处未判 `role`/参数为空即 strcmp/strstr
- **等级**：P2。判级理由：`SetChatMessage` 先 `if (... content ...)` 再 `std::strcmp(role,"system")`，但未判 `role!=nullptr`；上游若传 `role=nullptr`（部分调用点可能）→ strcmp 解引用空指针崩溃。
- **文件**：`main/display/emote_display.cc:145-159`
- **代码**：`if ((std::strcmp(role, "system") == 0) && std::strstr(content, "xiaozhi.me"))`
- **根因**：仅校验 content 未校验 role。
- **触发条件与影响面**：role 为空的消息路径（边缘）。
- **修复建议**：`if (role && std::strcmp(role,"system")==0 && ...)`。

### P2-4 `lvgl_theme.cc::ParseColor` 对短字符串 substr 越界/异常
- **等级**：P2。判级理由：传入以 '#' 开头但长度 <7 的字符串（如 "#12"），`color.substr(3,2)` / `substr(5,2)` 抛 `std::out_of_range`（substr pos 越界）→ 未捕获异常在 ESP32 上等于 abort。颜色来自 `index.json`（资产，可 OTA）。
- **文件**：`main/display/lvgl_display/lvgl_theme.cc:6-14`
- **代码**：
```c
uint8_t g = strtol(color.substr(3, 2).c_str(), nullptr, 16);
uint8_t b = strtol(color.substr(5, 2).c_str(), nullptr, 16);  // 长度<7 抛异常
```
- **根因**：未校验 `color.size() >= 7`。
- **触发条件与影响面**：损坏/格式错误的 skin 颜色字段。
- **修复建议**：开头 `if (color.size() < 7 || color[0] != '#') return lv_color_black();`。

### P2-5 `lv_display_set_buffers` 传入 `fb_bytes` 而非对齐后的 `fb_alloc`
- **等级**：P2。判级理由：framebuffer 按 64B 向上取整分配 `fb_alloc`，但 `lv_display_set_buffers(display_, fb1, fb2, fb_bytes, ...)` 传未对齐的 `fb_bytes` 作为 buffer 大小。LVGL 内部按该值计算可渲染像素与刷新区，与实际分配虽不致越界（fb_bytes<=fb_alloc），但若 LVGL 对 cache 行对齐有内部假设，部分刷新边界可能错位；属潜在显示瑕疵。
- **文件**：`main/display/lcd_display.cc:204`
- **代码**：`lv_display_set_buffers(display_, fb1, fb2, fb_bytes, LV_DISPLAY_RENDER_MODE_PARTIAL);`
- **根因**：对齐值与 LVGL buffer size 参数不一致（此处偏保守，影响小）。
- **触发条件与影响面**：极端 cache invalidate 边界，偶发刷新瑕疵。
- **修复建议**：确认 LVGL 期望的是真实像素字节数（fb_bytes 正确）即可忽略；若需 cache 对齐刷新，统一用全屏像素字节并保证 area 对齐。[待确认] LVGL 9 对该参数语义。

### P2-6 `preview_timer` 回调在非 WeChat 模式有效、WeChat 模式形同虚设
- **等级**：P2。判级理由：构造里 `preview_timer_` 回调为 `SetPreviewImage(nullptr)`。WeChat 风格的 `SetPreviewImage`（`lcd_display.cc:671`）在 `image==nullptr` 时直接 `return`，既不隐藏也不清理 → 该模式下预览图不会自动消失（且 `esp_timer_start_once` 从未在此分支被调用，timer 实际不触发，与非 WeChat 行为不一致），逻辑割裂易引发"预览图常驻"体验问题。
- **文件**：`main/display/lcd_display.cc:118-128`（构造），`:671-679`（WeChat 分支）
- **根因**：两套 `SetPreviewImage` 与同一 timer 语义不统一。
- **触发条件与影响面**：WeChat 消息风格构建下的图片预览。
- **修复建议**：统一预览图生命周期管理，或在 WeChat 分支也实现 nullptr→清理；移除无效 timer 依赖。

---

## P3 潜在远期风险

### P3-1 `ShowNotification` / `esp_timer_start_once` 的 `duration_ms*1000` 整数溢出
- **等级**：P3。判级理由：`duration_ms`（int）乘 1000 转 us，>~2147s 溢出为负 → `esp_timer_start_once` 行为异常。正常调用 ≤几秒，远期风险。
- **文件**：`main/display/lvgl_display/lvgl_display.cc:117`
- **代码**：`esp_timer_start_once(notification_timer_, duration_ms * 1000);`
- **修复建议**：`(uint64_t)duration_ms * 1000`。

### P3-2 `lvgl_image.cc` 大块 PSRAM/堆分配失败抛 C++ 异常（LvglAllocatedImage）
- **等级**：P3。判级理由：`LvglAllocatedImage` 构造在 `lv_image_decoder_get_info` 失败时 `throw std::runtime_error`。嵌入式默认常关异常或无人 catch → abort。当前调用面有限，远期风险。
- **文件**：`main/display/lvgl_display/lvgl_image.cc:50-53`
- **修复建议**：改为工厂函数返回 nullptr，或确保所有构造点 try/catch。

### P3-3 cbin 字库 `fallback` 链指向 RAM proxy，多字库 fallback 链存在循环可能
- **等级**：P3。判级理由：`EnsureDisplayFonts` 将 `edu_main_font_->fallback = clock_text_font_`、`clock_text_font_->fallback = &g_text_font`，而 `g_text_font.fallback = fallback_text_font_`。链较长，若未来某字库 fallback 误配成已在链上的字库 → LVGL 取字时无限递归栈溢出。当前配置无环，属配置脆弱性。
- **文件**：`main/display/ui_display.cc:269,278-283`、`:180`
- **修复建议**：集中管理 fallback 链并加环检测/深度上限。

### P3-4 `assets.cc::Download` 全程无超时 / 单调进度依赖、且 `content_length`(size_t) 与 `partition size`(uint32) 比较隐式转换
- **等级**：P3。判级理由：OTA 下载循环 `http->Read` 无显式超时退出，弱网下可能长阻塞；进度判断 `total_written == content_length` 若服务器多发会死循环（虽 ret==0 退出兜底）。属健壮性。
- **文件**：`main/assets.cc:480-542`
- **修复建议**：给 Read 设超时与最大重试；`total_written > content_length` 时主动报错退出。

### P3-5 `SetEmotion` GIF 创建失败/资产为空时 `emoji_image_` 残留旧 src
- **等级**：P3。判级理由：`IsGif()` 为真但 `LvglGif` 加载失败时仅 log + reset，未恢复 `emoji_label_`/隐藏 `emoji_image_`，可能停留在上一帧或空图。体验问题。
- **文件**：`main/display/lcd_display.cc:1098-1101`
- **修复建议**：加载失败时回退到 label 文本或 neutral 表情并正确切换可见性。

---

## 统计

| 等级 | 数量 |
|------|------|
| P0   | 2    |
| P1   | 6    |
| P2   | 6    |
| P3   | 5    |
| **合计** | **19** |

重点关注顺序：P0-1（GIF 越界读，OTA 资产可触发）、P0-2（LED 越界写）、P1-1（LED 任务 UAF）、P1-2（资产边界）、P1-5（JPEG 整数溢出）。这些与"可被资产内容/异常输入触发的内存安全"直接相关，建议优先修复。
