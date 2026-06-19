#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "../common/logger.h"
#include "tunnel_client.h"

// 用法: tunnel_client <server_ip> [server_port]
//   server_ip:   公网 server 的 IP
//   server_port: 控制隧道端口, 默认 7000
int main(int argc, char** argv) {
    ::signal(SIGPIPE, SIG_IGN);

    if (argc < 2) {
        fprintf(stderr, "usage: %s <server_ip> [server_port]\n", argv[0]);
        return 1;
    }
    std::string server_ip = argv[1];
    uint16_t server_port = 7000;
    if (argc >= 3) {
        int p = std::atoi(argv[2]);
        if (p <= 0 || p > 65535) {
            fprintf(stderr, "invalid server port: %s\n", argv[2]);
            return 1;
        }
        server_port = static_cast<uint16_t>(p);
    }

    LOG_INFO("tunnel client starting -> %s:%u", server_ip.c_str(), server_port);

    tunnel::client::TunnelClient client(server_ip, server_port);
    client.Run();

    LOG_INFO("tunnel client exit");
    return 0;
}
