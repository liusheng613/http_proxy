#ifndef TUNNEL_COMMON_WRITE_BUFFER_H_
#define TUNNEL_COMMON_WRITE_BUFFER_H_

#include <string>
#include <vector>

#ifndef _WIN32
#include <sys/uio.h>  // writev
#endif

namespace tunnel {

// =============================================================================
// WriteBuffer: 非阻塞 socket 的发送缓冲。
//
// 非阻塞 socket 上 write 可能写不完 (EAGAIN / 部分写), 剩余字节必须缓存,
// 并把 fd 加入 epoll 的可写监听, 等下次 EPOLLOUT 再继续发。
//
// 用法:
//   WriteBuffer wb;
//   wb.Append(frame_bytes);          // 入队
//   bool full = wb.Flush(fd);        // 尝试写出, false 表示还需要继续 (应监听 EPOLLOUT)
//   bool empty = wb.Empty();         // 是否全部发完
// =============================================================================

class WriteBuffer {
public:
    // 追加待发送字节。可多次调用后再 Flush。
    void Append(const std::string& data) { queue_.push_back(data); }

    // 追加 (避免拷贝的右值版本)
    void Append(std::string&& data) { queue_.push_back(std::move(data)); }

    // 尝试把所有待发送数据写到 fd。
    // 返回 true 表示全部写完 (或 fd 不可写时返回 false)。
    // 返回 false 表示还有数据未发出, 调用方应监听 EPOLLOUT 并再次调用 Flush。
    bool Flush(int fd);

    bool Empty() const { return queue_.empty(); }

private:
    // 用分散写 writev 一次性发多段; 写不完则把已写部分丢弃。
    std::vector<std::string> queue_;
};

}  // namespace tunnel

#endif  // TUNNEL_COMMON_WRITE_BUFFER_H_
