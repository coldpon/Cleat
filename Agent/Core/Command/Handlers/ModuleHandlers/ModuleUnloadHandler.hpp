#pragma once
/**
 * @file ModuleUnloadHandler.hpp
 * @brief ModuleUnload: 卸载反射模块(核心层命令)
 * @details
 * params: { "module_id": "..." }。执行体委托 ModuleManager::Unload —— 先注销
 * module_id 命令(阻止新命令进入), 再在锁外丢引用; DETACH+VirtualFree 由
 * shared_ptr 删除器在最后一个在途 handler 释放后执行(B1-1 延迟卸载语义)。
 * 归位说明见 ModuleLoadHandler.hpp。
 */
#include "Core/Command/Handlers/IHandler.hpp"

namespace cleat {

    class ModuleUnloadHandler : public IHandler {
    public:
        nlohmann::json Execute(const CommandRequest& req) override;
    };

} // namespace cleat
