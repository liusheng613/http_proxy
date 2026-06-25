#ifndef TUNNEL_SERVER_TUNNEL_SERVER_H_
#define TUNNEL_SERVER_TUNNEL_SERVER_H_

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>

#include "../common/frame.h"
#include "../common/protocol.h"
#include "../common/write_buffer.h"
#include "port_mapper.h"

namespace tunnel {
namespace server {

// 单条隧道连接的上下文。
struct TunnelSession {
    int fd = -1;
    FrameDecoder decoder;
    WriteBuffer  writer;
    time_t last_recv_heartbeat = 0;
    std::string name;  // client 注册的名字 (REGISTER 后填充)
    PortMapper mapper;  // 该 client 注册的端口映射

    // 按 session_id 查找该 tunnel 的 mapper 中的 user_fd。
    int GetUserFd(uint32_t sid) const { return mapper.GetUserFd(sid); }

    // 按 user_fd 查 session_id。
    uint32_t GetSessionIdByUserFd(int user_fd) const {
        return mapper.GetSessionIdByUserFd(user_fd);
    }

    bool IsUserListenFd(int fd) const { return mapper.IsUserListenFd(fd); }
    bool IsUserFd(int fd) const { return mapper.IsUserFd(fd); }
};

// 公网中继/协调服务端。
class TunnelServer {
public:
    explicit TunnelServer(uint16_t control_port);
    ~TunnelServer();

    TunnelServer(const TunnelServer&) = delete;
    TunnelServer& operator=(const TunnelServer&) = delete;

    void Run();

    // 向指定隧道 fd 发送一帧。供 PortMapper 调用。
    void SendFrame(int tunnel_fd, std::string frame_bytes);

    // 注册 user_fd -> tunnel_fd 映射, 供事件路由用。供 PortMapper 调用。
    void RegisterUserFd(int user_fd, int tunnel_fd) {
        user_to_tunnel_[user_fd] = tunnel_fd;
    }

private:
    bool Init();
    void Cleanup();

    void HandleAccept();
    bool HandleTunnelReadable(int fd);
    void HandleTunnelWritable(int fd);
    void HandleFrame(int fd, const Frame& frame);

    // 处理外部用户连接上的数据: 封装为 DATA 帧通过隧道转发给 client。
    void HandleUserReadable(int user_fd);

    // 关闭一条隧道连接 (含其所有映射端口和用户会话)。
    void CloseSession(int fd);
    // 关闭一个用户连接, 通知 client CLOSE。
    void CloseUser(int user_fd);

    void CheckHeartbeatTimeout();

    uint16_t control_port_;
    int      listen_fd_;
    int      epfd_;

    // fd -> 隧道会话
    std::unordered_map<int, std::unique_ptr<TunnelSession>> sessions_;

    // 快速查找: user_fd -> tunnel_fd (跨 session 定位)
    std::unordered_map<int, int> user_to_tunnel_;

    // client 名字 -> tunnel_fd (用于 PROBE 路由)
    std::unordered_map<std::string, int> name_to_tunnel_;

    // 全局 remote_port 占用表: remote_port -> tunnel_fd (端口冲突检测)
    std::unordered_map<uint16_t, int> port_owners_;
};

}  // namespace server
}  // namespace tunnel

#endif  // TUNNEL_SERVER_TUNNEL_SERVER_H_
