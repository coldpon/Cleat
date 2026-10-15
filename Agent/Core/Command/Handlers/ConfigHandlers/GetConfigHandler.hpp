/**
 * @file GetConfigHandler.hpp
 * @brief 配置回读命令处理器
 *
 * 回读 Agent 当前生效配置全量(可下发区 + _startup 启动期固定区),
 * 供 TeamServer/Web 面板做表单回填、diff 预览与审计核对。
 * 与 SetConfig 同属生存命令, 无条件注册, 不得随功能裁剪。
 */

#pragma once
#include "Core/Command/Handlers/IHandler.hpp"

namespace cleat {

    /**
     * @brief 配置回读命令处理器
     *
     * 响应结构:
     * @code
     * {
     *   "config": {
     *     "heartbeat": {...}, "reconnect": {...}, ...,
     *     "_startup": { "server_host": "...", "transport_mode": "...", ... }
     *   }
     * }
     * @endcode
     */
    class GetConfigHandler : public IHandler {
    public:
        nlohmann::json Execute(const CommandRequest& req) override;
    };

} // namespace cleat
