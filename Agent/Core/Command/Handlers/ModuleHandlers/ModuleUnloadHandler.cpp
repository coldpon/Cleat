/**
 * @file ModuleUnloadHandler.cpp
 * @brief ModuleUnload 命令处理器实现(纯转发至 ModuleManager)
 */

#include "ModuleUnloadHandler.hpp"
#include "Core/Module/ModuleManager.hpp"

namespace cleat {

    nlohmann::json ModuleUnloadHandler::Execute(const CommandRequest& req) {
        return ModuleManager::Instance().Unload(req.params);
    }

} // namespace cleat
