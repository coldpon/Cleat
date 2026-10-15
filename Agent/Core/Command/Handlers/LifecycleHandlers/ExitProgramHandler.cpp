/**
 * @file ExitProgramHandler.cpp
 * @brief 退出程序命令处理器实现
 * 
 * 实现Agent退出请求的处理逻辑，通过ClientContext通知主循环退出。
 */

#include "ExitProgramHandler.hpp"
#include "Core/Concurrency/ThreadGovernor.hpp"
#include "Core/Identity/SessionIdentity.hpp"
#include "Common/Protocol/Packet.hpp"

namespace cleat {

    /**
     * @brief 执行退出程序命令
     * @param req.params 请求参数（当前未使用）
     * @param req.payload 可选的二进制数据（当前未使用）
     * @return 成功响应JSON
     *
     * 走 ThreadGovernor::RequestExit —— 它置 Governor 退出标志并**并轨**置位
     * SessionIdentity 的退出标志: 主循环的既有 ShouldExit 检查照常生效, 同时所有
     * 已注册后台循环经 Checkpoint 收到同一退出请求。
     */
    nlohmann::json ExitProgramHandler::Execute(const CommandRequest&) {
        ThreadGovernor::RequestExit();
        return Packet::Ok();
    }

} // namespace cleat
