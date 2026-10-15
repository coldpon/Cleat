/**
 * @file ChannelHealth.hpp
 * @brief 通道健康档
 * @details
 * 三条判定原则(, 实现时逐条遵守):
 *   1. **一信号一归属**: 每个观测点映射到唯一档, 写死在判据表里, 不允许"看情况"。
 *   2. **Dead 集合 ≡ 今天 `connected_ = false` 的集合**(逐条对应, 不多不少);
 *      **Degraded 只能由今天"不判死"的信号产生**。
 *   3. **Degraded 在 P1 只观测**: 只发事件 + 日志, 不改变任何重连/调度/丢弃行为。
 *      允许 Degraded 影响行为的那天, 必须重做一次等价评估。
 *
 * 为什么需要第三档: 今天"通道是否可用"只有一个 bool, 于是"无数据/空 body/节流未到"
 * 这类**正常但非健康**的状态与"链路已死"混在一起, 使得新加通道时无法区分该不该重连。
 */
#pragma once

namespace cleat {

    /// 通道健康档
    enum class ChannelHealth {
        Healthy = 0,   ///< 正常
        Degraded = 1,  ///< 非致命异常(丢帧/未收齐/瞬态), **P1 只观测不改变行为**
        Dead = 2       ///< 通道自报故障 → 交上层换候选/重连
    };

    inline const char* ToString(ChannelHealth h) {
        switch (h) {
        case ChannelHealth::Healthy: return "Healthy";
        case ChannelHealth::Degraded: return "Degraded";
        case ChannelHealth::Dead: return "Dead";
        }
        return "?";
    }

} // namespace cleat
