#ifndef SSID_MANAGER_H
#define SSID_MANAGER_H

#include <string>
#include <vector>
#include <mutex>

struct SsidItem {
    std::string ssid;
    std::string password;
};

class SsidManager {
public:
    static SsidManager& GetInstance() {
        static SsidManager instance;
        return instance;
    }

    void AddSsid(const std::string& ssid, const std::string& password);
    void RemoveSsid(int index);
    void SetDefaultSsid(int index);
    void Clear();
    // 按值返回拷贝（持锁内复制）：消除引用逃逸，调用方在锁外遍历也安全
    std::vector<SsidItem> GetSsidList() const;

private:
    SsidManager();
    ~SsidManager();

    void LoadFromNvs();
    void SaveToNvs();

    std::vector<SsidItem> ssid_list_;
    mutable std::mutex mutex_;   // 守 ssid_list_：WiFi/HTTP/Blufi/SmartConfig 多任务并发
};

#endif // SSID_MANAGER_H
