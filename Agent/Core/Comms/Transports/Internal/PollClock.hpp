/**
 * @file PollClock.hpp
 * @brief 请求-响应通道（HTTPS/DNS/ICMP）的 check-in 节拍钟（节拍合一 A1/A2）
 * @details
 * 替代 CheckinThrottle：唯一抖动模型 = HeartbeatPolicy（正态/均匀分布 + 丢拍），
 * 每通道一个定时器（本类实例）。抖动必须到点抽一次并固化为下次时刻；
 * 每次调用现抽现比会让判定退化成"抽到多小就多早放行"，间隔塌缩到下界（实测 48~49s）。
 *
 * 三档（GET 取件与 DNS/ICMP check-in 共用）：
 *   fast   = 未注册(sid=0)/有上行积压 → 固定快速档（默认 1s），不抖动不丢拍（注册期需确定性）
 *   active = 自上次 check-in 以来本通道有成功上行 → active_interval_ms 收紧
 *   空闲   = base ± 抖动（正态/均匀）+ 丢拍(skip_beat)
 * 配置每次到点时从 AgentConfig 重读（SetConfig 下发即时生效）。
 *
 * interact 快速往返（A2）：上行（POST）完成即 ArmNow()，下一拍立即到期——
 * 只可能提前，不得无界缩短（ArmNow 不重抽间隔，下次 ShouldRun 到点后照常固化）。
 */
#pragma once

#include <chrono>
#include <cstdint>

#include "Core/Config/AgentConfig.hpp"
#include "Core/Comms/Session/HeartbeatPolicy.hpp"

namespace cleat {

    class PollClock {
    public:
        /// @param policy 唯一抖动模型（本类在到点时会先从 AgentConfig 同步配置）
        /// @param fast_interval_ms 快速档固定周期（毫秒；通常 1000，与 DNS/ICMP 的 fast_throttle 同义）
        PollClock(HeartbeatPolicy& policy, uint32_t fast_interval_ms)
            : policy_(policy), fast_interval_ms_(fast_interval_ms) {}

        /**
         * @brief 是否到点发起 check-in；到点时先做丢拍判定再固化下一拍
         * @param fast   未注册(sid=0)或有上行积压
         * @param active 自上次 check-in 以来有成功上行
         * @note 间隔自**发起时刻**起算（非事务完成时刻）—— 丢拍必须在发起前判定，
         *       且发起起算下节拍不受单次事务时长扰动（CS sleep 语义同此）。
         */
        bool ShouldRun(bool fast, bool active) {
            const auto now = std::chrono::steady_clock::now();
            if (armed_ && now < next_at_) {
                return false;   // 未到点(节拍已固化)
            }
            return Reschedule(fast, active);
        }

        /// 抽下一拍并固化；返回**本次**是否放行（丢拍 = false，且固化两倍间隔）
        bool Reschedule(bool fast, bool active) {
            const auto now = std::chrono::steady_clock::now();
            armed_ = true;
            if (fast) {
                next_at_ = now + std::chrono::milliseconds(fast_interval_ms_);
                return true;
            }
            // SetConfig 下发即时生效: 到点才同步配置(与 HeartbeatScheduler::Due 同形)
            policy_.UpdateConfig(AgentConfig::Instance().GetHeartbeat());
            const uint32_t interval = policy_.NextIntervalMs(active);
            if (policy_.ShouldSkipBeat()) {
                // 丢拍: 本次不发起, 再加一个间隔(节拍不规则化; 下一拍必到)
                next_at_ = now + std::chrono::milliseconds(interval) +
                           std::chrono::milliseconds(policy_.NextIntervalMs(active));
                return false;
            }
            next_at_ = now + std::chrono::milliseconds(interval);
            return true;
        }

        /// interact(A2): 上行完成即到期 —— 下次 ShouldRun 立即放行(只提前, 不缩短固有节拍)
        void ArmNow() {
            next_at_ = std::chrono::steady_clock::now();
            armed_ = true;
        }

        /**
         * @brief 距下一拍到期的毫秒数
         * @return 0 = **不可得/已到期** —— 未武装(下次 ShouldRun 立即放行)、
         *         已推进到 next_at_ 之后, 或剩余不足 1ms;
         *         其余 = 已固化的下一拍剩余毫秒(纯读数, 不重抽/不抖动)
         * @note 与 ShouldRun 同一时间源(steady_clock), 纯查询无副作用:
         *       抽拍(含丢拍判定与配置重读)只发生在 ShouldRun 的到点路径。
         */
        uint32_t NextDueInMs() const {
            if (!armed_) {
                return 0;
            }
            const auto now = std::chrono::steady_clock::now();
            if (now >= next_at_) {
                return 0;
            }
            return static_cast<uint32_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(next_at_ - now).count());
        }

        /// 清空节拍(未武装态: 下次 ShouldRun 立即放行)
        void Reset() {
            armed_ = false;
            next_at_ = {};
        }

    private:
        HeartbeatPolicy& policy_;
        uint32_t fast_interval_ms_;
        bool armed_ = false;                               ///< 是否已固化下一拍
        std::chrono::steady_clock::time_point next_at_{};  ///< 下次最早执行时刻
    };

} // namespace cleat
