/**
 * @file ModuleStatusHandler.cpp
 * @brief ModuleStatus 命令处理器实现(纯转发至 ModuleManager)
 */

#include "ModuleStatusHandler.hpp"
#include "Core/Module/ModuleManager.hpp"

namespace cleat {

    nlohmann::json ModuleStatusHandler::Execute(const CommandRequest& req) {
        return ModuleManager::Instance().Status(req.params);
    }

} // namespace cleat
