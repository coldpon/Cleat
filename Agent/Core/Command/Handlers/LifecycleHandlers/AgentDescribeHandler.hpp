/**
 * @file AgentDescribeHandler.hpp
 * @brief Agent 自述命令处理器
 *
 * 机器可读的自述("我是谁/我有什么"): 版本与构建标识 + 当前活注册命令清单
 * + 已加载模块摘要。与 GetConfig 同属生存命令, 无条件注册, 不得随功能裁剪。
 */

#pragma once
#include "Core/Command/Handlers/IHandler.hpp"

namespace cleat {

    class CommandDispatcher;

    /**
     * @brief Agent 自述命令处理器
     *
     * 响应结构:
     * @code
     * {
     *   "agent":    { "version": "0.1.0", "build_id": "Oct  1 2026 12:00:00" },
     *   "commands": [ { "cmd": "GetConfig", "priority": 0 }, ... ],
     *   "modules":  [ { "module_id": "...", "command": "...", "exports": 2 }, ... ],
     *   "channels": [ { "type": "tcp", "state": "active", "bandwidth": "High", "rank": 10 },
     *                 { "type": "quic", "state": "unavailable", "reason": "...", "rank": 60 }, ... ]
     * }
     * @endcode
     * commands 是 dispatcher 活注册表的锁内快照(按 cmd 字典序) —— CLEAT_MINIMAL
     * 裁剪与模块动态 Load/Unload 自动反映, **不是**编译期静态表; modules 直接
     * 复用 ModuleManager::Status 的列表本体(单一实现, 不建第二份模块状态源);
     * channels 是装配层写入 AgentConfig 的通道能力快照(三态语义见
     * Composition/AgentContextInternals.h::BuildChannelCapsSnapshot) —— handler 只读转发,
     * 网络依赖不出装配层。
     */
    class AgentDescribeHandler : public IHandler {
    public:
        /// @param dispatcher 命令分发器(活注册表的来源; 装配时注入)
        explicit AgentDescribeHandler(CommandDispatcher& dispatcher);

        nlohmann::json Execute(const CommandRequest& req) override;

    private:
        CommandDispatcher& dispatcher_;
    };

} // namespace cleat
