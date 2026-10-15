#include "Core/Comms/Transports/Smb/PipeFrame.hpp"

#include <mutex>

#include <sodium.h>

namespace cleat {
namespace PipeFrame {

    void EnsureSodium() {
        static std::once_flag flag;
        std::call_once(flag, [] { sodium_init(); });
    }

    void DeriveKey(const std::string& secret, uint8_t out[32]) {
        // 口令字节数不足/超长均不安全, 统一 SHA-256 定长派生
        static_assert(crypto_hash_sha256_BYTES == 32, "expect sha256 32B");
        EnsureSodium();
        crypto_hash_sha256(out,
            reinterpret_cast<const unsigned char*>(secret.data()), secret.size());
    }

    void AuthCode(const uint8_t key[32], const uint8_t* msg, size_t len,
        uint8_t out[32]) {
        static_assert(crypto_auth_BYTES == 32, "expect hmac 32B");
        EnsureSodium();
        crypto_auth(out, msg, len, key);
    }

} // namespace PipeFrame
} // namespace cleat
