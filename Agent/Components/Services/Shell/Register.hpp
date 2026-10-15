#pragma once
// Shell 服务(job 化, Phase 3): 会话句柄经 RunningServiceAttachment 供 Input 寻址。
#include "Core/Command/Dispatch/CommandDispatcher.hpp"
#include "Components/Services/Shell/Job/ShellJob.hpp"
#include "Components/Services/Shell/Handler/ShellStartHandler.hpp"
#include "Components/Services/Shell/Handler/ShellStopHandler.hpp"
#include "Components/Services/Shell/Handler/ShellInputHandler.hpp"
#include "Common/Protocol/CmdDef.hpp"

namespace cleat {

    inline void RegisterShell(CommandDispatcher& dispatcher) {
        JobRuntime::RegisterService("Shell", /*high_bandwidth=*/false, ShellJobFactory);

        dispatcher.RegisterHandler(Cmd::Terminal::Start(),
            []() { return std::make_unique<ShellStartHandler>(); }, Priority::HeavyIO);
        dispatcher.RegisterHandler(Cmd::Terminal::Close(),
            []() { return std::make_unique<ShellStopHandler>(); }, Priority::HeavyIO);
        dispatcher.RegisterHandler(Cmd::Terminal::Input(),
            []() { return std::make_unique<ShellInputHandler>(); }, Priority::HeavyIO);
    }

} // namespace cleat
