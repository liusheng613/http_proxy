#include "tunnel_server.h"

#include <sys/epoll.h>
#include <unistd.h>

#include <cerrno>

#include "../common/logger.h"
#include "../common/netutil.h"

namespace tunnel {
namespace server {

namespace {
constexpr int kMaxEvents = 1024;
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
    ev.events = EPOLLIN | EPOLLET;  // ET 模式, 配合非阻塞 accept 循环
    ev.data.fd = listen_fd_;
    if (epoll_ctl(epfd_, EPOLL_CTL_ADD, listen_fd_, &ev) < 0) {
        LOG_ERROR("epoll_ctl add listen_fd failed: %s", strerror(errno));
        return false;
    }

    LOG_INFO("server listening control port %u", control_port_);
    return true;
}

void TunnelServer::Cleanup() {
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
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break;  // ET 模式: 已无新连接
            }
            if (errno == EINTR) {
                continue;
            }
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
        LOG_INFO("new tunnel connection fd=%d from %s", fd,
                 net::peer_to_string(fd).c_str());
    }
}

void TunnelServer::HandleTunnelReadable(int fd) {
    // 阶段 0: 仅读取并打印帧类型, 验证收发链路。后续阶段实现具体业务。
    char buf[4096];
    ssize_t total = 0;
    for (;;) {
        ssize_t n = read(fd, buf, sizeof(buf));
        if (n > 0) {
            total += n;
            continue;
        }
        if (n == 0) {
            // 对端关闭
            LOG_INFO("tunnel fd=%d closed by peer", fd);
            epoll_ctl(epfd_, EPOLL_CTL_DEL, fd, nullptr);
            close(fd);
            return;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            break;  // 数据读完
        }
        if (errno == EINTR) {
            continue;
        }
        LOG_ERROR("read tunnel fd=%d failed: %s", fd, strerror(errno));
        epoll_ctl(epfd_, EPOLL_CTL_DEL, fd, nullptr);
        close(fd);
        return;
    }

    // 尝试解析至少一个完整帧头, 仅用于打印 (阶段 0 验证)。
    // 注意: 这里不做粘包/拆包的完整状态机, 留到阶段 1 实现。
    if (total >= kFrameHeaderLen) {
        MessageType type{};
        uint32_t payload_len = 0;
        if (decode_frame_header(buf, &type, &payload_len)) {
            LOG_INFO("recv frame fd=%d type=%s payload_len=%u", fd,
                     msg_type_str(type), payload_len);
        }
    }
}

void TunnelServer::Run() {
    if (!Init()) {
        Cleanup();
        return;
    }

    epoll_event events[kMaxEvents];
    for (;;) {
        int nready = epoll_wait(epfd_, events, kMaxEvents, -1);
        if (nready < 0) {
            if (errno == EINTR) continue;
            LOG_ERROR("epoll_wait failed: %s", strerror(errno));
            break;
        }
        for (int i = 0; i < nready; ++i) {
            int fd = events[i].data.fd;
            if (events[i].events & (EPOLLERR | EPOLLHUP)) {
                LOG_WARN("epoll error/hup on fd=%d", fd);
                epoll_ctl(epfd_, EPOLL_CTL_DEL, fd, nullptr);
                close(fd);
                continue;
            }
            if (fd == listen_fd_) {
                HandleAccept();
            } else if (events[i].events & EPOLLIN) {
                HandleTunnelReadable(fd);
            }
        }
    }

    Cleanup();
}

}  // namespace server
}  // namespace tunnel
