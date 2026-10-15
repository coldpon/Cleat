#pragma once
/**
 * @file ModuleStatusHandler.hpp
 * @brief ModuleStatus: 查询已加载模块状态(核心层命令, 只读)
 * @details
 * 无参数。输出 { "modules": [ {module_id, command, exports} ... ] }。
 * AgentDescribe 的 modules 段复用同一实现(ModuleManager::Status), 不建第二份状态源。
 * 归位说明见 ModuleLoadHandler.hpp。
 */
#include "Core/Command/Handlers/IHandler.hpp"

namespace cleat {

    class ModuleStatusHandler : public IHandler {
    public:
        nlohmann::json Execute(const CommandRequest& req) override;
    };

} // namespace cleat
