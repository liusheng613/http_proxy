#include "proxy.h"
#include <iostream>
#include <unistd.h>
#include <cstring>
#include <arpa/inet.h>
#include <sys/socket.h>

TcpProxy::TcpProxy(int listen_port, const char* backend_ip, int backend_port)
    : listen_port_(listen_port),
      backend_ip_(backend_ip),
      backend_port_(backend_port) {}

int TcpProxy::create_listen_socket()
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(listen_port_);
    addr.sin_addr.s_addr = INADDR_ANY;

    bind(fd, (sockaddr*)&addr, sizeof(addr));
    listen(fd, 10);

    return fd;
}

int TcpProxy::connect_backend()
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

void TcpProxy::forwar_data(int from_fd, int to_fd)
{
    char buffer[4096];

    while (true)
    {
        ssize_t n = read(from_fd, buffer, sizeof(buffer));
        if (n <= 0)
        {
            break;
        }

        ssize_t sent = 0;
        while (sent < n)
        {
            ssize_t m = write(to_fd, buffer + sent, n - sent);
            if (m <= 0)
            {
                return;
            }
            sent += m;
        }
    }
}

void TcpProxy::handle_connection(int client_fd)
{
    int server_fd = connect_backend();
    if (server_fd < 0)
    {
        close(client_fd);
        return;
    }

    // 简单版本,用fork处理双向通信
    pid_t pid = fork();

    if (pid == 0)
    {
        // 子进程: client -> server
        forwar_data(client_fd, server_fd);
        close(client_fd);
        close(server_fd);
        exit(0);
    }
    else
    {
        // 父进程：server -> client
        forwar_data(server_fd, client_fd);
        close(client_fd);
        close(server_fd);
    }
}

void TcpProxy::run()
{
    int listen_fd = create_listen_socket();

    std::cout << "Listening on port " << listen_port_ << std::endl;

    while (true)
    {
        sockaddr_in client_addr{};
        socklen_t len = sizeof(client_addr);

        int client_fd = accept(listen_fd, (sockaddr*)&client_addr, &len);
        if (client_fd < 0)
        {
            continue;
        }

        std::cout<< "New connection" << std::endl;

        handle_connection(client_fd);
    }
}