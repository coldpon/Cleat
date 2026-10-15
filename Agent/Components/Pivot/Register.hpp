#pragma once
#include "Core/Command/Dispatch/CommandDispatcher.hpp"
#include "Common/Protocol/CmdDef.hpp"
#include "Components/Pivot/PivotHandlers.hpp"

namespace cleat {

   
    inline void RegisterPivot(CommandDispatcher& dispatcher) {
        dispatcher.RegisterHandler(Cmd::Pivot::Start(),
            []() { return std::make_unique<PivotStartHandler>(); },Priority::Discovery);
        dispatcher.RegisterHandler(Cmd::Pivot::Data(),
            []() { return std::make_unique<PivotDataHandler>(); }, Priority::HeavyIO);
        dispatcher.RegisterHandler(Cmd::Pivot::Close(),
            []() { return std::make_unique<PivotCloseHandler>(); }, Priority::Discovery);
        dispatcher.RegisterHandler(Cmd::Pivot::Listen(),
            []() { return std::make_unique<PivotListenHandler>(); }, Priority::Discovery);
        dispatcher.RegisterHandler(Cmd::Pivot::RportfwdAdd(),
            []() { return std::make_unique<PivotRportfwdAddHandler>(); }, Priority::Discovery);
        dispatcher.RegisterHandler(Cmd::Pivot::RportfwdDel(),
            []() { return std::make_unique<PivotRportfwdDelHandler>(); }, Priority::Discovery);
        dispatcher.RegisterHandler(Cmd::Pivot::ListenPipe(),
            []() { return std::make_unique<PivotListenPipeHandler>(); }, Priority::Discovery);
        dispatcher.RegisterHandler(Cmd::Pivot::PipeStop(),
            []() { return std::make_unique<PivotPipeStopHandler>(); }, Priority::Discovery);
    }

}
