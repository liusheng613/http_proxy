#include "tunnel_client.h"

#include <sys/epoll.h>
#include <unistd.h>

#include <cerrno>
#include <ctime>

#include "../common/logger.h"
#include "../common/netutil.h"

namespace tunnel {
namespace client {

namespace {
constexpr int kMaxEvents = 64;
constexpr int kHeartbeatIntervalSec = 10;  // 每 10s 发一次心跳
constexpr int kHeartbeatTimeoutSec  = 30;  // 30s 没收到 server 消息则重连
constexpr int kReconnectIntervalSec = 5;   // 重连间隔
constexpr int kEpollTimeoutMs = 1000;      // epoll 周期醒来, 用于定时心跳/超时
}

TunnelClient::TunnelClient(const std::string& server_ip, uint16_t server_port)
    : server_ip_(server_ip), server_port_(server_port),
      tunnel_fd_(-1), epfd_(-1), connected_(false),
      last_send_heartbeat_(0), last_recv_heartbeat_(0) {}

TunnelClient::~TunnelClient() { Disconnect(); }

bool TunnelClient::Connect() {
    bool immediately = false;
    tunnel_fd_ = net::create_connect_socket(server_ip_, server_port_, &immediately);
    if (tunnel_fd_ < 0) {
        return false;
    }
    net::set_nonblock(tunnel_fd_);

    if (epfd_ < 0) {
        epfd_ = epoll_create1(EPOLL_CLOEXEC);
        if (epfd_ < 0) {
            LOG_ERROR("epoll_create1 failed: %s", strerror(errno));
            net::close_fd(tunnel_fd_);
            return false;
        }
    }

    // 同时监听可读和可写: 可写用于判断 connect 完成; 连上后可写用于排空缓冲
    epoll_event ev{};
    ev.events = EPOLLIN | EPOLLOUT | EPOLLET;
    ev.data.fd = tunnel_fd_;
    if (epoll_ctl(epfd_, EPOLL_CTL_ADD, tunnel_fd_, &ev) < 0) {
        LOG_ERROR("epoll_ctl add tunnel_fd failed: %s", strerror(errno));
        net::close_fd(tunnel_fd_);
        return false;
    }

    connected_ = immediately;
    time_t now = time(nullptr);
    last_recv_heartbeat_ = now;   // 重置接收计时, 给 connect 留出时间
    last_send_heartbeat_ = 0;     // 促使连接成功后立即发首心跳
    return true;
}

void TunnelClient::Disconnect() {
    if (tunnel_fd_ >= 0) {
        epoll_ctl(epfd_, EPOLL_CTL_DEL, tunnel_fd_, nullptr);
        net::close_fd(tunnel_fd_);
    }
    connected_ = false;
    // 解码器/缓冲复位
    decoder_ = FrameDecoder{};
    writer_ = WriteBuffer{};
}

void TunnelClient::HandleWritable() {
    if (!connected_) {
        // 非阻塞 connect 完成, 确认是否真正成功
        int err = 0;
        socklen_t len = sizeof(err);
        if (getsockopt(tunnel_fd_, SOL_SOCKET, SO_ERROR, &err, &len) < 0 || err != 0) {
            LOG_ERROR("connect to %s:%u failed: %s", server_ip_.c_str(),
                      server_port_, strerror(err ? err : errno));
            Disconnect();
            return;
        }
        connected_ = true;
        LOG_INFO("connected to server %s:%u (fd=%d)", server_ip_.c_str(),
                 server_port_, tunnel_fd_);
        last_recv_heartbeat_ = time(nullptr);
    }

    // 排空发送缓冲 (可能有心跳积压)
    if (writer_.Flush(tunnel_fd_)) {
        epoll_event ev{};
        ev.events = EPOLLIN | EPOLLET;
        ev.data.fd = tunnel_fd_;
        epoll_ctl(epfd_, EPOLL_CTL_MOD, tunnel_fd_, &ev);
    }
}

void TunnelClient::HandleReadable() {
    char buf[8192];
    for (;;) {
        ssize_t n = read(tunnel_fd_, buf, sizeof(buf));
        if (n > 0) {
            if (!decoder_.Feed(buf, static_cast<size_t>(n))) {
                LOG_WARN("protocol error from server, reconnecting");
                Disconnect();
                return;
            }
            continue;
        }
        if (n == 0) {
            LOG_WARN("server closed the tunnel");
            Disconnect();
            return;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            break;
        }
        if (errno == EINTR) {
            continue;
        }
        LOG_ERROR("read tunnel failed: %s", strerror(errno));
        Disconnect();
        return;
    }

    last_recv_heartbeat_ = time(nullptr);
    auto frames = decoder_.Output();
    for (const auto& f : frames) {
        HandleFrame(f);
    }
}

void TunnelClient::HandleFrame(const Frame& frame) {
    switch (frame.type) {
        case MessageType::HEARTBEAT:
            LOG_DEBUG("recv HEARTBEAT from server");
            break;
        default:
            LOG_INFO("recv %s (payload_len=%zu), not handled yet",
                     msg_type_str(frame.type), frame.payload.size());
            break;
    }
}

void TunnelClient::SendHeartbeat() {
    if (!connected_) return;
    std::string hb = FrameBuilder(MessageType::HEARTBEAT).Build();
    writer_.Append(std::move(hb));
    if (!writer_.Flush(tunnel_fd_)) {
        epoll_event ev{};
        ev.events = EPOLLIN | EPOLLOUT | EPOLLET;
        ev.data.fd = tunnel_fd_;
        epoll_ctl(epfd_, EPOLL_CTL_MOD, tunnel_fd_, &ev);
    }
    last_send_heartbeat_ = time(nullptr);
    LOG_DEBUG("send HEARTBEAT");
}

void TunnelClient::CheckHeartbeatTimeout() {
    if (!connected_) return;
    time_t now = time(nullptr);
    if (now - last_recv_heartbeat_ > kHeartbeatTimeoutSec) {
        LOG_WARN("heartbeat timeout (>%ds), reconnecting", kHeartbeatTimeoutSec);
        Disconnect();
    }
}

void TunnelClient::Run() {
    bool need_backoff = false;  // 上一次连接是否以失败告终, 需要退避后再试
    for (;;) {
        // 未连接 (或刚断开), 尝试重连
        if (tunnel_fd_ < 0) {
            if (need_backoff) {
                LOG_INFO("retry connecting in %ds ...", kReconnectIntervalSec);
                sleep(kReconnectIntervalSec);
            }
            LOG_INFO("connecting to %s:%u ...", server_ip_.c_str(), server_port_);
            if (!Connect()) {
                LOG_WARN("connect failed");
                need_backoff = true;
                continue;
            }
            need_backoff = false;
            // Connect 成功 (含异步 connect 已发起), 进入 epoll 循环
        }

        epoll_event events[kMaxEvents];
        int nready = epoll_wait(epfd_, events, kMaxEvents, kEpollTimeoutMs);
        if (nready < 0) {
            if (errno == EINTR) continue;
            LOG_ERROR("epoll_wait failed: %s", strerror(errno));
            break;
        }
        bool broke = false;
        for (int i = 0; i < nready; ++i) {
            if (events[i].events & (EPOLLERR | EPOLLHUP)) {
                LOG_WARN("epoll error/hup on tunnel");
                Disconnect();
                broke = true;
                break;
            }
            if (events[i].events & EPOLLIN) {
                HandleReadable();
            }
            // 可读处理后可能已断开
            if (tunnel_fd_ < 0) {
                broke = true;
                break;
            }
            if (events[i].events & EPOLLOUT) {
                HandleWritable();
            }
            if (tunnel_fd_ < 0) {
                broke = true;
                break;
            }
        }
        if (broke) {
            // 无论哪种断开, 下次重连前都退避, 避免连接被拒时 CPU 空转
            need_backoff = true;
            continue;
        }

        // 定时任务
        if (connected_) {
            time_t now = time(nullptr);
            if (now - last_send_heartbeat_ >= kHeartbeatIntervalSec) {
                SendHeartbeat();
            }
            CheckHeartbeatTimeout();
        }
    }

    Disconnect();
    if (epfd_ >= 0) {
        close(epfd_);
        epfd_ = -1;
    }
}

}  // namespace client
}  // namespace tunnel
