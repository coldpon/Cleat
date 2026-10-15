/**
 * @file MachineIdentity.cpp
 * @brief 本机身份生成实现(极简方案,  定稿)
 */
#include "MachineIdentity.hpp"

#include "Common/Logger/Logger.hpp"
#include "Common/Obfuscation/StringObf.hpp"
#include <sodium.h>

#include <cstring>
#include <memory>
#include <vector>

namespace cleat {
namespace identity {

    namespace {

        // AD7(修复 ): 此处原有 constexpr char kSaltPlaceholder[] 死常量
        // 避免"改这里就能换盐"的误导。

        // SHA256 输出 32B, 取前 16B 的 hex = 32 字符
        constexpr size_t kIdBytes = 16;

        std::string Sha256Hex16(const std::string& input) {
            // 独立实现(不依赖 CryptoManager —— 身份层早于加密初始化执行)
            // 单遍 SHA256: 引入 mbedtls 过重, 此处用 libsodium(已链入, sodium_init 幂等)
            unsigned char out[32];
            crypto_hash_sha256(out, reinterpret_cast<const unsigned char*>(input.data()),
                input.size());
            static const char* kHex = "0123456789abcdef";
            std::string hex;
            hex.reserve(kIdBytes * 2);
            for (size_t i = 0; i < kIdBytes; ++i) {
                hex += kHex[(out[i] >> 4) & 0xf];
                hex += kHex[out[i] & 0xf];
            }
            return hex;
        }

        std::string GetSalt() {
            // OBF_STR 返回 const char*(运行时解码); 盐变更 = 全体 mid 变化(重注册),
            // 生成器烧录时同步替换此值即可。
            static const std::string salt = std::string(OBF_STR("cleat-mid-salt-v1"));
            return salt;
        }

    } // namespace

    MachineIdentity::MachineIdentity(IMachineIdOps& ops) : ops_(ops) {
        //  返回值核查(0=成功 1=已初始化 -1=失败)
        if (sodium_init() == -1) {   // 幂等
            LOG_ERROR("MachineIdentity: sodium_init 失败(-1), 哈希原语不可用");
        }
    }

    // 平台实现注册点(条件编译; Platform/ 下同名符号按平台二选一)
#if defined(_WIN32)
    extern std::vector<std::string> CollectSourcesWin();
    extern std::string HostnameFallbackWin();
#elif defined(__linux__)
    extern std::vector<std::string> CollectSourcesLinux();
    extern std::string HostnameFallbackLinux();
#endif

    std::unique_ptr<IMachineIdOps> CreateMachineIdOps() {
        struct Impl : IMachineIdOps {
            std::vector<std::string> CollectSources() override {
#if defined(_WIN32)
                return CollectSourcesWin();
#else
                return CollectSourcesLinux();
#endif
            }
            std::string HostnameFallback() override {
#if defined(_WIN32)
                return HostnameFallbackWin();
#else
                return HostnameFallbackLinux();
#endif
            }
        };
        return std::make_unique<Impl>();
    }

    std::string MachineIdentity::Compute() const {
        const auto sources = ops_.CollectSources();
        for (const auto& src : sources) {
            if (src.empty()) continue;
            const std::string id = Sha256Hex16(src + GetSalt());
            LOG_DEBUG("MachineIdentity: machine_id 生成(源优先级候选命中)");
            return id;
        }
        // 全源失败(概率极低): hostname 哈希 —— 接受可能不稳定(定稿裁定)
        const std::string host = ops_.HostnameFallback();
        LOG_WARN("MachineIdentity: 全部身份源不可用, 回退 hostname 哈希(可能不稳定)");
        return Sha256Hex16(host + GetSalt());
    }

} // namespace identity
} // namespace cleat
