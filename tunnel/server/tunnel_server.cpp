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

    // 清理 name 映射
    if (!it->second->name.empty()) {
        name_to_tunnel_.erase(it->second->name);
    }

    // 清理涉及此隧道的所有 relay session
    std::vector<uint32_t> dead_relays;
    for (auto& kv : relay_sessions_) {
        if (kv.second.tunnel_a == fd || kv.second.tunnel_b == fd) {
            dead_relays.push_back(kv.first);
        }
    }
    for (uint32_t sid : dead_relays) {
        relay_sessions_.erase(sid);
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

        case MessageType::REGISTER:
            // client 注册名字: { uint8 name_len; char name[name_len]; }
            if (frame.payload.size() < 1) {
                LOG_WARN("REGISTER payload too short");
                break;
            }
            {
                uint8_t name_len = static_cast<uint8_t>(frame.payload[0]);
                if (frame.payload.size() < 1 + name_len) {
                    LOG_WARN("REGISTER payload truncated");
                    break;
                }
                std::string name = frame.payload.substr(1, name_len);
                // 检查名字是否已被占用
                auto nit = name_to_tunnel_.find(name);
                if (nit != name_to_tunnel_.end() && nit->second != fd) {
                    LOG_WARN("fd=%d REGISTER name='%s' already used by fd=%d, reject",
                             fd, name.c_str(), nit->second);
                    // 发 ACK(失败) 给 client
                    std::string ack = FrameBuilder(MessageType::ACK)
                                          .AppendU8(1)  // code=1 表示失败
                                          .Build();
                    sess.writer.Append(std::move(ack));
                    sess.writer.Flush(fd);
                    break;
                }
                // 如果该 fd 之前有旧名字,先清理
                if (!sess.name.empty()) {
                    name_to_tunnel_.erase(sess.name);
                }
                sess.name = name;
                name_to_tunnel_[name] = fd;
                LOG_INFO("fd=%d REGISTER name='%s'", fd, name.c_str());
                // 发 ACK(成功) 给 client
                std::string ack = FrameBuilder(MessageType::ACK)
                                      .AppendU8(0)  // code=0 表示成功
                                      .Build();
                sess.writer.Append(std::move(ack));
                sess.writer.Flush(fd);
            }
            break;

        case MessageType::PORT_MAP:
            LOG_INFO("fd=%d recv PORT_MAP (payload_len=%zu)", fd, frame.payload.size());
            sess.mapper.HandlePortMap(frame.payload);
            break;

        case MessageType::NEW_CONN:
            // 可能来自外部用户 (PortMapper 处理) 或来自 client 的组网请求
            // client->server: payload { session_id=0; target_name_len; target_name[]; target_port }
            if (frame.payload.size() < 1) break;
            {
                uint32_t req_sid = ntohl(*reinterpret_cast<const uint32_t*>(&frame.payload[0]));
                if (req_sid == 0 && frame.payload.size() >= 8) {
                    // client 间组网: session_id=0, 后面是目标名+端口
                    uint16_t name_len = ntohs(*reinterpret_cast<const uint16_t*>(&frame.payload[4]));
                    if (frame.payload.size() < 6 + name_len + 2) break;
                    std::string target_name = frame.payload.substr(6, name_len);
                    uint16_t target_port = ntohs(*reinterpret_cast<const uint16_t*>(&frame.payload[6 + name_len]));
                    
                    auto tit = name_to_tunnel_.find(target_name);
                    if (tit == name_to_tunnel_.end()) {
                        LOG_WARN("NEW_CONN relay: target '%s' not found", target_name.c_str());
                        break;
                    }
                    int target_fd = tit->second;
                    if (!sessions_.count(target_fd)) break;

                    uint32_t sid = alloc_session_id();
                    relay_sessions_[sid] = {fd, target_fd};

                    // 通知目标 client 连接本地服务
                    std::string to_target = FrameBuilder(MessageType::NEW_CONN)
                                                .AppendU32(sid)
                                                .AppendU16(target_port)
                                                .Build();
                    sessions_[target_fd]->writer.Append(std::move(to_target));
                    sessions_[target_fd]->writer.Flush(target_fd);

                    // 通知源 client session 已建立 (port=0 表示中继, 不需要连本地)
                    std::string to_source = FrameBuilder(MessageType::NEW_CONN)
                                                .AppendU32(sid)
                                                .AppendU16(0)  // port=0 表示这是 relay ack
                                                .Build();
                    sess.writer.Append(std::move(to_source));
                    sess.writer.Flush(fd);

                    LOG_INFO("relay session %u: fd=%d -> '%s'(fd=%d) port=%u",
                             sid, fd, target_name.c_str(), target_fd, target_port);
                }
            }
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
                // 1) 先查 PortMapper (外部用户 session)
                int user_fd = sess.GetUserFd(sid);
                if (user_fd >= 0) {
                    const char* data = &frame.payload[6];
                    ssize_t written = write(user_fd, data, dlen);
                    if (written < static_cast<ssize_t>(dlen)) {
                        if (written < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
                            LOG_ERROR("write user_fd=%d failed: %s", user_fd, strerror(errno));
                            CloseUser(user_fd);
                        }
                    }
                    break;
                }
                // 2) 再查 relay_sessions_ (client 间中继)
                auto rit = relay_sessions_.find(sid);
                if (rit != relay_sessions_.end()) {
                    int other_fd = (rit->second.tunnel_a == fd) ?
                                   rit->second.tunnel_b : rit->second.tunnel_a;
                    auto other_it = sessions_.find(other_fd);
                    if (other_it != sessions_.end()) {
                        std::string relay_data = FrameBuilder(MessageType::DATA)
                                                    .AppendBytes(frame.payload.data(),
                                                                 frame.payload.size())
                                                    .Build();
                        other_it->second->writer.Append(std::move(relay_data));
                        other_it->second->writer.Flush(other_fd);
                    } else {
                        LOG_WARN("relay session %u: other tunnel fd=%d gone, closing", sid, other_fd);
                        relay_sessions_.erase(rit);
                    }
                    break;
                }
                LOG_WARN("DATA for unknown session_id=%u from fd=%d", sid, fd);
            }
            break;

        case MessageType::CLOSE:
            // client 通知关闭某 session
            if (frame.payload.size() < 4) break;
            {
                uint32_t sid = ntohl(*reinterpret_cast<const uint32_t*>(&frame.payload[0]));
                // 1) 先查 PortMapper
                int user_fd = sess.GetUserFd(sid);
                if (user_fd >= 0) {
                    CloseUser(user_fd);
                    sess.mapper.RemoveSession(sid);
                    break;
                }
                // 2) 再查 relay_sessions_
                auto rit = relay_sessions_.find(sid);
                if (rit != relay_sessions_.end()) {
                    int other_fd = (rit->second.tunnel_a == fd) ?
                                   rit->second.tunnel_b : rit->second.tunnel_a;
                    auto other_it = sessions_.find(other_fd);
                    if (other_it != sessions_.end()) {
                        // 通知另一方 session 关闭
                        std::string close_data = FrameBuilder(MessageType::CLOSE)
                                                     .AppendBytes(frame.payload.data(),
                                                                  frame.payload.size())
                                                     .Build();
                        other_it->second->writer.Append(std::move(close_data));
                        other_it->second->writer.Flush(other_fd);
                    }
                    relay_sessions_.erase(rit);
                    LOG_DEBUG("relay session %u closed (by fd=%d)", sid, fd);
                }
            }
            break;

        case MessageType::PROBE:
            // client A 想探测 client B: { uint8 target_name_len; char target_name[]; uint32 probe_id; }
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
                std::string target_name = frame.payload.substr(1, name_len);
                uint32_t probe_id = ntohl(*reinterpret_cast<const uint32_t*>(&frame.payload[1 + name_len]));
                
                auto tit = name_to_tunnel_.find(target_name);
                if (tit == name_to_tunnel_.end()) {
                    // 目标不存在,直接回 PROBE_REPLY(not_found) 给源 client
                    LOG_INFO("PROBE: target '%s' not found, reply to fd=%d", target_name.c_str(), fd);
                    std::string reply = FrameBuilder(MessageType::PROBE_REPLY)
                                            .AppendU32(probe_id)
                                            .AppendU8(1)  // status=1 not_found
                                            .Build();
                    sess.writer.Append(std::move(reply));
                    sess.writer.Flush(fd);
                } else {
                    // 转发 PROBE 给目标 client
                    int target_fd = tit->second;
                    LOG_INFO("PROBE: fd=%d -> target '%s' (fd=%d), probe_id=%u",
                             fd, target_name.c_str(), target_fd, probe_id);
                    auto target_it = sessions_.find(target_fd);
                    if (target_it != sessions_.end()) {
                        std::string probe_frame = FrameBuilder(MessageType::PROBE)
                                                      .AppendBytes(frame.payload.data(),
                                                                   frame.payload.size())
                                                      .Build();
                        target_it->second->writer.Append(std::move(probe_frame));
                        target_it->second->writer.Flush(target_fd);
                    }
                }
            }
            break;

        case MessageType::PROBE_REPLY:
            // client B 回应 PROBE: { uint32 probe_id; uint8 status; }
            // 这里简化处理: server 不记录 probe_id 到源 client 的映射,
            // 而是让 client 在 PROBE payload 里带上自己的 fd 信息 (TODO: 后续优化)
            // 当前实现: server 收到 PROBE_REPLY 后, 广播给所有 client (除了源)
            if (frame.payload.size() < 5) {
                LOG_WARN("PROBE_REPLY payload too short");
                break;
            }
            {
                std::string reply_frame = FrameBuilder(MessageType::PROBE_REPLY)
                                              .AppendBytes(frame.payload.data(),
                                                           frame.payload.size())
                                              .Build();
                for (auto& kv : sessions_) {
                    if (kv.first != fd) {
                        kv.second->writer.Append(reply_frame);
                        kv.second->writer.Flush(kv.first);
                    }
                }
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
