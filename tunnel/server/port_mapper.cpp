#include "port_mapper.h"
#include "tunnel_server.h"

#include <arpa/inet.h>
#include <sys/epoll.h>
#include <unistd.h>

#include <cerrno>

#include "../common/frame.h"
#include "../common/logger.h"
#include "../common/netutil.h"
#include "../common/session.h"

namespace tunnel {
namespace server {

PortMapper::~PortMapper() {
    for (auto& kv : user_listen_fds_) {
        epoll_ctl(epfd_, EPOLL_CTL_DEL, kv.second, nullptr);
        close(kv.second);
    }
    user_listen_fds_.clear();
    // user_fd 由 TunnelServer 统一关闭
    sessions_.clear();
    user_to_session_.clear();
}

void PortMapper::SetContext(int epfd, TunnelServer* server, int tunnel_fd) {
    epfd_ = epfd;
    server_ = server;
    tunnel_fd_ = tunnel_fd;
}

bool PortMapper::HandlePortMap(const std::string& payload) {
    if (payload.size() < 1) {
        LOG_ERROR("PORT_MAP payload too short");
        return false;
    }
    uint8_t count = static_cast<uint8_t>(payload[0]);
    size_t offset = 1;
    // 每个 entry: uint16 local_port + uint16 remote_port = 4 字节
    if (payload.size() < 1 + static_cast<size_t>(count) * 4) {
        LOG_ERROR("PORT_MAP payload truncated (count=%u, need=%zu, have=%zu)",
                  count, static_cast<size_t>(1 + count * 4), payload.size());
        return false;
    }
    for (uint8_t i = 0; i < count; ++i) {
        uint16_t local_port = ntohs(*reinterpret_cast<const uint16_t*>(&payload[offset]));
        offset += 2;
        uint16_t remote_port = ntohs(*reinterpret_cast<const uint16_t*>(&payload[offset]));
        offset += 2;

        port_map_[remote_port] = local_port;
        if (!start_listen(remote_port)) {
            return false;
        }
        LOG_INFO("port map: remote:%u -> client local:%u (tunnel_fd=%d)",
                 remote_port, local_port, tunnel_fd_);
    }
    return true;
}

bool PortMapper::start_listen(uint16_t remote_port) {
    // 如果该端口已在监听则跳过
    if (user_listen_fds_.count(remote_port)) {
        LOG_WARN("remote port %u already listening, skip", remote_port);
        return true;
    }
    int lfd = net::create_listen_socket(remote_port);
    if (lfd < 0) {
        LOG_ERROR("failed to listen on remote port %u", remote_port);
        return false;
    }
    epoll_event ev{};
    ev.events = EPOLLIN | EPOLLET;
    ev.data.fd = lfd;
    if (epoll_ctl(epfd_, EPOLL_CTL_ADD, lfd, &ev) < 0) {
        LOG_ERROR("epoll_ctl add user_listen_fd=%d failed: %s", lfd, strerror(errno));
        close(lfd);
        return false;
    }
    user_listen_fds_[remote_port] = lfd;
    LOG_INFO("listening on public port %u (fd=%d)", remote_port, lfd);
    return true;
}

void PortMapper::AcceptUsers(int user_listen_fd) {
    // 找到对应的 remote_port
    uint16_t remote_port = 0;
    for (auto& kv : user_listen_fds_) {
        if (kv.second == user_listen_fd) {
            remote_port = kv.first;
            break;
        }
    }
    if (remote_port == 0) return;

    uint16_t local_port = port_map_[remote_port];

    for (;;) {
        int user_fd = accept(user_listen_fd, nullptr, nullptr);
        if (user_fd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            if (errno == EINTR) continue;
            LOG_ERROR("accept user on port %u failed: %s", remote_port, strerror(errno));
            break;
        }
        net::set_nonblock(user_fd);

        uint32_t sid = alloc_session_id();
        sessions_[sid] = user_fd;
        user_to_session_[user_fd] = sid;

        // 加入 epoll, 由 TunnelServer 的事件循环统一管理
        epoll_event ev{};
        ev.events = EPOLLIN | EPOLLET;
        ev.data.fd = user_fd;
        if (epoll_ctl(epfd_, EPOLL_CTL_ADD, user_fd, &ev) < 0) {
            LOG_ERROR("epoll_ctl add user_fd=%d failed: %s", user_fd, strerror(errno));
            close(user_fd);
            sessions_.erase(sid);
            user_to_session_.erase(user_fd);
            continue;
        }

        // 注册 user_fd -> tunnel_fd 映射, 让 TunnelServer 能路由该 fd 的数据事件
        server_->RegisterUserFd(user_fd, tunnel_fd_);

        // 派发 NEW_CONN 给 client: { session_id, local_port }
        std::string frame = FrameBuilder(MessageType::NEW_CONN)
                                .AppendU32(sid)
                                .AppendU16(local_port)
                                .Build();
        // 通过 TunnelServer 发送帧到隧道
        server_->SendFrame(tunnel_fd_, std::move(frame));

        LOG_INFO("user connected fd=%d on public port %u -> session_id=%u, "
                 "NEW_CONN sent to client for local:%u",
                 user_fd, remote_port, sid, local_port);
    }
}

int PortMapper::GetUserFd(uint32_t session_id) const {
    auto it = sessions_.find(session_id);
    return (it != sessions_.end()) ? it->second : -1;
}

void PortMapper::RemoveSession(uint32_t session_id) {
    auto it = sessions_.find(session_id);
    if (it == sessions_.end()) return;
    int user_fd = it->second;
    sessions_.erase(it);
    user_to_session_.erase(user_fd);
}

bool PortMapper::IsUserListenFd(int fd) const {
    for (auto& kv : user_listen_fds_) {
        if (kv.second == fd) return true;
    }
    return false;
}

bool PortMapper::IsUserFd(int fd) const {
    return user_to_session_.count(fd) > 0;
}

uint32_t PortMapper::GetSessionIdByUserFd(int user_fd) const {
    auto it = user_to_session_.find(user_fd);
    return (it != user_to_session_.end()) ? it->second : 0;
}

}  // namespace server
}  // namespace tunnel
