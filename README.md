# http_proxy

C++ 网络编程练习仓库，包含 TCP 代理、epoll 服务器、**内网穿透组网工具 Tunnel**。

## 目录

```
.
├── practice/                           # epoll LT/ET echo 服务器练习
├── proxy/                              # fork / epoll 版 TCP 代理
└── tunnel/                             # 内网穿透组网工具（核心）
    ├── README.md                       # 详细使用文档
    ├── common/                         # 公共库（帧协议/加密/日志/TUN）
    ├── server/                         # 公网服务端
    └── client/                         # 内网客户端
```

## 构建

```bash
sudo apt install build-essential cmake ninja-build libssl-dev
bash ninja_build.sh release
```

产物：`build/tunnel/tunnel_server`、`build/tunnel/tunnel_client`

> **Windows 用户**：通过 WSL2 运行，环境与 Linux 完全一致。详见 [tunnel/README.md](tunnel/README.md) 末尾「Windows / WSL2」章节。

## Tunnel 快速开始

```bash
# 1. 公网 server（带鉴权 + 加密，推荐）
./build/tunnel/tunnel_server 7000 -t my_token -k my_secret

# 2. 内网 client_A（暴露本地 SSH 到 server 公网端口）
./build/tunnel/tunnel_client <SERVER_IP> 7000 -n client_a -L 22:10022 -t my_token

# 3. 内网 client_B（暴露本地 Web 到另一个公网端口）
./build/tunnel/tunnel_client <SERVER_IP> 7000 -n client_b -L 8080:18080 -t my_token

# 外部用户直接访问：
#   ssh -p 10022 user@<SERVER_IP>   → client_A 的 SSH
#   curl http://<SERVER_IP>:18080   → client_B 的 Web
```

### TUN 虚拟网卡组网（client 间真 ping/SSH）

想让多个 client 之间**互相 ping 通、直接 SSH**，server 指定一个子网，client 加 `--tun` 即可，**无需手动配 IP**。同一 token 的所有 client 自动划入同一子网：

```bash
# server 指定 TUN 子网 + token 鉴权
./build/tunnel/tunnel_server 7000 --tun-subnet 10.0.0.0/24 -t my_token

# client 只需加 --tun + 相同 token，server 自动分配虚拟 IP
sudo ./build/tunnel/tunnel_client <SERVER_IP> 7000 -n node_a --tun -t my_token
sudo ./build/tunnel/tunnel_client <SERVER_IP> 7000 -n node_b --tun -t my_token
sudo ./build/tunnel/tunnel_client <SERVER_IP> 7000 -n node_c --tun -t my_token

# 分配结果：node_a→10.0.0.2, node_b→10.0.0.3, node_c→10.0.0.4
# 现在任意两台机器间可以 ping 和 SSH：
ping 10.0.0.3
ssh user@10.0.0.2

# TUN 组网和端口映射可以同时启用：
sudo ./build/tunnel/tunnel_client <SERVER_IP> 7000 -n node_a --tun -L 22:10022 -t my_token
```

## 常用参数

| 参数 | 说明 |
|------|------|
| `-L local:remote` | 端口映射：内网端口暴露到 server 公网端口 |
| `-R local:target:port` | 本地中继：本地监听后经 server 转发到另一 client |
| `--tun` | 启用 TUN 虚拟网卡（server 自动分配 IP，需 sudo） |
| `-t token` | token 鉴权（server 和所有 client 用同一个 token） |
| `-k secret` | AES-256-GCM 加密密钥（server 和所有 client 用同一个） |
| `--tun-subnet 10.0.0.0/24` | （仅 server）TUN 子网，server 从此子网分配 IP |
| `-c config` | 从配置文件加载参数 |
| `-d` | DEBUG 日志 |

详细文档：**[tunnel/README.md](tunnel/README.md)**
