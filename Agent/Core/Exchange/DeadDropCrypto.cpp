/**
 * @file DeadDropCrypto.cpp
 * @brief  死投递内容层实现(格式见头文件)
 * @details
 * 替代时间戳窗口(被控机时钟不可信), HMAC 是唯一防线, 失败原因不外泄。
 */
#include "DeadDropCrypto.hpp"

#include <sodium.h>
#include "Common/Logger/Logger.hpp"

#include <cstring>
#include <vector>

namespace cleat {
namespace deaddrop {

    namespace {

        constexpr char kLineHead[] = "DD1";      // 行头; 其后 tag 到首个 ':' 之前
        constexpr size_t  kMacBytes = 32;                       // HMAC-SHA256
        constexpr size_t  kNonceBytes = 24;                     // XChaCha20 nonce
        constexpr size_t  kB64FieldMax = 8192;                  // 单字段 b64 上限(防异常大行)
        /// seq 十进制位数上限(Unix 分钟数 10 位; 12 位余量已覆盖到 1970+7800 年)
        constexpr size_t  kSeqDigitsMax = 12;
        /// 单次解析接受的候选信封行数上限(平台内容是**公开可写**的, 防病态文本)
        constexpr size_t  kMaxCandidates = 64;

        std::vector<uint8_t> Sha256WithDomain(const std::string& key, uint8_t domain) {
            unsigned char out[32];
            std::string in = key;
            in.push_back(static_cast<char>(domain));
            crypto_hash_sha256(out, reinterpret_cast<const unsigned char*>(in.data()),
                in.size());
            //  派生用的密钥临时拷贝用后即擦(本函数每把子钥派生都复制一份 masterKey)
            sodium_memzero(in.data(), in.size());
            return std::vector<uint8_t>(out, out + 32);
        }

        std::string B64Encode(const unsigned char* p, size_t n) {
            const size_t need = sodium_base64_encoded_len(n,
                sodium_base64_VARIANT_URLSAFE_NO_PADDING);
            std::string out(need, 0);
            sodium_bin2base64(out.data(), need, p, n,
                sodium_base64_VARIANT_URLSAFE_NO_PADDING);
            out.resize(std::strlen(out.c_str()));
            return out;
        }

        bool B64Decode(const std::string& in, std::vector<unsigned char>& out) {
            if (in.empty() || in.size() > kB64FieldMax) return false;
            out.resize(in.size());   // 解码后 <= 输入长度
            size_t outLen = 0;
            if (sodium_base642bin(out.data(), out.size(), in.c_str(), in.size(),
                nullptr, &outLen, nullptr, sodium_base64_VARIANT_URLSAFE_NO_PADDING) != 0) {
                return false;
            }
            out.resize(outLen);
            return true;
        }

        /**
         * @brief 判定一行是否 DD1 信封并解析类型 tag
         * @return true = 是信封行, tagOut = 类型(空 = a 型历史形态, 或单个 'a'/'b'...)
         *         false = 不是信封行(含 "DD12:" 这类恰好前缀相似的普通文本)
         * @details tag 规则: "DD1" 到首个 ':' 之间为 tag; 空 = a 型(历史形态),
         *          恰一个字母/数字 = 该类型; 多字符 = 非信封(拒, 防前缀撞车)。
         */
        bool ParseDd1Head(const std::string& line, std::string& tagOut) {
            if (line.compare(0, 3, kLineHead) != 0) return false;
            const size_t colon = line.find(':');
            if (colon == std::string::npos) return false;
            tagOut = line.substr(3, colon - 3);
            if (tagOut.empty()) return true;              // "DD1:" = a 型历史形态
            if (tagOut.size() != 1) return false;
            const char t = tagOut[0];
            return (t >= 'a' && t <= 'z') || (t >= '0' && t <= '9');
        }

        /// 在平台文本中定位指定类型的**全部**候选信封行(行边界: \n)。
        /// 实测  原"命中即返回"让蓝队可用一行垃圾伪信封顶掉真信封
        /// (伪装散文里恰好出现 4 冒号 DD1 行同样致命) —— 改为收集全部候选, 由调用方
        /// 逐条跑完整校验管线取第一条通过者(HMAC 校验廉价且失败不外泄, 无探测面)。
        /// wantTag = 'a' 时同时接受历史形态("DD1:", 无 tag)与显式 "DD1a:"。
        std::vector<std::string> FindDd1Lines(const std::string& text, char wantTag) {
            std::vector<std::string> out;
            size_t pos = 0;
            while (out.size() < kMaxCandidates) {
                const size_t at = text.find(kLineHead, pos);
                if (at == std::string::npos) break;
                const size_t eol = text.find('\n', at);
                const size_t end = (eol == std::string::npos) ? text.size() : eol;
                std::string line = text.substr(at, end - at);
                while (!line.empty() && (line.back() == '\r' || line.back() == ' '))
                    line.pop_back();   // 平台可能以 CRLF 存文本
                std::string tag;
                int colons = 0;
                for (const char c : line) if (c == ':') ++colons;
                if (colons == 4 && ParseDd1Head(line, tag)) {
                    const char got = tag.empty() ? 'a' : tag[0];
                    if (got == wantTag) out.push_back(std::move(line));
                }
                pos = at + 3;
            }
            return out;
        }

        /// 切分信封行: 返回 [seqStr, nonceB64, ctB64, macB64]; 失败返回空 vector。
        /// p 起点跳过 "DD1" + tag + ':'(冒号计数已保证整行恰 4 个),
        /// 切 3 次取 seq/nonce/ct, **余下整段 = mac**(b64 无冒号, 可整段取)
        std::vector<std::string> SplitDd1(const std::string& line) {
            std::vector<std::string> f;
            const size_t head = line.find(':');
            if (head == std::string::npos) return {};
            size_t p = head + 1;
            for (int i = 0; i < 3; ++i) {
                const size_t c = line.find(':', p);
                if (c == std::string::npos) return {};
                f.push_back(line.substr(p, c - p));
                p = c + 1;
            }
            if (line.find(':', p) != std::string::npos) return {};   // mac 段不得含冒号
            f.push_back(line.substr(p));
            return f;
        }

    } // namespace

    DeadDropCrypto::DeadDropCrypto(const std::string& masterKey) {
        //  sodium_init 返回值核查(-1 = 自检失败)
        if (sodium_init() == -1) {   // 幂等; SecretVault 亦会调用
            LOG_ERROR("DeadDropCrypto: sodium_init 失败, 密码学原语不可用");
        }
        subKey_ = Sha256WithDomain(masterKey, 0x01);
        macKey_ = Sha256WithDomain(masterKey, 0x02);
    }

    DeadDropCrypto::~DeadDropCrypto() {
        WipeKeys();
    }

    void DeadDropCrypto::WipeKeys() {
        //  子钥明文驻留终结 —— 析构(含 Manager 每轮 SetMasterKey 的 emplace
        // 重建)统一擦除, 不再把 32B 密钥留在 freed heap 里等转储。
        if (!subKey_.empty()) sodium_memzero(subKey_.data(), subKey_.size());
        if (!macKey_.empty()) sodium_memzero(macKey_.data(), macKey_.size());
        subKey_.clear();
        macKey_.clear();
    }

    bool DeadDropCrypto::OpenEnvelope(const std::string& line, bool wantB,
        uint64_t lastAcceptedSeq, std::string& plainOut, uint64_t& seqOut) const {
        // ---- ① 切分 + seq 形态校验 ----
        const std::vector<std::string> f = SplitDd1(line);
        if (f.size() != 4) return false;
        if (f[0].empty() || f[0].size() > kSeqDigitsMax
            || f[0].find_first_not_of("0123456789") != std::string::npos) {
            return false;   // seq 必须纯数字且有长度上限
        }
        const unsigned long long seq = std::strtoull(f[0].c_str(), nullptr, 10);

        // ---- ② HMAC 验签(恒定时间比较) —— 主防线, 先于任何解码 ----
        // mac 覆盖 = 去掉 mac 字段后的完整前缀(含类型 tag 与尾冒号), 防字段
        // 拼接/截断/重排, 也防 a/b 互改。
        const std::string macPrefix = line.substr(0, line.size() - f[3].size());
        std::vector<unsigned char> macGiven;
        if (!B64Decode(f[3], macGiven) || macGiven.size() != kMacBytes) return false;
        unsigned char macRef[kMacBytes];
        crypto_auth_hmacsha256(macRef, reinterpret_cast<const unsigned char*>(macPrefix.data()),
            macPrefix.size(), reinterpret_cast<const unsigned char*>(macKey_.data()));
        if (sodium_memcmp(macRef, macGiven.data(), kMacBytes) != 0) {
            LOG_DEBUG("DeadDropCrypto: HMAC 校验失败(伪造/密钥不符/文本被改)");
            return false;
        }

        // ---- ③ 序列号单调(不依赖本地时钟) ----
        if (seq <= lastAcceptedSeq) {
            LOG_DEBUG("DeadDropCrypto: 丢弃重放/旧内容 (seq=" << seq << " <= last=" << lastAcceptedSeq << ")");
            return false;
        }

        // ---- ④ 解出 nonce/ct 并 AEAD open ----
        std::vector<unsigned char> nonce, ct;
        if (!B64Decode(f[1], nonce) || !B64Decode(f[2], ct)) return false;
        if (nonce.size() != kNonceBytes || ct.size() < 1 + 16) return false;

        std::vector<unsigned char> plain(ct.size() - 16);
        unsigned long long plainLen = 0;
        if (crypto_aead_xchacha20poly1305_ietf_decrypt(
            plain.data(), &plainLen, nullptr,
            ct.data(), ct.size(), nullptr, 0,
            nonce.data(), reinterpret_cast<const unsigned char*>(subKey_.data())) != 0) {
            LOG_DEBUG("DeadDropCrypto: AEAD 解密失败(密钥不符/ct 被改)");
            return false;
        }

        plainOut.assign(reinterpret_cast<char*>(plain.data()), plainLen);
        //  明文属于敏感产物(备胎地址), 临时缓冲随 vector 析构 —— 擦后再出栈
        sodium_memzero(plain.data(), plain.size());
        seqOut = seq;
        return true;
    }

    bool DeadDropCrypto::VerifyAndExtract(const std::string& platformText,
        uint64_t lastAcceptedSeq, ExtractResult& out) const {
        //  遍历全部 a 型候选行, 逐条跑完整管线取第一条通过者(见 FindDd1Lines 注释)
        for (const auto& line : FindDd1Lines(platformText, 'a')) {
            if (OpenEnvelope(line, false, lastAcceptedSeq, out.suffix, out.seq)) {
                return true;
            }
        }
        return false;
    }

    bool DeadDropCrypto::VerifyPayload(const std::string& platformText,
        uint64_t lastAcceptedSeq, PayloadResult& out) const {
        // b 型与 a 型同一条管线(macPrefix 覆盖 "DD1b:" 前缀, 类型即域分隔);
        //  同样遍历全部候选行
        for (const auto& line : FindDd1Lines(platformText, 'b')) {
            if (OpenEnvelope(line, true, lastAcceptedSeq, out.payload, out.seq)) {
                return true;
            }
        }
        return false;
    }

    std::string DeadDropCrypto::BuildEnvelope(const std::string& masterKey,
        const std::string& head, const std::string& plaintext,
        const std::string& disguiseText, uint64_t seq) {
        //  sodium_init 返回值核查(0=成功 1=已初始化 -1=失败)
        if (sodium_init() == -1) {
            LOG_ERROR("DeadDropCrypto: sodium_init 失败(-1), 发布侧行为不可保证");
        }
        DeadDropCrypto tmp(masterKey);

        unsigned char nonce[kNonceBytes];
        randombytes_buf(nonce, sizeof(nonce));
        std::vector<unsigned char> ct(plaintext.size() + 16);
        unsigned long long ctLen = 0;
        crypto_aead_xchacha20poly1305_ietf_encrypt(
            ct.data(), &ctLen, reinterpret_cast<const unsigned char*>(plaintext.data()),
            plaintext.size(), nullptr, 0, nullptr, nonce,
            reinterpret_cast<const unsigned char*>(tmp.subKey_.data()));
        ct.resize(ctLen);

        const std::string nonceB64 = B64Encode(nonce, sizeof(nonce));
        const std::string ctB64 = B64Encode(ct.data(), ct.size());
        const std::string body = head + std::to_string(seq) +
            ":" + nonceB64 + ":" + ctB64 + ":";

        unsigned char mac[kMacBytes];
        crypto_auth_hmacsha256(mac, reinterpret_cast<const unsigned char*>(body.data()),
            body.size(), reinterpret_cast<const unsigned char*>(tmp.macKey_.data()));
        const std::string macB64 = B64Encode(mac, kMacBytes);

        std::string out = disguiseText;
        if (!out.empty() && out.back() != '\n') out += '\n';
        out += body + macB64 + "\n";
        return out;
    }

    std::string DeadDropCrypto::Build(const std::string& masterKey,
        const std::string& disguiseText, const std::string& suffix, uint64_t seq) {
        // 历史形态 "DD1:"(无显式 tag) —— 与 publish.py/既有测试向量锁步, 不改
        return BuildEnvelope(masterKey, "DD1:", suffix, disguiseText, seq);
    }

    std::string DeadDropCrypto::BuildPayload(const std::string& masterKey,
        const std::string& disguiseText, const std::string& payload, uint64_t seq) {
        return BuildEnvelope(masterKey, "DD1b:", payload, disguiseText, seq);
    }

} // namespace deaddrop
} // namespace cleat
