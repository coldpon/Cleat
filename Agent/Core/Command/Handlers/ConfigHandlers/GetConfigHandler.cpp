/**
 * @file GetConfigHandler.cpp
 * @brief 配置回读命令处理器实现
 *
 * 通过 AgentConfig::DumpJson() 读锁内组装当前生效配置全量并回传。
 * 只读操作, 不修改任何运行参数。
 */

#include "GetConfigHandler.hpp"
#include "Core/Config/AgentConfig.hpp"
#include "Common/Protocol/Packet.hpp"

namespace cleat {

    /**
     * @brief 执行配置回读命令
     * @param req.params 未使用(GetConfig 无参数)
     * @param req.payload 未使用
     * @return 响应JSON: {config: {可下发区全量, _startup: 启动期固定参数}}
     */
    nlohmann::json GetConfigHandler::Execute(const CommandRequest&) {
        AgentConfig& cfg = AgentConfig::Instance();
        return Packet::Ok({{ "config", cfg.DumpJson() }});
    }

} // namespace cleat
