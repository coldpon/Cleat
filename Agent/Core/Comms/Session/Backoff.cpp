/**
 * @file Backoff.cpp
 * @brief 退避策略实现
 */

#include "Core/Comms/Session/Backoff.hpp"

#include <algorithm>

namespace cleat {

    Backoff::Backoff(uint32_t base_ms, uint32_t max_ms, uint32_t jitter_permille,
        uint32_t seed)
        : base_(base_ms), max_(max_ms), jitter_permille_(jitter_permille),
          current_(base_ms) {
        // 生产用随机种子(各 agent 的退避相位相互独立, 这是防惊群的前提);
        // 夹具传固定种子以断言精确序列。
        if (seed != 0) {
            rng_.seed(seed);
        }
        else {
            std::random_device rd;
            rng_.seed(rd());
        }
    }

    uint32_t Backoff::NextDelayMs() {
        // 与合一前的 JitteredDelay 逐条一致: j = base*jitter/1000;
        // delta = uniform(-j, +j); 结果 = max(100, base + delta)
        const int64_t j = (static_cast<int64_t>(current_) * jitter_permille_) / 1000;
        const int64_t delta = std::uniform_int_distribution<int64_t>(-j, j)(rng_);
        const uint32_t delay = static_cast<uint32_t>(
            std::max<int64_t>(100, static_cast<int64_t>(current_) + delta));

        // 指数增长: 本次返回后把基准翻倍(不超上限) —— 顺序与旧实现一致
        current_ = std::min(current_ * 2, max_);
        return delay;
    }

    void Backoff::Reset() {
        current_ = base_;
    }

} // namespace cleat
