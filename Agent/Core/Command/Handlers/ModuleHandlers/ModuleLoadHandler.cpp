/**
 * @file ModuleLoadHandler.cpp
 * @brief ModuleLoad 命令处理器实现(纯转发至 ModuleManager)
 */

#include "ModuleLoadHandler.hpp"
#include "Core/Module/ModuleManager.hpp"

namespace cleat {

    nlohmann::json ModuleLoadHandler::Execute(const CommandRequest& req) {
        return ModuleManager::Instance().Load(req.params, req.payload);
    }

} // namespace cleat
