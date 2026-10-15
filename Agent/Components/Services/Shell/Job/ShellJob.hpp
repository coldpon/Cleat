#pragma once
/**
 * @file ShellJob.hpp
 * @brief Shell 服务 job 化(Phase 3): 旧 ShellService + 会话句柄槽。
 * @details 读取循环在提供者自有 reader 线程(GovLoop "svc-shell-reader" 维持, 不变);
 *          job 线程承载生命周期并做停止。ShellInputHandler 经 RunningServiceAttachment
 *          取 ShellSession —— 取代 GetService+static_cast。
 *          会话句柄随 job 终态失效(未在跑 → 查无此项 → handler 报 "not running")。
 */

#include <memory>
#include <string>

#include "Common/Protocol/CmdDef.hpp"
#include "Common/Logger/ErrLog.hpp"
#include "Common/Logger/Logger.hpp"
#include "Common/Protocol/Packet.hpp"
#include "Common/ThirdParty/json.hpp"
#include "Components/Services/Shell/Provider/ShellProvider.hpp"
#include "Core/Identity/SessionIdentity.hpp"
#include "Core/Job/JobContext.hpp"
#include "Core/Job/JobRuntime.hpp"

namespace cleat {

    /// Shell 服务句柄(挂在 job 记录附件上; 供 ShellInputHandler 寻址)
    class ShellSession {
    public:
        explicit ShellSession(std::shared_ptr<ShellProvider> provider)
            : provider_(std::move(provider)) {}
        bool ExecuteCommand(const std::string& cmd) { return provider_->ExecuteCommand(cmd); }
        bool IsRunning() const { return provider_->IsRunning(); }
    private:
        std::shared_ptr<ShellProvider> provider_;
    };

    inline JobRuntime::ServiceJob ShellJobFactory(const nlohmann::json& /*params*/) {
        auto provider = std::make_shared<ShellProvider>(CreateShellOps());

        // 必须先设置输出回调,再启动 Provider(否则 readerThread 启动时的早期输出会
        // 因 callback_ 为空而丢失 —— 旧注释保留)
        provider->SetOutputCallback([](const std::string& output) {
            nlohmann::json packet;
            packet["cmd"] = Cmd::Terminal::Output();
            packet["data"]["session_id"] = SessionIdentity::Instance().GetSessionId();
            packet["data"]["output"] = output;
            const std::string s = packet.dump();
            JobRuntime::Emit(std::vector<uint8_t>(s.begin(), s.end()));
            });

        if (!provider->Start()) {
            CLEAT_LOGE(ErrDom::Svc, ErrCode::PlatformApiFailed, "ShellJob: Provider 启动失败");
            return {};
        }
        LOG_INFO("ShellJob 启动成功");

        auto session = std::make_shared<ShellSession>(provider);
        JobRuntime::JobFn fn = [provider](JobContext& ctx) {
            // 生命周期载体: 读取在 provider 自有 reader 线程。
            // shell 进程自行退出 → reader 自停 → job 跟着收(旧服务类无此检查,
            // 死 shell 会一直显示在运行 —— job_list 需要真实状态)。
            while (ctx.Tick()) {
                if (!provider->IsRunning()) break;
                if (!ctx.SleepFor(std::chrono::milliseconds(100))) break;
            }
            provider->Stop();
            };
        return { std::move(fn), session };
    }

} // namespace cleat
