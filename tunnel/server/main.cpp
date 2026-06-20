#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "../common/logger.h"
#include "tunnel_server.h"

// 用法: tunnel_server [control_port] [-d]
//   control_port: 监听的控制隧道端口, 默认 7000
//   -d: 开启 DEBUG 日志 (默认 INFO)
int main(int argc, char** argv) {
    ::signal(SIGPIPE, SIG_IGN);

    uint16_t control_port = 7000;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "-d") == 0 || std::strcmp(argv[i], "--debug") == 0) {
            tunnel::set_log_level(tunnel::LOG_DEBUG);
            continue;
        }
        int p = std::atoi(argv[i]);
        if (p <= 0 || p > 65535) {
            fprintf(stderr, "invalid control port: %s\n", argv[i]);
            return 1;
        }
        control_port = static_cast<uint16_t>(p);
    }

    LOG_INFO("tunnel server starting (control_port=%u)", control_port);

    tunnel::server::TunnelServer server(control_port);
    server.Run();

    LOG_INFO("tunnel server exit");
    return 0;
}
