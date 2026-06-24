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
constexpr int kHeartbeatIntervalSec = 10;
constexpr int kHeartbeatTimeoutSec  = 30;
constexpr int kReconnectIntervalSec = 5;
constexpr int kEpollTimeoutMs = 1000;
}

TunnelClient::TunnelClient(const std::string& server_ip, uint16_t server_port,
                           const std::vector<PortMapping>& mappings)
    : server_ip_(server_ip), server_port_(server_port),
      mappings_(mappings),
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
    last_recv_heartbeat_ = now;
    last_send_heartbeat_ = 0;
    return true;
}

void TunnelClient::Disconnect() {
    // 关闭所有 local_fd
    for (auto& kv : sessions_) {
        epoll_ctl(epfd_, EPOLL_CTL_DEL, kv.second, nullptr);
        close(kv.second);
    }
    sessions_.clear();
    local_to_session_.clear();

    if (tunnel_fd_ >= 0) {
        epoll_ctl(epfd_, EPOLL_CTL_DEL, tunnel_fd_, nullptr);
        net::close_fd(tunnel_fd_);
    }
    connected_ = false;
    decoder_ = FrameDecoder{};
    writer_ = WriteBuffer{};
}

void TunnelClient::HandleTunnelWritable() {
    if (!connected_) {
        int err = 0;
        socklen_t len = sizeof(err);
        if (getsockopt(tunnel_fd_, SOL_SOCKET, SO_ERROR, &err, &len) < 0 || err != 0) {
            LOG_ERROR("connect failed: %s", strerror(err ? err : errno));
            Disconnect();
            return;
        }
        connected_ = true;
        LOG_INFO("connected to server %s:%u (fd=%d)", server_ip_.c_str(),
                 server_port_, tunnel_fd_);
        last_recv_heartbeat_ = time(nullptr);

        // 连接成功, 立即发送端口映射 + 首个心跳
        SendPortMap();
        SendHeartbeat();
    }

    if (writer_.Flush(tunnel_fd_)) {
        epoll_event ev{};
        ev.events = EPOLLIN | EPOLLET;
        ev.data.fd = tunnel_fd_;
        epoll_ctl(epfd_, EPOLL_CTL_MOD, tunnel_fd_, &ev);
    }
}

void TunnelClient::HandleTunnelReadable() {
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
        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
        if (errno == EINTR) continue;
        LOG_ERROR("read tunnel failed: %s", strerror(errno));
        Disconnect();
        return;
    }

    last_recv_heartbeat_ = time(nullptr);
    auto frames = decoder_.Output();
    for (const auto& f : frames) {
        HandleFrame(f);
        if (tunnel_fd_ < 0) break;  // HandleFrame 可能触发 Disconnect
    }
}

void TunnelClient::HandleFrame(const Frame& frame) {
    switch (frame.type) {
        case MessageType::HEARTBEAT:
            LOG_DEBUG("recv HEARTBEAT from server");
            break;

        case MessageType::NEW_CONN:
            // server 通知: 有外部用户连到了映射端口, 需要连本地服务
            // payload: { uint32 session_id; uint16 local_port; }
            if (frame.payload.size() < 6) {
                LOG_WARN("NEW_CONN payload too short");
                break;
            }
            {
                uint32_t sid = ntohl(*reinterpret_cast<const uint32_t*>(&frame.payload[0]));
                uint16_t local_port = ntohs(*reinterpret_cast<const uint16_t*>(&frame.payload[4]));
                HandleNewConn(sid, local_port);
            }
            break;

        case MessageType::DATA:
            // server 转发来的外部用户数据
            // payload: { uint32 session_id; uint16 data_len; char data[data_len]; }
            if (frame.payload.size() < 6) {
                LOG_WARN("DATA payload too short");
                break;
            }
            {
                uint32_t sid = ntohl(*reinterpret_cast<const uint32_t*>(&frame.payload[0]));
                uint16_t dlen = ntohs(*reinterpret_cast<const uint16_t*>(&frame.payload[4]));
                if (frame.payload.size() < 6 + dlen) {
                    LOG_WARN("DATA payload truncated");
                    break;
                }
                auto it = sessions_.find(sid);
                if (it == sessions_.end()) {
                    LOG_WARN("DATA for unknown session_id=%u", sid);
                    break;
                }
                const char* data = &frame.payload[6];
                ssize_t w = write(it->second, data, dlen);
                if (w < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
                    LOG_ERROR("write local_fd=%d failed: %s", it->second, strerror(errno));
                    CloseLocal(sid);
                }
            }
            break;

        case MessageType::CLOSE:
            // server 通知关闭某 session (外部用户断开)
            if (frame.payload.size() < 4) break;
            {
                uint32_t sid = ntohl(*reinterpret_cast<const uint32_t*>(&frame.payload[0]));
                CloseLocal(sid);
            }
            break;

        default:
            LOG_INFO("recv %s (payload_len=%zu), not handled",
                     msg_type_str(frame.type), frame.payload.size());
            break;
    }
}

void TunnelClient::SendPortMap() {
    if (mappings_.empty()) return;
    // payload: { uint8 count; repeat(count) { uint16 local_port; uint16 remote_port; } }
    FrameBuilder builder(MessageType::PORT_MAP);
    builder.AppendU8(static_cast<uint8_t>(mappings_.size()));
    for (auto& m : mappings_) {
        builder.AppendU16(m.first);   // local_port
        builder.AppendU16(m.second);  // remote_port
    }
    std::string frame = builder.Build();
    writer_.Append(std::move(frame));
    writer_.Flush(tunnel_fd_);
    LOG_INFO("sent PORT_MAP (%zu mappings)", mappings_.size());
    for (auto& m : mappings_) {
        LOG_INFO("  local:%u -> remote:%u", m.first, m.second);
    }
}

void TunnelClient::HandleNewConn(uint32_t session_id, uint16_t local_port) {
    // 连接本地服务
    bool immediately = false;
    int local_fd = net::create_connect_socket("127.0.0.1", local_port, &immediately);
    if (local_fd < 0) {
        LOG_ERROR("connect to local service 127.0.0.1:%u failed for session %u",
                  local_port, session_id);
        // 通知 server 关闭该 session
        std::string frame = FrameBuilder(MessageType::CLOSE)
                                .AppendU32(session_id)
                                .Build();
        writer_.Append(std::move(frame));
        writer_.Flush(tunnel_fd_);
        return;
    }
    net::set_nonblock(local_fd);

    // 如果连接未立即完成, 需要等可写事件
    if (!immediately) {
        epoll_event ev{};
        ev.events = EPOLLIN | EPOLLOUT | EPOLLET;
        ev.data.fd = local_fd;
        epoll_ctl(epfd_, EPOLL_CTL_ADD, local_fd, &ev);
    } else {
        epoll_event ev{};
        ev.events = EPOLLIN | EPOLLET;
        ev.data.fd = local_fd;
        epoll_ctl(epfd_, EPOLL_CTL_ADD, local_fd, &ev);
    }

    sessions_[session_id] = local_fd;
    local_to_session_[local_fd] = session_id;
    LOG_INFO("session %u: connected to local 127.0.0.1:%u (local_fd=%d)",
             session_id, local_port, local_fd);
}

void TunnelClient::HandleLocalReadable(int local_fd) {
    char buf[65536];
    for (;;) {
        ssize_t n = read(local_fd, buf, sizeof(buf));
        if (n > 0) {
            auto it = local_to_session_.find(local_fd);
            if (it == local_to_session_.end()) {
                LOG_WARN("local_fd=%d has no session", local_fd);
                close(local_fd);
                epoll_ctl(epfd_, EPOLL_CTL_DEL, local_fd, nullptr);
                return;
            }
            uint32_t sid = it->second;
            std::string frame = FrameBuilder(MessageType::DATA)
                                    .AppendU32(sid)
                                    .AppendU16(static_cast<uint16_t>(n))
                                    .AppendBytes(buf, static_cast<size_t>(n))
                                    .Build();
            writer_.Append(std::move(frame));
            if (!writer_.Flush(tunnel_fd_)) {
                epoll_event ev{};
                ev.events = EPOLLIN | EPOLLOUT | EPOLLET;
                ev.data.fd = tunnel_fd_;
                epoll_ctl(epfd_, EPOLL_CTL_MOD, tunnel_fd_, &ev);
            }
            continue;
        }
        if (n == 0) {
            // 本地服务断开
            auto it = local_to_session_.find(local_fd);
            if (it != local_to_session_.end()) {
                uint32_t sid = it->second;
                // 通知 server
                std::string frame = FrameBuilder(MessageType::CLOSE)
                                        .AppendU32(sid)
                                        .Build();
                writer_.Append(std::move(frame));
                writer_.Flush(tunnel_fd_);
                sessions_.erase(sid);
                local_to_session_.erase(local_fd);
            }
            epoll_ctl(epfd_, EPOLL_CTL_DEL, local_fd, nullptr);
            close(local_fd);
            LOG_DEBUG("local_fd=%d closed (local service disconnected)", local_fd);
            return;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
        if (errno == EINTR) continue;
        LOG_ERROR("read local_fd=%d failed: %s", local_fd, strerror(errno));
        // 出错, 关闭
        auto it = local_to_session_.find(local_fd);
        if (it != local_to_session_.end()) {
            std::string frame = FrameBuilder(MessageType::CLOSE)
                                    .AppendU32(it->second)
                                    .Build();
            writer_.Append(std::move(frame));
            writer_.Flush(tunnel_fd_);
            sessions_.erase(it->second);
            local_to_session_.erase(local_fd);
        }
        epoll_ctl(epfd_, EPOLL_CTL_DEL, local_fd, nullptr);
        close(local_fd);
        return;
    }
}

void TunnelClient::CloseLocal(uint32_t session_id) {
    auto it = sessions_.find(session_id);
    if (it == sessions_.end()) return;
    int local_fd = it->second;
    sessions_.erase(it);
    local_to_session_.erase(local_fd);
    epoll_ctl(epfd_, EPOLL_CTL_DEL, local_fd, nullptr);
    close(local_fd);
    LOG_DEBUG("closed local_fd=%d for session %u", local_fd, session_id);
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
    bool need_backoff = false;
    for (;;) {
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
            int fd = events[i].data.fd;
            if (events[i].events & (EPOLLERR | EPOLLHUP)) {
                if (fd == tunnel_fd_) {
                    LOG_WARN("epoll error/hup on tunnel");
                    Disconnect();
                    broke = true;
                    break;
                }
                // local_fd error
                LOG_WARN("epoll error/hup on local_fd=%d", fd);
                auto it = local_to_session_.find(fd);
                if (it != local_to_session_.end()) {
                    std::string frame = FrameBuilder(MessageType::CLOSE)
                                            .AppendU32(it->second)
                                            .Build();
                    writer_.Append(std::move(frame));
                    writer_.Flush(tunnel_fd_);
                    sessions_.erase(it->second);
                    local_to_session_.erase(fd);
                }
                epoll_ctl(epfd_, EPOLL_CTL_DEL, fd, nullptr);
                close(fd);
                continue;
            }
            if (fd == tunnel_fd_) {
                if (events[i].events & EPOLLIN) {
                    HandleTunnelReadable();
                }
                if (tunnel_fd_ >= 0 && (events[i].events & EPOLLOUT)) {
                    HandleTunnelWritable();
                }
                if (tunnel_fd_ < 0) {
                    broke = true;
                    break;
                }
            } else {
                // local_fd
                if (events[i].events & EPOLLIN) {
                    HandleLocalReadable(fd);
                }
                // TODO: 如果 local_fd 也是非阻塞 connect (未立即完成),
                //       需要在 EPOLLOUT 时确认连接完成。这里简化处理,
                //       本地服务一般在同一台机器, connect 会立即成功。
            }
        }
        if (broke) {
            need_backoff = true;
            continue;
        }

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
