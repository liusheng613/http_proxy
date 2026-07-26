# Tunnel - 内网穿透组网工具

轻量级的内网穿透 + 客户端组网工具，基于 C++ / epoll 实现，支持多客户端同时在线、端口映射、客户端间中继互访。

## 架构

```
                   公网服务器 (tunnel_server)
                  ┌──────────────────────────┐
                  │  控制端口 (默认 7000)      │
                  │  + 映射端口 (动态分配)     │
                  └──────┬──────────────────┘
                        ╱ ╲
                隧道(TCP)  隧道(TCP)
                  ╱              ╲
        ┌────────┘              ┌────────┘
  内网客户端 A            内网客户端 B
   (tunnel_client)         (tunnel_client)
        │                       │
   本地服务(SSH/HTTP/...)    本地服务(SSH/HTTP/...)
```

- **tunnel_server**：部署在公网服务器，接受所有 client 的隧道连接，负责协调和数据中继
- **tunnel_client**：部署在内网机器，主动连 server，暴露本地服务或访问其他 client

## 快速开始

### 编译

```bash
# 依赖: cmake, ninja, g++ (支持 C++17)
cd http_proxy
bash ninja_build.sh release
```

编译产物：
- `build/tunnel/tunnel_server` — 服务端
- `build/tunnel/tunnel_client` — 客户端

### 1. 启动服务端

控制端口默认 `7000`，可通过参数修改：

```bash
./build/tunnel/tunnel_server          # 默认 7000
./build/tunnel/tunnel_server 7000     # 显式指定 7000
./build/tunnel/tunnel_server 8888     # 改用 8888
# 或带 DEBUG 日志:
./build/tunnel/tunnel_server 7000 -d
```

### 2. 启动客户端

```bash
# client_A: 暴露本地 SSH(22) 到 server 公网 10022 端口
# 7000 是 server 的控制端口(与 server 启动时的端口一致即可)
./build/tunnel/tunnel_client <SERVER_IP> 7000 -n client_A -L 22:10022

# client_B: 只注册名字, 不暴露服务
./build/tunnel/tunnel_client <SERVER_IP> 7000 -n client_B
```

> **注意**：示例中的 `7000` 是 server 的控制端口（默认值），可根据需要修改，client 连 server 时保持一致即可。

## 使用方式

### 方式 1：端口映射（-L）— 外网访问内网服务

将内网某个端口映射到 server 的公网端口，外部用户直接访问 server 即可。

```
外部用户 ──► server:10022 ──隧道──► client_A 本地 :22 (SSH)
```

```bash
# client_A 上执行 (7000 是 server 的控制端口, 保持一致即可):
./tunnel_client <SERVER_IP> 7000 -n client_A -L 22:10022
```

- `-L <local_port>:<remote_port>`
- `local_port`：client 本地服务的端口
- `remote_port`：server 上对外监听的公网端口

**SSH 示例：**

```bash
# client_A 暴露 SSH 到 server 的 10022 端口
client_A$ ./tunnel_client <SERVER_IP> 7000 -n client_A -L 22:10022

# 在任意机器上 SSH 到 client_A
any_machine$ ssh -p 10022 user@<SERVER_IP>
```

可以同时映射多个端口：

```bash
./tunnel_client <SERVER_IP> 7000 -n client_A -L 22:10022 -L 8080:18080
```

### 方式 2：本地中继（-R）— 客户端组网互访

在本地监听一个端口，收到连接后经 server 中继转发到**另一个客户端**的指定端口。
这种方式不需要在 server 上开公网端口，更安全。

```
ssh -p 10022 127.0.0.1  ──►  client_A:10022  ──中继──►  server  ──中继──►  client_B:22
```

```bash
# client_B 只需要注册名字（不需要 -L 暴露端口）
client_B$ ./tunnel_client <SERVER_IP> 7000 -n client_B

# client_A 本地监听 10022，收到连接后中继到 client_B 的 22 端口
client_A$ ./tunnel_client <SERVER_IP> 7000 -n client_A -R 10022:client_B:22
```

- `-R <local_port>:<target_name>:<target_port>`
- `local_port`：本机监听端口
- `target_name`：目标 client 注册的名字（-n 参数指定的名字）
- `target_port`：目标 client 本地服务的端口

**SSH 示例：**

```bash
client_B$ ./tunnel_client <SERVER_IP> 7000 -n client_B

client_A$ ./tunnel_client <SERVER_IP> 7000 -n client_A -R 10022:client_B:22

# client_A 直接 SSH 到本地端口，经中继到达 client_B
client_A$ ssh -p 10022 user@127.0.0.1
```

SSH 密钥交换、密码认证等全部正常工作。

### 方式 3：自动中继（-C）— 命令行触发

启动后自动发起中继连接到目标 client 的指定端口。

```bash
./tunnel_client <SERVER_IP> 7000 -n client_A -C client_B:22
```

启动后 client_A 会自动发送中继请求到 client_B:22，建立中继会话。
这种方式适合脚本化 / 自动化使用。

### 交互命令

不带 `-R` 或 `-C` 参数启动时，client 进入交互模式，支持以下命令：

```
connect <target_name> <port>  发起中继连接 (stdin ↔ relay)
quit / exit                   退出客户端
```

示例：

```bash
$ ./tunnel_client <SERVER_IP> 7000 -n client_A
...
> connect client_B 22
relay session 1 established, stdin ↔ relay active
> hello                    # 输入内容会转发到 client_B:22
```

### 链路探活

PROBE 命令用于检测某个 client 是否在线：

```bash
# 向 server 发送 PROBE 请求（使用 Python 脚本）
python3 << 'EOF'
import socket, struct
s=socket.socket(); s.settimeout(5)
s.connect((SERVER_IP, 7000))
# REGISTER
s.sendall(struct.pack(">HBI",0x544e,0x02,6)+struct.pack("B",5)+b"probe")
# PROBE client_B
target=b"client_B"
payload=struct.pack("B",len(target))+target+struct.pack(">I",1)
s.sendall(struct.pack(">HBI",0x544e,0x08,len(payload))+payload)
# 读回复
data=s.recv(1024)
EOF
```

返回 `status=0` 表示在线，`status=1` 表示不存在。

### 方式 4：TUN 虚拟网卡（--tun）— 全透明组网，自动分配 IP

server 指定一个子网，client 加 `--tun` 即可，**无需手动配 IP**。server 自动从子网中分配未使用的 IP。需要 root 权限。

```
client_a (自动分配 10.0.0.2)  ←── TUN ──→  server  ←── TUN ──→  client_b (自动分配 10.0.0.3)
```

```bash
# server 指定子网（+ token 鉴权，推荐）
server$ ./tunnel_server 7000 -t "my_token" --tun-subnet 10.0.0.0/24

# client 只需 --tun + token，无需指定 IP
client_A$ sudo ./tunnel_client <SERVER_IP> 7000 -n node_a -t "my_token" --tun
client_B$ sudo ./tunnel_client <SERVER_IP> 7000 -n node_b -t "my_token" --tun

# server 自动分配：node_a→10.0.0.2, node_b→10.0.0.3
# 同一 token 的所有 client 自动进入同一子网
# 然后可以直接 ping、SSH
ping 10.0.0.3
ssh user@10.0.0.2
```

### Token 鉴权（-t）

防止未授权的 client 接入 server。**server 和所有 client 共享同一个 token**。

**生成 token**（推荐 16 字符以上随机字符串）：

```bash
# 生成随机 token
openssl rand -base64 16          # 例: xG8kP2vL9nQ4mR7sA==
openssl rand -hex 16             # 例: a3f2c89b1e4d5f7a0b2c3d4e5f6a7b8c
```

**工作原理**：

```
client ──TCP连接──► server
client ──AUTH{token}──► server ──token 正确→ ACK(ok) → 后续正常通信（含 TUN 分配）
                                  token 错误→ 断开连接 → client 自动重连
```

**配置方式（三种等价，选一种）**：

```bash
# 方式 A：命令行参数
server$  ./tunnel_server 7000 -t "xG8kP2vL9nQ4mR7sA=="
client$  ./tunnel_client <IP> 7000 -n host -L 22:10022 -t "xG8kP2vL9nQ4mR7sA=="

# 方式 B：配置文件 (推荐，不泄密)
# server.conf         client.conf
# token = xG8k...      token = xG8k...
server$  ./tunnel_server 7000 -c server.conf
client$  ./tunnel_client <IP> 7000 -c client.conf

# 方式 C：同时使用 token + TUN 组网
server$  ./tunnel_server 7000 -t "xG8k..." --tun-subnet 10.0.0.0/24
client$  sudo ./tunnel_client <IP> 7000 -n node_a -t "xG8k..." --tun
# ↑ token 正确才分配 IP，同 token 的所有 client 自动划入同一子网
```

**行为说明**：

| 场景 | 结果 |
|------|------|
| client 带正确 token | ✅ AUTH 通过，正常通信 |
| client 带错误 token | ❌ server 断开连接，client 每 5s 重试 |
| client 不带 token（server 需要） | ❌ 第一条非 AUTH 消息就触发断开 |
| server 不设 token（`-t` 省略） | 所有 client 跳过鉴权，直接通信 |

**安全性**：token 在 AUTH 帧中以**明文**传输（帧 payload 未加密）。如果担心 token 被网络抓包，建议同时启用 **-k 加密**（下一节），所有帧 payload 被 AES 加密，token 自然也被保护。

### AES-256-GCM 加密（-k）

对所有帧的 payload（含控制消息和用户数据）进行加密，保护传输内容不被窃听或篡改。

**密钥要求**：任意字符串，server 和所有 client 必须一致。

```bash
# 生成密钥
openssl rand -base64 32    # 32 字节随机密钥
```

**工作原理**：

```
原始 payload ──AES-256-GCM──► nonce(12B) + ciphertext + tag(16B)
                           密钥 = SHA256(原始字符串)
                           每帧随机 nonce，防止重放攻击
                           tag 提供完整性校验（篡改即检测）
```

**配置**：

```bash
server$  ./tunnel_server 7000 -t my_token -k "my_secret_key"
client$  ./tunnel_client <IP> 7000 -t my_token -k "my_secret_key" ...
```

**性能影响**：每帧多 28 字节（nonce 12 + tag 16），加解密耗时微秒级（AES-NI 硬件加速）。

### 鉴权 + 加密 推荐配置

建议 **同时启用 -t 和 -k**，安全级别最高：

```bash
# server（配置文件：server.conf）
port  = 7000
token = xG8kP2vL9nQ4mR7
key   = jF6bD3sA1wE5tY9

# client（配置文件：client.conf）
server_ip   = 1.2.3.4
server_port = 7000
name        = my_host
token       = xG8kP2vL9nQ4mR7
key         = jF6bD3sA1wE5tY9
mappings    = 22:10022
```

## 参数说明

### tunnel_server

```
tunnel_server [control_port] [-t token] [-k secret] [-c config] [-d]
```

| 参数 | 说明 | 默认值 |
|------|------|--------|
| `control_port` | 控制隧道端口 | 7000 |
| `-t token` | 鉴权 token（所有 client 须一致） | 无（不鉴权） |
| `-k secret` | AES-256-GCM 加密密钥（所有 client 须一致） | 无（不加密） |
| `-c config` | 配置文件路径 | — |
| `--tun-subnet 10.0.0.0/24` | TUN 子网，server 从此子网自动分配 IP | 无（不启用 TUN） |
| `-d` | 开启 DEBUG 级别日志 | INFO |

### tunnel_client

```
tunnel_client <server_ip> [server_port] [-n name] [-L local:remote] [-R local:target:port] [-C target:port] [-i tun_ip] [-t token] [-k secret] [-c config] [-d]
```

| 参数 | 说明 | 默认值 |
|------|------|--------|
| `server_ip` | 公网 server 的 IP（必填） | — |
| `server_port` | 控制隧道端口 | 7000 |
| `-n name` | client 名字，用于中继路由和探活 | 空 |
| `-L local:remote` | 端口映射（可多个） | — |
| `-R local:target:port` | 本地中继（可多个） | — |
| `-C target:port` | 启动后自动中继连接 | — |
| `--tun` | 启用 TUN 虚拟网卡（server 自动分配 IP，需 sudo） | 不启用 |
| `-t token` | 鉴权 token（与 server 一致） | 无 |
| `-k secret` | AES-256-GCM 加密密钥（与 server 一致） | 无 |
| `-c config` | 配置文件路径 | — |
| `-d` | 开启 DEBUG 级别日志 | INFO |

## 完整示例

### 场景：两台内网机器通过公网 server 互访 SSH

```
机器 A (有 SSH 服务)    机器 B (要 SSH 到 A)
       │                       │
       └─── tunnel_client ─────┼──── server(公网)
                               │
                    tunnel_client ───┘
                               │
                          ssh -p 10022 127.0.0.1
```

1. 公网 server 启动：

```bash
server$ ./tunnel_server 7000
```

2. 内网机器 A 启动 client（注册名字 + 暴露 SSH 端口）：

```bash
# 方案一：端口映射（server 开公网端口）
A$ ./tunnel_client server_ip 7000 -n host_a -L 22:10022
# 然后任意机器：ssh -p 10022 user@server_ip

# 方案二：本地中继（更安全，不需要 server 开端口）
A$ ./tunnel_client server_ip 7000 -n host_a
# 注意：方案二不需要 -L，只需要注册名字
```

3. 内网机器 B 启动 client，通过本地中继访问 A 的 SSH：

```bash
B$ ./tunnel_client server_ip 7000 -n host_b -R 10022:host_a:22
B$ ssh -p 10022 user@127.0.0.1   # 直接 SSH！
```

### 场景：同时暴露多个服务

```bash
# 暴露 SSH(22) 和 HTTP(8080)
./tunnel_client server_ip 7000 -n my_server -L 22:10022 -L 8080:18080

# 外部访问：
#   ssh -p 10022 user@server_ip
#   curl http://server_ip:18080
```

## 通信协议

帧格式（二进制，网络字节序）：

```
 0                   1                   2                   3
 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|          magic (0x544e)      |  type (1 byte) |  payload_len  |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                         payload ...                           |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
```

消息类型：

| 类型 | 值 | 方向 | 说明 |
|------|-----|------|------|
| HEARTBEAT | 0x01 | 双向 | 心跳保活 |
| REGISTER | 0x02 | client→server | 注册名字 |
| PORT_MAP | 0x03 | client→server | 端口映射上报 |
| NEW_CONN | 0x04 | 双向 | 新建连接/中继请求 |
| DATA | 0x05 | 双向 | 用户数据 |
| CLOSE | 0x06 | 双向 | 关闭会话 |
| ACK | 0x07 | server→client | 通用应答 |
| PROBE | 0x08 | client→server→client | 链路探活 |
| PROBE_REPLY | 0x09 | client→server→client | 探活应答 |
| AUTH | 0x0F | client→server | token 鉴权 |
| P2P_TRY/P2P_PORT/P2P_INFO/P2P_OK/P2P_FAIL | 0x0A~0x0E | 双向 | P2P 打洞协商 |
| TUN_PACKET | 0x10 | 双向 | TUN 虚拟网卡 IP 包 |

## 构建

```bash
# 依赖安装 (Debian/Ubuntu)
sudo apt install build-essential cmake ninja-build

# 编译
cd http_proxy
bash ninja_build.sh release    # Release 模式
bash ninja_build.sh debug      # Debug 模式
```

## 注意事项

1. **SIGPIPE**：程序已忽略 SIGPIPE 信号，写已关闭 socket 不会崩溃
2. **心跳超时**：30 秒无消息判定断线，client 自动重连
3. **端口冲突**：两个 client 注册相同 remote_port 时，后注册的被拒绝
4. **名字冲突**：两个 client 注册相同名字时，后注册的被拒绝
5. **非阻塞**：所有 socket 均为非阻塞 + epoll ET 模式

## 配置文件

支持通过 `key=value` 格式的配置文件简化启动参数：

```bash
# server
./tunnel_server -c server.conf

# client
./tunnel_client -c client.conf
```

命令行参数优先级高于配置文件（即 `-t` 会覆盖配置文件中的 `token` 值）。

### server 配置示例 (`server.conf`)

```ini
port  = 7000          # 控制端口
token = my_token      # 鉴权 (可选)
key   = my_secret     # 加密 (可选)
```

### client 配置示例 (`client.conf`)

```ini
server_ip   = 1.2.3.4       # server 公网 IP
server_port = 7000          # 控制端口
name        = my_client     # client 名字
token       = my_token      # 鉴权 (可选)
key         = my_secret     # 加密 (可选)
mappings    = 22:10022      # 端口映射，多个用逗号分隔
tun         = true          # 启用 TUN 虚拟网卡 (可选)
```

## 部署

### 安装

```bash
# 复制二进制文件
sudo cp build/tunnel/tunnel_server /usr/local/bin/
sudo cp build/tunnel/tunnel_client /usr/local/bin/

# 创建配置目录
sudo mkdir -p /etc/tunnel
sudo cp tunnel/server.conf.example /etc/tunnel/server.conf
sudo cp tunnel/client.conf.example /etc/tunnel/client.conf
# 编辑配置文件

# 安装 systemd 服务
sudo cp tunnel/tunnel_server.service /etc/systemd/system/
sudo cp tunnel/tunnel_client.service /etc/systemd/system/
sudo systemctl daemon-reload
```

### 启动

```bash
# server
sudo systemctl enable --now tunnel_server

# client
sudo systemctl enable --now tunnel_client
```

### 查看日志

```bash
journalctl -u tunnel_server -f
journalctl -u tunnel_client -f
```
