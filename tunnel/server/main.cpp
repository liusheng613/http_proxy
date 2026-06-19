#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "../common/logger.h"
#include "tunnel_server.h"

// 用法: tunnel_server [control_port]
//   control_port: 监听的控制隧道端口, 默认 7000
int main(int argc, char** argv) {
    ::signal(SIGPIPE, SIG_IGN);  // 写已关闭 socket 不崩溃

    uint16_t control_port = 7000;
    if (argc >= 2) {
        int p = std::atoi(argv[1]);
        if (p <= 0 || p > 65535) {
            fprintf(stderr, "invalid control port: %s\n", argv[1]);
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
