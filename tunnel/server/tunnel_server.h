#ifndef TUNNEL_SERVER_TUNNEL_SERVER_H_
#define TUNNEL_SERVER_TUNNEL_SERVER_H_

#include <cstdint>

#include "protocol.h"

namespace tunnel {
namespace server {

// 公网中继/协调服务端。
//
// 阶段 0: 只负责监听控制隧道端口, 接受 client 连接, 跑 epoll 事件循环。
// 后续阶段会在此基础上增加:
//   - client 注册管理 (REGISTER)
//   - 端口映射 (PORT_MAP)
//   - 会话中继 (NEW_CONN / DATA / CLOSE)
class TunnelServer {
public:
    explicit TunnelServer(uint16_t control_port);
    ~TunnelServer();

    TunnelServer(const TunnelServer&) = delete;
    TunnelServer& operator=(const TunnelServer&) = delete;

    // 阻塞运行, 直到出错或被信号中断。
    void Run();

private:
    // 初始化监听 socket 与 epoll, 成功返回 true。
    bool Init();
    void Cleanup();

    // 处理 listen_fd 上的新连接(循环 accept 到 EAGAIN)。
    void HandleAccept();

    // 处理隧道连接上的可读事件: 解析帧。
    // 阶段 0 仅打印收到的消息类型, 后续阶段填充具体业务。
    void HandleTunnelReadable(int fd);

    uint16_t control_port_;
    int      listen_fd_;  // 监听控制隧道的 fd
    int      epfd_;       // epoll 实例
};

}  // namespace server
}  // namespace tunnel

#endif  // TUNNEL_SERVER_TUNNEL_SERVER_H_
