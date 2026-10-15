#pragma once
#include "Crypto/Engine/ICrypto.hpp"
#include <vector>
#include <string>

namespace cleat {

    // libsodium backend: XChaCha20-Poly1305 (AEAD)
    // Frame encryption rule: each ciphertext MUST use an independent random
    // nonce (24B). Reusing a nonce with the same key is catastrophic.
    // Wire format: nonce(24B) || ciphertext(plaintext + 16B tag),
    // interoperable with the Go server side.
    class SodiumBackend : public ICrypto {
    public:
        SodiumBackend();
        ~SodiumBackend() override;

        // key: 64-char hex (32 bytes) or raw 32+ bytes (first 32 used)
        // iv: ignored (per-frame random nonce, no fixed IV)
        bool Init(const std::string& key, const std::string& iv = "") override;
        bool Encrypt(const std::vector<uint8_t>& plaintext, std::vector<uint8_t>& ciphertext) override;
        bool Decrypt(const std::vector<uint8_t>& ciphertext, std::vector<uint8_t>& plaintext) override;
        std::string GetAlgorithmName() const override { return "XChaCha20-Poly1305"; }
        size_t GetKeyLength() const override { return key_len_; }
        size_t GetIVLength() const override { return iv_len_; }

    private:
        static bool HexDecode(const std::string& hex, std::vector<uint8_t>& out);
        // Phase C: 栈上临时还原会话密钥(masked_key_ XOR mask_); 用后 sodium_memzero
        bool UnmaskKeyToStack(uint8_t out32[32]) const;

        // Phase C(敏感数据保护): 密钥不以明文驻留 —— random 掩码 XOR 后驻留,
        // 每次加解密现场还原到栈, 用后擦除。访问已由 CryptoManager::crypto_mutex_ 串行化。
        std::vector<uint8_t> masked_key_;
        std::vector<uint8_t> mask_;
        size_t key_len_ = 32;   // XChaCha20 key: 32 bytes
        size_t iv_len_ = 24;    // XChaCha20 nonce: 24 bytes
        size_t tag_len_ = 16;   // Poly1305 tag: 16 bytes
        bool initialized_ = false;
    };

} // namespace cleat
