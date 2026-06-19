#ifndef TUNNEL_COMMON_NETUTIL_H_
#define TUNNEL_COMMON_NETUTIL_H_

#include <cstdint>
#include <string>

namespace tunnel {
namespace net {

// 创建监听 socket: 绑定本机 port, 非阻塞, 复用地址。失败返回 -1。
// tcp_backlog 为 listen backlog。
int create_listen_socket(uint16_t port, int tcp_backlog = 128);

// 发起 TCP 连接, 成功返回 fd(非阻塞已设置), 失败返回 -1。
// 如果 connect 正在进行(EINPROGRESS), 这里返回 fd 并通过 connected 标记是否立即连上。
int create_connect_socket(const std::string& ip, uint16_t port,
                          bool* connected = nullptr);

// 设置 fd 非阻塞。成功 0, 失败 -1。
int set_nonblock(int fd);

// 设置 fd 阻塞。成功 0, 失败 -1。
int set_block(int fd);

// 设置 SO_REUSEADDR。成功 0, 失败 -1。
int set_reuse_addr(int fd);

// 设置 TCP_NODELAY (关闭 Nagle)。成功 0, 失败 -1。
int set_nodelay(int fd);

// 安全关闭 fd 并置为 -1。
void close_fd(int& fd);

// 读取对端地址为可读字符串 "ip:port", 失败返回 ""。
std::string peer_to_string(int fd);

}  // namespace net
}  // namespace tunnel

#endif  // TUNNEL_COMMON_NETUTIL_H_
