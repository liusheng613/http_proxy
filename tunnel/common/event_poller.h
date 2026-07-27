#ifndef TUNNEL_COMMON_EVENT_POLLER_H_
#define TUNNEL_COMMON_EVENT_POLLER_H_

#include <cstdint>
#include <vector>

namespace tunnel {

// 跨平台事件循环抽象 (epoll / select)。
// Linux: 基于 epoll 的高效实现
// Windows: 基于 select 的实现 (仅支持 socket fd)
class EventPoller {
public:
    static constexpr int kInvalidFd = -1;

    enum : uint32_t {
        READABLE  = 0x01,   // 可读
        WRITABLE  = 0x02,   // 可写
        ERROR_EVT = 0x04,   // 错误/HUP
        ET_MODE   = 0x08,   // 边缘触发 (Linux epoll 专用, Windows 忽略)
        ALL_EVENTS = READABLE | WRITABLE | ERROR_EVT,
    };

    struct Event {
        int fd = kInvalidFd;
        uint32_t events = 0;   // bitmask of READABLE/WRITABLE/ERROR_EVT

        bool readable() const { return (events & READABLE) != 0; }
        bool writable() const { return (events & WRITABLE) != 0; }
        bool error()    const { return (events & ERROR_EVT) != 0; }
    };

    EventPoller() = default;
    ~EventPoller();

    EventPoller(const EventPoller&) = delete;
    EventPoller& operator=(const EventPoller&) = delete;

    // 创建 poller 实例。失败返回 false。
    bool create();

    // 添加 fd 到事件监听。events 为 READABLE|WRITABLE|ET_MODE 的组合。
    bool add(int fd, uint32_t events);

    // 修改已注册 fd 的事件。
    bool mod(int fd, uint32_t events);

    // 从事件监听中移除 fd。
    bool del(int fd);

    // 等待事件。timeout_ms: -1 = 无限, 0 = 立即返回, >0 = 超时毫秒。
    // 返回就绪事件数 (可能为 0)，失败返回 -1。
    int wait(Event* events, int max_events, int timeout_ms);

    // 关闭 poller。
    void close();

private:
    struct Impl;
    Impl* impl_ = nullptr;
};

}  // namespace tunnel

#endif  // TUNNEL_COMMON_EVENT_POLLER_H_
