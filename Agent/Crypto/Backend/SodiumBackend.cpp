#include "SodiumBackend.hpp"
#include <sodium.h>
#include <stdexcept>

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#endif

namespace cleat {

    namespace {
        // 防换页(与 SecretVault 同款): 密钥掩码驻留内存不可换出到磁盘 ——
        // 未 LockMem, 密钥可被换页落盘(swap file 取证)。
        void LockMem(void* p, size_t n) {
#ifdef _WIN32
            if (p && n) ::VirtualLock(p, n);
#else
            if (p && n) ::mlock(p, n);
#endif
        }
    } // namespace

    SodiumBackend::SodiumBackend() {
        if (sodium_init() == -1) {
            throw std::runtime_error("SodiumBackend: sodium_init failed");
        }
    }

    // 析构时清理对称密钥: 不留明文/掩码态密钥在堆内存(取证/内存转储防护)
    SodiumBackend::~SodiumBackend() {
        if (!masked_key_.empty()) {
            sodium_memzero(masked_key_.data(), masked_key_.size());
            masked_key_.clear();
        }
        if (!mask_.empty()) {
            sodium_memzero(mask_.data(), mask_.size());
            mask_.clear();
        }
        initialized_ = false;
    }

    bool SodiumBackend::HexDecode(const std::string& hex, std::vector<uint8_t>& out) {
        if (hex.size() % 2 != 0) return false;
        out.resize(hex.size() / 2);
        auto nib = [](char c) -> int {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            return -1;
        };
        for (size_t i = 0; i < out.size(); ++i) {
            int hi = nib(hex[i * 2]);
            int lo = nib(hex[i * 2 + 1]);
            if (hi < 0 || lo < 0) return false;
            out[i] = static_cast<uint8_t>((hi << 4) | lo);
        }
        return true;
    }

	bool SodiumBackend::Init(const std::string& key, const std::string& /*iv*/) {
		if (key.empty()) return false;

		std::vector<uint8_t> decoded;
		// Case 1: 64-char hex -> decode to 32 bytes
		if (key.size() == key_len_ * 2 && HexDecode(key, decoded)) {
			// decoded 已含明文密钥
		}
		// Case 2: raw 32-byte binary key
		else if (key.size() == key_len_) {
			decoded.assign(key.begin(), key.end());
		}
		else {
			// Invalid format: length mismatch or hex decode failure
			//  Case 1 解码失败时 decoded 可能已含部分密钥
			// 字节 —— 失败路径同样擦除, 不把明文密钥留在 freed heap。
			if (!decoded.empty()) {
				sodium_memzero(decoded.data(), decoded.size());
			}
			return false;
		}

		// Phase C: 明文不驻留 —— 随机掩码 XOR 后驻留, 每次加解密现场还原到栈。
		// 重 Init 时先擦旧掩码态。
		if (!masked_key_.empty()) {
			sodium_memzero(masked_key_.data(), masked_key_.size());
			masked_key_.clear();
		}
		if (!mask_.empty()) {
			sodium_memzero(mask_.data(), mask_.size());
			mask_.clear();
		}
		mask_.resize(key_len_);
		randombytes_buf(mask_.data(), mask_.size());
		LockMem(mask_.data(), mask_.size());
		masked_key_.assign(key_len_, 0);
		LockMem(masked_key_.data(), masked_key_.size());
		for (size_t i = 0; i < key_len_; ++i) {
			masked_key_[i] = static_cast<uint8_t>(decoded[i] ^ mask_[i]);
		}
		sodium_memzero(decoded.data(), decoded.size());
		decoded.clear();
		decoded.shrink_to_fit();

		initialized_ = true;
		return true;
	}

	bool SodiumBackend::UnmaskKeyToStack(uint8_t out32[32]) const {
		if (!initialized_ || key_len_ != 32
			|| masked_key_.size() != 32 || mask_.size() != 32) {
			return false;
		}
		for (size_t i = 0; i < 32; ++i) {
			out32[i] = static_cast<uint8_t>(masked_key_[i] ^ mask_[i]);
		}
		return true;
	}

    bool SodiumBackend::Encrypt(const std::vector<uint8_t>& plaintext, std::vector<uint8_t>& ciphertext) {
        if (!initialized_) return false;

        constexpr size_t NS = crypto_aead_xchacha20poly1305_ietf_NPUBBYTES; // 24
        constexpr size_t ABYTES = crypto_aead_xchacha20poly1305_ietf_ABYTES; // 16

        // Phase C: 现场还原密钥到栈, 用后擦除(明文密钥生命周期 = 单次加解密)
        uint8_t key[32];
        if (!UnmaskKeyToStack(key)) return false;

        // Output: nonce(24) || ciphertext(plaintext + 16B tag)
        ciphertext.resize(NS + plaintext.size() + ABYTES);

        // Fresh random nonce per frame (never reuse)
        randombytes_buf(ciphertext.data(), NS);

        unsigned long long ct_len = 0;
        const int rc = crypto_aead_xchacha20poly1305_ietf_encrypt(
            ciphertext.data() + NS, &ct_len,
            plaintext.data(), plaintext.size(),
            nullptr, 0,        // no AD
            nullptr,           // no key derivation params
            ciphertext.data(), // nonce
            key);
        sodium_memzero(key, sizeof(key));
        if (rc != 0) {
            return false;
        }
        ciphertext.resize(NS + ct_len);
        return true;
    }

    bool SodiumBackend::Decrypt(const std::vector<uint8_t>& ciphertext, std::vector<uint8_t>& plaintext) {
        if (!initialized_) return false;

        constexpr size_t NS = crypto_aead_xchacha20poly1305_ietf_NPUBBYTES;
        constexpr size_t ABYTES = crypto_aead_xchacha20poly1305_ietf_ABYTES;

        if (ciphertext.size() < NS + ABYTES) return false;

        uint8_t key[32];
        if (!UnmaskKeyToStack(key)) return false;

        plaintext.resize(ciphertext.size() - NS);
        unsigned long long pt_len = 0;
        const int rc = crypto_aead_xchacha20poly1305_ietf_decrypt(
            plaintext.data(), &pt_len, nullptr,
            ciphertext.data() + NS, ciphertext.size() - NS,
            nullptr, 0,
            ciphertext.data(), // nonce
            key);
        sodium_memzero(key, sizeof(key));
        if (rc != 0) {
            // 但残留的 resize 后垃圾数据若被误用(未判返回值)会成为脏明文,
            // 统一 fail-closed 防误用。
            plaintext.clear();
            return false;
        }
        plaintext.resize(pt_len);
        return true;
    }

}
