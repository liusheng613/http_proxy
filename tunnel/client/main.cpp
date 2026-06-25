#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "../common/logger.h"
#include "tunnel_client.h"

// 用法: tunnel_client <server_ip> [server_port] [-n name] [-L local_port:remote_port] [-d]
//   server_ip:   公网 server 的 IP
//   server_port: 控制隧道端口, 默认 7000
//   -n name:     client 名字 (可选, 用于链路探活路由)
//   -L: 端口映射 (可多个), 例如 -L 22:10022 表示把本地 22 映射到 server 公网 10022
//   -d: 开启 DEBUG 日志 (默认 INFO)
int main(int argc, char** argv) {
    ::signal(SIGPIPE, SIG_IGN);

    if (argc < 2) {
        fprintf(stderr,
                "usage: %s <server_ip> [server_port] [-n name] [-L local:remote] [-d]\n"
                "  example: %s 1.2.3.4 7000 -n client_a -L 22:10022 -L 8080:18080 -d\n",
                argv[0], argv[0]);
        return 1;
    }

    std::string server_ip;
    uint16_t server_port = 7000;
    bool got_port = false;
    std::vector<tunnel::client::PortMapping> mappings;
    std::string name;

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "-d") == 0 || std::strcmp(argv[i], "--debug") == 0) {
            tunnel::set_log_level(tunnel::LOG_DEBUG);
            continue;
        }
        if (std::strcmp(argv[i], "-n") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "-n requires argument (client name)\n");
                return 1;
            }
            ++i;
            name = argv[i];
            continue;
        }
        if (std::strcmp(argv[i], "-L") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "-L requires argument (local_port:remote_port)\n");
                return 1;
            }
            ++i;
            // 解析 "local_port:remote_port"
            char* colon = std::strchr(argv[i], ':');
            if (!colon) {
                fprintf(stderr, "invalid mapping format: %s (expected local:remote)\n",
                        argv[i]);
                return 1;
            }
            uint16_t local_port = static_cast<uint16_t>(std::atoi(argv[i]));
            uint16_t remote_port = static_cast<uint16_t>(std::atoi(colon + 1));
            if (local_port == 0 || remote_port == 0) {
                fprintf(stderr, "invalid port in mapping: %s\n", argv[i]);
                return 1;
            }
            mappings.emplace_back(local_port, remote_port);
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
        fprintf(stderr, "usage: %s <server_ip> [server_port] [-n name] [-L local:remote] [-d]\n",
                argv[0]);
        return 1;
    }

    LOG_INFO("tunnel client starting -> %s:%u (%zu mappings, name='%s')",
             server_ip.c_str(), server_port, mappings.size(), name.c_str());

    tunnel::client::TunnelClient client(server_ip, server_port, mappings, name);
    client.Run();

    LOG_INFO("tunnel client exit");
    return 0;
}
