#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "../common/crypto.h"
#include "../common/frame.h"
#include "../common/logger.h"
#include "tunnel_server.h"

// 用法: tunnel_server [control_port] [-t token] [-k secret] [-d]
//   control_port: 控制端口 (默认 7000)
//   -t token:    鉴权 token (未指定则不鉴权)
//   -k secret:   AES-256 加密密钥 (未指定则不加密)
//   -d:          开启 DEBUG 日志
int main(int argc, char** argv) {
    ::signal(SIGPIPE, SIG_IGN);

    uint16_t control_port = 7000;
    std::string secret;
    std::string token;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "-d") == 0 || std::strcmp(argv[i], "--debug") == 0) {
            tunnel::set_log_level(tunnel::LOG_DEBUG);
            continue;
        }
        if (std::strcmp(argv[i], "-k") == 0) {
            if (i + 1 >= argc) { fprintf(stderr, "-k requires argument\n"); return 1; }
            ++i; secret = argv[i]; continue;
        }
        if (std::strcmp(argv[i], "-t") == 0) {
            if (i + 1 >= argc) { fprintf(stderr, "-t requires argument\n"); return 1; }
            ++i; token = argv[i]; continue;
        }
        int p = std::atoi(argv[i]);
        if (p <= 0 || p > 65535) {
            fprintf(stderr, "invalid control port: %s\n", argv[i]);
            return 1;
        }
        control_port = static_cast<uint16_t>(p);
    }

    if (!secret.empty()) {
        tunnel::tunnel_set_crypto_key(tunnel::crypto::derive_key(secret));
        LOG_INFO("encryption enabled (AES-256-GCM)");
    }
    if (!token.empty()) {
        LOG_INFO("token auth enabled");
    }

    LOG_INFO("tunnel server starting (port=%u)", control_port);

    tunnel::server::TunnelServer server(control_port, token);
    server.Run();

    LOG_INFO("tunnel server exit");
    return 0;
}
