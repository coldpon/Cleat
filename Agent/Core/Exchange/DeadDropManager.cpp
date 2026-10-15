/**
 * @file DeadDropManager.cpp
 * @brief  死投递管理器实现
 */
#include "DeadDropManager.hpp"

#include "Common/Logger/Logger.hpp"

namespace cleat {
namespace deaddrop {

    void DeadDropManager::Register(std::unique_ptr<IDeadDrop> src) {
        if (src) sources_.push_back({ std::move(src) });
    }

    void DeadDropManager::SetMasterKey(const std::string& masterKey) {
        //  空串派生出的子钥(SHA256(0x01)/SHA256(0x02))是
        // 公开可推导常量, 验签形同虚设 —— fail-closed 拒绝注入, 维持"未注入"的
        // 强度是运营选择, HMAC 门本身仍然成立。
        if (masterKey.empty()) {
            LOG_WARN("DeadDropManager: 拒绝注入空 masterKey(常量子钥 = 验签失效)");
            return;
        }
        if (masterKey.size() < 32) {
            LOG_WARN("DeadDropManager: masterKey 偏短(" << masterKey.size()
                << "B < 32B), 联调缺省可接受但交付构建应为烧录随机钥");
        }
        crypto_.emplace(masterKey);
    }

    void DeadDropManager::SetAddressPrefix(const std::string& prefix) {
        prefix_ = prefix;
    }

    bool DeadDropManager::TryRecover(std::string& newUrl, uint64_t* outSeq) {
        if (!crypto_) {
            LOG_WARN("DeadDropManager: masterKey 未注入, 拒绝恢复(防明文默认密钥)");
            return false;
        }
        for (const auto& e : sources_) {
            const auto raw = e.src->FetchRaw();
            if (!raw) {
                LOG_DEBUG("DeadDropManager: [" << e.src->PlatformName() << "] 不可达, 尝试下一平台");
                continue;
            }
            ExtractResult out;
            // seq 传 lastSeq_(严格大于) —— 失败(验签/重放)统一 false, 继续下一平台
            if (!crypto_->VerifyAndExtract(*raw, lastSeq_, out)) {
                LOG_DEBUG("DeadDropManager: [" << e.src->PlatformName()
                    << "] 内容校验失败, 尝试下一平台");
                continue;
            }
            newUrl = prefix_ + out.suffix;
            if (outSeq) {
                //  seq 推进权交还调用方 —— 换址应用(密封
                // 通道链)可能失败, 此处先推进会把唯一有效信封烧掉(同内容下次被
                // seq 单调门拒绝, 恢复通道从此失灵)。调用方应用成功后再 CommitSeq。
                *outSeq = out.seq;
            }
            else {
                lastSeq_ = out.seq;   // 旧调用形态: 无外部应用步骤, 立即生效
            }
            LOG_INFO("DeadDropManager: [" << e.src->PlatformName() << "] 恢复成功 seq="
                << out.seq << " -> " << newUrl);
            return true;
        }
        LOG_DEBUG("DeadDropManager: 全部平台尝试完毕, 无有效内容");
        return false;
    }

    void DeadDropManager::CommitSeq(uint64_t seq) {
        if (seq > lastSeq_) lastSeq_ = seq;
    }

    bool DeadDropManager::TryFetchPayload(std::string& payload) {
        if (!crypto_) {
            LOG_WARN("DeadDropManager: masterKey 未注入, 拒绝取载荷(防明文默认密钥)");
            return false;
        }
        for (const auto& e : sources_) {
            const auto raw = e.src->FetchRaw();
            if (!raw) continue;
            PayloadResult out;
            if (!crypto_->VerifyPayload(*raw, lastSeq_, out)) continue;
            lastSeq_ = out.seq;
            payload = std::move(out.payload);
            LOG_INFO("DeadDropManager: [" << e.src->PlatformName() << "] 载荷取件成功 seq="
                << out.seq << " (" << payload.size() << "B)");
            return true;
        }
        LOG_DEBUG("DeadDropManager: 全部平台无有效 b 型载荷");
        return false;
    }

} // namespace deaddrop
} // namespace cleat
