#pragma once
#include <cstdint>
#include <vector>
#include <string>

namespace cleat {

    /**
     * @brief 加密接口（纯虚）
     *
     * 加密层对上层透明，支持多种后端（如 libsodium、OpenSSL）。
     * 当前实现使用 libsodium（ChaCha20-Poly1305）。
     */
    class ICrypto {
    public:
        virtual ~ICrypto() = default;

        /**
         * @brief 初始化加密器
         * @param key 对称密钥（Base64 或原始二进制，由实现决定）
         * @param iv 初始向量（可选，若不提供则由实现生成随机 IV）
         * @return 是否成功
         */
        virtual bool Init(const std::string& key, const std::string& iv = "") = 0;

        /**
         * @brief 加密数据
         * @param plaintext 明文数据
         * @param ciphertext 输出密文（包含 IV + 认证标签）
         * @return 是否成功
         */
        virtual bool Encrypt(const std::vector<uint8_t>& plaintext, std::vector<uint8_t>& ciphertext) = 0;

        /**
         * @brief 解密数据
         * @param ciphertext 密文（包含 IV + 认证标签）
         * @param plaintext 输出明文
         * @return 是否成功
         */
        virtual bool Decrypt(const std::vector<uint8_t>& ciphertext, std::vector<uint8_t>& plaintext) = 0;

        /**
         * @brief 获取当前使用的算法名称（用于调试）
         */
        virtual std::string GetAlgorithmName() const = 0;

        /**
         * @brief 获取密钥长度（字节）
         */
        virtual size_t GetKeyLength() const = 0;

        /**
         * @brief 获取 IV 长度（字节）
         */
        virtual size_t GetIVLength() const = 0;
    };

}