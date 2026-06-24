#ifndef TUNNEL_COMMON_SESSION_H_
#define TUNNEL_COMMON_SESSION_H_

#include <atomic>

namespace tunnel {

// 全局单调递增的 session id 生成器。
// server 在接受一个外部用户连接时分配, 用 session_id 把 user_fd 与
// 隧道另一端 client 的 local_fd 关联起来。
// 起始值从一个非零随机化点开始, 便于日志里区分; 这里简单用 1 起。
inline uint32_t alloc_session_id() {
    static std::atomic<uint32_t> counter{1};
    return counter.fetch_add(1, std::memory_order_relaxed);
}

}  // namespace tunnel

#endif  // TUNNEL_COMMON_SESSION_H_
