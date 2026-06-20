#ifndef TUNNEL_COMMON_FRAME_H_
#define TUNNEL_COMMON_FRAME_H_

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "protocol.h"

namespace tunnel {

// 完整解码出的一帧 (供业务层使用)
struct Frame {
    MessageType type;
    std::string payload;  // 可为空
};

// =============================================================================
// FrameDecoder: 处理 TCP 粘包/拆包的流式状态机。
// 每条连接绑定一个实例, Feed() 喂入新读到的字节, Output() 取出完整帧。
//
// 状态:
//   READ_HEADER : 累积字节直到凑齐 kFrameHeaderLen, 解析帧头
//   READ_PAYLOAD: 累积字节直到凑齐 payload_len
// =============================================================================

class FrameDecoder {
public:
    FrameDecoder() = default;

    // 喂入刚读到的字节 [data, data+len)。返回 false 表示协议错误(magic 非法/
    // payload 过大), 调用方应关闭连接。
    bool Feed(const char* data, size_t len);

    // 取出所有已凑齐的完整帧, 取出后内部清空。空 vector 表示尚无完整帧。
    std::vector<Frame> Output() {
        std::vector<Frame> out;
        out.swap(completed_);
        return out;
    }

private:
    void ConsumeHeader();
    void ConsumePayload();

    enum class State { READ_HEADER, READ_PAYLOAD };
    State state_ = State::READ_HEADER;

    std::string buf_;          // 累积缓冲 (含未凑齐的头部或 payload)
    MessageType cur_type_;     // 当前帧的 type (头部解析后填充)
    uint32_t    cur_payload_len_ = 0;  // 当前帧 payload 长度

    std::vector<Frame> completed_;  // 已凑齐的完整帧
};

// =============================================================================
// FrameBuilder: 把消息序列化成可发送的字节流。
//   Append* 系列追加 payload 的各字段 (网络字节序)。
//   Build() 返回完整帧字节 (帧头 + payload), 并重置内部状态。
//
// 用法:
//   std::string bytes = FrameBuilder(MessageType::HEARTBEAT).Build();
//   std::string bytes = FrameBuilder(MessageType::REGISTER)
//                          .AppendU16(name.size()).AppendStr(name).Build();
// =============================================================================

class FrameBuilder {
public:
    explicit FrameBuilder(MessageType type) : type_(type) {}

    FrameBuilder& AppendU8(uint8_t v);
    FrameBuilder& AppendU16(uint16_t v);
    FrameBuilder& AppendU32(uint32_t v);
    FrameBuilder& AppendStr(const std::string& s);
    FrameBuilder& AppendBytes(const char* data, size_t len);

    // 返回完整帧 (帧头 + payload)。调用后 Builder 可复用构造下一帧。
    std::string Build() const;

private:
    MessageType type_;
    std::string payload_;
};

}  // namespace tunnel

#endif  // TUNNEL_COMMON_FRAME_H_
