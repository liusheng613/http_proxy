/**
LT
ET
非阻塞
多客户端
 */

#include <sys/epoll.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <cstring>
#include <iostream>
#include <signal.h>
#include <fcntl.h>

const int listen_port_ = 1024;
const int max_events_ = 1024;
int create_listen_socket()
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

    if (listen(fd, 10) < 0)
    {
        std::cerr << "listen socket failed" << std::endl;
        close(fd);
        return -1;
    }

    return fd;
}

// epoll LT(电平触发) 模式
// 只要fd上有数据可读,就会一直触发事件,直到数据被读完
// LT 为epoll的默认模式(EPOLLLT)
void LT()
{
    int listen_fd = create_listen_socket();

    if (listen_fd < 0)
    {
        std::cerr << "create listen socket failed" << std::endl;
        return;
    }

    int epfd = epoll_create1(EPOLL_CLOEXEC);
    if (epfd < 0)
    {
        std::cerr << "create epoll instance failed" << std::endl;
        close(listen_fd);
        return;
    }
    struct epoll_event ev{};
    ev.events = EPOLLIN;
    ev.data.fd = listen_fd;
    epoll_ctl(epfd, EPOLL_CTL_ADD, listen_fd, &ev);

    struct epoll_event events[max_events_];

    while (true)
    {
        int nready = epoll_wait(epfd, events, max_events_, -1);
        for (int i = 0; i < nready; ++i)
        {
            // 处理新连接
            if (events[i].data.fd == listen_fd)
            {
                // 此次 accept虽为阻塞模式,因为listen_fd上有事件,所以会立即返回
                int client_fd = accept(listen_fd, nullptr, nullptr);
                if (client_fd < 0)
                {
                    std::cerr << "accept failed" << std::endl;
                    continue;
                }
                ev.events = EPOLLIN;
                ev.data.fd = client_fd;
                epoll_ctl(epfd, EPOLL_CTL_ADD, client_fd, &ev);
            }
            else
            {
                // 处理客户端数据
                char buffer[4096];
                ssize_t n = read(events[i].data.fd, buffer, sizeof(buffer));
                if (n <= 0)
                {
                    epoll_ctl(epfd, EPOLL_CTL_DEL, events[i].data.fd, nullptr);
                    close(events[i].data.fd);
                    std::cerr << "client " << events[i].data.fd << " disconnected" << std::endl;
                }
                else
                {
                    if (write(events[i].data.fd, buffer, n) <= 0)
                    {
                        std::cerr << "write to client " << events[i].data.fd << " failed" << std::endl;
                        epoll_ctl(epfd, EPOLL_CTL_DEL, events[i].data.fd, nullptr);
                        close(events[i].data.fd);
                    }
                }
            }
        }
    }
}

int set_nonblock(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1)
        return -1;
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    return 0;
}

void close_fd(int epfd, int fd)
{
    epoll_ctl(epfd, EPOLL_CTL_DEL, fd, nullptr);
    close(fd);
}

// epoll ET(边缘触发) 模式
// 只有当fd上有数据可读时,才会触发事件
void ET()
{
    int listen_fd = create_listen_socket();

    if (listen_fd < 0)
    {
        std::cerr << "create listen socket failed" << std::endl;
        return;
    }

    if (set_nonblock(listen_fd) < 0)
    {
        std::cerr << "set listen_fd non-blocking failed" << std::endl;
        close(listen_fd);
        return;
    }

    int epfd = epoll_create1(EPOLL_CLOEXEC);
    if (epfd < 0)
    {
        std::cerr << "create epoll instance failed" << std::endl;
        close(listen_fd);
        return;
    }

    struct epoll_event ev{};
    ev.events = EPOLLIN | EPOLLET; // 设置为边缘触发
    ev.data.fd = listen_fd;

    if (epoll_ctl(epfd, EPOLL_CTL_ADD, listen_fd, &ev) < 0)
    {
        std::cerr << "epoll_ctl failed" << std::endl;
        close(listen_fd);
        close(epfd);
        return;
    }

    struct epoll_event events[max_events_];
    while (true)
    {
        int nready = epoll_wait(epfd, events, max_events_, -1);
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
            /*
             * 错误事件
             */
            if (events[i].events & (EPOLLERR | EPOLLHUP))
            {
                printf("fd error: %d\n", events[i].data.fd);

                close_fd(epfd, events[i].data.fd);
                continue;
            }

            
            if (events[i].data.fd == listen_fd)
            {
                while (true)
                {
                    int client_fd = accept(listen_fd, nullptr, nullptr);
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
                    struct epoll_event client_ev;
                    client_ev.events = EPOLLIN | EPOLLET; // 设置为边缘触发
                    client_ev.data.fd = client_fd;
                    if (epoll_ctl(epfd, EPOLL_CTL_ADD, client_fd, &client_ev) < 0)
                    {
                        std::cerr << "epoll_ctl add client failed" << std::endl;
                        close(client_fd);
                        continue;
                    }
                }
            }
            // 数据可读
            else if (events[i].events & EPOLLIN)
            {
                char buffer[4096];
                while (true)
                {
                    ssize_t n = read(events[i].data.fd, buffer, sizeof(buffer));
                    if (n < 0)
                    {
                        if (errno == EAGAIN || errno == EWOULDBLOCK)
                        {
                            // 没有更多数据了
                            break;
                        }
                        else
                        {
                            std::cerr << "read from client " << events[i].data.fd << " failed" << std::endl;
                            close_fd(epfd, events[i].data.fd);
                            break;
                        }
                    }
                    else if (n == 0)
                    {
                        // 客户端关闭连接
                        std::cerr << "client " << events[i].data.fd << " disconnected" << std::endl;
                        close_fd(epfd, events[i].data.fd);
                        break;
                    }
                    else
                    {
                        if (write(events[i].data.fd, buffer, n) < 0)
                        {
                            std::cerr << "write to client " << events[i].data.fd << " failed" << std::endl;
                            close_fd(epfd, events[i].data.fd);
                            break;
                        }
                    }
                }
            }
        }
    }
    close(listen_fd);
    close(epfd);
}

int main(int argc, char** argv)
{
    if (argc > 1)
    {
        // 忽略 SIGPIPE 信号,当向已关闭的socket写数据时,
        // 默认会触发SIGPIPE信号,导致程序崩溃
        signal(SIGPIPE, SIG_IGN);
        if (strcmp(argv[1], "LT") == 0)
        {
            std::cout << "listen " << listen_port_ << " with LT mode" << std::endl;
            LT();
        }
        else if (strcmp(argv[1], "ET") == 0)
        {
            std::cout << "listen " << listen_port_ << " with ET mode" << std::endl;
            ET();
        }
        else
        {
            std::cerr << "unknown mode: " << argv[1] << std::endl;
            return -1;
        }
    }

    return 0;
}