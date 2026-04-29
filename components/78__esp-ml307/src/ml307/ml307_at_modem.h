#ifndef _ML307_AT_MODEM_H_
#define _ML307_AT_MODEM_H_

#include "at_modem.h"
#include "tcp.h"
#include "udp.h"
#include "http.h"
#include "mqtt.h"
#include "web_socket.h"

class Ml307AtModem : public AtModem {
public:
    Ml307AtModem(std::shared_ptr<AtUart> at_uart);
    ~Ml307AtModem() override = default;

    void Reboot() override;
    bool SetSleepMode(bool enable, int delay_seconds=0) override;
    NetworkStatus WaitForNetworkReady(int timeout_ms=-1) override;

    // 实现基类的纯虚函数
    std::unique_ptr<Http> CreateHttp(int connect_id) override;
    std::unique_ptr<Tcp> CreateTcp(int connect_id) override;
    std::unique_ptr<Tcp> CreateSsl(int connect_id) override;
    std::unique_ptr<Udp> CreateUdp(int connect_id) override;
    std::unique_ptr<Mqtt> CreateMqtt(int connect_id) override;
    std::unique_ptr<WebSocket> CreateWebSocket(int connect_id) override;

protected:
    void HandleUrc(const std::string& command, const std::vector<AtArgumentValue>& arguments) override;
    void ResetConnections();

    // Patch A · 弱网稳定性增强（来自 189 v3.5.3 验证版）
    // ConfigureRadioProfile：开机锁 LTE-only（AT+MRATLIST="LTE"），避免弱信号区在 2G/3G 间反复回退
    // SendRadioCommandWithRetry：AT 命令带重试（默认 2 次 · 1500ms 超时 · 200ms 退避）
    void ConfigureRadioProfile();
    bool SendRadioCommandWithRetry(const char* command, const char* description, int max_attempts = 2);
};


#endif // _ML307_AT_MODEM_H_