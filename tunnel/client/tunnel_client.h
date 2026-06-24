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

// 内网客户端: 主动连公网 server, 建立控制隧道。
//
// 阶段 2: 连接成功后发 PORT_MAP, 收 NEW_CONN 连本地服务, DATA 双向转发。
class TunnelClient {
public:
    TunnelClient(const std::string& server_ip, uint16_t server_port,
                 const std::vector<PortMapping>& mappings);
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

    // 连接成功后发 PORT_MAP
    void SendPortMap();

    // 收到 NEW_CONN: 连接本地服务, 加入 epoll。
    void HandleNewConn(uint32_t session_id, uint16_t local_port);

    // 关闭一个本地会话, 通知 server CLOSE。
    void CloseLocal(uint32_t session_id);

    void SendHeartbeat();
    void CheckHeartbeatTimeout();

    std::string server_ip_;
    uint16_t    server_port_;
    std::vector<PortMapping> mappings_;

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
};

}  // namespace client
}  // namespace tunnel

#endif  // TUNNEL_CLIENT_TUNNEL_CLIENT_H_
