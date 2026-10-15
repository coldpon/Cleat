#pragma once
/**
 * @file ModuleLoadHandler.hpp
 * @brief ModuleLoad: 下发并加载反射模块(核心层命令, binary = 模块字节)
 * @details
 * 模块字节随帧二进制体单帧下发(req.payload), params 携带 module_id/type/platform/sha256。
 * 执行体委托 ModuleManager::Load —— 反射加载、SHA-256 完整性校验、ModInit(CoreApi)
 * 注入、以 module_id 动态注册命令等语义见 ModuleManager.hpp。
 *
 * 归位说明: 本类与 Unload/Status 是核心层命令面, 从 ModuleManager.cpp
 * 匿名 namespace 迁入 Handlers/ 并入 RegisterCore 统一注册; 模块命令的**动态**
 * 注册/注销(ModuleExecHandler, 随 Load/Unload 生命周期)仍留在 ModuleManager ——
 * 那是运行时产物, 非静态命令面。
 */
#include "Core/Command/Handlers/IHandler.hpp"

namespace cleat {

    class ModuleLoadHandler : public IHandler {
    public:
        nlohmann::json Execute(const CommandRequest& req) override;
    };

} // namespace cleat
