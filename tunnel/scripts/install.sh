#!/bin/bash
set -euo pipefail

# =============================================
# Tunnel 一键部署脚本
# 用法:
#   sudo ./install.sh server    # 安装服务端
#   sudo ./install.sh client    # 安装客户端
#   sudo ./install.sh all       # 全部安装
# =============================================

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
BUILD_DIR="$PROJECT_DIR/build"

RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; NC='\033[0m'

info()  { echo -e "${GREEN}[INFO]${NC} $*"; }
warn()  { echo -e "${YELLOW}[WARN]${NC} $*"; }
die()   { echo -e "${RED}[FATAL]${NC} $*" >&2; exit 1; }

require_root() {
    if [[ $EUID -ne 0 ]]; then
        die "请用 root 或 sudo 运行此脚本"
    fi
}

# ---- 检查依赖 ----
check_deps() {
    local missing=()
    command -v cmake  >/dev/null 2>&1 || missing+=(cmake)
    command -v ninja  >/dev/null 2>&1 || missing+=(ninja-build)
    command -v g++    >/dev/null 2>&1 || missing+=(g++)
    pkg-config --exists openssl 2>/dev/null || missing+=(libssl-dev)

    if [[ ${#missing[@]} -gt 0 ]]; then
        warn "缺少依赖: ${missing[*]}"
        if command -v apt-get >/dev/null 2>&1; then
            info "尝试自动安装..."
            apt-get update -qq
            apt-get install -y -qq "${missing[@]}"
        elif command -v yum >/dev/null 2>&1; then
            yum install -y "${missing[@]}"
        else
            die "请手动安装: ${missing[*]}"
        fi
    fi
}

# ---- 编译 ----
do_build() {
    info "编译中..."
    mkdir -p "$BUILD_DIR"
    cd "$BUILD_DIR"
    cmake .. -DCMAKE_BUILD_TYPE=Release -G Ninja 2>&1 | tail -3
    ninja -j"$(nproc)" 2>&1 | tail -5
}

# ---- 安装二进制 ----
install_bin() {
    info "安装二进制到 /usr/local/bin ..."
    cp "$BUILD_DIR/tunnel/tunnel_server" /usr/local/bin/
    cp "$BUILD_DIR/tunnel/tunnel_client" /usr/local/bin/
    chmod 755 /usr/local/bin/tunnel_{server,client}
}

# ---- 安装服务端配置 ----
install_server_config() {
    mkdir -p /etc/tunnel
    if [[ ! -f /etc/tunnel/server.conf ]]; then
        info "生成默认服务端配置 /etc/tunnel/server.conf"
        cat > /etc/tunnel/server.conf <<'EOF'
# Tunnel Server 配置
port=7000
token=your_shared_token_here
key=your_32char_encryption_key_here
tun_subnet=10.0.0.0/24
EOF
        warn "请编辑 /etc/tunnel/server.conf 填入实际的 token 和 key"
        warn "生成 key:  openssl rand -base64 32 | head -c32"
        warn "生成 token: openssl rand -base64 18 | head -c22"
    else
        info "服务端配置已存在, 跳过"
    fi
    chmod 600 /etc/tunnel/server.conf
}

# ---- 安装客户端配置 ----
install_client_config() {
    mkdir -p /etc/tunnel
    if [[ ! -f /etc/tunnel/client.conf ]]; then
        info "生成默认客户端配置 /etc/tunnel/client.conf"
        cat > /etc/tunnel/client.conf <<'EOF'
# Tunnel Client 配置
server_ip=your.server.ip.here
server_port=7000
token=your_shared_token_here
key=your_32char_encryption_key_here
name=my_client
tun=true
EOF
        warn "请编辑 /etc/tunnel/client.conf 填入实际的 server_ip、token 和 key"
    else
        info "客户端配置已存在, 跳过"
    fi
    chmod 600 /etc/tunnel/client.conf
}

# ---- 安装 systemd 服务 ----
install_service() {
    local role="$1"  # server or client
    info "安装 tunnel-${role}.service ..."
    cp "$SCRIPT_DIR/tunnel-${role}.service" /etc/systemd/system/
    systemctl daemon-reload
    systemctl enable "tunnel-${role}"
    systemctl restart "tunnel-${role}"
    sleep 1
    systemctl status "tunnel-${role}" --no-pager -l || true
}

# ---- 主流程 ----
main() {
    local target="${1:-}"
    case "$target" in
        server|client|all) ;;
        *) die "用法: sudo $0 {server|client|all}" ;;
    esac

    require_root
    check_deps
    do_build
    install_bin

    if [[ "$target" == "server" || "$target" == "all" ]]; then
        install_server_config
        install_service server
    fi

    if [[ "$target" == "client" || "$target" == "all" ]]; then
        install_client_config
        install_service client
    fi

    info "部署完成!"
    echo ""
    info "常用命令:"
    echo "  systemctl status tunnel-server"
    echo "  systemctl status tunnel-client"
    echo "  journalctl -u tunnel-server -f"
    echo "  journalctl -u tunnel-client -f"
}

main "$@"
