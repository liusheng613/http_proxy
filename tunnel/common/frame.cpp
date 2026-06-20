#include "frame.h"

#include <arpa/inet.h>

namespace tunnel {

// --------------------------- FrameDecoder -----------------------------------

bool FrameDecoder::Feed(const char* data, size_t len) {
    buf_.append(data, len);

    for (;;) {
        if (state_ == State::READ_HEADER) {
            if (buf_.size() < static_cast<size_t>(kFrameHeaderLen)) {
                return true;  // 头部还没凑齐, 等下次
            }
            MessageType type{};
            uint32_t payload_len = 0;
            if (!decode_frame_header(buf_.data(), &type, &payload_len)) {
                // magic 非法或 payload 过大
                return false;
            }
            cur_type_ = type;
            cur_payload_len_ = payload_len;
            buf_.erase(0, kFrameHeaderLen);
            state_ = State::READ_PAYLOAD;
            // 继续往下走, 看 payload 是否也已凑齐
        }

        if (state_ == State::READ_PAYLOAD) {
            if (buf_.size() < cur_payload_len_) {
                return true;  // payload 还没凑齐
            }
            ConsumePayload();
            state_ = State::READ_HEADER;
            // 继续循环, 处理 buf_ 里可能已经包含的下一帧
        }
    }
}

void FrameDecoder::ConsumePayload() {
    Frame f;
    f.type = cur_type_;
    if (cur_payload_len_ > 0) {
        f.payload.assign(buf_.data(), cur_payload_len_);
        buf_.erase(0, cur_payload_len_);
    }
    completed_.push_back(std::move(f));
}

// --------------------------- FrameBuilder -----------------------------------

FrameBuilder& FrameBuilder::AppendU8(uint8_t v) {
    payload_.push_back(static_cast<char>(v));
    return *this;
}

FrameBuilder& FrameBuilder::AppendU16(uint16_t v) {
    uint16_t n = htons(v);
    payload_.append(reinterpret_cast<const char*>(&n), sizeof(n));
    return *this;
}

FrameBuilder& FrameBuilder::AppendU32(uint32_t v) {
    uint32_t n = htonl(v);
    payload_.append(reinterpret_cast<const char*>(&n), sizeof(n));
    return *this;
}

FrameBuilder& FrameBuilder::AppendStr(const std::string& s) {
    payload_.append(s);
    return *this;
}

FrameBuilder& FrameBuilder::AppendBytes(const char* data, size_t len) {
    payload_.append(data, len);
    return *this;
}

std::string FrameBuilder::Build() const {
    std::string out;
    out.resize(kFrameHeaderLen + payload_.size());
    encode_frame_header(type_, static_cast<uint32_t>(payload_.size()),
                        &out[0]);
    if (!payload_.empty()) {
        out.replace(kFrameHeaderLen, payload_.size(), payload_);
    }
    return out;
}

}  // namespace tunnel
