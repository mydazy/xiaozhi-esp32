#include "ui_image_manager.h"
#include "ui_img_paths.h"
#include "assets.h"
#include <esp_log.h>
#include <cstring>

#define TAG "UiImageMgr"

// 所有需要加载的图片文件名列表
static const char* IMAGE_FILES[] = {
    IMG_FILE_APP_CHAT,
    IMG_FILE_APP_TIME_ROOM,
    IMG_FILE_APP_PHOTO,
    IMG_FILE_APP_LOGO,
    IMG_FILE_APP_POMODORO,
    IMG_FILE_APP_ALARM,
    IMG_FILE_APP_CLOCK,
    IMG_FILE_APP_TODO,
    IMG_FILE_ICON_BACK,
    IMG_FILE_ICON_POMODORO_START,
    IMG_FILE_ICON_POMODORO_STOP,
    IMG_FILE_ICON_CHECKBOX,
    IMG_FILE_ICON_CHECKBOX_CHECKED,
    IMG_FILE_BUTTON_LEFT,
    IMG_FILE_BUTTON_RIGHT,
    // 状态栏电池/信号图标已迁至 FontAwesome 字体（见 ui_display.cc · CreateGlobalStatusBar）
};

static constexpr int IMAGE_COUNT = sizeof(IMAGE_FILES) / sizeof(IMAGE_FILES[0]);

// LVGL v9 bin 文件头格式 (12 字节, little-endian)
struct LvglBinHeader {
    uint8_t magic;      // 0x19
    uint8_t cf;         // color format
    uint16_t flags;
    uint16_t w;         // width
    uint16_t h;         // height
    uint16_t stride;    // bytes per row
    uint16_t reserved;
} __attribute__((packed));

static_assert(sizeof(LvglBinHeader) == 12, "LvglBinHeader must be 12 bytes");

UiImageManager& UiImageManager::GetInstance() {
    static UiImageManager instance;
    return instance;
}

UiImageManager::~UiImageManager() {
    FreeAll();
}

void UiImageManager::LoadAll() {
    if (!images_.empty()) return;  // 已加载

    ESP_LOGI(TAG, "从 assets 分区加载 %d 个图片...", IMAGE_COUNT);
    int success = 0;

    for (int i = 0; i < IMAGE_COUNT; i++) {
        const char* name = IMAGE_FILES[i];
        void* data = nullptr;
        size_t size = 0;

        if (!Assets::GetInstance().GetAssetData(name, data, size)) {
            ESP_LOGW(TAG, "图片未找到: %s", name);
            continue;
        }

        lv_image_dsc_t* dsc = ParseBinImage(data, size);
        if (dsc) {
            images_[name] = dsc;
            success++;
        } else {
            ESP_LOGW(TAG, "图片解析失败: %s", name);
        }
    }

    ESP_LOGI(TAG, "图片加载完成: %d/%d", success, IMAGE_COUNT);
}

const lv_image_dsc_t* UiImageManager::Get(const char* name) const {
    auto it = images_.find(name);
    if (it != images_.end()) {
        return it->second;
    }
    return nullptr;
}

void UiImageManager::FreeAll() {
    for (auto& pair : images_) {
        if (pair.second) {
            // 如果 data 是转换后分配的（ARGB8888），也需要释放
            if (pair.second->header.cf == LV_COLOR_FORMAT_ARGB8888 && pair.second->data) {
                lv_free(const_cast<uint8_t*>(pair.second->data));
            }
            lv_free(pair.second);
        }
    }
    images_.clear();
    ESP_LOGI(TAG, "图片资源已释放");
}

lv_image_dsc_t* UiImageManager::ParseBinImage(void* data, size_t size) {
    if (!data || size < sizeof(LvglBinHeader)) {
        return nullptr;
    }

    const auto* header = static_cast<const LvglBinHeader*>(data);

    // 验证 magic
    if (header->magic != 0x19) {
        ESP_LOGW(TAG, "无效的 bin magic: 0x%02x", header->magic);
        return nullptr;
    }

    const uint8_t* src = static_cast<const uint8_t*>(data) + sizeof(LvglBinHeader);
    size_t src_size = size - sizeof(LvglBinHeader);
    uint16_t w = header->w;
    uint16_t h = header->h;

    auto* dsc = static_cast<lv_image_dsc_t*>(lv_malloc(sizeof(lv_image_dsc_t)));
    if (!dsc) return nullptr;
    memset(dsc, 0, sizeof(lv_image_dsc_t));

    dsc->header.magic = LV_IMAGE_HEADER_MAGIC;
    dsc->header.w = w;
    dsc->header.h = h;

    // bin 文件 cf=0x0B 实际是 RGB565+A8 交错格式（每像素 3 字节: 2B color + 1B alpha）
    // 由 convert_c_to_lvgl_bin.py 生成，与标准 LVGL cf 枚举不一致
    // 转换为 ARGB8888 以确保跨平台渲染正确
    if (header->cf == 0x0B && header->stride > 0 && w > 0 &&
        header->stride / w == 3 && header->stride % w == 0) {
        // RGB565A8 分离平面格式 → ARGB8888
        // 前 w*h*2 字节: RGB565 颜色数据（小端序）
        // 后 w*h*1 字节: Alpha 通道数据
        uint32_t pixel_count = (uint32_t)w * h;
        uint32_t rgb_size = pixel_count * 2;
        uint32_t argb_size = pixel_count * 4;
        const uint8_t* rgb_data = src;
        const uint8_t* alpha_data = src + rgb_size;

        uint8_t* argb_buf = static_cast<uint8_t*>(lv_malloc(argb_size));
        if (!argb_buf) { lv_free(dsc); return nullptr; }

        for (uint32_t i = 0; i < pixel_count; i++) {
            uint16_t rgb565 = rgb_data[i * 2] | (rgb_data[i * 2 + 1] << 8);
            uint8_t alpha = (i + rgb_size < src_size) ? alpha_data[i] : 0xFF;
            // RGB565 → RGB888
            uint8_t r = ((rgb565 >> 11) & 0x1F) * 255 / 31;
            uint8_t g = ((rgb565 >> 5)  & 0x3F) * 255 / 63;
            uint8_t b = ((rgb565)       & 0x1F) * 255 / 31;
            // ARGB8888 内存布局: B, G, R, A (lv_color32_t)
            argb_buf[i * 4 + 0] = b;
            argb_buf[i * 4 + 1] = g;
            argb_buf[i * 4 + 2] = r;
            argb_buf[i * 4 + 3] = alpha;
        }

        dsc->header.cf = LV_COLOR_FORMAT_ARGB8888;
        dsc->header.stride = w * 4;
        dsc->data = argb_buf;
        dsc->data_size = argb_size;
    } else {
        // 其他格式直接使用原始数据
        dsc->header.cf = static_cast<lv_color_format_t>(header->cf);
        dsc->header.stride = header->stride;
        dsc->data = src;
        dsc->data_size = src_size;
    }

    ESP_LOGI(TAG, "解析: %dx%d cf_orig=0x%02x → cf=%d stride=%d",
             w, h, header->cf, dsc->header.cf, dsc->header.stride);

    return dsc;
}
