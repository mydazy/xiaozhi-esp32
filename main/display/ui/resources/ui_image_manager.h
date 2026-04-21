#ifndef UI_IMAGE_MANAGER_H
#define UI_IMAGE_MANAGER_H

#include <lvgl.h>
#include <string>
#include <unordered_map>

// ============================================================================
// UI 图片管理器
// 从 mmap assets 分区加载 LVGL v9 bin 格式图片，构造 lv_image_dsc_t
// 零拷贝：像素数据直接指向 mmap 地址，只在堆上分配描述符结构体
// ============================================================================

class UiImageManager {
public:
    static UiImageManager& GetInstance();

    // 从 assets 分区批量加载所有图片（在 Assets::Apply() 之后调用）
    void LoadAll();

    // 获取指定名称的图片描述符（返回 nullptr 表示未找到）
    // name: assets 中的文件名，如 "app_chat.bin"
    const lv_image_dsc_t* Get(const char* name) const;

    // 释放所有图片描述符
    void FreeAll();

private:
    UiImageManager() = default;
    ~UiImageManager();
    UiImageManager(const UiImageManager&) = delete;
    UiImageManager& operator=(const UiImageManager&) = delete;

    // 从 mmap 数据解析 LVGL v9 bin 头并构造 lv_image_dsc_t
    // data: 指向 bin 文件数据（已跳过 "ZZ" 魔数）
    // size: 数据大小
    lv_image_dsc_t* ParseBinImage(void* data, size_t size);

    // 图片名 -> 描述符指针
    std::unordered_map<std::string, lv_image_dsc_t*> images_;
};

// 便捷宏：获取图片描述符
#define UI_IMG(name) UiImageManager::GetInstance().Get(name)

#endif // UI_IMAGE_MANAGER_H
