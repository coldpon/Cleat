/**
 * @file ExitProgramHandler.hpp
 * @brief 退出程序命令处理器
 * 
 * 处理Agent退出请求，由Server在自提权新实例上线后发送给旧实例。
 */

#pragma once
#include "Core/Command/Handlers/IHandler.hpp"

namespace cleat {

    /**
     * @brief 退出程序命令处理器
     * 
     * 当Server启动新的提升权限实例后，会向当前实例发送此命令，
     * 请求旧实例安全退出。
     */
    class ExitProgramHandler : public IHandler {
    public:
        /**
         * @brief 执行退出程序命令
         * @param req.params 请求参数（当前未使用）
         * @param req.payload 可选的二进制数据（当前未使用）
         * @return 响应JSON（包含成功状态）
         */
        nlohmann::json Execute(const CommandRequest& req) override;
    };

} // namespace cleat
