#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "../common/logger.h"
#include "tunnel_client.h"

// 用法: tunnel_client <server_ip> [server_port] [-d]
//   server_ip:   公网 server 的 IP
//   server_port: 控制隧道端口, 默认 7000
//   -d: 开启 DEBUG 日志 (默认 INFO)
int main(int argc, char** argv) {
    ::signal(SIGPIPE, SIG_IGN);

    if (argc < 2) {
        fprintf(stderr, "usage: %s <server_ip> [server_port] [-d]\n", argv[0]);
        return 1;
    }

    std::string server_ip;
    uint16_t server_port = 7000;
    bool got_port = false;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "-d") == 0 || std::strcmp(argv[i], "--debug") == 0) {
            tunnel::set_log_level(tunnel::LOG_DEBUG);
            continue;
        }
        if (server_ip.empty()) {
            server_ip = argv[i];
            continue;
        }
        if (!got_port) {
            int p = std::atoi(argv[i]);
            if (p <= 0 || p > 65535) {
                fprintf(stderr, "invalid server port: %s\n", argv[i]);
                return 1;
            }
            server_port = static_cast<uint16_t>(p);
            got_port = true;
        }
    }
    if (server_ip.empty()) {
        fprintf(stderr, "usage: %s <server_ip> [server_port] [-d]\n", argv[0]);
        return 1;
    }

    LOG_INFO("tunnel client starting -> %s:%u", server_ip.c_str(), server_port);

    tunnel::client::TunnelClient client(server_ip, server_port);
    client.Run();

    LOG_INFO("tunnel client exit");
    return 0;
}
