#include "write_buffer.h"

#ifndef _WIN32
  #include <sys/uio.h>
#else
  #include <winsock2.h>
#endif
#include <unistd.h>

#include <cerrno>
#include <cstring>

namespace tunnel {

bool WriteBuffer::Flush(int fd) {
    while (!queue_.empty()) {
#ifndef _WIN32
        // Linux: writev 批量发送
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
        if (iovs.empty()) { queue_.clear(); return true; }
        ssize_t n = writev(fd, iovs.data(), static_cast<int>(iovs.size()));
#else
        // Windows: 拼接后单次 send (socket 不能用 write)
        std::string combined;
        for (const auto& s : queue_) combined += s;
        if (combined.empty()) { queue_.clear(); return true; }
        ssize_t n = send(fd, combined.data(), combined.size(), 0);
#endif
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) return false;
            if (errno == EINTR) continue;
            return false;
        }
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
