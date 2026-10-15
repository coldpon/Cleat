/**
 * @file SetWorkerTimeoutHandler.hpp
 * @brief 工作超时设置命令处理器
 * 
 * 专门处理命令执行超时时间的配置，支持设置无限超时。
 */

#pragma once
#include "Core/Command/Handlers/IHandler.hpp"

namespace cleat {

    /**
     * @brief 工作超时设置命令处理器
     * 
     * 用于配置命令执行的超时时间，防止长时间运行的命令阻塞系统。
     * 支持设置0表示不限制超时时间。
     */
    class SetWorkerTimeoutHandler : public IHandler {
    public:
        /**
         * @brief 执行工作超时设置命令
         * @param req.params 配置参数JSON对象
         * @param req.payload 可选的二进制数据（当前未使用）
         * @return 响应JSON（包含设置结果）
         * 
         * @note 参数格式：
         * @code
         * {
         *   "timeout_ms": 30000  // 毫秒，0 = 不限时
         * }
         * @endcode
         */
        nlohmann::json Execute(const CommandRequest& req) override;
    };

} // namespace cleat
