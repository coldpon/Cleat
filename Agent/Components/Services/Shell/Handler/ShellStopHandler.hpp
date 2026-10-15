#pragma once
#include "Core/Command/Handlers/IHandler.hpp"
#include "Common/Protocol/Packet.hpp"
#include "Core/Job/JobRuntime.hpp"
#include "Core/Command/Handlers/JobHandlers/JobServiceResponses.hpp"

namespace cleat {
    class ShellStopHandler : public IHandler {
    public:
        ResourceKey ResourceOf(const CommandRequest&) const override { return GlobalKey(ResourceKind::Terminal); }
        nlohmann::json Execute(const CommandRequest&) override {
            // 任务系统 Phase 3: 协作式取消(服务 job 自带门闸;
            // 暂停期单停不挂死 —— alive 绑定语义由 ctx 承担)
            if (JobRuntime::ServiceStop("Shell")) {
                return Packet::Ok({{ "message", "Shell stopped" }});
            }
            return Packet::Error("Failed to stop Shell");
        }
    };
}