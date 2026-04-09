#ifndef _OTA_HTTP_DOWNLOAD_H_
#define _OTA_HTTP_DOWNLOAD_H_

#include <string>
#include <functional>
#include <vector>
#include <queue>
#include "board.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define MAX_FILE_SIZE (1024 * 1024) // 500KB最大文件限制

// OTA专用的HTTP下载器，参考ota.cc的下载方案，重点优化文件替换
// 使用单例模式复用1MB缓冲区，避免频繁分配导致内存碎片
class OtaHttpDownload {
public:
    // 获取单例实例
    static OtaHttpDownload& GetInstance();

    // 禁止拷贝和移动
    OtaHttpDownload(const OtaHttpDownload&) = delete;
    OtaHttpDownload& operator=(const OtaHttpDownload&) = delete;

    ~OtaHttpDownload();

    // 添加下载任务
    void add_download(const char* url, const char* save_path, const char* md5 = "");
    
    // 设置进度回调函数
    void set_progress_callback(std::function<void(int progress, size_t downloaded, size_t total)> callback);
    
    // 设置完成回调函数
    void set_complete_callback(std::function<void(bool success, const std::string& error)> callback);
    
    // 获取队列中的任务数量
    size_t get_task_count() const;
    
    // 检查是否正在下载
    bool is_downloading() const;
    
    // 启动下载任务
    void start_downloads();

private:
    // 私有构造函数，只能通过 GetInstance 获取实例
    OtaHttpDownload();

    struct DownloadTask {
        std::string url;
        std::string save_path;
        std::string md5;
        DownloadTask(const char* u, const char* p, const char* m = "") 
        : url(u), save_path(p), md5(m) {}
    };

    std::queue<DownloadTask> task_queue_;
    uint8_t* file_buffer_ = nullptr;
    size_t file_size_ = 0;
    size_t total_size_ = 0;
    bool downloading_ = false;

    // 回调函数
    std::function<void(int progress, size_t downloaded, size_t total)> progress_callback_;
    std::function<void(bool success, const std::string& error)> complete_callback_;

    void start_next_download();
    bool download_file(const std::string& url, const std::string& save_path, const std::string& expected_md5);
    bool save_to_spiffs(const char* path, const std::string& expected_md5);
    std::string calculate_md5(const uint8_t* data, size_t len);
    std::string calculate_md5_from_file(const char* path);
    void reload_file_and_verify(const char* path, const std::string& expected_md5);
    bool file_exists(const char* path);
    bool check_md5_match(const char* path, const std::string& expected_md5);
    bool replace_file_safely(const char* temp_path, const char* target_path, const std::string& expected_md5);
};

#endif // _OTA_HTTP_DOWNLOAD_H_
