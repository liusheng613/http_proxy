#include "tunnel_server.h"

#include <sys/epoll.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <ctime>
#include <vector>

#include "../common/logger.h"
#include "../common/netutil.h"
#include "../common/session.h"

namespace tunnel {
namespace server {

namespace {
constexpr int kMaxEvents = 1024;
constexpr int kHeartbeatTimeoutSec = 30;
}

TunnelServer::TunnelServer(uint16_t control_port)
    : control_port_(control_port), listen_fd_(-1), epfd_(-1) {}

TunnelServer::~TunnelServer() { Cleanup(); }

bool TunnelServer::Init() {
    listen_fd_ = net::create_listen_socket(control_port_);
    if (listen_fd_ < 0) {
        return false;
    }

    epfd_ = epoll_create1(EPOLL_CLOEXEC);
    if (epfd_ < 0) {
        LOG_ERROR("epoll_create1 failed: %s", strerror(errno));
        net::close_fd(listen_fd_);
        return false;
    }

    epoll_event ev{};
    ev.events = EPOLLIN | EPOLLET;
    ev.data.fd = listen_fd_;
    if (epoll_ctl(epfd_, EPOLL_CTL_ADD, listen_fd_, &ev) < 0) {
        LOG_ERROR("epoll_ctl add listen_fd failed: %s", strerror(errno));
        return false;
    }

    LOG_INFO("server listening control port %u", control_port_);
    return true;
}

void TunnelServer::Cleanup() {
    // 先关闭所有隧道会话 (会级联关闭其 user_listen_fd)
    while (!sessions_.empty()) {
        CloseSession(sessions_.begin()->first);
    }
    user_to_tunnel_.clear();
    net::close_fd(listen_fd_);
    if (epfd_ >= 0) {
        close(epfd_);
        epfd_ = -1;
    }
}

void TunnelServer::HandleAccept() {
    for (;;) {
        int fd = accept(listen_fd_, nullptr, nullptr);
        if (fd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            if (errno == EINTR) continue;
            LOG_ERROR("accept failed: %s", strerror(errno));
            break;
        }

        net::set_nonblock(fd);
        epoll_event ev{};
        ev.events = EPOLLIN | EPOLLET;
        ev.data.fd = fd;
        if (epoll_ctl(epfd_, EPOLL_CTL_ADD, fd, &ev) < 0) {
            LOG_ERROR("epoll_ctl add tunnel fd=%d failed: %s", fd, strerror(errno));
            close(fd);
            continue;
        }

        auto sess = std::make_unique<TunnelSession>();
        sess->fd = fd;
        sess->last_recv_heartbeat = time(nullptr);
        sess->mapper.SetContext(epfd_, this, fd);
        sessions_[fd] = std::move(sess);
        LOG_INFO("new tunnel connection fd=%d from %s", fd,
                 net::peer_to_string(fd).c_str());
    }
}

void TunnelServer::CloseSession(int fd) {
    auto it = sessions_.find(fd);
    if (it == sessions_.end()) {
        epoll_ctl(epfd_, EPOLL_CTL_DEL, fd, nullptr);
        close(fd);
        return;
    }
    // 先关闭该隧道下所有 user_fd
    std::vector<int> user_fds_to_close;
    for (auto& kv : user_to_tunnel_) {
        if (kv.second == fd && it->second->IsUserFd(kv.first)) {
            user_fds_to_close.push_back(kv.first);
        }
    }
    for (int ufd : user_fds_to_close) {
        CloseUser(ufd);
    }

    epoll_ctl(epfd_, EPOLL_CTL_DEL, fd, nullptr);
    close(fd);
    sessions_.erase(it);
    LOG_INFO("tunnel fd=%d closed (sessions=%zu)", fd, sessions_.size());
}

void TunnelServer::CloseUser(int user_fd) {
    // 找到所属隧道
    auto tit = user_to_tunnel_.find(user_fd);
    if (tit == user_to_tunnel_.end()) return;
    int tunnel_fd = tit->second;
    auto sit = sessions_.find(tunnel_fd);
    if (sit == sessions_.end()) return;

    uint32_t sid = sit->second->GetSessionIdByUserFd(user_fd);

    // 通知 client 关闭该 session
    if (sid > 0) {
        std::string frame = FrameBuilder(MessageType::CLOSE)
                                .AppendU32(sid)
                                .Build();
        SendFrame(tunnel_fd, std::move(frame));
    }

    sit->second->mapper.RemoveSession(sid);
    user_to_tunnel_.erase(user_fd);
    epoll_ctl(epfd_, EPOLL_CTL_DEL, user_fd, nullptr);
    close(user_fd);
    LOG_DEBUG("closed user fd=%d (session_id=%u)", user_fd, sid);
}

bool TunnelServer::HandleTunnelReadable(int fd) {
    auto it = sessions_.find(fd);
    if (it == sessions_.end()) return false;
    TunnelSession& sess = *it->second;

    char buf[8192];
    for (;;) {
        ssize_t n = read(fd, buf, sizeof(buf));
        if (n > 0) {
            if (!sess.decoder.Feed(buf, static_cast<size_t>(n))) {
                LOG_WARN("tunnel fd=%d protocol error, closing", fd);
                CloseSession(fd);
                return false;
            }
            continue;
        }
        if (n == 0) {
            LOG_INFO("tunnel fd=%d closed by peer", fd);
            CloseSession(fd);
            return false;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
        if (errno == EINTR) continue;
        LOG_ERROR("read tunnel fd=%d failed: %s", fd, strerror(errno));
        CloseSession(fd);
        return false;
    }

    sess.last_recv_heartbeat = time(nullptr);
    auto frames = sess.decoder.Output();
    for (const auto& f : frames) {
        HandleFrame(fd, f);
        if (sessions_.find(fd) == sessions_.end()) break;
    }
    return true;
}

void TunnelServer::HandleTunnelWritable(int fd) {
    auto it = sessions_.find(fd);
    if (it == sessions_.end()) return;
    if (it->second->writer.Flush(fd)) {
        epoll_event ev{};
        ev.events = EPOLLIN | EPOLLET;
        ev.data.fd = fd;
        epoll_ctl(epfd_, EPOLL_CTL_MOD, fd, &ev);
    }
}

void TunnelServer::HandleUserReadable(int user_fd) {
    char buf[65536];  // 用户数据可以大一点
    for (;;) {
        ssize_t n = read(user_fd, buf, sizeof(buf));
        if (n > 0) {
            // 找到所属隧道
            auto tit = user_to_tunnel_.find(user_fd);
            if (tit == user_to_tunnel_.end()) {
                LOG_WARN("user_fd=%d has no tunnel, closing", user_fd);
                CloseUser(user_fd);
                return;
            }
            int tunnel_fd = tit->second;
            auto sit = sessions_.find(tunnel_fd);
            if (sit == sessions_.end()) {
                CloseUser(user_fd);
                return;
            }
            uint32_t sid = sit->second->GetSessionIdByUserFd(user_fd);

            // 封装 DATA 帧: { session_id, data_len, data }
            std::string frame = FrameBuilder(MessageType::DATA)
                                    .AppendU32(sid)
                                    .AppendU16(static_cast<uint16_t>(n))
                                    .AppendBytes(buf, static_cast<size_t>(n))
                                    .Build();
            SendFrame(tunnel_fd, std::move(frame));
            continue;
        }
        if (n == 0) {
            // 用户断开
            CloseUser(user_fd);
            return;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
        if (errno == EINTR) continue;
        LOG_ERROR("read user_fd=%d failed: %s", user_fd, strerror(errno));
        CloseUser(user_fd);
        return;
    }
}

void TunnelServer::HandleFrame(int fd, const Frame& frame) {
    auto it = sessions_.find(fd);
    if (it == sessions_.end()) return;
    TunnelSession& sess = *it->second;

    switch (frame.type) {
        case MessageType::HEARTBEAT:
            LOG_DEBUG("fd=%d recv HEARTBEAT", fd);
            {
                std::string hb = FrameBuilder(MessageType::HEARTBEAT).Build();
                sess.writer.Append(std::move(hb));
                if (!sess.writer.Flush(fd)) {
                    epoll_event ev{};
                    ev.events = EPOLLIN | EPOLLOUT | EPOLLET;
                    ev.data.fd = fd;
                    epoll_ctl(epfd_, EPOLL_CTL_MOD, fd, &ev);
                }
            }
            break;

        case MessageType::PORT_MAP:
            LOG_INFO("fd=%d recv PORT_MAP (payload_len=%zu)", fd, frame.payload.size());
            sess.mapper.HandlePortMap(frame.payload);
            break;

        case MessageType::DATA:
            // client 回传的用户数据: { session_id, data_len, data }
            if (frame.payload.size() < 6) {
                LOG_WARN("DATA payload too short (%zu)", frame.payload.size());
                break;
            }
            {
                uint32_t sid = ntohl(*reinterpret_cast<const uint32_t*>(&frame.payload[0]));
                uint16_t dlen = ntohs(*reinterpret_cast<const uint16_t*>(&frame.payload[4]));
                if (frame.payload.size() < 6 + dlen) {
                    LOG_WARN("DATA payload truncated (sid=%u, dlen=%u, have=%zu)",
                             sid, dlen, frame.payload.size());
                    break;
                }
                int user_fd = sess.GetUserFd(sid);
                if (user_fd < 0) {
                    LOG_WARN("DATA for unknown session_id=%u, closing session", sid);
                    sess.mapper.RemoveSession(sid);
                    std::string close_frame = FrameBuilder(MessageType::CLOSE)
                                                  .AppendU32(sid)
                                                  .Build();
                    sess.writer.Append(std::move(close_frame));
                    sess.writer.Flush(fd);
                    break;
                }
                const char* data = &frame.payload[6];
                ssize_t written = write(user_fd, data, dlen);
                if (written < static_cast<ssize_t>(dlen)) {
                    // 简单处理: 写不完先丢弃余量 (非阻塞 socket 正常)
                    // TODO: 如果需要可靠, 给 user_fd 也加 WriteBuffer
                    if (written < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
                        LOG_ERROR("write user_fd=%d failed: %s", user_fd, strerror(errno));
                        CloseUser(user_fd);
                    }
                }
            }
            break;

        case MessageType::CLOSE:
            // client 通知关闭某 session
            if (frame.payload.size() < 4) break;
            {
                uint32_t sid = ntohl(*reinterpret_cast<const uint32_t*>(&frame.payload[0]));
                int user_fd = sess.GetUserFd(sid);
                if (user_fd >= 0) {
                    CloseUser(user_fd);
                }
                sess.mapper.RemoveSession(sid);
            }
            break;

        default:
            LOG_INFO("fd=%d recv %s (payload_len=%zu), not handled yet", fd,
                     msg_type_str(frame.type), frame.payload.size());
            break;
    }
}

void TunnelServer::SendFrame(int tunnel_fd, std::string frame_bytes) {
    auto it = sessions_.find(tunnel_fd);
    if (it == sessions_.end()) return;
    it->second->writer.Append(std::move(frame_bytes));
    if (!it->second->writer.Flush(tunnel_fd)) {
        epoll_event ev{};
        ev.events = EPOLLIN | EPOLLOUT | EPOLLET;
        ev.data.fd = tunnel_fd;
        epoll_ctl(epfd_, EPOLL_CTL_MOD, tunnel_fd, &ev);
    }
}

void TunnelServer::CheckHeartbeatTimeout() {
    time_t now = time(nullptr);
    std::vector<int> dead;
    for (auto& kv : sessions_) {
        if (now - kv.second->last_recv_heartbeat > kHeartbeatTimeoutSec) {
            dead.push_back(kv.first);
        }
    }
    for (int fd : dead) {
        LOG_WARN("tunnel fd=%d heartbeat timeout, closing", fd);
        CloseSession(fd);
    }
}

void TunnelServer::Run() {
    if (!Init()) {
        Cleanup();
        return;
    }

    epoll_event events[kMaxEvents];
    for (;;) {
        int nready = epoll_wait(epfd_, events, kMaxEvents, 5000);
        if (nready < 0) {
            if (errno == EINTR) {
                CheckHeartbeatTimeout();
                continue;
            }
            LOG_ERROR("epoll_wait failed: %s", strerror(errno));
            break;
        }
        for (int i = 0; i < nready; ++i) {
            int fd = events[i].data.fd;
            if (events[i].events & (EPOLLERR | EPOLLHUP)) {
                if (fd == listen_fd_) {
                    LOG_ERROR("listen_fd error, fatal");
                    break;
                }
                // 判断 fd 类型
                auto sit = sessions_.find(fd);
                if (sit != sessions_.end()) {
                    CloseSession(fd);
                    continue;
                }
                // 可能是 user_fd 或 user_listen_fd
                if (user_to_tunnel_.count(fd)) {
                    CloseUser(fd);
                    continue;
                }
                // user_listen_fd 的 error, 找到对应 tunnel
                for (auto& kv : sessions_) {
                    if (kv.second->IsUserListenFd(fd)) {
                        // user_listen_fd 出错比较致命, 暂直接关闭
                        LOG_ERROR("user_listen_fd=%d error", fd);
                        break;
                    }
                }
                continue;
            }

            if (fd == listen_fd_) {
                HandleAccept();
            } else {
                // 判断 fd 类型: tunnel_fd / user_listen_fd / user_fd
                if (sessions_.count(fd)) {
                    // 隧道 fd
                    if (events[i].events & EPOLLIN) {
                        HandleTunnelReadable(fd);
                    }
                    if (sessions_.count(fd) && (events[i].events & EPOLLOUT)) {
                        HandleTunnelWritable(fd);
                    }
                } else {
                    // 隧道内用户相关 fd
                    // 先判断是哪个 tunnel 的
                    int owner_tunnel = -1;
                    for (auto& kv : sessions_) {
                        if (kv.second->IsUserListenFd(fd)) {
                            kv.second->mapper.AcceptUsers(fd);
                            owner_tunnel = kv.first;
                            break;
                        }
                    }
                    if (owner_tunnel >= 0) continue;

                    // 可能是 user_fd
                    if (user_to_tunnel_.count(fd)) {
                        if (events[i].events & EPOLLIN) {
                            HandleUserReadable(fd);
                        }
                    }
                }
            }
        }
        CheckHeartbeatTimeout();
    }

    Cleanup();
}

}  // namespace server
}  // namespace tunnel
