#pragma once
/**
 * @file JobKillHandler.hpp
 * @brief JobKill: 受理停止请求
 * @details
 * 入参(二选一): {"id": 7} 或 {"name": "Audio"}。
 * 语义( 协作派): **受理** = 置取消令牌 + 唤醒可打断等待 + Running→Stopping;
 * 线程在自己的安全点(Tick/SleepFor)退出, 不做强杀 —— 响应里的 state 是"受理后的
 * 状态", 线程是否已消亡看后续 JobStatus 的 `draining`/`finished`。
 * 按名歧义(多个同名在跑)拒绝并提示用 id —— 不猜"杀最新"。
 */

#include "Core/Command/Handlers/IHandler.hpp"

namespace cleat {

    class JobKillHandler : public IHandler {
    public:
        nlohmann::json Execute(const CommandRequest& req) override;
    };

} // namespace cleat
