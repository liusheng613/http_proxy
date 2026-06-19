#pragma once
#include <vector>
#include <unordered_map>

/*
✅ 单进程
✅ 非阻塞 socket
✅ epoll 统一管理
✅ 支持多连接 + 双向转发
*/
class TcpProxy2
{
public:


public:
    TcpProxy2(int listen_port, const char* backend_ip, int backend_port);
    ~TcpProxy2();
    void run();
private:

    // key, fd from, value: fd to
    std::unordered_map<int, int> from_to_;
    int listen_port_;
    int listen_fd_;
    const char* backend_ip_;
    int backend_port_;
    int epfd_;

    void cleanup();
    int create_listen_socket();
    int connect_backend();
    bool forward_data(int epfd, int from_fd);

    int set_nonblock(int fd);
    void close_fd(int fd);
};
