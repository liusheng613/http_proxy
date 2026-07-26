#ifndef TUNNEL_COMMON_CRYPTO_H_
#define TUNNEL_COMMON_CRYPTO_H_

#include <cstdint>
#include <string>

namespace tunnel {
namespace crypto {

// 从共享密钥派生 AES-256 key (SHA256)。
std::string derive_key(const std::string& secret);

// AES-256-GCM 加密。
// plaintext: 明文
// key: 32 字节 AES key
// 返回: nonce(12字节) + ciphertext + tag(16字节), 失败返回空字符串。
std::string encrypt(const std::string& plaintext, const std::string& key);

// AES-256-GCM 解密。
// encrypted: nonce(12) + ciphertext + tag(16) 的拼接
// key: 32 字节 AES key
// 返回: 明文, 失败(认证失败/数据损坏)返回空字符串。
std::string decrypt(const std::string& encrypted, const std::string& key);

}  // namespace crypto
}  // namespace tunnel

#endif  // TUNNEL_COMMON_CRYPTO_H_
