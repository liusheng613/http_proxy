#include "crypto.h"

#include <openssl/evp.h>
#include <openssl/rand.h>
#include <openssl/sha.h>

namespace tunnel {
namespace crypto {

static constexpr int kNonceLen = 12;   // AES-GCM 推荐 nonce 长度
static constexpr int kTagLen   = 16;   // AES-GCM tag 长度
static constexpr int kKeyLen   = 32;   // AES-256 key 长度

std::string derive_key(const std::string& secret) {
    std::string key(kKeyLen, '\0');
    SHA256(reinterpret_cast<const unsigned char*>(secret.data()),
           secret.size(),
           reinterpret_cast<unsigned char*>(&key[0]));
    return key;
}

std::string encrypt(const std::string& plaintext, const std::string& key) {
    if (key.size() != kKeyLen) return {};

    // 生成随机 nonce
    unsigned char nonce[kNonceLen];
    if (RAND_bytes(nonce, sizeof(nonce)) != 1) return {};

    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (!ctx) return {};
    EVP_EncryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, nullptr, nullptr);
    EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, kNonceLen, nullptr);
    EVP_EncryptInit_ex(ctx, nullptr, nullptr,
                       reinterpret_cast<const unsigned char*>(key.data()),
                       nonce);

    std::string ciphertext;
    ciphertext.resize(plaintext.size() + kTagLen);  // 预留 tag 空间
    int out_len = 0;
    EVP_EncryptUpdate(ctx, reinterpret_cast<unsigned char*>(&ciphertext[0]),
                      &out_len,
                      reinterpret_cast<const unsigned char*>(plaintext.data()),
                      static_cast<int>(plaintext.size()));
    int total = out_len;
    EVP_EncryptFinal_ex(ctx, reinterpret_cast<unsigned char*>(&ciphertext[total]),
                        &out_len);
    total += out_len;

    unsigned char tag[kTagLen];
    EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, kTagLen, tag);
    EVP_CIPHER_CTX_free(ctx);

    // 拼接: nonce(12) + ciphertext + tag(16)
    std::string result;
    result.append(reinterpret_cast<const char*>(nonce), kNonceLen);
    result.append(ciphertext.data(), total);
    result.append(reinterpret_cast<const char*>(tag), kTagLen);
    return result;
}

std::string decrypt(const std::string& encrypted, const std::string& key) {
    if (key.size() != kKeyLen) return {};
    if (encrypted.size() < static_cast<size_t>(kNonceLen + kTagLen)) return {};

    const unsigned char* nonce =
        reinterpret_cast<const unsigned char*>(encrypted.data());
    const unsigned char* tag =
        nonce + encrypted.size() - kTagLen;
    const unsigned char* ciphertext = nonce + kNonceLen;
    int ciphertext_len = static_cast<int>(
        encrypted.size() - kNonceLen - kTagLen);

    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (!ctx) return {};
    EVP_DecryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, nullptr, nullptr);
    EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, kNonceLen, nullptr);
    EVP_DecryptInit_ex(ctx, nullptr, nullptr,
                       reinterpret_cast<const unsigned char*>(key.data()),
                       nonce);

    std::string plaintext;
    plaintext.resize(ciphertext_len);
    int out_len = 0;
    EVP_DecryptUpdate(ctx, reinterpret_cast<unsigned char*>(&plaintext[0]),
                      &out_len, ciphertext, ciphertext_len);
    int total = out_len;

    EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, kTagLen,
                        const_cast<unsigned char*>(tag));
    int ret = EVP_DecryptFinal_ex(ctx,
        reinterpret_cast<unsigned char*>(&plaintext[total]), &out_len);
    EVP_CIPHER_CTX_free(ctx);

    if (ret <= 0) return {};  // 解密/认证失败
    total += out_len;
    plaintext.resize(total);
    return plaintext;
}

}  // namespace crypto
}  // namespace tunnel
