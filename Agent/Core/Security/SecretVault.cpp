#include "SecretVault.hpp"

#include <sodium.h>
#include "Common/Logger/ErrLog.hpp"
#include "Common/Logger/Logger.hpp"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <sys/mman.h>
#endif

namespace cleat {

    namespace {
        constexpr size_t kKeyBytes = 32;

        // 防换页 best-effort: 失败仅降级(半片可能进 swap), 不影响功能
        void LockMem(void* p, size_t n) {
#ifdef _WIN32
            VirtualLock(p, n);
#else
            mlock(p, n);
#endif
        }
    } // namespace

    SecretVault& SecretVault::Instance() {
        static SecretVault inst;
        return inst;
    }

    SecretVault::~SecretVault() {
        Wipe(mk_a_);
        Wipe(mk_b_);
    }

    bool SecretVault::Init() {
        std::lock_guard<std::mutex> lock(mu_);
        if (init_) return true;
        if (sodium_init() == -1) return false;

        // 主密钥随机生成 → 拆两个随机半片驻留; 明文主密钥只存在于栈上瞬间
        std::vector<uint8_t> mk(kKeyBytes);
        randombytes_buf(mk.data(), kKeyBytes);

        mk_a_.resize(kKeyBytes);
        randombytes_buf(mk_a_.data(), kKeyBytes);
        mk_b_.assign(kKeyBytes, 0);
        for (size_t i = 0; i < kKeyBytes; ++i) {
            mk_b_[i] = static_cast<uint8_t>(mk[i] ^ mk_a_[i]);
        }
        LockMem(mk_a_.data(), mk_a_.size());
        LockMem(mk_b_.data(), mk_b_.size());

        Wipe(mk);
        init_ = true;
        return true;
    }

    bool SecretVault::UnsealKeyToStack(uint8_t key32[32]) const {
        if (!init_ || mk_a_.size() != kKeyBytes || mk_b_.size() != kKeyBytes) return false;
        for (size_t i = 0; i < kKeyBytes; ++i) {
            key32[i] = static_cast<uint8_t>(mk_a_[i] ^ mk_b_[i]);
        }
        return true;
    }

    std::vector<uint8_t> SecretVault::Seal(const uint8_t* p, size_t n) {
        std::vector<uint8_t> out;
        if ((!p && n != 0) || !init_) return out;

        uint8_t key[32];
        {
            std::lock_guard<std::mutex> lock(mu_);
            if (!UnsealKeyToStack(key)) return out;
        }
        constexpr size_t NS = crypto_aead_xchacha20poly1305_ietf_NPUBBYTES;   // 24
        constexpr size_t ABYTES = crypto_aead_xchacha20poly1305_ietf_ABYTES;  // 16

        out.resize(NS + n + ABYTES);
        randombytes_buf(out.data(), NS);
        unsigned long long ct_len = 0;
        const int rc = crypto_aead_xchacha20poly1305_ietf_encrypt(
            out.data() + NS, &ct_len,
            p, n,
            nullptr, 0,        // 无 AD
            nullptr,           // 非对称接口专用
            out.data(),        // nonce
            key);
        sodium_memzero(key, sizeof(key));
        if (rc != 0) {
            Wipe(out);
            CLEAT_LOGE(ErrDom::Cry, ErrCode::CryptoOpFailed, "SecretVault: Seal 失败");
            return {};
        }
        out.resize(NS + ct_len);
        return out;
    }

    bool SecretVault::Open(const std::vector<uint8_t>& sealed, std::vector<uint8_t>& out) const {
        out.clear();
        if (!init_) return false;

        constexpr size_t NS = crypto_aead_xchacha20poly1305_ietf_NPUBBYTES;
        constexpr size_t ABYTES = crypto_aead_xchacha20poly1305_ietf_ABYTES;
        if (sealed.size() < NS + ABYTES) return false;

        uint8_t key[32];
        {
            std::lock_guard<std::mutex> lock(mu_);
            if (!UnsealKeyToStack(key)) return false;
        }
        out.resize(sealed.size() - NS);
        unsigned long long pt_len = 0;
        const int rc = crypto_aead_xchacha20poly1305_ietf_decrypt(
            out.data(), &pt_len, nullptr,
            sealed.data() + NS, sealed.size() - NS,
            nullptr, 0,
            sealed.data(),     // nonce
            key);
        sodium_memzero(key, sizeof(key));
        if (rc != 0) {
            out.clear();
            CLEAT_LOGE(ErrDom::Cry, ErrCode::IntegrityMismatch, "SecretVault: Open 失败(损坏/被篡改?)");
            return false;
        }
        out.resize(pt_len);
        return true;
    }

    void SecretVault::Wipe(std::vector<uint8_t>& b) {
        if (!b.empty()) {
            sodium_memzero(b.data(), b.size());
        }
        b.clear();
        b.shrink_to_fit();
    }

} // namespace cleat
