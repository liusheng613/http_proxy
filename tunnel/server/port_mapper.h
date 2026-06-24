#ifndef TUNNEL_SERVER_PORT_MAPPER_H_
#define TUNNEL_SERVER_PORT_MAPPER_H_

#include <cstdint>
#include <string>
#include <unordered_map>

namespace tunnel {
namespace server {

class TunnelServer;  // 前向声明, 避免循环依赖

// 管理"某个 client 注册的一组端口映射"。
// 每个 TunnelSession 持有一个 PortMapper。
//
// 职责:
//   - 解析 PORT_MAP 帧的 payload, 创建监听 socket
//   - 接受外部用户连接, 分配 session_id, 派发 NEW_CONN 给 client
//   - 维护 session_id -> user_fd 的映射
class PortMapper {
public:
    // 构造时不做任何事。Setup 由 TunnelServer 在收到 PORT_MAP 后调用。
    PortMapper() = default;
    ~PortMapper();  // 关闭所有 user_listen_fd 和 user_fd

    PortMapper(const PortMapper&) = delete;
    PortMapper& operator=(const PortMapper&) = delete;

    // 解析 PORT_MAP payload, 创建监听 socket 并加入 epoll。
    // epfd/server 通过 Setup 传入, 此后 Accept/中继都通过它们操作。
    void SetContext(int epfd, TunnelServer* server, int tunnel_fd);

    // 从 payload 解析端口映射列表并开始监听。payload 格式见 protocol.h。
    bool HandlePortMap(const std::string& payload);

    // 接受新用户连接 (由 epoll 事件驱动调用)。
    void AcceptUsers(int user_listen_fd);

    // 获取 session_id 对应的 user_fd。不存在返回 -1。
    int GetUserFd(uint32_t session_id) const;

    // 移除一个 session (user_fd 断开或 client 通知 CLOSE)。
    void RemoveSession(uint32_t session_id);

    // 判断某个 fd 是否是本 mapper 管理的 user_listen_fd。
    bool IsUserListenFd(int fd) const;

    // 判断某个 fd 是否是本 mapper 管理的 user_fd。
    bool IsUserFd(int fd) const;

    // 根据 user_fd 查 session_id。不存在返回 0。
    uint32_t GetSessionIdByUserFd(int user_fd) const;

private:
    // 为一个映射端口创建监听 socket, 加入 epoll。
    bool start_listen(uint16_t remote_port);

    int epfd_ = -1;
    TunnelServer* server_ = nullptr;
    int tunnel_fd_ = -1;

    // remote_port -> user_listen_fd
    std::unordered_map<uint16_t, int> user_listen_fds_;

    // remote_port -> local_port (client 本地服务端口)
    std::unordered_map<uint16_t, uint16_t> port_map_;

    // session_id -> user_fd
    std::unordered_map<uint32_t, int> sessions_;

    // user_fd -> session_id (反向映射)
    std::unordered_map<int, uint32_t> user_to_session_;
};

}  // namespace server
}  // namespace tunnel

#endif  // TUNNEL_SERVER_PORT_MAPPER_H_
