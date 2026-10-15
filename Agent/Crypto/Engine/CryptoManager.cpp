#include "Crypto/Engine/CryptoManager.hpp"
#include "Crypto/Backend/SodiumBackend.hpp"
#include "Common/Logger/ErrLog.hpp"
#include "Common/Logger/Logger.hpp"
#include <sodium.h>
#include <cstring>

namespace cleat {

    // X25519 握手帧明文 magic: "KE"。业务帧以 JSON '{' 开头, 不会碰撞。
    static constexpr uint8_t kHandshakeMagic[2] = { 'K', 'E' };
    static constexpr size_t kHandshakeSize = 42;   // "KE"(2) + key_id(8, 大端) + eph_pub(32)

    // 编译期密钥优先级: CLEAT_DEFAULT_PSK(生成器烧参数) > 旧 CLEAT_PSK_HEX(兼容);
    // X25519 公钥(CLEAT_DEFAULT_SERVER_PUBKEY)优先于 PSK, 见构造。
#ifdef CLEAT_DEFAULT_PSK
    static const char* kCompileTimePsk = CLEAT_DEFAULT_PSK;
#elif defined(CLEAT_PSK_HEX)
    static const char* kCompileTimePsk = CLEAT_PSK_HEX;
#else
    static const char* kCompileTimePsk = nullptr;
#endif

    CryptoManager& CryptoManager::Instance() {
        static CryptoManager instance;
        return instance;
    }

    // 析构清理会话密钥
    CryptoManager::~CryptoManager() {
        eph_pub_.clear();
        // eph_sk_ 已在派生完成后立即擦除(Phase C, 不再驻留)
        // backend_(SodiumBackend) 析构内 sodium_memzero 掩码态密钥
        backend_.reset();
    }

    CryptoManager::CryptoManager() {
#ifdef CLEAT_ENCRYPTION
        try {
#ifdef CLEAT_DEFAULT_SERVER_PUBKEY
            // 编译期烧入 Server 公钥(Artifact 生成器 per-binary): 自动启用 X25519 模式,
            // 每二进制独立密钥烧入二进制, 产物零参数运行、命令行干净
            if (!SetServerPublicKey(CLEAT_DEFAULT_SERVER_PUBKEY)) {
                CLEAT_LOGE(ErrDom::Cry, ErrCode::InvalidParam, "CryptoManager: 编译期 Server 公钥无效, 加密不可用");
            }
#else
            if (kCompileTimePsk) {
                backend_ = std::make_unique<SodiumBackend>();
                if (backend_->Init(kCompileTimePsk)) {
                    available_ = true;
                    LOG_INFO("CryptoManager: transport encryption enabled ("
                        << backend_->GetAlgorithmName() << ", compile-time PSK)");
                }
                else {
                    CLEAT_LOGE(ErrDom::Cry, ErrCode::InvalidParam, "CryptoManager: invalid compile-time PSK, encryption unavailable");
                }
            }
            else {
                // 无编译期密钥: 等待运行时 SetPsk(--psk)/SetServerPublicKey(--server-pubkey) 注入
                LOG_INFO("CryptoManager: 等待运行时密钥注入 (--psk / --server-pubkey)");
            }
#endif
        }
        catch (const std::exception& e) {
            CLEAT_LOGE(ErrDom::Cry, ErrCode::CryptoOpFailed, "CryptoManager: init failed: " << e.what());
        }
#else
        LOG_WARN("CryptoManager: encryption not compiled (CLEAT_ENCRYPTION=OFF)");
#endif
    }

    bool CryptoManager::SetPsk(const std::string& hex_psk) {
#ifdef CLEAT_ENCRYPTION
        // 当前仅启动期调用, 但运行时换钥(热更新 PSK)已具备安全并发前提。
        std::lock_guard<std::mutex> lock(crypto_mutex_);
        try {
            auto backend = std::make_unique<SodiumBackend>();
            if (backend->Init(hex_psk)) {
                backend_ = std::move(backend);
                available_ = true;
                LOG_INFO("CryptoManager: PSK 运行时注入成功 ("
                    << backend_->GetAlgorithmName() << ")");
                return true;
            }
            CLEAT_LOGE(ErrDom::Cry, ErrCode::InvalidParam, "CryptoManager: 无效 PSK(须 64 位 hex), 加密不可用");
            available_ = false;
        }
        catch (const std::exception& e) {
            CLEAT_LOGE(ErrDom::Cry, ErrCode::CryptoOpFailed, "CryptoManager: PSK init failed: " << e.what());
            available_ = false;
        }
        return false;
#else
        (void)hex_psk;
        LOG_WARN("CryptoManager: 加密未编译(CLEAT_ENCRYPTION=OFF), 忽略 PSK");
        return false;
#endif
    }

    bool CryptoManager::IsEnabled() const {
        // 已加锁, 读路径不加则"运行时换钥安全"不成立(bool 撕裂在 x64 上虽难观测,
        // 但编译器可把该读重排/缓存, 语义上仍是数据竞争)。
        std::lock_guard<std::mutex> lock(crypto_mutex_);
        return available_;
    }

    bool CryptoManager::SetServerPublicKey(const std::string& pub_hex) {
#ifdef CLEAT_ENCRYPTION
        std::lock_guard<std::mutex> lock(crypto_mutex_);
        try {
            if (pub_hex.size() != 64) {
                CLEAT_LOGE(ErrDom::Cry, ErrCode::InvalidParam, "CryptoManager: Server 公钥须 64 位 hex, 实际 " << pub_hex.size());
                return false;
            }
            // Server 长期公钥解码(32B)
            std::vector<uint8_t> server_pub(32);
            auto nib = [](char c) -> int {
                if (c >= '0' && c <= '9') return c - '0';
                if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                return -1;
            };
            for (size_t i = 0; i < 32; ++i) {
                const int hi = nib(pub_hex[i * 2]);
                const int lo = nib(pub_hex[i * 2 + 1]);
                if (hi < 0 || lo < 0) {
                    CLEAT_LOGE(ErrDom::Cry, ErrCode::InvalidParam, "CryptoManager: Server 公钥含非法 hex 字符");
                    return false;
                }
                server_pub[i] = static_cast<uint8_t>((hi << 4) | lo);
            }

            // 进程级临时 X25519 密钥对
            std::vector<uint8_t> pk(32), sk(32);
            if (crypto_box_keypair(pk.data(), sk.data()) != 0) {
                CLEAT_LOGE(ErrDom::Cry, ErrCode::CryptoOpFailed, "CryptoManager: crypto_box_keypair 失败");
                return false;
            }
            // 会话密钥 = X25519(eph_sk, server_pub)
            std::vector<uint8_t> shared(32);
            if (crypto_scalarmult_curve25519(shared.data(), sk.data(), server_pub.data()) != 0) {
                CLEAT_LOGE(ErrDom::Cry, ErrCode::CryptoOpFailed, "CryptoManager: X25519 派生失败(公钥无效?)");
                return false;
            }
            // 小阶点防护: 全零共享密钥必须拒绝
            bool all_zero = true;
            for (const auto b : shared) {
                if (b != 0) { all_zero = false; break; }
            }
            if (all_zero) {
                CLEAT_LOGE(ErrDom::Cry, ErrCode::IntegrityMismatch, "CryptoManager: X25519 派生全零密钥(小阶点), 拒绝");
                return false;
            }

            auto backend = std::make_unique<SodiumBackend>();
            std::string ks(reinterpret_cast<const char*>(shared.data()), shared.size());
            if (!backend->Init(ks)) {
                sodium_memzero(ks.data(), ks.size());
                sodium_memzero(shared.data(), shared.size());
                sodium_memzero(sk.data(), sk.size());
                CLEAT_LOGE(ErrDom::Cry, ErrCode::CryptoOpFailed, "CryptoManager: 会话密钥初始化失败");
                return false;
            }
            // Phase C: 会话密钥/临时私钥用后即擦 —— backend 内部已转掩码态驻留
            sodium_memzero(ks.data(), ks.size());
            sodium_memzero(shared.data(), shared.size());
            sodium_memzero(sk.data(), sk.size());
            backend_ = std::move(backend);
            available_ = true;

            // 随机 key_id(8B, 大端; 0 保留给 PSK/未知): 64 位随机碰撞概率 2^-64 可忽略
            uint64_t id = 0;
            do {
                randombytes_buf(&id, sizeof(id));
            } while (id == 0);

            eph_pub_ = std::move(pk);
            key_id_ = id;
            x25519_mode_ = true;
            // 与 server 侧完整 key_id 对不上。uint64 直接流输出即完整值。
            LOG_INFO("CryptoManager: X25519 握手模式启用 (key_id=" << key_id_
                << ", " << backend_->GetAlgorithmName() << ")");
            return true;
        }
        catch (const std::exception& e) {
            CLEAT_LOGE(ErrDom::Cry, ErrCode::CryptoOpFailed, "CryptoManager: X25519 init failed: " << e.what());
            available_ = false;
        }
        return false;
#else
        (void)pub_hex;
        LOG_WARN("CryptoManager: 加密未编译(CLEAT_ENCRYPTION=OFF), 忽略 Server 公钥");
        return false;
#endif
    }

    std::vector<uint8_t> CryptoManager::HandshakeFrameLocked() const {
        std::vector<uint8_t> f;
        if (!x25519_mode_ || eph_pub_.size() != 32) return f;   // PSK 模式无握手
        f.reserve(kHandshakeSize);
        f.push_back(kHandshakeMagic[0]);
        f.push_back(kHandshakeMagic[1]);
        // key_id 8B 大端
        const uint64_t kid = key_id_;
        for (int i = 7; i >= 0; --i) {
            f.push_back(static_cast<uint8_t>((kid >> (i * 8)) & 0xFF));
        }
        f.insert(f.end(), eph_pub_.begin(), eph_pub_.end());
        return f;
    }

    std::vector<uint8_t> CryptoManager::BuildHandshakeFrame() const {
        // x25519_mode_/eph_pub_/key_id_, 不持锁则换钥期读到半更新状态。
        std::lock_guard<std::mutex> lock(crypto_mutex_);
        return HandshakeFrameLocked();
    }

    bool CryptoManager::IsX25519Mode() const {
        std::lock_guard<std::mutex> lock(crypto_mutex_);
        return x25519_mode_;
    }

    uint64_t CryptoManager::KeyId() const {
        // 由 SetServerPublicKey 在同一临界区内成组提交, 裸读会观测到中间态。
        // 读路径一并持锁(与 IsEnabled/IsX25519Mode 同款)。
        std::lock_guard<std::mutex> lock(crypto_mutex_);
        return key_id_;
    }

    bool CryptoManager::Encrypt(const std::vector<uint8_t>& plaintext, std::vector<uint8_t>& out) {
        //并发保护 —— 模块 v2 api->send(命令线程)与主循环心跳/响应并发调用
        std::lock_guard<std::mutex> lock(crypto_mutex_);
        if (!available_ || !backend_) return false;
        // ): 旁路条件从"42B 且 'KE' 开头"的内容启发式收紧为"逐字节等于**当前
        // 会话**的握手帧"(锁内重建比对) —— 业务载荷恰好形似握手帧不再被静默明文
        // 上线(需同时命中 64 位随机 key_id 与本机 eph_pub, 不可行)。
        if (plaintext.size() == kHandshakeSize
            && plaintext[0] == kHandshakeMagic[0] && plaintext[1] == kHandshakeMagic[1]
            && plaintext == HandshakeFrameLocked()) {
            out = plaintext;
            return true;
        }
        std::vector<uint8_t> enc;
        if (!backend_->Encrypt(plaintext, enc)) return false;
        if (x25519_mode_) {
            // 线上帧 = [key_id 8B 大端][nonce||ct]: Server 按 key_id 路由会话密钥
            out.clear();
            out.reserve(8 + enc.size());
            const uint64_t kid = key_id_;
            for (int i = 7; i >= 0; --i) {
                out.push_back(static_cast<uint8_t>((kid >> (i * 8)) & 0xFF));
            }
            out.insert(out.end(), enc.begin(), enc.end());
        }
        else {
            out = std::move(enc);
        }
        return true;
    }

    bool CryptoManager::Decrypt(const std::vector<uint8_t>& in, std::vector<uint8_t>& plaintext) {
        std::lock_guard<std::mutex> lock(crypto_mutex_);
        if (!available_ || !backend_) return false;
        if (x25519_mode_) {
            // 剥 8B key_id 并校验(Server 响应必须用同一会话密钥)
            if (in.size() < 8 + 24 + 16) return false;
            uint64_t got = 0;
            for (int i = 0; i < 8; ++i) {
                got = (got << 8) | in[i];
            }
            if (got != key_id_) {
                CLEAT_LOGE(ErrDom::Cry, ErrCode::IntegrityMismatch, "CryptoManager: key_id 不匹配 (got " << got
                    << ", expect " << key_id_ << "), 可能需重新握手");
                return false;
            }
            return backend_->Decrypt(std::vector<uint8_t>(in.begin() + 8, in.end()), plaintext);
        }
        return backend_->Decrypt(in, plaintext);
    }

    const std::string CryptoManager::GetAlgorithmName() const {
        // available_/backend_ 与运行期换钥(释放旧 backend_)并发会读到悬垂指针。
        std::lock_guard<std::mutex> lock(crypto_mutex_);
        if (available_ && backend_) {
            return backend_->GetAlgorithmName();
        }
        static const std::string kDisabled = "disabled";
        return kDisabled;
    }

} // namespace cleat
