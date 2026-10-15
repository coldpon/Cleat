#pragma once
/**
 * @file LifecycleGate.hpp
 * @brief 生命周期门闸判定
 * @details
 * 纯判定器：回答"现在该 Run / Pause / Exit"，不知道也不执行任何副作用
 * （断链、置位 Governor、退出都是主循环消费点的职责 —— 消费点唯一 = SessionLoop::Run）。
 *
 * 时间语义：
 * - killdate 用 UTC epoch 秒直接比墙钟（时区无关）；到点即 Exit，进程将退出，
 *   故无需"单次触发闩"（回拨场景：已触发即进程消亡，不存在重触发面）。
 * - working hours 用本机时区当日分钟数；start==end = 全天 = 窗口未启用；
 *   start > end 视为跨午夜窗口（如 22:00-06:00）。
 * - 死亡优先于暂停：killdate 与"窗外 + exit 策略"都汇入 Exit。
 *
 * **落位说明**: 归 **Core/Lifecycle/** —— 本类是行为判定器
 * (读配置做 Run/Pause/Exit 决策), 不是配置数据; 消费点在 SessionLoop, 联动
 * ThreadGovernor(Concurrency)。配置结构体 LifecycleConfig 仍在 AgentConfig.hpp
 * (Core/Config), 属配置语义。
 */

#include <cstdint>

namespace cleat {

    class LifecycleGate {
    public:
        enum class Decision : uint8_t { Run, Pause, Exit };

        /**
         * @brief 墙钟便捷版（主循环每拍调用；廉价：读配置 + 取系统时间）
         */
        static Decision Evaluate();

        /**
         * @brief 纯判定（可测性注入：时间由调用方给）
         * @param epoch_s      当前 UTC epoch 秒
         * @param local_minute 当前本机时区当日分钟数（0-1439）
         */
        static Decision Evaluate(uint64_t epoch_s, uint32_t local_minute);

        /**
         * @brief 工作窗口判定（纯函数；start > end 视为跨午夜）
         * @return true = 处于工作窗口内（或窗口未启用 start==end）
         */
        static bool InWorkWindow(uint32_t start_minute, uint32_t end_minute, uint32_t now_minute);
    };

} // namespace cleat
