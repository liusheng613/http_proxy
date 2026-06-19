#ifndef TUNNEL_CLIENT_TUNNEL_CLIENT_H_
#define TUNNEL_CLIENT_TUNNEL_CLIENT_H_

#include <cstdint>
#include <string>

#include "protocol.h"

namespace tunnel {
namespace client {

// 内网客户端: 主动连公网 server, 建立控制隧道。
//
// 阶段 0: 仅完成连接建立 + epoll 读事件, 打印收到的帧。
// 后续阶段叠加: 心跳/重连/REGISTER/PORT_MAP/数据中继等。
class TunnelClient {
public:
    TunnelClient(const std::string& server_ip, uint16_t server_port);
    ~TunnelClient();

    TunnelClient(const TunnelClient&) = delete;
    TunnelClient& operator=(const TunnelClient&) = delete;

    void Run();

private:
    // 连接 server。成功返回 fd 并加入 epoll, 失败返回 false。
    bool Connect();
    void Cleanup();

    // 处理 server 下发的可读事件。
    void HandleReadable();

    std::string server_ip_;
    uint16_t    server_port_;
    int         tunnel_fd_;  // 到 server 的控制隧道
    int         epfd_;
};

}  // namespace client
}  // namespace tunnel

#endif  // TUNNEL_CLIENT_TUNNEL_CLIENT_H_
