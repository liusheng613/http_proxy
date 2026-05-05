#pragma once

class TcpProxy
{
public:
    TcpProxy(int listen_port, const char* backend_ip, int backend_port);
    void run();

private:
    int listen_port_;
    const char* backend_ip_;
    int backend_port_;

    int create_listen_socket();
    int connect_backend();

    void forwar_data(int from_fd, int to_fd);
    void handle_connection(int client_fd);
};
