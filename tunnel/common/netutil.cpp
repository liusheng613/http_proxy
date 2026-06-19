#include "netutil.h"
#include "logger.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

namespace tunnel {
namespace net {

int set_nonblock(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1) {
        return -1;
    }
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

int set_block(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1) {
        return -1;
    }
    return fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);
}

int set_reuse_addr(int fd) {
    int yes = 1;
    return setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
}

int set_nodelay(int fd) {
    int yes = 1;
    return setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes));
}

int create_listen_socket(uint16_t port, int tcp_backlog) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        LOG_ERROR("socket() failed: %s", strerror(errno));
        return -1;
    }

    set_reuse_addr(fd);
    set_nonblock(fd);

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        LOG_ERROR("bind(%u) failed: %s", port, strerror(errno));
        close(fd);
        return -1;
    }

    if (listen(fd, tcp_backlog) < 0) {
        LOG_ERROR("listen(%u) failed: %s", port, strerror(errno));
        close(fd);
        return -1;
    }

    return fd;
}

int create_connect_socket(const std::string& ip, uint16_t port, bool* connected) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        LOG_ERROR("socket() failed: %s", strerror(errno));
        return -1;
    }
    set_nonblock(fd);

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (inet_pton(AF_INET, ip.c_str(), &addr.sin_addr) <= 0) {
        LOG_ERROR("inet_pton(%s) failed", ip.c_str());
        close(fd);
        return -1;
    }

    int ret = connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    if (ret == 0) {
        if (connected) *connected = true;
        return fd;
    }
    if (errno == EINPROGRESS) {
        // 非阻塞连接进行中, 由 epoll 通知可写后再判断是否真正连上
        if (connected) *connected = false;
        return fd;
    }
    LOG_ERROR("connect(%s:%u) failed: %s", ip.c_str(), port, strerror(errno));
    close(fd);
    return -1;
}

void close_fd(int& fd) {
    if (fd >= 0) {
        close(fd);
        fd = -1;
    }
}

std::string peer_to_string(int fd) {
    sockaddr_in addr{};
    socklen_t len = sizeof(addr);
    if (getpeername(fd, reinterpret_cast<sockaddr*>(&addr), &len) < 0) {
        return "";
    }
    char buf[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &addr.sin_addr, buf, sizeof(buf));
    return std::string(buf) + ":" + std::to_string(ntohs(addr.sin_port));
}

}  // namespace net
}  // namespace tunnel
