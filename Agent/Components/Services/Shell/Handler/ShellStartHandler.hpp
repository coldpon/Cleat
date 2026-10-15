#pragma once
#include "Core/Command/Handlers/IHandler.hpp"
#include "Common/Protocol/Packet.hpp"
#include "Core/Job/JobRuntime.hpp"
#include "Core/Command/Handlers/JobHandlers/JobServiceResponses.hpp"

namespace cleat {
    class ShellStartHandler : public IHandler {
    public:
        ResourceKey ResourceOf(const CommandRequest&) const override { return GlobalKey(ResourceKind::Terminal); }
        nlohmann::json Execute(const CommandRequest& req) override {
            // 任务系统 Phase 3: 拒绝原因可区分( 服务半边),
            // 单例/双门/提供者启动语义与旧 StartService 一致
            const auto start = JobRuntime::ServiceStart("Shell", req.params);
            return ServiceStartResponse(start, "Shell",
                { { "message", "Shell started" } },
                "Failed to start Shell");
        }
    };
}