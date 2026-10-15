#pragma once
#include "Crypto/Engine/ICrypto.hpp"
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace cleat {

    class CryptoManager {
    public:
        static CryptoManager& Instance();

        
        bool IsEnabled() const;

      
        bool SetPsk(const std::string& hex_psk);

      
        bool SetServerPublicKey(const std::string& pub_hex);

    
        std::vector<uint8_t> BuildHandshakeFrame() const;

       
        bool IsX25519Mode() const;

       
        /// 当前会话 key_id(读路径持锁, 同 IsEnabled/IsX25519Mode)。
        /// key_id_ 同批写入的 x25519_mode_/available_/eph_pub_ 构成一组
        /// 会话状态, 无锁读可能观测到"新 key_id + 旧模式"的中间态,
        /// 据此拼出的帧与握手帧不一致。改为全程持锁。
        uint64_t KeyId() const;

      
        bool Encrypt(const std::vector<uint8_t>& plaintext, std::vector<uint8_t>& out);
      
        bool Decrypt(const std::vector<uint8_t>& in, std::vector<uint8_t>& plaintext);

        const std::string GetAlgorithmName() const;

    private:
        CryptoManager();
     
        ~CryptoManager();
        CryptoManager(const CryptoManager&) = delete;
        CryptoManager& operator=(const CryptoManager&) = delete;

        /// 组装当前会话握手帧(**调用方须已持 crypto_mutex_**) —— BuildHandshakeFrame
        /// 的锁内本体, Encrypt 的握手旁路比对同源复用
        std::vector<uint8_t> HandshakeFrameLocked() const;

        std::unique_ptr<ICrypto> backend_;
        bool available_ = false;
      
        mutable std::mutex crypto_mutex_;

      
        bool x25519_mode_ = false;
        uint64_t key_id_ = 0;
        std::vector<uint8_t> eph_pub_;   // 临时公钥(重连握手复用; 公开信息无需保护)
        // eph_sk_: Phase C 起不再驻留 —— 派生完成即擦除(此后唯一用途为零)
    };

}
