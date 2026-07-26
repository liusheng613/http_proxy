#ifndef TUNNEL_COMMON_PROTOCOL_H_
#define TUNNEL_COMMON_PROTOCOL_H_

#include <cstdint>
#include <cstring>
#include <string>
#include <arpa/inet.h>  // htons/htonl/ntohs/ntohl

// =============================================================================
// 隧道应用层帧协议 (所有 client <-> server 通信都走这个帧格式)
//
// 线上格式 (网络字节序):
//   +--------+--------+--------+-----------+
//   | magic  | type   |  payload_len       |
//   | 2 byte | 1 byte |  4 byte            |
//   +--------+--------+--------+-----------+
//   |            payload (变长)             |
//   +---------------------------------------+
//
//   magic : 0x544e ('TN'), 用于帧同步/校验
//   type  : MessageType
//   len   : payload 字节数
//
// 说明: payload 的具体结构由各消息类型自行约定 (见各 message 的注释)。
//       frame 头部固定 7 字节, 便于收发解析。
// =============================================================================

namespace tunnel {

static constexpr uint16_t kFrameMagic = 0x544e;  // 'TN'
static constexpr int kFrameHeaderLen = 7;
static constexpr uint32_t kMaxPayloadLen = 16 * 1024 * 1024;  // 16MB 防护

// 消息类型。注意: client<->server 是控制隧道消息; 用户数据的承载是 DATA。
enum class MessageType : uint8_t {
    // 心跳: 双向, 用于保活与链路探活。payload 空。
    HEARTBEAT = 0x01,

    // 注册: client 上线时上报自己的名字, 供其它 client 路由访问。
    //   payload: { uint8 name_len; char name[name_len]; }
    REGISTER = 0x02,

    // 端口映射上报: client 告知 server "我暴露了哪些本地端口"。
    //   payload: { uint8 count; repeat(count) { uint16 local_port; uint16 remote_port; } }
    //   remote_port: server 上对外监听的公网端口
    PORT_MAP = 0x03,

    // 新建会话请求。方向不同语义不同:
    //   server -> client: "有外部用户连到了 remote_port, 你去连 local_port"
    //   payload: { uint32 session_id; uint16 local_port; }
    //   client -> server: "我要访问目标 client 的某端口, 请帮我中继"
    //   payload: { uint32 session_id; uint16 target_name_len; char target_name[];
    //              uint16 target_port; }
    NEW_CONN = 0x04,

    // 数据流: payload 原样为某 session 的用户数据。靠 session_id 关联两端。
    //   payload: { uint32 session_id; uint16 data_len; char data[data_len]; }
    DATA = 0x05,

    // 关闭会话: payload { uint32 session_id; }
    CLOSE = 0x06,

    // 通用应答 (成功/失败)。payload { uint8 code; }
    ACK = 0x07,

    // 链路探活请求: client A 想检测是否能到 client B。
    //   方向: client A -> server -> client B
    //   payload: { uint8 target_name_len; char target_name[]; uint32 probe_id; }
    PROBE = 0x08,

    // 链路探活应答: client B 回应 client A 的探活请求。
    //   方向: client B -> server -> client A
    //   payload: { uint32 probe_id; uint8 status; }  // status: 0=ok, 1=not_found
    PROBE_REPLY = 0x09,

    // P2P 打洞协商: server 通知双方尝试 P2P。payload: { uint32 session_id; }
    P2P_TRY = 0x0A,

    // P2P 端口上报: client 告知 server 自己的 P2P 端口。payload: { uint32 session_id; uint16 p2p_port; }
    P2P_PORT = 0x0B,

    // P2P 端口信息: server 通知 client 对端的 IP+端口。payload: { uint32 session_id; uint32 peer_ip; uint16 peer_port; }
    P2P_INFO = 0x0C,

    // P2P 成功: client 通知 server 直连已建立。payload: { uint32 session_id; }
    P2P_OK = 0x0D,

    // P2P 失败: client 通知 server 打洞失败, 回退中继。payload: { uint32 session_id; }
    P2P_FAIL = 0x0E,

    // Token 鉴权: client 连接后立即发送。payload: { uint8 token_len; char token[token_len]; }
    AUTH = 0x0F,
};

inline const char* msg_type_str(MessageType t) {
    switch (t) {
        case MessageType::HEARTBEAT: return "HEARTBEAT";
        case MessageType::REGISTER:  return "REGISTER";
        case MessageType::PORT_MAP:  return "PORT_MAP";
        case MessageType::NEW_CONN:  return "NEW_CONN";
        case MessageType::DATA:      return "DATA";
        case MessageType::CLOSE:     return "CLOSE";
        case MessageType::ACK:       return "ACK";
        case MessageType::PROBE:     return "PROBE";
        case MessageType::PROBE_REPLY: return "PROBE_REPLY";
        case MessageType::P2P_TRY:  return "P2P_TRY";
        case MessageType::P2P_PORT: return "P2P_PORT";
        case MessageType::P2P_INFO: return "P2P_INFO";
        case MessageType::P2P_OK:   return "P2P_OK";
        case MessageType::P2P_FAIL: return "P2P_FAIL";
        case MessageType::AUTH:     return "AUTH";
    }
    return "UNKNOWN";
}

// ---- 帧头解析 / 序列化 (扁平结构, 便于在缓冲区上直接读写) ----

#pragma pack(push, 1)
struct FrameHeader {
    uint16_t magic;
    uint8_t  type;
    uint32_t payload_len;
};
#pragma pack(pop)
static_assert(sizeof(FrameHeader) == kFrameHeaderLen, "frame header size mismatch");

// 把帧头写入 host 缓冲区(内部转网络字节序), 返回写入字节数(=kFrameHeaderLen)。
inline void encode_frame_header(MessageType type, uint32_t payload_len,
                                char* out /* at least kFrameHeaderLen */) {
    FrameHeader h;
    h.magic = htons(kFrameMagic);
    h.type = static_cast<uint8_t>(type);
    h.payload_len = htonl(payload_len);
    std::memcpy(out, &h, sizeof(h));
}

// 从 host 缓冲区解析帧头(网络字节序 -> 主机)。校验 magic。
// 成功返回 true, 并填充 type/payload_len; 失败返回 false。
inline bool decode_frame_header(const char* in, MessageType* type,
                                uint32_t* payload_len) {
    FrameHeader h;
    std::memcpy(&h, in, sizeof(h));
    if (ntohs(h.magic) != kFrameMagic) {
        return false;
    }
    if (ntohl(h.payload_len) > kMaxPayloadLen) {
        return false;
    }
    *type = static_cast<MessageType>(h.type);
    *payload_len = ntohl(h.payload_len);
    return true;
}

}  // namespace tunnel

#endif  // TUNNEL_COMMON_PROTOCOL_H_
