#ifndef TUNNEL_CLIENT_TUNNEL_CLIENT_H_
#define TUNNEL_CLIENT_TUNNEL_CLIENT_H_

#include <cstdint>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "../common/frame.h"
#include "../common/protocol.h"
#include "../common/write_buffer.h"

namespace tunnel {
namespace client {

// 端口映射配置项: (本地端口, 远程公网端口)
using PortMapping = std::pair<uint16_t, uint16_t>;

// 本地中继监听配置: 在本地端口监听, 收到连接后经中继转发到目标 client 的端口
struct LocalRelayConfig {
    uint16_t local_port;
    std::string target_name;
    uint16_t target_port;
};

// 内网客户端: 主动连公网 server, 建立控制隧道。
//
// 阶段 2: 连接成功后发 PORT_MAP, 收 NEW_CONN 连本地服务, DATA 双向转发。
// 阶段 3: 支持 client 名字 (REGISTER), 链路探活 (PROBE/PROBE_REPLY)。
class TunnelClient {
public:
    TunnelClient(const std::string& server_ip, uint16_t server_port,
                 const std::vector<PortMapping>& mappings,
                 const std::string& name = "",
                 const std::string& auto_connect = "",
                 const std::vector<LocalRelayConfig>& relay_listens = {},
                 const std::string& token = "",
                 const std::string& tun_ip = "");
    ~TunnelClient();

    TunnelClient(const TunnelClient&) = delete;
    TunnelClient& operator=(const TunnelClient&) = delete;

    void Run();

private:
    bool Connect();
    void Disconnect();

    // 隧道事件处理
    void HandleTunnelReadable();
    void HandleTunnelWritable();

    // 本地服务 fd 事件处理
    void HandleLocalReadable(int local_fd);

    // 帧处理
    void HandleFrame(const Frame& frame);

    // 连接成功后发 REGISTER + PORT_MAP
    void SendRegister();
    void SendPortMap();
    // 发送 AUTH 鉴权帧
    void SendAuth();

    // 收到 NEW_CONN: 连接本地服务, 加入 epoll。
    void HandleNewConn(uint32_t session_id, uint16_t local_port);

    // 收到 PROBE: 回应 PROBE_REPLY
    void HandleProbe(uint32_t probe_id, const std::string& source_name);

    // 收到 PROBE_REPLY: 打印结果
    void HandleProbeReply(uint32_t probe_id, uint8_t status);

    // 关闭一个本地会话, 通知 server CLOSE。
    void CloseLocal(uint32_t session_id);

    void SendHeartbeat();
    void CheckHeartbeatTimeout();

    // 发送探活请求到目标 client (可选功能, 用于测试)
    void SendProbe(const std::string& target_name, uint32_t probe_id);

    // P2P 打洞: 收到 P2P_TRY, 创建 P2P socket
    void HandleP2pTry(uint32_t session_id);
    // P2P 打洞: 收到 P2P_INFO, 开始 connect
    void HandleP2pInfo(uint32_t session_id, uint32_t peer_ip, uint16_t peer_port);
    // P2P 数据: 从 P2P socket 读取数据
    void HandleP2pReadable(int p2p_fd);

    // 处理 stdin 输入 (交互命令)
    void HandleStdinReadable();
    // 发起 client 间中继连接
    void SendRelayNewConn(const std::string& target_name, uint16_t target_port);
    // 处理中继数据的读写 (stdin -> server, server -> stdout)
    void HandleRelayData(uint32_t sid, const char* data, uint16_t dlen);

    // 处理本地中继监听端口的 accept
    void HandleLocalRelayAccept(int listen_fd);
    // 处理本地中继用户连接上的数据 (作为 relay 发起方)
    void HandleLocalRelayUserReadable(int user_fd);
    // 设置本地中继监听 (连接 server 后调用)
    void SetupLocalRelayListeners();
    // 初始化 TUN 设备 (连接后调用)
    void SetupTun();
    // 处理 TUN 设备可读事件
    void HandleTunReadable();

    std::string server_ip_;
    uint16_t    server_port_;
    std::vector<PortMapping> mappings_;
    std::string name_;  // client 名字 (可选, 用于 PROBE 路由)
    std::string auto_connect_target_; // 启动后自动连接的目标 "name:port"
    std::vector<LocalRelayConfig> relay_listens_; // 本地中继监听配置
    std::string token_;  // 鉴权 token (空=不鉴权)
    std::string tun_ip_;  // TUN 虚拟 IP (空=不启用)
    int tun_fd_ = -1;     // TUN 设备 fd

    int tunnel_fd_;
    int epfd_;
    bool connected_;

    FrameDecoder decoder_;
    WriteBuffer  writer_;

    time_t last_send_heartbeat_;
    time_t last_recv_heartbeat_;

    // session_id -> local_fd (到本地服务的连接)
    std::unordered_map<uint32_t, int> sessions_;

    // local_fd -> session_id (反向映射)
    std::unordered_map<int, uint32_t> local_to_session_;

    // 当前活跃的中继 session_id (0=无)。用于 stdin ↔ server 交互。
    uint32_t active_relay_sid_;

    // 本地中继监听: fd -> (target_name, target_port)
    std::unordered_map<int, LocalRelayConfig> relay_listen_fds_;
    // 等待 relay ack 的 user_fd (本地用户连接, 等待 server 分配 session_id)
    int pending_relay_user_fd_ = -1;
    // pending 期间读到的数据 (在 relay ack 到达前用户已经发来的数据)
    std::string pending_relay_buf_;
    // 本地用户连接对应的中继目标名 (pending 时暂存)
    std::string pending_relay_target_;

    // P2P 打洞状态: session_id -> {p2p_fd, timeout}
    struct P2pState {
        int p2p_fd = -1;
        time_t start_time = 0;
        std::string write_buf;  // P2P 写缓冲
        bool connected = false; // TCP 握手已完成
    };
    std::unordered_map<uint32_t, P2pState> p2p_states_;
    // P2P fd -> session_id 反向映射
    std::unordered_map<int, uint32_t> p2p_to_session_;
};

}  // namespace client
}  // namespace tunnel

#endif  // TUNNEL_CLIENT_TUNNEL_CLIENT_H_
