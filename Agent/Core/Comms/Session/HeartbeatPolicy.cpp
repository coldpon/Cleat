/**
 * @file HeartbeatPolicy.cpp
 * @brief 心跳策略实现
 * @details
 * 实现 NextIntervalMs 和 ShouldSkipBeat 函数，提供均匀/正态分布两种抖动模式。
 */

#include "HeartbeatPolicy.hpp"
#include <algorithm>

namespace cleat {

    /**
     * @brief 计算下一次心跳间隔
     * @param active 是否处于活跃状态
     * @return 间隔（毫秒）
     * @details
     * 根据配置从 GetConfig 获取当前参数（加锁拷贝），
     * 若为正态分布模式，则使用正态分布生成间隔；否则使用均匀分布（历史行为）。
     * 间隔下限为 1000ms。
     */
    uint32_t HeartbeatPolicy::NextIntervalMs(bool active) {
     
        // 本函数在主循环线程调用。
        const HeartbeatConfig cfg = GetConfig();
        uint32_t base = active ? cfg.active_interval_ms : cfg.base_interval_ms;

        // 正态分布模式（抗统计建模）：均值 = base * (1 + bias)，标准差 = base * std
        // 均匀分布模式：历史行为 base ± jitter
        int64_t v;
        if (cfg.jitter_distribution == 1) {
            const double mean = static_cast<double>(base)
                * (1.0 + static_cast<double>(cfg.jitter_mean_bias_permille) / 1000.0);
            const uint32_t std_pm = cfg.jitter_std_permille
                ? cfg.jitter_std_permille : cfg.jitter_ratio_permille;
            const double stddev = static_cast<double>(base) * std_pm / 1000.0;
            std::normal_distribution<double> nd(mean, std::max(1.0, stddev));
            v = static_cast<int64_t>(nd(rng_));
            // 下限 1000ms（防忙等）; 上限 24h: base/bias 为
            // **服务端 SetConfig 即时生效值**, 无上界时正态长尾可越过 2^32 ms,
            // static_cast<uint32_t> 回绕 → 间隔塌缩到 1000ms 且随配置**持续**
            // (beacon 每 1s 一拍 = 行为特征暴露 + 流量暴增)。长尾抗建模由
            // bias/std 参数承载, 不靠无界。
            v = std::min<int64_t>(v, 86400000);
            v = std::max<int64_t>(1000, v);
        }
        else {
            int64_t delta = static_cast<int64_t>(base) * cfg.jitter_ratio_permille / 1000;
            if (delta < 1) delta = 1;
            std::uniform_int_distribution<int64_t> dist(-delta, delta);
            v = static_cast<int64_t>(base) + dist(rng_);
            // 上限 24h: base/jitter 为**服务端 SetConfig
            // 即时生效值**, 无上界时 base 接近 2^32 再叠加正向抖动可越过 2^32 →
            // static_cast<uint32_t> 回绕, 间隔塌缩到 1000ms 且随配置**持续**(beacon
            // 每 1s 一拍 = 行为特征暴露 + 流量暴增)。正态分支已 clamp, 均匀分支此前漏了。
            v = std::min<int64_t>(v, 86400000);
            v = std::max<int64_t>(1000, v);
        }
        return static_cast<uint32_t>(v);
    }

    /**
     * @brief 判断本次心跳是否应被跳过（随机丢拍）
     * @return true 表示跳过，false 表示发送
     * @details 根据配置的 skip_beat_permille 概率随机决定。
     */
    bool HeartbeatPolicy::ShouldSkipBeat() {
        const HeartbeatConfig cfg = GetConfig();
        if (cfg.skip_beat_permille == 0) return false;
        const uint32_t p = std::min<uint32_t>(cfg.skip_beat_permille, 1000);
        return (rng_() % 1000) < p;
    }

} // namespace cleat