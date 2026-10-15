/**
 * @file AgentDescribeHandler.cpp
 * @brief Agent 自述命令处理器实现
 *
 * 只读聚合, 不修改任何运行状态: dispatcher 活注册表快照(ListHandlers)
 * + 模块管理器状态复用。
 */

#include "AgentDescribeHandler.hpp"
#include "Common/Version.hpp"
#include "Common/Protocol/Packet.hpp"
#include "Core/Command/Dispatch/CommandDispatcher.hpp"
#include "Core/Config/AgentConfig.hpp"
#include "Core/Module/ModuleManager.hpp"

namespace cleat {

    AgentDescribeHandler::AgentDescribeHandler(CommandDispatcher& dispatcher)
        : dispatcher_(dispatcher) {}

    /**
     * @brief 执行自述命令
     * @param req.params 未使用(AgentDescribe 无参数)
     * @param req.payload 未使用
     * @return 响应JSON: {agent: {version, build_id}, commands: [...], modules: [...]}
     */
    nlohmann::json AgentDescribeHandler::Execute(const CommandRequest&) {
        nlohmann::json data;
        data["agent"] = {
            { "version",  ver::kVersion },
            { "build_id", ver::BuildId() },
        };

        nlohmann::json commands = nlohmann::json::array();
        for (const auto& info : dispatcher_.ListHandlers()) {
            commands.push_back({ { "cmd", info.cmd }, { "priority", info.priority } });
        }
        data["commands"] = std::move(commands);

        // 已加载模块: 复用 ModuleStatus 的既有实现, 只取列表本体嵌入
        const nlohmann::json status = ModuleManager::Instance().Status({});
        if (status.contains("data") && status["data"].contains("modules")) {
            data["modules"] = status["data"]["modules"];
        }
        else {
            data["modules"] = nlohmann::json::array();
        }

        // 通道能力快照: 装配层写好的三态表(active/not_configured/
        // unavailable), 本 handler 只读转发 —— 依赖仅 AgentConfig(与 GetConfig
        // 同款惯例), 不触碰通信层。服务端据此做能力感知下发: 裁剪形态的 quic
        // 是 unavailable(reason 说明), 未下发的全量 quic 是 not_configured,
        // 盲发 quic:// 配置前先看这里。空数组 = 装配未完成(理论上 Describe
        // 注册晚于网络装配, 实际不会出现; 防御性原样透传)。
        data["channels"] = AgentConfig::Instance().GetChannelCapabilities();

        return Packet::Ok(std::move(data));
    }

} // namespace cleat
