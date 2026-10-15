/**
 * @file Schedulers.hpp
 * @brief 会话侧调度器(心跳 / Poll)
 * @details
 * 合一前: `ConnectionManager::RunConnectionLoop` 里塞着**四件事** —— 收包驱动、
 * 会话状态推进、定时器(心跳/Poll/EventReport)、降级策略。其中心跳与 Poll 的
 * 时间推进各自引用一串成员(`next_heartbeat_at_` / `next_poll_at_` / `last_activity_` /
 * `heartbeat_policy_`), 无法单独测试, 也无法在别处复用。
 *
 * 本文件把两者各抽为一个**被动调度器**: 只回答"现在该不该发", 不知道发送本身怎么做。
 * 时间由调用方注入(便于夹具用假时钟断言节奏与抖动), 不读系统时钟 —— 这是可测性的关键。
 *
 * **行为保持**: 心跳的"活动判定 / 随机丢拍 / 每次到点前同步配置"与 Poll 的
 * "抖动 + 每次到点读配置"均逐条对上合一前的实现。
 */
#pragma once

#include <chrono>
#include <cstdint>
#include <random>

#include "Core/Comms/Session/HeartbeatPolicy.hpp"

namespace cleat {

    /// 心跳调度器
    class HeartbeatScheduler {
    public:
        using Clock = std::chrono::steady_clock;
        using TimePoint = Clock::time_point;

        /// 连接建立/重连成功时调用: 下一次心跳立即(与合一前一致)
        void Reset(TimePoint now);

        /// 记录活动(收到命令即视为活动; 影响 active/idle 间隔选择)
        void NoteActivity(TimePoint now);

        /**
         * @brief 到点则推进下一次时间并返回 true
         * @details 推进方式与合一前一致: `next = now + NextIntervalMs(active)`,
         *          其中 active = (now - last_activity) < activity_window_ms;
         *          到点前先 `UpdateConfig`(SetConfig 下发即时生效 —— AgentConfig 每次读)。
         */
        bool Due(TimePoint now);

        /// 本次是否随机丢拍(调用方据此跳过发送, 但**仍然推进**下一次时间)
        bool ShouldSkipBeat();

        /// 当前策略配置(只读; 供观测。**按值**返回 —— policy 侧的 GetConfig() 本就是拷贝,
        /// 早前写成 `const HeartbeatConfig&` 是对临时量的悬垂引用(MSVC C4172))
        HeartbeatConfig Config() const;

        /// 注入种子(夹具确定性; 0 = 随机)
        void Seed(uint32_t seed);

    private:
        HeartbeatPolicy policy_;
        TimePoint last_activity_{};
        TimePoint next_{};
    };

} // namespace cleat
