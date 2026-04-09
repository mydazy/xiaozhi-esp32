#include "ota_http_download.h"
#include "esp_log.h"
#include <sys/stat.h>
#include <sys/unistd.h>
#include <errno.h>
#include <cstring>
#include "mbedtls/md5.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"

#define TAG "OTA_HTTP_DOWNLOAD"

// 单例实例获取 - 延迟初始化，只在第一次使用时分配内存
OtaHttpDownload& OtaHttpDownload::GetInstance() {
    static OtaHttpDownload instance;
    return instance;
}

OtaHttpDownload::OtaHttpDownload() {
    // 创建文件缓冲区 - 只在首次创建单例时分配
    file_buffer_ = (uint8_t*)malloc(MAX_FILE_SIZE);
    if(!file_buffer_) {
        ESP_LOGE(TAG, "Failed to allocate file buffer!");
    }
    downloading_ = false;
    total_size_ = 0;
    ESP_LOGI(TAG, "OtaHttpDownload singleton initialized, buffer: %p", file_buffer_);
}

OtaHttpDownload::~OtaHttpDownload() {
    if(file_buffer_) free(file_buffer_);
    ESP_LOGI(TAG, "OtaHttpDownload destroyed");
}

std::string OtaHttpDownload::calculate_md5(const uint8_t* data, size_t len) {
    unsigned char md5_result[16];
    mbedtls_md5(data, len, md5_result);
    
    char md5_str[33];
    for(int i = 0; i < 16; i++) {
        sprintf(&md5_str[i*2], "%02x", md5_result[i]);
    }
    md5_str[32] = '\0';
    
    return std::string(md5_str);
}

// 流式 MD5 计算，避免将整个文件加载到内存
std::string OtaHttpDownload::calculate_md5_from_file(const char* path) {
    FILE* f = fopen(path, "rb");
    if(!f) {
        ESP_LOGE(TAG, "Failed to open file for MD5 calculation: %s", path);
        return "";
    }

    mbedtls_md5_context ctx;
    mbedtls_md5_init(&ctx);
    mbedtls_md5_starts(&ctx);

    // 使用小缓冲区流式读取，避免大块内存分配
    uint8_t buffer[512];
    size_t bytes_read;
    while ((bytes_read = fread(buffer, 1, sizeof(buffer), f)) > 0) {
        mbedtls_md5_update(&ctx, buffer, bytes_read);
    }
    fclose(f);

    unsigned char md5_result[16];
    mbedtls_md5_finish(&ctx, md5_result);
    mbedtls_md5_free(&ctx);

    char md5_str[33];
    for(int i = 0; i < 16; i++) {
        sprintf(&md5_str[i*2], "%02x", md5_result[i]);
    }
    md5_str[32] = '\0';

    return std::string(md5_str);
}

void OtaHttpDownload::reload_file_and_verify(const char* path, const std::string& expected_md5) {
    // 等待一小段时间，确保文件系统操作完成
    vTaskDelay(pdMS_TO_TICKS(100));
    
    // 重新计算文件的MD5并显示
    std::string actual_md5 = calculate_md5_from_file(path);
    ESP_LOGI(TAG, "文件重新加载完成: %s", path);
    ESP_LOGI(TAG, "文件MD5: %s", actual_md5.c_str());
    
    if(!expected_md5.empty()) {
        if(actual_md5 == expected_md5) {
            ESP_LOGI(TAG, "MD5验证成功: 期望=%s, 实际=%s", expected_md5.c_str(), actual_md5.c_str());
        } else {
            ESP_LOGE(TAG, "MD5验证失败: 期望=%s, 实际=%s", expected_md5.c_str(), actual_md5.c_str());
        }
    }
    
    // 文件已成功替换，UI下次调用lv_gif_set_src()或重新打开文件时会自动加载新文件
    // LVGL的文件系统驱动会在打开文件时获取最新内容，不会受到正在使用的旧文件句柄影响
    ESP_LOGI(TAG, "文件已更新，UI将在下次加载时使用新文件: %s", path);
}

bool OtaHttpDownload::file_exists(const char* path) {
    FILE* f = fopen(path, "rb");
    if(f) {
        fclose(f);
        return true;
    }
    return false;
}

// 使用流式 MD5 计算检查文件匹配，避免大块内存分配
bool OtaHttpDownload::check_md5_match(const char* path, const std::string& expected_md5) {
    std::string actual_md5 = calculate_md5_from_file(path);
    if (actual_md5.empty()) {
        return false;
    }

    ESP_LOGD(TAG, "MD5 check: expected %s, got %s",
             expected_md5.c_str(), actual_md5.c_str());

    return (actual_md5 == expected_md5);
}

void OtaHttpDownload::set_progress_callback(std::function<void(int progress, size_t downloaded, size_t total)> callback) {
    progress_callback_ = callback;
}

void OtaHttpDownload::set_complete_callback(std::function<void(bool success, const std::string& error)> callback) {
    complete_callback_ = callback;
}

size_t OtaHttpDownload::get_task_count() const {
    return task_queue_.size();
}

bool OtaHttpDownload::is_downloading() const {
    return downloading_;
}

void OtaHttpDownload::start_downloads() {
    if (!downloading_ && !task_queue_.empty()) {
        start_next_download();
    }
}

void OtaHttpDownload::add_download(const char* url, const char* save_path, const char* md5) {
    if(task_queue_.size() < 16) {
        task_queue_.emplace(url, save_path, md5);
        ESP_LOGI(TAG, "Added task: %s -> %s (MD5: %s)", url, save_path, md5);
        
        // 如果当前没有在下载，自动开始下载
        if(!downloading_) {
            start_next_download();
        }
    } else {
        ESP_LOGW(TAG, "Task queue full (max 16)");
    }
}

// 执行队列中的下一个下载任务
void OtaHttpDownload::start_next_download() {
    if(task_queue_.empty()) {
        ESP_LOGI(TAG, "所有下载任务完成");
        downloading_ = false;
        if(complete_callback_) {
            complete_callback_(true, "");
        }
        return;
    }

    DownloadTask task = task_queue_.front();
    task_queue_.pop();

    // 检查文件是否存在且MD5匹配
    if(file_exists(task.save_path.c_str())) {
        if(task.md5.empty() || check_md5_match(task.save_path.c_str(), task.md5)) {
            ESP_LOGI(TAG, "文件已存在且MD5匹配，跳过下载: %s", task.save_path.c_str());
            downloading_ = false;
            vTaskDelay(pdMS_TO_TICKS(100)); // 短暂等待
            start_next_download();
            return;
        } else {
            ESP_LOGI(TAG, "文件存在但MD5不匹配，需要重新下载: %s", task.save_path.c_str());
        }
    } else {
        ESP_LOGI(TAG, "文件不存在，需要下载: %s", task.save_path.c_str());
    }

    downloading_ = true;
    ESP_LOGI(TAG, "开始下载: %s", task.url.c_str());

    // 使用Board的Network接口下载文件，参考ota.cc的方案
    bool success = download_file(task.url, task.save_path, task.md5);
    
    if(success) {
        ESP_LOGI(TAG, "下载完成: %s", task.save_path.c_str());
        
        // 文件替换成功，重新加载文件并显示新的MD5
        reload_file_and_verify(task.save_path.c_str(), task.md5);
        
        // 触发图片自动加载 - 通知UI刷新
        if(progress_callback_) {
            progress_callback_(100, 0, 0); // 发送完成信号
        }
    } else {
        ESP_LOGE(TAG, "Download failed: %s", task.url.c_str());
        if(complete_callback_) {
            complete_callback_(false, "下载失败: " + task.url);
        }
    }
    
    // 继续处理下一个任务
    downloading_ = false;
    vTaskDelay(pdMS_TO_TICKS(1000)); // 等待1秒
    start_next_download();
}

// 下载文件，参考ota.cc的Upgrade方法实现
bool OtaHttpDownload::download_file(const std::string& url, const std::string& save_path, const std::string& expected_md5) {
    ESP_LOGI(TAG, "Downloading file from %s", url.c_str());
    
    auto& board = Board::GetInstance();
    auto network = board.GetNetwork();
    auto http = network->CreateHttp(0);
    
    if (!http->Open("GET", url)) {
        ESP_LOGE(TAG, "Failed to open HTTP connection");
        return false;
    }

    if (http->GetStatusCode() != 200) {
        ESP_LOGE(TAG, "Failed to get file, status code: %d", http->GetStatusCode());
        return false;
    }

    size_t content_length = http->GetBodyLength();
    if (content_length == 0) {
        ESP_LOGE(TAG, "Failed to get content length");
        return false;
    }
    
    if (content_length > MAX_FILE_SIZE) {
        ESP_LOGE(TAG, "File size %d exceeds limit %d", content_length, MAX_FILE_SIZE);
        return false;
    }

    total_size_ = content_length;
    file_size_ = 0;
    
    char buffer[512];
    size_t total_read = 0, recent_read = 0;
    auto last_calc_time = esp_timer_get_time();
    
    while (true) {
        int ret = http->Read(buffer, sizeof(buffer));
        if (ret < 0) {
            ESP_LOGE(TAG, "Failed to read HTTP data: %s", esp_err_to_name(ret));
            return false;
        }

        // 检查文件是否超限
        if(file_size_ + ret > MAX_FILE_SIZE) {
            ESP_LOGE(TAG, "File exceeds 500KB limit!");
            return false;
        }
        
        // 数据复制到RAM缓冲区
        memcpy(file_buffer_ + file_size_, buffer, ret);
        file_size_ += ret;
        total_read += ret;
        recent_read += ret;

        // Calculate speed and progress every second
        if (esp_timer_get_time() - last_calc_time >= 1000000 || ret == 0) {
            size_t progress = total_read * 100 / content_length;
            ESP_LOGI(TAG, "Progress: %u%% (%u/%u), Speed: %uB/s", progress, total_read, content_length, recent_read);
            if (progress_callback_) {
                progress_callback_(progress, total_read, content_length);
            }
            last_calc_time = esp_timer_get_time();
            recent_read = 0;
        }

        // 让出CPU时间，防止阻塞系统任务
        if (ret > 0) {
            vTaskDelay(pdMS_TO_TICKS(1)); // 每次读取后让出1ms
        }

        if (ret == 0) {
            break;
        }
    }
    http->Close();

    // 保存文件到SPIFFS，使用安全的文件替换
    return save_to_spiffs(save_path.c_str(), expected_md5);
}

// 保存缓冲区内容到SPIFFS，重点优化文件替换逻辑
bool OtaHttpDownload::save_to_spiffs(const char* path, const std::string& expected_md5) {
    if(file_size_ == 0) {
        ESP_LOGE(TAG, "No data to save");
        return false;
    }

    // 计算下载数据的MD5
    std::string actual_md5 = calculate_md5(file_buffer_, file_size_);

    // 验证MD5（如果提供了预期MD5）
    if(!expected_md5.empty() && actual_md5 != expected_md5) {
        ESP_LOGE(TAG, "MD5 mismatch! Expected %s, got %s", 
                 expected_md5.c_str(), actual_md5.c_str());
        return false;
    }

    // 生成临时文件路径
    std::string temp_path = std::string(path) + ".tmp";
    
    // 先写入临时文件
    FILE* f = fopen(temp_path.c_str(), "wb");
    if(f == nullptr) {
        ESP_LOGE(TAG, "Failed to open temp file: %s", temp_path.c_str());
        return false;
    }
    
    size_t written = fwrite(file_buffer_, 1, file_size_, f);
    fclose(f);
    
    if(written != file_size_) {
        ESP_LOGE(TAG, "Write error: expected %d, wrote %d", file_size_, written);
        unlink(temp_path.c_str()); // 删除临时文件
        return false;
    }
    
    ESP_LOGI(TAG, "Written %d bytes to temp file: %s", file_size_, temp_path.c_str());
    
    // 使用安全的文件替换
    if(!replace_file_safely(temp_path.c_str(), path, expected_md5)) {
        ESP_LOGE(TAG, "Failed to replace file: %s", path);
        unlink(temp_path.c_str()); // 删除临时文件
        return false;
    }
    
    ESP_LOGI(TAG, "Successfully replaced file: %s", path);
    return true;
}

// 直接文件替换，无需备份
// SPIFFS文件系统中rename不能覆盖已存在文件，需要先删除目标文件
bool OtaHttpDownload::replace_file_safely(const char* temp_path, const char* target_path, const std::string& expected_md5) {
    // 检查临时文件是否存在
    if(!file_exists(temp_path)) {
        ESP_LOGE(TAG, "Temp file does not exist: %s", temp_path);
        return false;
    }
    
    // SPIFFS的rename不支持覆盖已存在的文件，需要先删除目标文件
    // 先删除目标文件（如果存在）
    if(file_exists(target_path)) {
        // 尝试多次删除，避免文件正在被UI使用
        // 增加重试次数和等待时间，给UI足够时间释放文件句柄
        int retry_count = 5;
        bool deleted = false;
        for(int i = 0; i < retry_count; i++) {
            if(unlink(target_path) == 0) {
                deleted = true;
                ESP_LOGI(TAG, "Removed existing file: %s (attempt %d)", target_path, i + 1);
                vTaskDelay(pdMS_TO_TICKS(100)); // 等待确保删除完成
                break;
            } else {
                int errno_val = errno;
                ESP_LOGW(TAG, "Failed to remove existing file (attempt %d/%d, errno: %d): %s", 
                         i + 1, retry_count, errno_val, target_path);
                // 文件可能正在被UI使用，等待更长时间后再重试
                // LVGL通常会在短时间内完成文件读取，所以增加等待时间
                if(i < retry_count - 1) {
                    vTaskDelay(pdMS_TO_TICKS(200)); // 等待200ms后重试，给UI时间释放文件
                }
            }
        }
        
        if(!deleted) {
            ESP_LOGW(TAG, "Cannot remove existing file (may be in use by UI), will try rename anyway: %s", target_path);
            // 即使删除失败，也尝试rename
            // 在某些文件系统中，如果文件句柄已关闭但目录项未释放，rename可能仍然成功
        }
    }
    
    // 将临时文件重命名为目标文件
    // 这是原子操作，即使之前删除失败，rename也可能成功（如果文件已经释放）
    if(rename(temp_path, target_path) != 0) {
        int errno_val = errno;
        ESP_LOGE(TAG, "Failed to rename temp file to target: %s -> %s (errno: %d)", 
                 temp_path, target_path, errno_val);
        
        // rename失败的原因可能是：
        // 1. 目标文件仍在被使用（UI正在读取）
        // 2. SPIFFS文件系统限制
        // 这种情况下，临时文件仍然存在，可以保留供后续重试
        // 但为了不占用存储空间，我们还是删除临时文件
        // 调用者可以选择稍后重试下载
        ESP_LOGE(TAG, "File replacement failed. The target file may be in use. Consider retrying later.");
        return false;
    }
    
    // 替换成功，验证新文件的MD5
    ESP_LOGI(TAG, "File replaced successfully: %s", target_path);
    
    // 验证替换后的文件MD5（如果提供了预期MD5）
    if(!expected_md5.empty()) {
        std::string new_file_md5 = calculate_md5_from_file(target_path);
        if(new_file_md5 == expected_md5) {
            ESP_LOGI(TAG, "File MD5 verification successful: %s", new_file_md5.c_str());
        } else {
            ESP_LOGE(TAG, "File MD5 verification failed! Expected: %s, Got: %s", 
                     expected_md5.c_str(), new_file_md5.c_str());
            return false;
        }
    }
    
    return true;
}