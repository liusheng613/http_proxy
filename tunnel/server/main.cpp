#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>

#include "../common/config.h"
#include "../common/crypto.h"
#include "../common/frame.h"
#include "../common/logger.h"
#include "tunnel_server.h"

// 用法: tunnel_server [port] [-t token] [-k secret] [-c config] [-d]
int main(int argc, char** argv) {
    ::signal(SIGPIPE, SIG_IGN);

    uint16_t control_port = 7000;
    std::string secret;
    std::string token;
    std::string config_file;
    std::string tun_subnet;

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "-d") == 0 || std::strcmp(argv[i], "--debug") == 0) {
            tunnel::set_log_level(tunnel::LOG_DEBUG); continue;
        }
        if (std::strcmp(argv[i], "-k") == 0) {
            if (i + 1 >= argc) { fprintf(stderr, "-k requires argument\n"); return 1; }
            ++i; secret = argv[i]; continue;
        }
        if (std::strcmp(argv[i], "-t") == 0) {
            if (i + 1 >= argc) { fprintf(stderr, "-t requires argument\n"); return 1; }
            ++i; token = argv[i]; continue;
        }
        if (std::strcmp(argv[i], "-c") == 0) {
            if (i + 1 >= argc) { fprintf(stderr, "-c requires argument\n"); return 1; }
            ++i; config_file = argv[i]; continue;
        }
        if (std::strcmp(argv[i], "--tun-subnet") == 0) {
            if (i + 1 >= argc) { fprintf(stderr, "--tun-subnet requires argument\n"); return 1; }
            ++i; tun_subnet = argv[i]; continue;
        }
        int p = std::atoi(argv[i]);
        if (p <= 0 || p > 65535) { fprintf(stderr, "invalid port: %s\n", argv[i]); return 1; }
        control_port = static_cast<uint16_t>(p);
    }

    // 未指定 -c 时自动查找默认配置文件
    if (config_file.empty()) {
        const char* defaults[] = {
            "/etc/tunnel/server.conf",
            "tunnel_server.conf",
            "../tunnel/server.conf",
        };
        std::ifstream f;
        for (const char* p : defaults) {
            f.open(p);
            if (f.good()) { config_file = p; break; }
            f.close();
        }
    }

    // 加载配置文件 (命令行参数优先)
    if (!config_file.empty()) {
        tunnel::Config cfg;
        if (cfg.LoadFromFile(config_file)) {
            if (control_port == 7000 && cfg.HasKey("port"))
                control_port = static_cast<uint16_t>(std::atoi(cfg.Get("port").c_str()));
            if (token.empty()) token = cfg.Get("token");
            if (secret.empty()) secret = cfg.Get("key");
            if (tun_subnet.empty()) tun_subnet = cfg.Get("tun_subnet");
        }
    }

    if (!secret.empty()) {
        tunnel::tunnel_set_crypto_key(tunnel::crypto::derive_key(secret));
        LOG_INFO("encryption enabled (AES-256-GCM)");
    }
    if (!token.empty()) LOG_INFO("token auth enabled");

    LOG_INFO("tunnel server starting (port=%u)", control_port);
    tunnel::server::TunnelServer server(control_port, token, tun_subnet);
    server.Run();
    LOG_INFO("tunnel server exit");
    return 0;
}
