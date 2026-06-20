#include "tunnel_server.h"

#include <sys/epoll.h>
#include <unistd.h>

#include <cerrno>
#include <ctime>
#include <vector>

#include "../common/logger.h"
#include "../common/netutil.h"

namespace tunnel {
namespace server {

namespace {
constexpr int kMaxEvents = 1024;
constexpr int kHeartbeatTimeoutSec = 30;  // 30s 收不到任何消息即判定断线
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
    for (auto& kv : sessions_) {
        if (kv.second && kv.second->fd >= 0) {
            close(kv.second->fd);
        }
    }
    sessions_.clear();

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
        sessions_[fd] = std::move(sess);
        LOG_INFO("new tunnel connection fd=%d from %s", fd,
                 net::peer_to_string(fd).c_str());
    }
}

void TunnelServer::CloseSession(int fd) {
    auto it = sessions_.find(fd);
    if (it == sessions_.end()) {
        // 可能是已清理过的 fd, 直接从 epoll 移除并关闭
        epoll_ctl(epfd_, EPOLL_CTL_DEL, fd, nullptr);
        close(fd);
        return;
    }
    epoll_ctl(epfd_, EPOLL_CTL_DEL, fd, nullptr);
    close(fd);
    sessions_.erase(it);
    LOG_INFO("tunnel fd=%d closed (sessions=%zu)", fd, sessions_.size());
}

bool TunnelServer::HandleTunnelReadable(int fd) {
    auto it = sessions_.find(fd);
    if (it == sessions_.end()) {
        return false;  // 未知 fd
    }
    TunnelSession& sess = *it->second;

    char buf[8192];
    for (;;) {
        ssize_t n = read(fd, buf, sizeof(buf));
        if (n > 0) {
            // 喂入帧状态机; 协议错误则断开
            if (!sess.decoder.Feed(buf, static_cast<size_t>(n))) {
                LOG_WARN("tunnel fd=%d protocol error (bad frame), closing", fd);
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
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            break;
        }
        if (errno == EINTR) {
            continue;
        }
        LOG_ERROR("read tunnel fd=%d failed: %s", fd, strerror(errno));
        CloseSession(fd);
        return false;
    }

    // 更新心跳时间: 任何收到数据都视为链路活跃
    sess.last_recv_heartbeat = time(nullptr);

    // 取出完整帧并处理
    auto frames = sess.decoder.Output();
    for (const auto& f : frames) {
        HandleFrame(fd, f);
        // HandleFrame 可能 CloseSession(fd), 一旦关闭就停止处理后续帧
        if (sessions_.find(fd) == sessions_.end()) {
            break;
        }
    }
    return true;
}

void TunnelServer::HandleTunnelWritable(int fd) {
    auto it = sessions_.find(fd);
    if (it == sessions_.end()) return;
    TunnelSession& sess = *it->second;
    if (sess.writer.Flush(fd)) {
        // 缓冲已空, 取消可写监听
        epoll_event ev{};
        ev.events = EPOLLIN | EPOLLET;
        ev.data.fd = fd;
        epoll_ctl(epfd_, EPOLL_CTL_MOD, fd, &ev);
    }
}

void TunnelServer::HandleFrame(int fd, const Frame& frame) {
    switch (frame.type) {
        case MessageType::HEARTBEAT:
            // 收到心跳, 回一个心跳 (双向保活)
            LOG_DEBUG("fd=%d recv HEARTBEAT", fd);
            {
                auto it = sessions_.find(fd);
                if (it != sessions_.end()) {
                    std::string hb = FrameBuilder(MessageType::HEARTBEAT).Build();
                    it->second->writer.Append(std::move(hb));
                    if (!it->second->writer.Flush(fd)) {
                        // 没写完, 监听可写
                        epoll_event ev{};
                        ev.events = EPOLLIN | EPOLLOUT | EPOLLET;
                        ev.data.fd = fd;
                        epoll_ctl(epfd_, EPOLL_CTL_MOD, fd, &ev);
                    }
                }
            }
            break;
        default:
            // 阶段 1 暂不处理 REGISTER/PORT_MAP/... , 仅记录
            LOG_INFO("fd=%d recv %s (payload_len=%zu), not handled yet", fd,
                     msg_type_str(frame.type), frame.payload.size());
            break;
    }
}

void TunnelServer::CheckHeartbeatTimeout() {
    time_t now = time(nullptr);
    // 收集超时的 fd (不能边遍历边 erase)
    std::vector<int> dead;
    for (auto& kv : sessions_) {
        if (now - kv.second->last_recv_heartbeat > kHeartbeatTimeoutSec) {
            dead.push_back(kv.first);
        }
    }
    for (int fd : dead) {
        LOG_WARN("tunnel fd=%d heartbeat timeout (>%ds), closing", fd,
                 kHeartbeatTimeoutSec);
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
        // timeout=5000ms: 即使无事件也定期醒来检查心跳超时
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
                LOG_WARN("epoll error/hup on fd=%d", fd);
                if (fd == listen_fd_) {
                    break;  // listen fd 出错, 致命, 退出主循环
                }
                CloseSession(fd);
                continue;
            }
            if (fd == listen_fd_) {
                HandleAccept();
            } else {
                if (events[i].events & EPOLLIN) {
                    HandleTunnelReadable(fd);
                }
                // readable 之后 fd 可能已关闭, 需复查
                if (sessions_.count(fd) && (events[i].events & EPOLLOUT)) {
                    HandleTunnelWritable(fd);
                }
            }
        }
        // 每轮事件处理完检查心跳超时
        CheckHeartbeatTimeout();
    }

    Cleanup();
}

}  // namespace server
}  // namespace tunnel
