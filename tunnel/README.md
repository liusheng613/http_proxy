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

## 参数说明

### tunnel_server

```
tunnel_server [control_port] [-d]
```

| 参数 | 说明 | 默认值 |
|------|------|--------|
| `control_port` | 控制隧道端口 | 7000 |
| `-d` | 开启 DEBUG 级别日志 | INFO |

### tunnel_client

```
tunnel_client <server_ip> [server_port] [-n name] [-L local:remote] [-R local:target:port] [-C target:port] [-d]
```

| 参数 | 说明 | 默认值 |
|------|------|--------|
| `server_ip` | 公网 server 的 IP（必填） | — |
| `server_port` | 控制隧道端口 | 7000 |
| `-n name` | client 名字，用于中继路由和探活 | 空 |
| `-L local:remote` | 端口映射（可多个）：暴露本地端口到 server 公网 | — |
| `-R local:target:port` | 本地中继（可多个）：本地监听, 收到连接后中继到目标 client | — |
| `-C target:port` | 启动后自动发起中继连接到目标 client | — |
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
