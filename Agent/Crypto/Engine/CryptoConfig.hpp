#pragma once
#include <string>

namespace cleat {

    enum class CryptoMode {
        PSK,
        ECDH
    };

    enum class CryptoAlgorithm {
        ChaCha20Poly1305,
        AES256GCM
    };

    struct CryptoConfig {
        CryptoAlgorithm algorithm = CryptoAlgorithm::ChaCha20Poly1305;
        CryptoMode mode = CryptoMode::PSK;
        std::string psk_key;
        std::string psk_iv;
    };

} // namespace cleat