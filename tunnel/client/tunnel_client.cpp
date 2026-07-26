#include "tunnel_client.h"

#include <sys/epoll.h>
#include <unistd.h>

#include <cerrno>
#include <ctime>

#include "../common/logger.h"
#include "../common/netutil.h"
#include "../common/tun.h"

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
                           const std::vector<PortMapping>& mappings,
                           const std::string& name,
                           const std::string& auto_connect,
                           const std::vector<LocalRelayConfig>& relay_listens,
                           const std::string& token,
                           const std::string& tun_ip)
    : server_ip_(server_ip), server_port_(server_port),
      mappings_(mappings), name_(name), auto_connect_target_(auto_connect),
      relay_listens_(relay_listens), token_(token), tun_ip_(tun_ip),
      tunnel_fd_(-1), epfd_(-1), connected_(false),
      last_send_heartbeat_(0), last_recv_heartbeat_(0), active_relay_sid_(0) {}

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
    // 关闭所有 local_fd (端口映射)
    for (auto& kv : sessions_) {
        epoll_ctl(epfd_, EPOLL_CTL_DEL, kv.second, nullptr);
        close(kv.second);
    }
    sessions_.clear();
    local_to_session_.clear();

    // 清理 stdin 监听 (如果 relay session 活跃)
    if (active_relay_sid_ > 0) {
        epoll_ctl(epfd_, EPOLL_CTL_DEL, 0, nullptr);
        active_relay_sid_ = 0;
    }

    // 关闭中继监听 fd
    for (auto& kv : relay_listen_fds_) {
        epoll_ctl(epfd_, EPOLL_CTL_DEL, kv.first, nullptr);
        close(kv.first);
    }
    relay_listen_fds_.clear();

    // 关闭 TUN 设备
    if (tun_fd_ >= 0) {
        epoll_ctl(epfd_, EPOLL_CTL_DEL, tun_fd_, nullptr);
        close(tun_fd_);
        tun_fd_ = -1;
    }

    // 清理 pending relay user fd
    if (pending_relay_user_fd_ >= 0) {
        close(pending_relay_user_fd_);
        pending_relay_user_fd_ = -1;
        pending_relay_target_.clear();
    }

    // 清理 P2P sockets
    for (auto& kv : p2p_states_) {
        epoll_ctl(epfd_, EPOLL_CTL_DEL, kv.second.p2p_fd, nullptr);
        close(kv.second.p2p_fd);
    }
    p2p_states_.clear();
    p2p_to_session_.clear();

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

        // 连接成功, 先鉴权, 再注册, 再端口映射, 设置本地中继, 最后心跳
        if (!token_.empty()) {
            SendAuth();
        }
        if (!name_.empty()) {
            SendRegister();
        }
        SendPortMap();
        SetupLocalRelayListeners();
        SetupTun();
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
            // server 通知: { uint32 session_id; uint16 local_port; }
            //   local_port>0: 连本地服务 (外部用户或中继目标)
            //   local_port=0: 中继 ACK (组网连接已建立)
            if (frame.payload.size() < 6) {
                LOG_WARN("NEW_CONN payload too short");
                break;
            }
            {
                uint32_t sid = ntohl(*reinterpret_cast<const uint32_t*>(&frame.payload[0]));
                uint16_t local_port = ntohs(*reinterpret_cast<const uint16_t*>(&frame.payload[4]));
                if (local_port == 0) {
                    // 中继 ACK
                    if (pending_relay_user_fd_ >= 0) {
                        // 来自本地监听中继: 关联 sid -> user_fd
                        sessions_[sid] = pending_relay_user_fd_;
                        local_to_session_[pending_relay_user_fd_] = sid;
                        LOG_INFO("relay session %u established -> local user_fd=%d (target=%s)",
                                 sid, pending_relay_user_fd_, pending_relay_target_.c_str());
                        // 发送 pending 期间积压的数据
                        if (!pending_relay_buf_.empty()) {
                            std::string frame = FrameBuilder(MessageType::DATA)
                                                    .AppendU32(sid)
                                                    .AppendU16(static_cast<uint16_t>(pending_relay_buf_.size()))
                                                    .AppendBytes(pending_relay_buf_.data(), pending_relay_buf_.size())
                                                    .Build();
                            writer_.Append(std::move(frame));
                            writer_.Flush(tunnel_fd_);
                            LOG_DEBUG("flushed %zu buffered bytes to relay session %u",
                                     pending_relay_buf_.size(), sid);
                            pending_relay_buf_.clear();
                        }
                        pending_relay_user_fd_ = -1;
                        pending_relay_target_.clear();
                    } else {
                        // 来自 stdin 命令: 进入 stdin 中继模式
                        LOG_INFO("relay session %u established, stdin ↔ relay active", sid);
                        active_relay_sid_ = sid;
                        epoll_event ev{};
                        ev.events = EPOLLIN;
                        ev.data.fd = 0;
                        epoll_ctl(epfd_, EPOLL_CTL_ADD, 0, &ev);
                        LOG_INFO("type/paste data and press Enter to send to relay session %u", sid);
                    }
                } else {
                    HandleNewConn(sid, local_port);
                }
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
                // 检查是否是中继 session (数据来自另一个 client)
                if (sid == active_relay_sid_) {
                    const char* data = &frame.payload[6];
                    HandleRelayData(sid, data, dlen);
                    break;
                }
                // 否则是本地服务 session
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
            // server 通知关闭某 session (外部用户或中继断开)
            if (frame.payload.size() < 4) break;
            {
                uint32_t sid = ntohl(*reinterpret_cast<const uint32_t*>(&frame.payload[0]));
                if (sid == active_relay_sid_) {
                    LOG_INFO("relay session %u closed by peer", sid);
                    // 从 epoll 移除 stdin
                    epoll_ctl(epfd_, EPOLL_CTL_DEL, 0, nullptr);
                    active_relay_sid_ = 0;
                    break;
                }
                CloseLocal(sid);
            }
            break;

        case MessageType::ACK:
            // server 对 REGISTER 的应答: { uint8 code; }  0=成功, 1=名字被占用
            if (frame.payload.size() < 1) {
                LOG_WARN("ACK payload too short");
                break;
            }
            {
                uint8_t code = static_cast<uint8_t>(frame.payload[0]);
                if (code == 0) {
                    LOG_INFO("REGISTER success (name='%s')", name_.c_str());
                    // 注册成功后, 如果有自动中继目标, 发起连接
                    if (!auto_connect_target_.empty()) {
                        auto colon = auto_connect_target_.find(':');
                        if (colon != std::string::npos) {
                            std::string target = auto_connect_target_.substr(0, colon);
                            int port = std::atoi(auto_connect_target_.substr(colon+1).c_str());
                            if (port > 0 && port <= 65535) {
                                SendRelayNewConn(target, static_cast<uint16_t>(port));
                            }
                        }
                    }
                } else {
                    LOG_WARN("REGISTER failed: name '%s' already used by another client",
                             name_.c_str());
                }
            }
            break;

        case MessageType::PROBE:
            // server 转发来的探活请求 (来自另一个 client): 原 payload 不变
            // payload: { uint8 target_name_len; char target_name[]; uint32 probe_id; }
            if (frame.payload.size() < 1) {
                LOG_WARN("PROBE payload too short");
                break;
            }
            {
                uint8_t name_len = static_cast<uint8_t>(frame.payload[0]);
                if (frame.payload.size() < 1 + name_len + 4) {
                    LOG_WARN("PROBE payload truncated");
                    break;
                }
                // 提取 source_name (即发起方, 这里 payload 没带, 用 target_name 占位)
                // 实际上 PROBE 的 payload 不包含 source_name, 只有 target_name 和 probe_id
                uint32_t probe_id = ntohl(*reinterpret_cast<const uint32_t*>(
                    &frame.payload[1 + name_len]));
                HandleProbe(probe_id, "");  // source_name 暂不需要
            }
            break;

        case MessageType::PROBE_REPLY:
            // server 转发回来的探活应答: { uint32 probe_id; uint8 status; }
            if (frame.payload.size() < 5) {
                LOG_WARN("PROBE_REPLY payload too short");
                break;
            }
            {
                uint32_t probe_id = ntohl(*reinterpret_cast<const uint32_t*>(&frame.payload[0]));
                uint8_t status = static_cast<uint8_t>(frame.payload[4]);
                HandleProbeReply(probe_id, status);
            }
            break;

        case MessageType::P2P_TRY:
            // server 通知尝试 P2P: { uint32 session_id; }
            if (frame.payload.size() < 4) break;
            HandleP2pTry(ntohl(*reinterpret_cast<const uint32_t*>(&frame.payload[0])));
            break;

        case MessageType::P2P_INFO:
            // server 交换对端信息: { uint32 session_id; uint32 peer_ip; uint16 peer_port; }
            if (frame.payload.size() < 10) break;
            {
                uint32_t sid = ntohl(*reinterpret_cast<const uint32_t*>(&frame.payload[0]));
                uint32_t ip  = ntohl(*reinterpret_cast<const uint32_t*>(&frame.payload[4]));
                uint16_t port = ntohs(*reinterpret_cast<const uint16_t*>(&frame.payload[8]));
                HandleP2pInfo(sid, ip, port);
            }
            break;

        case MessageType::TUN_PACKET:
            if (tun_fd_ >= 0) {
                write(tun_fd_, frame.payload.data(), frame.payload.size());
            }
            break;

        default:
            LOG_INFO("recv %s (payload_len=%zu), not handled",
                     msg_type_str(frame.type), frame.payload.size());
            break;
    }
}

void TunnelClient::SendAuth() {
    if (token_.empty()) return;
    FrameBuilder builder(MessageType::AUTH);
    builder.AppendU8(static_cast<uint8_t>(token_.size()));
    builder.AppendStr(token_);
    std::string frame = builder.Build();
    writer_.Append(std::move(frame));
    writer_.Flush(tunnel_fd_);
    LOG_INFO("sent AUTH");
}

void TunnelClient::SendRegister() {
    if (name_.empty()) return;
    // payload: { uint8 name_len; char name[name_len]; uint32 ip; }
    FrameBuilder builder(MessageType::REGISTER);
    builder.AppendU8(static_cast<uint8_t>(name_.size()));
    builder.AppendStr(name_);
    // 解析 IP 字符串为 uint32
    uint32_t ip = 0;
    if (!tun_ip_.empty()) {
        inet_pton(AF_INET, tun_ip_.c_str(), &ip);
    }
    builder.AppendU32(ntohl(ip));  // 已经是 host byte order, encode_frame_header 会转
    std::string frame = builder.Build();
    writer_.Append(std::move(frame));
    writer_.Flush(tunnel_fd_);
    LOG_INFO("sent REGISTER (name='%s', ip=%s)", name_.c_str(), tun_ip_.c_str());
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
            // P2P 活跃? 直接写到 P2P socket, 不走 tunnel
            auto pit = p2p_states_.find(sid);
            if (pit != p2p_states_.end() && pit->second.connected) {
                pit->second.write_buf.append(buf, static_cast<size_t>(n));
                // 尝试刷新
                ssize_t w = write(pit->second.p2p_fd, pit->second.write_buf.data(),
                                  pit->second.write_buf.size());
                if (w > 0) pit->second.write_buf.erase(0, w);
                if (w < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
                    LOG_ERROR("P2P write local_fd=%d -> p2p_fd=%d failed: %s",
                              local_fd, pit->second.p2p_fd, strerror(errno));
                }
                continue;  // 无论写成功与否, 都已消费了本地数据
            }
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

void TunnelClient::SendProbe(const std::string& target_name, uint32_t probe_id) {
    if (!connected_) return;
    // payload: { uint8 target_name_len; char target_name[]; uint32 probe_id; }
    FrameBuilder builder(MessageType::PROBE);
    builder.AppendU8(static_cast<uint8_t>(target_name.size()));
    builder.AppendStr(target_name);
    builder.AppendU32(probe_id);
    std::string frame = builder.Build();
    writer_.Append(std::move(frame));
    writer_.Flush(tunnel_fd_);
    LOG_INFO("sent PROBE to '%s' (probe_id=%u)", target_name.c_str(), probe_id);
}

void TunnelClient::CheckHeartbeatTimeout() {
    if (!connected_) return;
    time_t now = time(nullptr);
    if (now - last_recv_heartbeat_ > kHeartbeatTimeoutSec) {
        LOG_WARN("heartbeat timeout (>%ds), reconnecting", kHeartbeatTimeoutSec);
        Disconnect();
    }
}

void TunnelClient::HandleProbe(uint32_t probe_id, const std::string& source_name) {
    // 收到探活请求, 回复 PROBE_REPLY
    // payload: { uint32 probe_id; uint8 status; }  status=0 表示在线
    FrameBuilder builder(MessageType::PROBE_REPLY);
    builder.AppendU32(probe_id);
    builder.AppendU8(0);  // status=0, 表示在线
    std::string frame = builder.Build();
    writer_.Append(std::move(frame));
    writer_.Flush(tunnel_fd_);
    LOG_INFO("recv PROBE (probe_id=%u), replied PROBE_REPLY", probe_id);
}

void TunnelClient::HandleProbeReply(uint32_t probe_id, uint8_t status) {
    // 收到探活应答
    if (status == 0) {
        LOG_INFO("PROBE_REPLY: probe_id=%u, target is online", probe_id);
    } else {
        LOG_WARN("PROBE_REPLY: probe_id=%u, target not found", probe_id);
    }
}

void TunnelClient::SendRelayNewConn(const std::string& target_name, uint16_t target_port) {
    if (!connected_) return;
    FrameBuilder builder(MessageType::NEW_CONN);
    builder.AppendU32(0);  // session_id=0 表示新的组网请求
    builder.AppendU16(static_cast<uint16_t>(target_name.size()));
    builder.AppendStr(target_name);
    builder.AppendU16(target_port);
    std::string frame = builder.Build();
    writer_.Append(std::move(frame));
    writer_.Flush(tunnel_fd_);
    LOG_INFO("sent relay NEW_CONN to '%s':%u", target_name.c_str(), target_port);
}

void TunnelClient::HandleStdinReadable() {
    char buf[4096];
    ssize_t n = read(0, buf, sizeof(buf));
    if (n <= 0) {
        if (n == 0) {
            LOG_INFO("stdin closed (EOF)");
        } else if (errno != EAGAIN && errno != EINTR) {
            LOG_ERROR("stdin read error: %s", strerror(errno));
        }
        if (active_relay_sid_ > 0) {
            std::string frame = FrameBuilder(MessageType::CLOSE)
                                    .AppendU32(active_relay_sid_)
                                    .Build();
            writer_.Append(std::move(frame));
            writer_.Flush(tunnel_fd_);
            epoll_ctl(epfd_, EPOLL_CTL_DEL, 0, nullptr);
            active_relay_sid_ = 0;
        }
        return;
    }
    while (n > 0 && (buf[n-1] == '\n' || buf[n-1] == '\r')) --n;
    if (n == 0) return;

    if (active_relay_sid_ > 0) {
        std::string frame = FrameBuilder(MessageType::DATA)
                                .AppendU32(active_relay_sid_)
                                .AppendU16(static_cast<uint16_t>(n))
                                .AppendBytes(buf, static_cast<size_t>(n))
                                .Build();
        writer_.Append(std::move(frame));
        writer_.Flush(tunnel_fd_);
        LOG_DEBUG("relay data (%zu bytes) -> session %u", n, active_relay_sid_);
    } else {
        buf[n] = '\0';
        std::string cmd(buf);
        if (cmd == "quit" || cmd == "exit") {
            LOG_INFO("exit by user command");
            Disconnect();
            return;
        }
        if (cmd.find("connect ") == 0) {
            auto rest = cmd.substr(8);
            auto space = rest.find(' ');
            if (space != std::string::npos) {
                std::string target = rest.substr(0, space);
                int port = std::atoi(rest.substr(space + 1).c_str());
                if (port > 0 && port <= 65535) {
                    SendRelayNewConn(target, static_cast<uint16_t>(port));
                } else {
                    LOG_WARN("invalid port: %s", rest.substr(space + 1).c_str());
                }
            } else {
                LOG_WARN("usage: connect <target_name> <port>");
            }
        } else {
            LOG_WARN("unknown cmd: '%s' (try: connect <name> <port>, quit, exit)", cmd.c_str());
        }
    }
}

void TunnelClient::HandleRelayData(uint32_t sid, const char* data, uint16_t dlen) {
    ssize_t w = write(1, data, dlen);
    if (w < 0) {
        LOG_ERROR("write relay data to stdout failed: %s", strerror(errno));
    }
    write(1, "\n", 1);
    LOG_DEBUG("relay data (%u bytes) from session %u written to stdout", dlen, sid);
}

void TunnelClient::HandleP2pTry(uint32_t session_id) {
    LOG_INFO("P2P_TRY sid=%u, creating P2P socket", session_id);
    int p2p_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (p2p_fd < 0) {
        LOG_ERROR("P2P socket() failed: %s", strerror(errno));
        return;
    }
    net::set_nonblock(p2p_fd);
    net::set_reuse_addr(p2p_fd);
    // bind 到随机端口
    sockaddr_in local{};
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = INADDR_ANY;
    local.sin_port = 0;  // 让内核选端口
    if (bind(p2p_fd, (sockaddr*)&local, sizeof(local)) < 0) {
        LOG_ERROR("P2P bind failed: %s", strerror(errno));
        close(p2p_fd);
        return;
    }
    // 获取分配的端口
    sockaddr_in addr{};
    socklen_t len = sizeof(addr);
    getsockname(p2p_fd, (sockaddr*)&addr, &len);
    uint16_t local_port = ntohs(addr.sin_port);

    // 上报端口给 server
    std::string report = FrameBuilder(MessageType::P2P_PORT)
                             .AppendU32(session_id)
                             .AppendU16(local_port)
                             .Build();
    writer_.Append(std::move(report));
    writer_.Flush(tunnel_fd_);
    LOG_INFO("P2P_PORT sid=%u local_port=%u", session_id, local_port);

    // 记录 P2P 状态
    P2pState st;
    st.p2p_fd = p2p_fd;
    st.start_time = time(nullptr);
    p2p_states_[session_id] = st;
    p2p_to_session_[p2p_fd] = session_id;
    // 先不加入 epoll, 等 HandleP2pInfo 中 connect() 后再加
}

void TunnelClient::HandleP2pInfo(uint32_t session_id, uint32_t peer_ip, uint16_t peer_port) {
    auto it = p2p_states_.find(session_id);
    if (it == p2p_states_.end()) {
        LOG_WARN("P2P_INFO for unknown session %u", session_id);
        return;
    }
    P2pState& st = it->second;
    struct in_addr ip_addr;
    ip_addr.s_addr = peer_ip;
    LOG_INFO("P2P_INFO sid=%u peer=%s:%u, connecting",
             session_id, inet_ntoa(ip_addr), peer_port);

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = peer_ip;
    addr.sin_port = htons(peer_port);
    int ret = connect(st.p2p_fd, (sockaddr*)&addr, sizeof(addr));
    if (ret < 0 && errno != EINPROGRESS) {
        // TCP simultaneous open 有时序问题, 快速重试几次
        bool ok = false;
        for (int try_n = 0; try_n < 5; ++try_n) {
            usleep(20000);  // 20ms
            ret = connect(st.p2p_fd, (sockaddr*)&addr, sizeof(addr));
            if (ret == 0 || (ret < 0 && errno == EINPROGRESS)) { ok = true; break; }
            if (ret < 0 && errno == EALREADY) { ok = true; break; }
            // 关闭旧 fd 重建
            close(st.p2p_fd);
            st.p2p_fd = socket(AF_INET, SOCK_STREAM, 0);
            net::set_nonblock(st.p2p_fd);
            net::set_reuse_addr(st.p2p_fd);
            p2p_to_session_.erase(session_id);
            p2p_to_session_[st.p2p_fd] = session_id;
        }
        if (!ok) {
            LOG_ERROR("P2P connect failed after retries: %s", strerror(errno));
            std::string fail = FrameBuilder(MessageType::P2P_FAIL)
                                   .AppendU32(session_id).Build();
            writer_.Append(std::move(fail));
            writer_.Flush(tunnel_fd_);
            epoll_ctl(epfd_, EPOLL_CTL_DEL, st.p2p_fd, nullptr);
            p2p_to_session_.erase(st.p2p_fd);
            close(st.p2p_fd);
            p2p_states_.erase(it);
            return;
        }
    }
    // connect() 已发起, 加入 epoll 监听可写(连接完成) + 可读
    epoll_event ev{};
    ev.events = EPOLLIN | EPOLLOUT | EPOLLET;
    ev.data.fd = st.p2p_fd;
    epoll_ctl(epfd_, EPOLL_CTL_ADD, st.p2p_fd, &ev);
    // EINPROGRESS → 等待 EPOLLOUT
}

void TunnelClient::HandleP2pReadable(int p2p_fd) {
    auto it = p2p_to_session_.find(p2p_fd);
    if (it == p2p_to_session_.end()) return;
    uint32_t sid = it->second;
    // 查找对应的 local_fd (sessions_[sid])
    auto sit = sessions_.find(sid);
    if (sit == sessions_.end()) {
        // relay 模式下 sessions_ 存的是 user_fd (发起方) 或 local_fd (目标方)
        LOG_WARN("P2P readable for session %u but no local_fd/relay_fd", sid);
        return;
    }
    char buf[65536];
    for (;;) {
        ssize_t n = read(p2p_fd, buf, sizeof(buf));
        if (n > 0) {
            ssize_t w = write(sit->second, buf, static_cast<size_t>(n));
            if (w < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
                LOG_ERROR("P2P write to target fd=%d failed: %s", sit->second, strerror(errno));
            }
            continue;
        }
        if (n == 0) {
            LOG_INFO("P2P fd=%d closed (session %u)", p2p_fd, sid);
            epoll_ctl(epfd_, EPOLL_CTL_DEL, p2p_fd, nullptr);
            close(p2p_fd);
            p2p_to_session_.erase(p2p_fd);
            p2p_states_.erase(sid);
            // 通知 server 回退中继
            std::string fail = FrameBuilder(MessageType::P2P_FAIL)
                                   .AppendU32(sid).Build();
            writer_.Append(std::move(fail));
            writer_.Flush(tunnel_fd_);
            return;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
        if (errno == EINTR) continue;
        LOG_ERROR("P2P read fd=%d failed: %s", p2p_fd, strerror(errno));
        break;
    }
}

void TunnelClient::SetupLocalRelayListeners() {
    if (relay_listens_.empty()) return;
    // 需要在 Connect() 之后调用, 此时 epfd_ 已就绪
    for (auto& cfg : relay_listens_) {
        int lfd = net::create_listen_socket(cfg.local_port);
        if (lfd < 0) {
            LOG_ERROR("failed to listen on local port %u for relay to %s:%u",
                      cfg.local_port, cfg.target_name.c_str(), cfg.target_port);
            continue;
        }
        epoll_event ev{};
        ev.events = EPOLLIN | EPOLLET;
        ev.data.fd = lfd;
        if (epoll_ctl(epfd_, EPOLL_CTL_ADD, lfd, &ev) < 0) {
            LOG_ERROR("epoll_ctl add relay listen fd=%d failed", lfd);
            close(lfd);
            continue;
        }
        relay_listen_fds_[lfd] = cfg;
        LOG_INFO("local relay listen 127.0.0.1:%u -> relay to '%s':%u",
                 cfg.local_port, cfg.target_name.c_str(), cfg.target_port);
    }
}

void TunnelClient::SetupTun() {
    if (tun_ip_.empty()) return;
    tun_fd_ = tun::create("tun%d");
    if (tun_fd_ < 0) {
        LOG_ERROR("TUN device creation failed, TUN disabled");
        tun_ip_.clear();
        return;
    }
    // 获取设备名
    ifreq ifr{};
    std::strncpy(ifr.ifr_name, "tun%d", IFNAMSIZ - 1);
    ioctl(tun_fd_, TUNGETIFF, &ifr);
    std::string dev_name = ifr.ifr_name;
    net::set_nonblock(tun_fd_);

    // 配置 IP + 路由 (需要 root)
    tun::set_ip(dev_name, tun_ip_, 24);
    tun::add_route("10.0.0.0/24", dev_name);

    epoll_event ev{};
    ev.events = EPOLLIN | EPOLLET;
    ev.data.fd = tun_fd_;
    epoll_ctl(epfd_, EPOLL_CTL_ADD, tun_fd_, &ev);
    LOG_INFO("TUN ready: %s -> %s", dev_name.c_str(), tun_ip_.c_str());
}

void TunnelClient::HandleTunReadable() {
    if (tun_fd_ < 0) return;
    char buf[65536];
    for (;;) {
        ssize_t n = read(tun_fd_, buf, sizeof(buf));
        if (n > 0) {
            std::string frame = FrameBuilder(MessageType::TUN_PACKET)
                                    .AppendBytes(buf, static_cast<size_t>(n))
                                    .Build();
            writer_.Append(std::move(frame));
            writer_.Flush(tunnel_fd_);
            continue;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
        if (errno == EINTR) continue;
        LOG_ERROR("read TUN fd failed: %s", strerror(errno));
        break;
    }
}

void TunnelClient::HandleLocalRelayAccept(int listen_fd) {
    auto it = relay_listen_fds_.find(listen_fd);
    if (it == relay_listen_fds_.end()) return;
    for (;;) {
        int user_fd = accept(listen_fd, nullptr, nullptr);
        if (user_fd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            if (errno == EINTR) continue;
            LOG_ERROR("local relay accept failed: %s", strerror(errno));
            break;
        }
        net::set_nonblock(user_fd);
        epoll_event ev{};
        ev.events = EPOLLIN | EPOLLET;  // ET mode, pending 数据由 pending_relay_buf_ 处理
        ev.data.fd = user_fd;
        if (epoll_ctl(epfd_, EPOLL_CTL_ADD, user_fd, &ev) < 0) {
            LOG_ERROR("epoll_ctl add relay user_fd=%d failed", user_fd);
            close(user_fd);
            continue;
        }
        // 暂存 pending, 发起中继连接
        pending_relay_user_fd_ = user_fd;
        pending_relay_target_ = it->second.target_name;
        SendRelayNewConn(it->second.target_name, it->second.target_port);
        LOG_INFO("local relay user connected fd=%d, requesting relay to '%s':%u",
                 user_fd, it->second.target_name.c_str(), it->second.target_port);
    }
}

void TunnelClient::HandleLocalRelayUserReadable(int user_fd) {
    auto it = local_to_session_.find(user_fd);
    bool is_pending = (it == local_to_session_.end());

    char buf[65536];
    for (;;) {
        ssize_t n = read(user_fd, buf, sizeof(buf));
        if (n > 0) {
            if (is_pending) {
                // 中继 ACK 还没到, 先缓存起来
                pending_relay_buf_.append(buf, static_cast<size_t>(n));
                continue;
            }
            // 转发到中继 session (或 P2P 直连)
            uint32_t sid = it->second;
            // P2P 活跃? 直接写到 P2P socket, 不走 tunnel
            auto pit = p2p_states_.find(sid);
            if (pit != p2p_states_.end() && pit->second.connected) {
                pit->second.write_buf.append(buf, static_cast<size_t>(n));
                ssize_t w = write(pit->second.p2p_fd, pit->second.write_buf.data(),
                                  pit->second.write_buf.size());
                if (w > 0) pit->second.write_buf.erase(0, w);
                if (w < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
                    LOG_ERROR("P2P write user_fd=%d -> p2p_fd=%d failed: %s",
                              user_fd, pit->second.p2p_fd, strerror(errno));
                }
                continue;
            }
            std::string frame = FrameBuilder(MessageType::DATA)
                                    .AppendU32(sid)
                                    .AppendU16(static_cast<uint16_t>(n))
                                    .AppendBytes(buf, static_cast<size_t>(n))
                                    .Build();
            writer_.Append(std::move(frame));
            writer_.Flush(tunnel_fd_);
            continue;
        }
        if (n == 0) {
            if (is_pending) {
                LOG_INFO("local relay user fd=%d disconnected before session established", user_fd);
                pending_relay_user_fd_ = -1;
                pending_relay_target_.clear();
                pending_relay_buf_.clear();
            } else {
                uint32_t sid = it->second;
                LOG_INFO("local relay user fd=%d disconnected (session %u)", user_fd, sid);
                std::string frame = FrameBuilder(MessageType::CLOSE)
                                        .AppendU32(sid).Build();
                writer_.Append(std::move(frame));
                writer_.Flush(tunnel_fd_);
                sessions_.erase(sid);
                local_to_session_.erase(user_fd);
            }
            epoll_ctl(epfd_, EPOLL_CTL_DEL, user_fd, nullptr);
            close(user_fd);
            return;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
        if (errno == EINTR) continue;
        LOG_ERROR("read local relay user_fd=%d failed: %s", user_fd, strerror(errno));
        if (!is_pending) {
            uint32_t sid = it->second;
            sessions_.erase(sid);
            local_to_session_.erase(user_fd);
        } else {
            pending_relay_user_fd_ = -1;
            pending_relay_target_.clear();
            pending_relay_buf_.clear();
        }
        epoll_ctl(epfd_, EPOLL_CTL_DEL, user_fd, nullptr);
        close(user_fd);
        return;
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
                if (fd == 0) {
                    if (events[i].events & EPOLLIN) {
                        HandleStdinReadable();
                    }
                } else if (fd == tun_fd_) {
                    if (events[i].events & EPOLLIN) {
                        HandleTunReadable();
                    }
                } else if (relay_listen_fds_.count(fd)) {
                    if (events[i].events & EPOLLIN) {
                        HandleLocalRelayAccept(fd);
                    }
                } else if (fd == pending_relay_user_fd_) {
                    // 等待 relay ack 的 user_fd, 保持 pending 处理
                    if (events[i].events & EPOLLIN) {
                        HandleLocalRelayUserReadable(fd);
                    }
                } else if (p2p_to_session_.count(fd)) {
                    // P2P socket 事件
                    if (events[i].events & EPOLLOUT) {
                        auto pit = p2p_to_session_.find(fd);
                        if (pit != p2p_to_session_.end()) {
                            auto sit = p2p_states_.find(pit->second);
                            if (sit == p2p_states_.end()) continue;
                            if (!sit->second.connected) {
                                // 检查 TCP 握手是否完成
                                int err = 0; socklen_t len = sizeof(err);
                                getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len);
                                if (err == 0) {
                                    sit->second.connected = true;
                                    LOG_INFO("P2P connect ok fd=%d session=%u", fd, pit->second);
                                    std::string ok = FrameBuilder(MessageType::P2P_OK)
                                                         .AppendU32(pit->second).Build();
                                    writer_.Append(std::move(ok));
                                    writer_.Flush(tunnel_fd_);
                                } else {
                                    LOG_ERROR("P2P connect failed: %s", strerror(err));
                                    std::string fail = FrameBuilder(MessageType::P2P_FAIL)
                                                           .AppendU32(pit->second).Build();
                                    writer_.Append(std::move(fail));
                                    writer_.Flush(tunnel_fd_);
                                    epoll_ctl(epfd_, EPOLL_CTL_DEL, fd, nullptr);
                                    close(fd);
                                    p2p_states_.erase(sit);
                                    p2p_to_session_.erase(pit);
                                }
                            }
                            // 刷新写缓冲区
                            if (!sit->second.write_buf.empty()) {
                                ssize_t w = write(fd, sit->second.write_buf.data(),
                                                  sit->second.write_buf.size());
                                if (w > 0) sit->second.write_buf.erase(0, w);
                            }
                        }
                    }
                    if (events[i].events & EPOLLIN) {
                        HandleP2pReadable(fd);
                    }
                } else if (local_to_session_.count(fd)) {
                    // 本地中继用户连接上有数据
                    if (events[i].events & EPOLLIN) {
                        HandleLocalRelayUserReadable(fd);
                    }
                } else {
                    // 普通 local_fd (端口映射模式)
                    if (events[i].events & EPOLLIN) {
                        HandleLocalReadable(fd);
                    }
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
            // P2P 超时检测: 3 秒未完成握手则回退中继
            std::vector<uint32_t> p2p_dead;
            for (auto& kv : p2p_states_) {
                if (!kv.second.connected && now - kv.second.start_time > 3) {
                    p2p_dead.push_back(kv.first);
                }
            }
            for (uint32_t sid : p2p_dead) {
                auto it = p2p_states_.find(sid);
                if (it == p2p_states_.end()) continue;
                LOG_WARN("P2P timeout sid=%u, fallback to relay", sid);
                std::string fail = FrameBuilder(MessageType::P2P_FAIL)
                                       .AppendU32(sid).Build();
                writer_.Append(std::move(fail));
                writer_.Flush(tunnel_fd_);
                epoll_ctl(epfd_, EPOLL_CTL_DEL, it->second.p2p_fd, nullptr);
                p2p_to_session_.erase(it->second.p2p_fd);
                close(it->second.p2p_fd);
                p2p_states_.erase(it);
            }
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
