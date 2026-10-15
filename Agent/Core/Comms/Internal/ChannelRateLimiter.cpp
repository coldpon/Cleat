/**
 * @file ChannelRateLimiter.cpp
 * @brief 低带宽通道报文级限流器实现
 */

#include "ChannelRateLimiter.hpp"

#include <chrono>

#include "Core/Config/AgentConfig.hpp"

namespace cleat {

    namespace {
        /// 限流桶表上限(条): 防"每出现一个新 cmd 常驻一个桶"的无界增长(实测:
        /// 懒清理只在该 cmd 再次 Check 时触发, Reset() 无生产调用者)。取值远大于任何
        /// 合理的命令种类数, 正常运营远达不到。
        constexpr size_t kMaxBuckets = 1024;
    }

    int64_t ChannelRateLimiter::NowMs() {
        using namespace std::chrono;
        return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
    }

    ChannelRateLimiter::Decision
        ChannelRateLimiter::Check(const std::string& cmd, size_t payload_bytes) {
        const ChannelLimitConfig cfg = AgentConfig::Instance().GetChannelLimit();
        if (!cfg.enabled) {
            return Decision::Allow;
        }

        std::lock_guard<std::mutex> lock(mutex_);

        // 1. 命令类型：黑名单直接拒绝
        for (const auto& d : cfg.deny) {
            if (d == cmd) {
                return Decision::Deny;
            }
        }

        // 2. 规则查找：明细命中优先，否则 default 兜底
        ChannelLimitRule rule = cfg.default_rule;
        auto it = cfg.commands.find(cmd);
        if (it != cfg.commands.end()) {
            rule = it->second;
        }

        // 3. 大小限制
        if (rule.max_bytes > 0 && payload_bytes > rule.max_bytes) {
            return Decision::TooLarge;
        }

        // 4. 频率限制（60s 滑动窗口，每命令独立计数）
        if (rule.rate_per_min > 0) {
            const int64_t now = NowMs();
            constexpr int64_t kWindowMs = static_cast<int64_t>(60) * 1000;
            // 桶表上限: 超限先清"窗口内已无时间戳"的死桶; 仍超限
            // 整体清空兜底 —— 内存有界优先, 代价 = 一轮限流计数从零起(与 Reset 同义)。
            if (buckets_.size() > kMaxBuckets) {
                const int64_t cutoff = now - kWindowMs;
                for (auto it2 = buckets_.begin(); it2 != buckets_.end();) {
                    if (it2->second.empty() || it2->second.back() <= cutoff) {
                        it2 = buckets_.erase(it2);
                    }
                    else {
                        ++it2;
                    }
                }
                if (buckets_.size() > kMaxBuckets) {
                    buckets_.clear();
                }
            }
            auto& bucket = buckets_[cmd];
            // 懒清理窗口外的时间戳，避免桶无限增长
            while (!bucket.empty() && bucket.front() <= now - kWindowMs) {
                bucket.pop_front();
            }
            if (bucket.size() >= rule.rate_per_min) {
                return Decision::RateLimited;
            }
            bucket.push_back(now);
        }

        return Decision::Allow;
    }

    void ChannelRateLimiter::Reset() {
        std::lock_guard<std::mutex> lock(mutex_);
        buckets_.clear();
    }

    const char* ChannelRateLimiter::CodeOf(Decision d) {
        switch (d) {
        case Decision::Deny:        return "channel_denied";
        case Decision::TooLarge:    return "channel_too_large";
        case Decision::RateLimited: return "channel_rate_limited";
        default:                    return "channel_limited";
        }
    }

    const char* ChannelRateLimiter::ReasonOf(Decision d) {
        switch (d) {
        case Decision::Deny:        return "command denied on low-bandwidth channel";
        case Decision::TooLarge:    return "response exceeds channel size limit";
        case Decision::RateLimited: return "command rate limit exceeded";
        default:                    return "blocked by channel rate limiter";
        }
    }

} // namespace cleat