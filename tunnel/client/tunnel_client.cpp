#include "tunnel_client.h"

#include <sys/epoll.h>
#include <unistd.h>

#include <cerrno>

#include "../common/logger.h"
#include "../common/netutil.h"

namespace tunnel {
namespace client {

namespace {
constexpr int kMaxEvents = 64;
}

TunnelClient::TunnelClient(const std::string& server_ip, uint16_t server_port)
    : server_ip_(server_ip), server_port_(server_port),
      tunnel_fd_(-1), epfd_(-1) {}

TunnelClient::~TunnelClient() { Cleanup(); }

bool TunnelClient::Connect() {
    bool connected = false;
    tunnel_fd_ = net::create_connect_socket(server_ip_, server_port_, &connected);
    if (tunnel_fd_ < 0) {
        return false;
    }
    net::set_nonblock(tunnel_fd_);

    if (connected) {
        LOG_INFO("connected to server %s:%u (fd=%d)", server_ip_.c_str(),
                 server_port_, tunnel_fd_);
    } else {
        LOG_INFO("connecting to server %s:%u in progress (fd=%d)",
                 server_ip_.c_str(), server_port_, tunnel_fd_);
    }

    epfd_ = epoll_create1(EPOLL_CLOEXEC);
    if (epfd_ < 0) {
        LOG_ERROR("epoll_create1 failed: %s", strerror(errno));
        net::close_fd(tunnel_fd_);
        return false;
    }

    epoll_event ev{};
    ev.events = EPOLLIN | EPOLLET;
    ev.data.fd = tunnel_fd_;
    if (epoll_ctl(epfd_, EPOLL_CTL_ADD, tunnel_fd_, &ev) < 0) {
        LOG_ERROR("epoll_ctl add tunnel_fd failed: %s", strerror(errno));
        return false;
    }
    return true;
}

void TunnelClient::Cleanup() {
    net::close_fd(tunnel_fd_);
    if (epfd_ >= 0) {
        close(epfd_);
        epfd_ = -1;
    }
}

void TunnelClient::HandleReadable() {
    // 阶段 0: 仅读取并打印, 验证链路。
    char buf[4096];
    ssize_t total = 0;
    for (;;) {
        ssize_t n = read(tunnel_fd_, buf, sizeof(buf));
        if (n > 0) {
            total += n;
            continue;
        }
        if (n == 0) {
            LOG_WARN("server closed the tunnel");
            epoll_ctl(epfd_, EPOLL_CTL_DEL, tunnel_fd_, nullptr);
            net::close_fd(tunnel_fd_);
            return;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            break;
        }
        if (errno == EINTR) {
            continue;
        }
        LOG_ERROR("read tunnel failed: %s", strerror(errno));
        epoll_ctl(epfd_, EPOLL_CTL_DEL, tunnel_fd_, nullptr);
        net::close_fd(tunnel_fd_);
        return;
    }

    if (total >= kFrameHeaderLen) {
        MessageType type{};
        uint32_t payload_len = 0;
        if (decode_frame_header(buf, &type, &payload_len)) {
            LOG_INFO("recv frame type=%s payload_len=%u", msg_type_str(type),
                     payload_len);
        }
    }
}

void TunnelClient::Run() {
    if (!Connect()) {
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
                if (fd == tunnel_fd_) {
                    net::close_fd(tunnel_fd_);
                } else {
                    close(fd);
                }
                continue;
            }
            if (fd == tunnel_fd_ && (events[i].events & EPOLLIN)) {
                HandleReadable();
            }
        }
    }

    Cleanup();
}

}  // namespace client
}  // namespace tunnel
