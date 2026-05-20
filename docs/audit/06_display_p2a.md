# 第二轮补充-渲染/解码核心

> 范围：lcd_display.cc / lvgl_display.cc / lvgl_image.cc / lvgl_font.cc / lvgl_theme.cc / emoji_collection.cc / gif/gifdec.c / gif/lvgl_gif.cc / jpg/jpeg_to_image.c / jpg/image_to_jpeg.cpp / oled_display.cc / emote_display.cc / display.cc
> 已对照第一轮 `06_display_assets.md`（19 项）去重，本文仅记**新发现**。
> 判级口径沿用第一轮：固件资产出厂烧录，但 `Assets::Download()` 支持 OTA 下载新资产分区，GIF/字库/图像/JPEG 解析须按"可能处理半受控数据"对待。

---

## P0 必崩 / 安全漏洞

### P0-1（新）image_to_jpeg 输入转换缓冲尺寸 `int` 溢出 + 无 src_len 校验 → 堆溢出
- **等级**：P0。判级理由：`convert_input_to_encoder_buf` / `convert_input_to_hw_encoder_buf` 全部用 `(int)width * (int)height * N` 计算 `sz`，width/height 为 `uint16_t`。大尺寸（如 4096×4096×2）在 `int` 域溢出为小值/负值 → `jpeg_calloc_align(sz,16)` 分配过小（或 malloc 负数转巨大失败）→ 随后 `memcpy(buf, src, sz)` 按溢出后的 `sz` 拷贝；更严重的是**完全不校验 `src_len >= sz`**，当 `src_len` 与 width/height 不一致（相机分辨率切换、上游传错 format）时 `memcpy`/color_convert 直接越界读 src 或越界写 buf。这是相机抓拍/JPEG 编码的内存安全漏洞，区别于第一轮 P1-5（仅指出 jpeg_to_image.c 解码侧及 image_to_jpeg.cpp:135/139/149 的 RGB src_len 溢出，未覆盖 GRAY/YUYV/UYVY/YUV422P 这几条 `memcpy(buf,src,sz)` 路径与 src_len 缺校验）。
- **文件**：`main/display/lvgl_display/jpg/image_to_jpeg.cpp:43-51,57-65,71-90,219-227,231-242,247-256,261-272`
- **代码**：
```cpp
if (format == V4L2_PIX_FMT_GREY) {
    int sz = (int)width * (int)height;            // int 溢出
    uint8_t* buf = (uint8_t*)jpeg_calloc_align(sz, 16);
    if (!buf) return NULL;
    memcpy(buf, src, sz);                          // 无 src_len>=sz 校验
```
- **根因**：尺寸用 `int` 而非 `size_t` 运算且无上界；函数签名拿不到 `src_len`（只有 width/height/format），无法交叉校验。
- **触发条件与影响面**：`SnapshotToJpeg`（屏幕快照，width/height 来自 draw_buffer 受控）相对安全；但 `image_to_jpeg`/`image_to_jpeg_cb` 是相机帧编码公共入口，分辨率与 format 由上游传入，异常组合即触发。
- **修复建议**：所有 `sz` 用 `(size_t)width*(size_t)height*N` 计算并校验上限（按相机最大分辨率）；给 `convert_input_*` 增加 `src_len` 参数，进入前断言 `src_len >= 期望字节数`，不足则返回 NULL。

---

## P1 高频崩溃 / 体验严重退化

### P1-1（新）`spi_psram_flush_cb` / `s_flush_panel` / `s_te_sem` 为文件级全局，多实例与刷屏回调存在错配/竞争
- **等级**：P1。判级理由：刷屏目标 panel（`s_flush_panel`）、TE 信号量（`s_te_sem`）都是 `static` 文件全局，而 `lv_display_set_flush_cb` 注册的 `spi_psram_flush_cb` 始终向 `s_flush_panel` 推图。若构造第二个 `SpiLcdDisplay`（双屏/重建），`s_flush_panel = panel_`（lcd_display.cc:194）被后者覆盖 → 前一 display 的 flush 把数据画到错误 panel；TE 信号量同理被共享。即使单屏，flush_cb 与 DMA 完成回调 `on_color_trans_done` 跨 LVGL 任务（P5）/SPI ISR 之间靠该全局沟通，析构 `lv_display_delete` 后 `s_flush_panel` 仍指向已 `esp_lcd_panel_del` 的句柄，残留 flush → use-after-free。
- **文件**：`main/display/lcd_display.cc:37,45-58,194,304-313`
- **代码**：
```cpp
static esp_lcd_panel_handle_t s_flush_panel = nullptr;          // 文件全局
static void spi_psram_flush_cb(...) {
    ...
    esp_lcd_panel_draw_bitmap(s_flush_panel, ...);              // 不区分是哪个 display
}
// 构造里：
s_flush_panel = panel_;                                        // 后构造者覆盖
```
- **根因**：把每-display 状态做成文件全局，与 LVGL flush_cb 无法携带 user_data 的写法耦合。
- **触发条件与影响面**：双屏板型 / 显示对象重建（热重配、测试）/ 析构后残留 flush。单屏正常路径不触发，故 P1 而非 P0。
- **修复建议**：在 flush_cb 内用 `lv_display_get_user_data(disp)` 取回 `LcdDisplay*`，从对象成员拿 panel 与 TE 信号量；析构时先注销 flush_cb / 等 DMA 完成再删 panel，并清 `s_flush_panel=nullptr`。

### P1-2（新）`LcdDisplay::SetEmotion` 未判 `emotion==nullptr` 即构造 std::string 查表
- **等级**：P1。判级理由：`GetEmojiImage(emotion)` 内 `emoji_collection_.find(name)` 用 `const char*` 隐式构造 `std::string`，`emotion==nullptr` → `std::string(nullptr)` UB/崩溃；后续 `font_awesome_get_utf8(emotion)`、`strcmp(emotion,"neutral")` 同样解引用空指针。SetEmotion 是高频调用（每次情绪切换/省电模式 `SetPowerSaveMode` 也走它），上游若某分支传 null 即崩。第一轮 P2-3 仅覆盖 `emote_display.cc` 的 role 空指针，未覆盖 `lcd_display.cc::SetEmotion` 的 emotion。
- **文件**：`main/display/lcd_display.cc:1046-1058,1111`
- **代码**：
```cpp
void LcdDisplay::SetEmotion(const char* emotion) {
    ...
    auto image = emoji_collection != nullptr ? emoji_collection->GetEmojiImage(emotion) : nullptr;
    // GetEmojiImage: emoji_collection_.find(name) → std::string(nullptr) UB
```
- **根因**：入参缺空指针防御。
- **触发条件与影响面**：任何传 `emotion=nullptr` 的调用路径。
- **修复建议**：函数开头 `if (emotion == nullptr) emotion = "neutral";` 或直接 return。

### P1-3（新）`LvglGif` 加载/渲染全程无源 GIF 长度，构造时 `gd_render_frame` 在 canvas 上越界写（与第一轮 P0-1 不同环节）
- **等级**：P1。判级理由：第一轮 P0-1 指出 `f_gif_read` 内存路径无上界（读越界）。此处补充**写越界**：`gd_open_gif_data` 成功后，`gif_open` 已据文件内声明的 width/height 分配 `5*w*h` 的 canvas/frame；但 `read_image` 的帧坐标校验只比对 `fx+fw<=width && fy+fh<=height`（gifdec.c:640），**不校验声明的 width/height 与真实可解码数据是否一致**。配合无源长度（P0-1），LZW 解出的像素写入 `gif->frame`，而 `gd_render_frame`→`render_frame_rect`（gifdec.c:672-684）按 `fw*fh` 写 `buffer[(i+k)*4+...]`，`buffer` 即 `img_dsc_.data=gif_->canvas`，其声明大小 `width*height*4`（lvgl_gif.cc:30）。当 canvas 被 LVGL 当 ARGB8888 源渲染、而 GIF 内 width/height 与首帧实际不符时，渲染与解码对同一单缓冲的尺寸假设不一致 → 写/读越界。
- **文件**：`main/display/lvgl_display/gif/lvgl_gif.cc:26-35`，`gif/gifdec.c:660-686,730`
- **代码**：
```cpp
img_dsc_.header.w = gif_->width;
img_dsc_.header.h = gif_->height;
img_dsc_.data_size = gif_->width * gif_->height * 4;   // 与解码用 5*w*h 不同口径
if (gif_->canvas) gd_render_frame(gif_, gif_->canvas);
```
- **根因**：单缓冲 canvas 同时被解码器与 LVGL 用，尺寸校验分散；与 P0-1 同源（无 data_len）但表现为写侧。
- **触发条件与影响面**：损坏/篡改的 OTA emoji GIF。修 P0-1（加 data_len 上界）后此项风险大幅下降，但仍建议独立校验。
- **修复建议**：随 P0-1 给 `gd_GIF` 加 `data_len` 后，在 `gif_open` 读 GCT/首帧时即校验剩余字节足够；`LvglGif` 构造校验 `gif_->width*gif_->height*4` 不溢出 uint32 再赋 `data_size`。

### P1-4（新）`EmojiCollection::AddEmoji` 覆盖同名键时泄漏旧 `LvglImage`
- **等级**：P1。判级理由：`AddEmoji` 用 `emoji_collection_[name] = image` 直接覆盖，若 `name` 已存在则旧指针（`LvglImage*`，内部可能持 PSRAM 大图/cbin/GIF）被直接丢弃，永久泄漏。`ReplaceEmoji` 正确做了 `delete`，但 `AddEmoji` 没有。OTA/动态情绪包（自定义 emoji 通过 assets 反复 `AddEmoji` 同名）场景下每次切换都泄漏一张图，PSRAM 很快耗尽 → 后续解码/分配失败连锁崩溃。
- **文件**：`main/display/lvgl_display/emoji_collection.cc:9-11`
- **代码**：
```cpp
void EmojiCollection::AddEmoji(const std::string& name, LvglImage* image) {
    emoji_collection_[name] = image;   // 同名旧 image 未 delete → 泄漏
}
```
- **根因**：`operator[]` 赋值不释放被覆盖对象。
- **触发条件与影响面**：构造函数中 21 个键均唯一不触发；动态加载/覆盖自定义 emoji 时触发。
- **修复建议**：`AddEmoji` 复用 `ReplaceEmoji` 的逻辑（先 find 并 delete 旧值），或直接让 `AddEmoji` 调 `ReplaceEmoji`。

---

## P2 偶发 / 边缘场景

### P2-1（新）`SpiLcdDisplay` 构造早退留下半构造对象（display_/fb 为空）后续无校验使用
- **等级**：P2。判级理由：PSRAM framebuffer 分配失败（lcd_display.cc:185）或 `lv_display_create` 失败（:197）时构造函数 `return`，但 C++ 构造无法中止，对象仍被创建且 `display_==nullptr`。后续 `SetupUI()`/`SetEmotion()` 多处虽有 `xxx_label_!=nullptr` 防护，但 `EnableTearingEffectSync`、`SetTheme` 里对 `lv_screen_active()`、`lv_display_*` 的调用未必都判 `display_`。低内存启动时偶发空指针。
- **文件**：`main/display/lcd_display.cc:185-190,196-201`
- **根因**：构造失败无显式失败标志，调用方不知对象不可用。
- **触发条件与影响面**：PSRAM 不足/碎片化导致 2×全屏 fb（如 240×320×2≈150KB×2）分配失败。
- **修复建议**：增加 `bool initialized_` 成员，构造失败置 false；所有公开方法开头检查；或工厂函数返回 nullptr。

### P2-2（新）`convert_input_to_encoder_buf` YUV422P 平面指针无 src_len 校验，越界读
- **等级**：P2。判级理由：YUV422P 分支用 `u_plane = src + w*h`、`v_plane = u_plane + (w/2)*h`，逐行读 `y_row/u_row/v_row`，全程不校验 `src_len` 是否覆盖到这些偏移。该分支注释"当前版本暂时不会出现"，但代码已编译可达，若未来格式启用或上游误传即越界读 src。与 P0-1 同属 src_len 缺校验，但此为多平面指针运算，单列。
- **文件**：`main/display/lvgl_display/jpg/image_to_jpeg.cpp:95-125`
- **代码**：
```cpp
const uint8_t* u_plane = y_plane + (int)width * (int)height;
const uint8_t* v_plane = u_plane + ((int)width / 2) * (int)height;
... y_row[x+1]; u_row[x/2]; v_row[x/2];   // 无 src_len 边界
```
- **根因**：平面偏移基于 width/height 推算，未与实际 src_len 交叉验证。
- **触发条件与影响面**：启用 YUV422P/UYVY 输入路径时。
- **修复建议**：进入分支先校验 `src_len >= w*h*2`；偏移用 size_t。

### P2-3（新）`lvgl_theme.cc::ParseColor` 仍未校验长度（第一轮 P2-4 描述 substr(3,2)/(5,2)，此处实际代码为 substr(1,2)/(3,2)/(5,2)，但越界本质相同且未修复）
- **等级**：P2。判级理由：实际代码 `color.substr(1,2)/substr(3,2)/substr(5,2)`，对 `color.size()<7` 的 "#" 串，`substr(pos>size())` 抛 `std::out_of_range`，ESP32 默认无 catch → abort。第一轮已记录此类（P2-4），但行号/调用形态与实文件不符（第一轮写的是 substr(3,2)/(5,2) 两处，实为三处且起始 1），且至今未修。此处更正坐标并确认仍存活，便于定位修复。
- **文件**：`main/display/lvgl_display/lvgl_theme.cc:7-12`
- **代码**：
```cpp
if (color.find("#") == 0) {
    uint8_t r = strtol(color.substr(1, 2).c_str(), nullptr, 16);
    uint8_t g = strtol(color.substr(3, 2).c_str(), nullptr, 16);  // size<5 → 抛异常
    uint8_t b = strtol(color.substr(5, 2).c_str(), nullptr, 16);  // size<7 → 抛异常
```
- **根因**：未校验 `color.size() >= 7`。
- **触发条件与影响面**：损坏/格式错误的 skin index.json 颜色字段（OTA 资产）。
- **修复建议**：开头 `if (color.size() < 7 || color[0] != '#') return lv_color_black();`。

### P2-4（新）`decode_with_new_jpeg` 软件解码同样无 width/height 上界，整数溢出（区别于第一轮 P1-5 的"提及"，此处精确到行确认未修）
- **等级**：P2。判级理由：第一轮 P1-5 已点名 jpeg_to_image.c:56/75-78 软件路径。此处补充确认：`out_info.width/height` 为解码库返回的 `int`，`out_info.width*out_info.height*2`（line 56）在 `int` 域计算，溢出后传给 `jpeg_calloc_align`；line 71 的 `ESP_LOG_BUFFER_HEXDUMP(... MIN(w*h*2,256) ...)` 同样溢出（仅 DEBUG 触发）。降级为 P2 是因为 P1-5 已立项，此处仅补充 HEXDUMP 行与硬件路径 line 158/163 的 `(w+15)&~15` 对齐乘法在 `int` 域同样可溢出这一未被点名的子项。
- **文件**：`main/display/lvgl_display/jpg/jpeg_to_image.c:56,71,158,163`
- **代码**：
```c
out_buf_len = ((header_info.width + 15) & ~15) * ((header_info.height + 15) & ~15) * 2;  // int 溢出
```
- **根因**：尺寸 int 运算无上界。
- **触发条件与影响面**：异常尺寸 JPEG（远程预览图/相机回读）。
- **修复建议**：随 P1-5 统一用 size_t 并校验 `width/height <= 屏幕/PSRAM 上限`。

### P2-5（新）`oled_display.cc::SetChatMessage` 用 `std::string content_str = content` 未判 `content==nullptr`
- **等级**：P2。判级理由：`std::string content_str = content;`（line 153）在 `content==nullptr` 时构造 `std::string(nullptr)` UB/崩溃，发生在 `content==nullptr` 检查（line 159）**之前**。lcd_display 版本先 `lv_label_set_text(...,content)`（LVGL 内部判 null）相对安全，OLED 版本则提前解引用。
- **文件**：`main/display/oled_display.cc:152-159`
- **代码**：
```cpp
std::string content_str = content;                       // content==nullptr → UB
std::replace(content_str.begin(), content_str.end(), '\n', ' ');
if (content_right_ == nullptr) { ... }
else { if (content == nullptr || content[0] == '\0') {   // 检查太晚
```
- **根因**：先构造 string 后才判空。
- **触发条件与影响面**：OLED 板型收到 `content=nullptr` 的聊天消息。
- **修复建议**：函数开头 `if (content == nullptr) content = "";`。

---

## P3 潜在远期风险

### P3-1（新）PSRAM framebuffer 直接作为 SPI DMA 源（ESP32 内存红线 [待确认]）
- **等级**：P3。判级理由：项目内存规范明确"DMA 缓冲必须用内部 RAM（PSRAM 不可 DMA 直接访问）"，但 `fb1/fb2` 以 `MALLOC_CAP_SPIRAM` 分配并直接传给 `esp_lcd_panel_draw_bitmap`（SPI DMA）。在 ESP32-S3 上 SPI master 支持通过 cache 从 PSRAM DMA，故功能可行；但需确认目标 SoC 与 esp_lcd 配置（GDMA + PSRAM cache 对齐 64B 已做）确实支持，否则在不支持的 SoC 上 DMA 取到错误数据/花屏。[待确认] 具体 SoC 与 esp_lcd 是否启用 PSRAM-DMA 通道。
- **文件**：`main/display/lcd_display.cc:182-184,54-56`
- **根因**：与项目"DMA 用内部 RAM"红线表面冲突，依赖 S3 PSRAM-DMA 特性。
- **触发条件与影响面**：移植到不支持 PSRAM-DMA 的 SoC 时花屏/数据错误。
- **修复建议**：确认 SoC=S3 且 esp_lcd SPI 已配 PSRAM 源；在注释中标明依赖（4KB+ 大分配应注明用途/生命周期/申请方，当前注释已说明，建议补"依赖 S3 PSRAM-DMA"）。

### P3-2（新）`LvglGif::img_dsc_.data_size = width*height*4` 在 `int` 域计算，极端尺寸溢出
- **等级**：P3。判级理由：`gif_->width * gif_->height * 4`，两 uint16 相乘提升为 `int`，再 *4。虽 `gif_open` 的 `(INT_MAX-...)/w/h/5==0` 检查已把 `w*h` 限制在约 4.29 亿以内（*4≈17 亿仍 < UINT32），当前不会真正溢出；但口径（解码用 5*w*h，描述符用 4*w*h）分散，属脆弱点。
- **文件**：`main/display/lvgl_display/gif/lvgl_gif.cc:30`
- **修复建议**：`img_dsc_.data_size = (uint32_t)gif_->width * gif_->height * 4;` 显式提升，并复用 gif_open 的尺寸上界常量。

### P3-3（新）`emote_display.cc::EmoteDisplay` 构造未判 `emote_handle_` 失败即注册 IO 回调
- **等级**：P3。判级理由：`InitializeEmote` 可能返回 nullptr（panel 无效/emote_init 失败），构造函数随后仍 `esp_lcd_panel_io_register_event_callbacks(panel_io, &cbs, emote_handle_)`，把 `user_ctx=nullptr` 注册。回调 `OnFlushIoReady` 已判 `if(handle)`，故不崩，但 emote 全程不可用而对象看似构造成功，调用方无从感知。
- **文件**：`main/display/emote_display.cc:118-127`
- **修复建议**：构造检查 `emote_handle_`，失败置错误标志或日志告警并跳过回调注册。

### P3-4（新）`InitializeEmote` 的 `buf_pixels = width*16`、`gfx_emote` 尺寸用 `int` 运算
- **等级**：P3。判级理由：`static_cast<size_t>(width * 16)` 先在 `int` 域算 `width*16` 再转 size_t，width 为屏宽（受控小值）不会溢出，属代码风格脆弱（与其余 int 溢出同类）。
- **文件**：`main/display/emote_display.cc:93`
- **修复建议**：`static_cast<size_t>(width) * 16`。

### P3-5（新）`LvglGif::NextFrame` 循环检测依赖 `gif_->f_rw_p` 仅内存模式有效，文件模式语义错位
- **等级**：P3。判级理由：`pos_before = gif_->f_rw_p` 并比较 `gif_->f_rw_p < pos_before` 判断 loop（lvgl_gif.cc:198,214）。`f_rw_p` 只在内存模式（`gd_open_gif_data`）由 `f_gif_seek` 维护；若改走文件模式（`gd_open_gif_file`），`f_rw_p` 不更新，loop-delay 检测永久失效。当前 emoji 走内存模式，故仅远期/移植风险。
- **文件**：`main/display/lvgl_display/gif/lvgl_gif.cc:198,214`
- **修复建议**：循环检测改用 `gd_get_frame` 的返回语义或解码器统一暴露的"已 rewind"标志，不直接读 `f_rw_p`。

---

## 统计

| 等级 | 数量 |
|------|------|
| P0   | 1    |
| P1   | 4    |
| P2   | 5    |
| P3   | 5    |
| **合计** | **15** |

**优先修复**：P0-1（image_to_jpeg 尺寸 int 溢出 + 无 src_len 校验，相机编码可触发堆溢出）、P1-1（flush_cb 全局 panel 错配/UAF）、P1-2（SetEmotion 空指针）、P1-4（AddEmoji 同名泄漏）。其余多与 OTA 半受控资产或多实例/移植相关，按窗口排期。
