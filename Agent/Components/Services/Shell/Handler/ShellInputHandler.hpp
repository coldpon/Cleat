#pragma once
#include "Core/Command/Handlers/IHandler.hpp"
#include "Common/Protocol/Packet.hpp"
#include "Components/Services/Shell/Job/ShellJob.hpp"
#include "Core/Job/JobRuntime.hpp"

#include <memory>
#include "Common/Logger/Logger.hpp"

namespace cleat {
    class ShellInputHandler : public IHandler {
    public:
        ResourceKey ResourceOf(const CommandRequest&) const override { return GlobalKey(ResourceKind::Terminal); }
        nlohmann::json Execute(const CommandRequest& req) override {
            LOG_DEBUG("ShellInputHandler: received command");
            auto it = req.params.find("command");
            if (it == req.params.end() || !it->is_string()) {
                return Packet::Error("Missing 'command' parameter");
            }
            std::string command = it->get<std::string>();
            LOG_DEBUG("ShellInputHandler: command=" << command);

            // 任务系统 Phase 3: 经服务句柄槽寻址(取代 GetService+static_cast);
            // 未在跑 → 查无此项(与"服务已停"同义)
            auto session = std::static_pointer_cast<ShellSession>(
                JobRuntime::RunningServiceAttachment("Shell"));
            if (!session || !session->IsRunning()) {
                LOG_DEBUG("ShellInputHandler: service not running");
                return Packet::Error("Shell service is not running");
            }

            const bool ok = session->ExecuteCommand(command);
            LOG_DEBUG("ShellInputHandler: ExecuteCommand returned " << ok);
            if (ok) {
                return Packet::Ok({{ "message", "Command executed" }});
            }
            else {
                return Packet::Error("Failed to execute command");
            }
        }
    };
}