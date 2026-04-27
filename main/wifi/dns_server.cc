#include "dns_server.h"
#include <esp_log.h>
#include <esp_heap_caps.h>
#include <lwip/sockets.h>
#include <lwip/netdb.h>

#define TAG "DnsServer"

DnsServer::DnsServer() {
}

DnsServer::~DnsServer() {
    Stop();
}

void DnsServer::Start(esp_ip4_addr_t gateway) {
    // 如果已经在运行，先停止
    if (running_) {
        ESP_LOGW(TAG, "DNS server already running, stopping first");
        Stop();
    }

    ESP_LOGI(TAG, "Starting DNS server");
    gateway_ = gateway;

    fd_ = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (fd_ < 0) {
        ESP_LOGE(TAG, "Failed to create socket");
        return;
    }

    // 设置 socket 超时，以便能够检查 running_ 标志
    struct timeval timeout;
    timeout.tv_sec = 1;  // 1秒超时
    timeout.tv_usec = 0;
    setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    server_addr.sin_port = htons(port_);

    if (bind(fd_, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        ESP_LOGE(TAG, "failed to bind port %d", port_);
        close(fd_);
        fd_ = -1;
        return;
    }

    running_ = true;
    // 栈必须内部 RAM：tskNO_AFFINITY 可漂移到 Core 0，配网时 NVS 写入频繁，PSRAM 栈会撞 flash op
    xTaskCreatePinnedToCore([](void* arg) {
        DnsServer* dns_server = static_cast<DnsServer*>(arg);
        dns_server->Run();
        vTaskDelete(NULL);
    }, "dns_server", 4096, this, 2, &task_handle_, tskNO_AFFINITY);
}

void DnsServer::Stop() {
    ESP_LOGI(TAG, "Stopping DNS server");

    // 设置停止标志
    running_ = false;

    // 关闭 socket，这会导致 recvfrom 返回错误
    if (fd_ >= 0) {
        shutdown(fd_, SHUT_RDWR);
        close(fd_);
        fd_ = -1;
    }

    // 等待任务结束（最多等待 2 秒）
    if (task_handle_ != nullptr) {
        for (int i = 0; i < 20; i++) {
            if (eTaskGetState(task_handle_) == eDeleted) {
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(100));
        }
        task_handle_ = nullptr;
    }

    ESP_LOGI(TAG, "DNS server stopped");
}

void DnsServer::Run() {
    char buffer[512];
    while (running_) {
        struct sockaddr_in client_addr;
        socklen_t client_addr_len = sizeof(client_addr);
        int len = recvfrom(fd_, buffer, sizeof(buffer), 0, (struct sockaddr *)&client_addr, &client_addr_len);

        // 检查是否应该停止
        if (!running_) {
            break;
        }

        if (len < 0) {
            // 超时或错误，检查是否应该继续
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                // 超时，继续循环
                continue;
            }
            // 其他错误（如 socket 被关闭），退出循环
            ESP_LOGD(TAG, "recvfrom returned %d, errno=%d, exiting", len, errno);
            break;
        }

        // 边界检查：确保有足够空间追加 DNS 应答（16字节）
        if (len > (int)(sizeof(buffer) - 16)) {
            ESP_LOGW(TAG, "DNS query too large (%d bytes), skipping", len);
            continue;
        }

        // Simple DNS response: point all queries to 192.168.4.1
        buffer[2] |= 0x80;  // Set response flag
        buffer[3] |= 0x80;  // Set Recursion Available
        buffer[7] = 1;      // Set answer count to 1

        // Add answer section
        memcpy(&buffer[len], "\xc0\x0c", 2);  // Name pointer
        len += 2;
        memcpy(&buffer[len], "\x00\x01\x00\x01\x00\x00\x00\x1c\x00\x04", 10);  // Type, class, TTL, data length
        len += 10;
        memcpy(&buffer[len], &gateway_.addr, 4);  // 192.168.4.1
        len += 4;
        ESP_LOGD(TAG, "Sending DNS response to %s", inet_ntoa(gateway_.addr));

        sendto(fd_, buffer, len, 0, (struct sockaddr *)&client_addr, client_addr_len);
    }

    ESP_LOGI(TAG, "DNS server task exiting");
}
