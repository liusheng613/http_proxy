#include "frame.h"

#ifndef _WIN32
#include <arpa/inet.h>
#endif

#include "crypto.h"

namespace tunnel {

// 全局加密密钥
static std::string g_crypto_key;

void tunnel_set_crypto_key(const std::string& key) { g_crypto_key = key; }
bool tunnel_has_crypto_key() { return !g_crypto_key.empty(); }

// --------------------------- FrameDecoder -----------------------------------

bool FrameDecoder::Feed(const char* data, size_t len) {
    buf_.append(data, len);

    for (;;) {
        if (state_ == State::READ_HEADER) {
            if (buf_.size() < static_cast<size_t>(kFrameHeaderLen)) {
                return true;
            }
            MessageType type{};
            uint32_t payload_len = 0;
            if (!decode_frame_header(buf_.data(), &type, &payload_len)) {
                return false;
            }
            cur_type_ = type;
            cur_payload_len_ = payload_len;
            buf_.erase(0, kFrameHeaderLen);
            state_ = State::READ_PAYLOAD;
        }

        if (state_ == State::READ_PAYLOAD) {
            if (buf_.size() < cur_payload_len_) {
                return true;
            }
            if (!ConsumePayload()) return false;
            state_ = State::READ_HEADER;
        }
    }
}

bool FrameDecoder::ConsumePayload() {
    Frame f;
    f.type = cur_type_;
    if (cur_payload_len_ > 0) {
        f.payload.assign(buf_.data(), cur_payload_len_);
        buf_.erase(0, cur_payload_len_);
        // 解密
        if (!g_crypto_key.empty()) {
            std::string decrypted = crypto::decrypt(f.payload, g_crypto_key);
            if (decrypted.empty()) return false;  // 解密失败
            f.payload = std::move(decrypted);
        }
    }
    completed_.push_back(std::move(f));
    return true;
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
    std::string final_payload = payload_;
    // 加密
    if (!g_crypto_key.empty() && !final_payload.empty()) {
        final_payload = crypto::encrypt(final_payload, g_crypto_key);
        if (final_payload.empty()) return {};  // 加密失败
    }
    std::string out;
    out.resize(kFrameHeaderLen + final_payload.size());
    encode_frame_header(type_, static_cast<uint32_t>(final_payload.size()),
                        &out[0]);
    if (!final_payload.empty()) {
        out.replace(kFrameHeaderLen, final_payload.size(), final_payload);
    }
    return out;
}

}  // namespace tunnel
