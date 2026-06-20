#include "write_buffer.h"

#include <unistd.h>

#include <cerrno>
#include <cstring>

namespace tunnel {

bool WriteBuffer::Flush(int fd) {
    while (!queue_.empty()) {
        // 构造 iovec 数组 (批量发送, 减少系统调用)
        std::vector<iovec> iovs;
        iovs.reserve(queue_.size());
        for (const auto& s : queue_) {
            if (!s.empty()) {
                iovec iv;
                iv.iov_base = const_cast<char*>(s.data());
                iv.iov_len = s.size();
                iovs.push_back(iv);
            }
        }

        if (iovs.empty()) {
            // 都是空字符串, 直接丢弃
            queue_.clear();
            return true;
        }

        ssize_t n = writev(fd, iovs.data(), static_cast<int>(iovs.size()));
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return false;  // 发送缓冲区满, 等下次 EPOLLOUT
            }
            if (errno == EINTR) {
                continue;
            }
            // 真正出错 (如对端 reset): 这里直接清空, 由上层判断连接状态
            return false;
        }

        // 丢弃已发送的字节
        size_t remaining = static_cast<size_t>(n);
        while (remaining > 0 && !queue_.empty()) {
            if (queue_.front().size() <= remaining) {
                remaining -= queue_.front().size();
                queue_.erase(queue_.begin());
            } else {
                queue_.front().erase(0, remaining);
                remaining = 0;
            }
        }
    }
    return true;
}

}  // namespace tunnel
