#pragma once
#include "Core/Command/Dispatch/CommandDispatcher.hpp"
#include "Core/Module/IModule.hpp"
#include "Components/Commands/SystemInfo/Handler/SystemInfoHandler.hpp"
#include "Components/Commands/SystemInfo/Provider/SystemInfoProvider.hpp"

namespace cleat {

    inline void RegisterSystemInfo(CommandDispatcher& dispatcher, SystemInfoProvider& provider) {
        dispatcher.RegisterHandler(Cmd::GetSystemInfo(), [&provider]() {
            return std::make_unique<SystemInfoHandler>(provider);
            }, Priority::Discovery);
    }

    class SystemInfoModule : public IModule {
    public:
        SystemInfoModule() : ops_(CreateSystemInfo()), provider_(*ops_) {}
        void Register(CommandDispatcher& dispatcher) override {
            RegisterSystemInfo(dispatcher, provider_);
        }
        SystemInfoProvider& Provider() { return provider_; }  // 无条件注册, provider_ 恒有效
    private:
        std::unique_ptr<ISystemInfo> ops_;
        SystemInfoProvider provider_;
    };

} // namespace cleat