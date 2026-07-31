#ifndef TUNNEL_SERVER_TUNNEL_SERVER_H_
#define TUNNEL_SERVER_TUNNEL_SERVER_H_

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>

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
    explicit TunnelServer(uint16_t control_port, const std::string& token = "",
                          const std::string& tun_subnet = "");
    ~TunnelServer();

    TunnelServer(const TunnelServer&) = delete;
    TunnelServer& operator=(const TunnelServer&) = delete;

    void Run();

    // 分配一个未使用的 TUN IP。返回 0 表示无可用 IP。
    uint32_t AllocateTunIp();

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

    // 向所有已注册客户端广播当前 TUN 节点列表
    void BroadcastPeerList();

    uint16_t control_port_;
    int      listen_fd_;
    int      epfd_;
    std::string token_;  // 鉴权 token (空=不需要)
    uint32_t tun_subnet_base_ = 0;  // TUN 子网基地址 (网络字节序, 0=不启用)
    uint32_t tun_subnet_mask_ = 0;  // 子网掩码

    // fd -> 隧道会话
    std::unordered_map<int, std::unique_ptr<TunnelSession>> sessions_;

    // 尚未通过 AUTH 的 fd (空 token 时永远为空)
    std::unordered_set<int> auth_pending_;

    // 快速查找: user_fd -> tunnel_fd (跨 session 定位)
    std::unordered_map<int, int> user_to_tunnel_;

    // client 名字 -> tunnel_fd (用于 PROBE 路由)
    std::unordered_map<std::string, int> name_to_tunnel_;

    // TUN 路由: 虚拟 IP -> tunnel_fd
    std::unordered_map<uint32_t, int> ip_to_tunnel_;

    // 全局 remote_port 占用表: remote_port -> tunnel_fd (端口冲突检测)
    std::unordered_map<uint16_t, int> port_owners_;

    // client 间中继会话: session_id -> {tunnel_a, tunnel_b}
    // 当 client A 发起连接 client B 时创建, 数据在两者间转发。
    struct RelaySession {
        int tunnel_a;  // 发起方
        int tunnel_b;  // 目标方
        // P2P 协商状态
        uint16_t port_a = 0;  // client A 的 P2P 端口 (0=未上报)
        uint16_t port_b = 0;  // client B 的 P2P 端口
        bool p2p_active = false; // P2P 已建立, 不再中继 DATA
    };
    std::unordered_map<uint32_t, RelaySession> relay_sessions_;
};

}  // namespace server
}  // namespace tunnel

#endif  // TUNNEL_SERVER_TUNNEL_SERVER_H_
