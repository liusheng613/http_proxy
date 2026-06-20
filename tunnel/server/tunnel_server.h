#ifndef TUNNEL_SERVER_TUNNEL_SERVER_H_
#define TUNNEL_SERVER_TUNNEL_SERVER_H_

#include <cstdint>
#include <memory>
#include <unordered_map>

#include "../common/frame.h"
#include "../common/protocol.h"
#include "../common/write_buffer.h"

namespace tunnel {
namespace server {

// 单条隧道连接的上下文。
// 阶段 1: 仅维护帧解码器、发送缓冲、心跳时间戳。
struct TunnelSession {
    int fd = -1;
    FrameDecoder decoder;
    WriteBuffer  writer;
    time_t last_recv_heartbeat = 0;  // 最近一次收到对端消息(含心跳)的时间
};

// 公网中继/协调服务端。
class TunnelServer {
public:
    explicit TunnelServer(uint16_t control_port);
    ~TunnelServer();

    TunnelServer(const TunnelServer&) = delete;
    TunnelServer& operator=(const TunnelServer&) = delete;

    void Run();

private:
    bool Init();
    void Cleanup();

    void HandleAccept();
    // 处理一条隧道连接的可读事件。返回 false 表示连接已断, 需清理。
    bool HandleTunnelReadable(int fd);
    // 处理可写事件: 尝试把发送缓冲写空。
    void HandleTunnelWritable(int fd);
    // 处理一个完整帧 (业务分发)。
    void HandleFrame(int fd, const Frame& frame);
    // 关闭并清理一条隧道连接。
    void CloseSession(int fd);
    // 扫描所有连接, 关闭长时间无心跳的连接。
    void CheckHeartbeatTimeout();

    uint16_t control_port_;
    int      listen_fd_;
    int      epfd_;

    // fd -> 隧道会话。accept 时创建, 断开时销毁。
    std::unordered_map<int, std::unique_ptr<TunnelSession>> sessions_;
};

}  // namespace server
}  // namespace tunnel

#endif  // TUNNEL_SERVER_TUNNEL_SERVER_H_
