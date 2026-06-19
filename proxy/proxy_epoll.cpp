#include "proxy_epoll.h"
#include <sys/epoll.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <cstring>
#include <iostream>
#include <signal.h>
#include <fcntl.h>

TcpProxy2::TcpProxy2(int listen_port, const char* backend_ip, int backend_port)
    : listen_port_(listen_port),
      backend_ip_(backend_ip),
      backend_port_(backend_port) {}

TcpProxy2::~TcpProxy2()
{
    cleanup();
}

void TcpProxy2::cleanup()
{
    for (auto& p : from_to_)
    {
        close(p.first);
    }

    from_to_.clear();

    if (listen_fd_ >= 0)
    {
        close(listen_fd_);
        listen_fd_ = -1;
    }

    if (epfd_ >= 0)
    {
        close(epfd_);
        epfd_ = -1;
    }
}

void TcpProxy2::run()
{
    listen_fd_ = create_listen_socket();

    if (listen_fd_ < 0)
    {
        std::cerr << "create listen socket failed" << std::endl;
        return;
    }

    if (set_nonblock(listen_fd_) < 0)
    {
        std::cerr << "set listen_fd non-blocking failed" << std::endl;
        close(listen_fd_);
        return;
    }

    printf("epoll proxy listen on port:%d\n", listen_port_);

    epfd_ = epoll_create1(EPOLL_CLOEXEC);
    if (epfd_ < 0)
    {
        std::cerr << "create epoll instance failed" << std::endl;
        close(listen_fd_);
        return;
    }

    struct epoll_event ev{};
    ev.events = EPOLLIN | EPOLLET; // 设置为边缘触发
    ev.data.fd = listen_fd_;

    if (epoll_ctl(epfd_, EPOLL_CTL_ADD, listen_fd_, &ev) < 0)
    {
        std::cerr << "epoll_ctl failed" << std::endl;
        close(listen_fd_);
        close(epfd_);
        return;
    }

    const int max_events = 1024;
    struct epoll_event events[max_events];
    while(true)
    {
        int nready = epoll_wait(epfd_, events, max_events, -1);
        // printf("epoll_wait return %d events\n", nready);
        if (nready < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }
            std::cerr << "epoll_wait error" << std::endl;
            break;
        }

        for (int i = 0; i < nready; ++i)
        {
            // printf(
            // "event fd=%d events=0x%x\n",
            // events[i].data.fd,
            // events[i].events
            // );

            // 错误事件
            if (events[i].events & (EPOLLERR | EPOLLHUP))
            {
                printf("fd error: %d\n", events[i].data.fd);
                close_fd(events[i].data.fd);
                continue;
            }

            if (events[i].data.fd == listen_fd_)
            {
                // printf("accept start\n");
                while (true)
                {
                    int client_fd = accept(listen_fd_, nullptr, nullptr);
                    // printf(
                    // "accept fd=%d errno=%d\n",
                    // client_fd,
                    // errno
                    // );

                    // printf("receive client from client:%d\n", client_fd);
                    if (client_fd < 0)
                    {
                        if (errno == EAGAIN || errno == EWOULDBLOCK)
                        {
                            // 没有更多连接了
                            break;
                        }
                        else
                        {
                            std::cerr << "accept failed" << std::endl;
                            break;
                        }
                    }

                    // 设置为非阻塞
                    if (set_nonblock(client_fd) < 0)
                    {
                        close(client_fd);
                        continue;
                    }
                    struct epoll_event ev;
                    ev.events = EPOLLIN | EPOLLET; // 设置为边缘触发
                    ev.data.fd = client_fd;
                    if (epoll_ctl(epfd_, EPOLL_CTL_ADD, client_fd, &ev) < 0)
                    {
                        std::cerr << "epoll_ctl add client failed" << std::endl;
                        close(client_fd);
                        continue;
                    }

                    // printf("before connect_backend\n");
                    int server_fd = connect_backend();
                    // printf("after connect_backend\n");

                    if (server_fd < 0)
                    {
                        close(client_fd);
                        return;
                    }

                    set_nonblock(server_fd);

                    ev.events = EPOLLIN | EPOLLET; // 设置为边缘触发
                    ev.data.fd = server_fd;
                    if (epoll_ctl(epfd_, EPOLL_CTL_ADD, server_fd, &ev) < 0)
                    {
                        std::cerr << "epoll_ctl add client failed" << std::endl;
                        close(client_fd);
                        continue;
                    }

                    from_to_[client_fd] = server_fd;
                    from_to_[server_fd] = client_fd;
                }
                printf("accept end\n");
            }
            else if(events[i].events & EPOLLIN)// 数据可读
            {
                // printf("receive data from:%d\n", events[i].data.fd);
                if (!from_to_.count(events[i].data.fd))
                {
                    continue;
                }
                // 处理数据交互
                if(!forward_data(epfd_, events[i].data.fd))
                {
                    break;
                }
            }
        }
    }
}

bool TcpProxy2::forward_data(int epfd, int from_fd)
{
    char buffer[4096];
    while (true)
    {
        ssize_t n = read(from_fd, buffer, sizeof(buffer));
        if (n < 0)
        {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
            {
                // 没有更多数据了
                return true;
            }
            else
            {
                std::cerr << "read from " << from_fd << " failed" << std::endl;
                close_fd(from_fd);
                return false;
            }
        }
        else if (n == 0)
        {
            // 客户端关闭连接
            std::cerr << "client " << from_fd << " disconnected" << std::endl;
            close_fd(from_fd);
            return false;
        }
        else
        {
            int to_fd = from_to_[from_fd];
            if (write(to_fd, buffer, n) < 0)
            {
                std::cerr << "write to " << to_fd << " failed" << std::endl;
                close_fd(to_fd);
                return false;
            }
        }
    }
    return true;
}

int TcpProxy2::create_listen_socket()
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(listen_port_);
    addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(fd, (sockaddr*)&addr, sizeof(addr)) < 0)
    {
        std::cerr << "bind socket failed" << std::endl;
        close(fd);
        return -1;
    }

    const int maxConnection = 20;
    if (listen(fd, maxConnection) < 0)
    {
        std::cerr << "listen socket failed" << std::endl;
        close(fd);
        return -1;
    }

    return fd;
}

int TcpProxy2::connect_backend()
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(backend_port_);
    inet_pton(AF_INET, backend_ip_, &addr.sin_addr);

    if (connect(fd, (sockaddr*)&addr, sizeof(addr)) < 0)
    {
        perror("connect backend failed");
        close(fd);
        return -1;
    }

    return fd;
}

int TcpProxy2::set_nonblock(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1)
        return -1;
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    return 0;
}

void TcpProxy2::close_fd(int fd)
{
    if (!from_to_.count(fd)) return;

    epoll_ctl(epfd_, EPOLL_CTL_DEL, fd, nullptr);

    int to_fd = from_to_[fd];
    from_to_.erase(fd);
    close(fd);

    if (from_to_.count(to_fd))
    {
        epoll_ctl(epfd_, EPOLL_CTL_DEL, to_fd, nullptr);
        from_to_.erase(to_fd);
        close(to_fd);
    }
}
