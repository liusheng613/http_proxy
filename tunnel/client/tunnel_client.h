#ifndef TUNNEL_CLIENT_TUNNEL_CLIENT_H_
#define TUNNEL_CLIENT_TUNNEL_CLIENT_H_

#include <cstdint>
#include <string>

#include "../common/frame.h"
#include "../common/protocol.h"
#include "../common/write_buffer.h"

namespace tunnel {
namespace client {

// 内网客户端: 主动连公网 server, 建立控制隧道。
//
// 阶段 1: 帧状态机 + 定时心跳 + 断线重连。
class TunnelClient {
public:
    TunnelClient(const std::string& server_ip, uint16_t server_port);
    ~TunnelClient();

    TunnelClient(const TunnelClient&) = delete;
    TunnelClient& operator=(const TunnelClient&) = delete;

    void Run();

private:
    // 建立到 server 的连接 (非阻塞 connect, 通过 epoll 可写判断连上)。
    // 成功把 fd 加入 epoll 并返回 true。
    bool Connect();
    // 关闭当前隧道连接, 清理解码器/缓冲, 准备重连。
    void Disconnect();

    // 处理事件: 可读 / 可写(连接完成或缓冲排空)。
    void HandleReadable();
    void HandleWritable();
    // 处理一个完整帧。
    void HandleFrame(const Frame& frame);

    // 发送心跳 (定时调用)。
    void SendHeartbeat();
    // 检查心跳超时 (超过阈值没收到 server 回应则主动断开重连)。
    void CheckHeartbeatTimeout();

    std::string server_ip_;
    uint16_t    server_port_;

    int tunnel_fd_;
    int epfd_;

    // 连接状态: 是否已完成 TCP 握手
    bool connected_;

    FrameDecoder decoder_;
    WriteBuffer  writer_;

    time_t last_send_heartbeat_;  // 最近一次发心跳时间
    time_t last_recv_heartbeat_;  // 最近一次收到 server 消息时间
};

}  // namespace client
}  // namespace tunnel

#endif  // TUNNEL_CLIENT_TUNNEL_CLIENT_H_
