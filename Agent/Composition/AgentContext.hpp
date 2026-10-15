/**
 * @file AgentContext.hpp
 * @brief Agent运行时上下文
 * 
 * 管理Agent的完整运行时图，包括网络栈、模块、工作线程等核心组件。
 */

#pragma once
#include <atomic>
#include <deque>
#include <functional>
#include <mutex>
#include <memory>
#include <string>
#include <vector>

#include "Core/Module/IModule.hpp"
#include "Core/Comms/Channel/SendResult.hpp"
#include "Core/Diagnostics/EventReporter.hpp"
#include "Core/Exchange/DeadDropManager.hpp"
#include "Core/Identity/MachineIdentity.hpp"
#include "Core/Identity/Platform/IMachineIdOps.hpp"
#include "Core/Comms/Internal/ChannelRateLimiter.hpp"

namespace cleat {

    // Forward declarations - core components
    class INetworkOps;
    class IHttpOps;
    class Chain;
    class CommsManager;
    class CommandDispatcher;
    class CommandProcessor;
    class SessionLoop;
    class OutboundSink;
    class SystemInfoProvider;


    /**
     * @brief Agent运行时上下文类
     * 
     * 管理Agent的完整运行时图，包括网络栈、模块、工作线程等核心组件。
     * 提供启动、停止、初始化和有序清理等功能。
     */
    class AgentContext {
    public:
        AgentContext();
        ~AgentContext();

        AgentContext(const AgentContext&) = delete;
        AgentContext& operator=(const AgentContext&) = delete;

        /**
         * @brief 启动所有组件
         * 
         * 启动网络栈、模块、工作线程等所有核心组件。
         */
        void Initialize();

        /**
         * @brief 有序清理
         * 
         * 按照以下顺序清理：worker -> connection -> services -> tasks -> modules。
         */
        void Shutdown();

        /**
         * @brief 阻塞连接和处理循环
         * @param host 服务器主机地址
         * @param port 服务器端口
         * @return 运行状态码
         */
        int Run(const std::string& host, uint16_t port);
        void Stop();

        /**
         * @brief 只读: 当前待执行的收尾钩子数(装配后 >0; Shutdown 后归零)
         * @details 供测试断言"注册→注销"闭环: 跨单例裸 this 回调在注册时登记注销,
         *          Shutdown 统一执行后应归零。装配根治理第 5 步的验收口。
         */
        size_t TeardownHookCount() const { return teardown_hooks_.size(); }

    private:
        /**
         * @brief 初始化拆分方法
         */
        void SetupCommsLayer();      ///< 设置通信层
        void ReloadChannels();         ///< SetConfig channels 热切换: 重建候选链并原子替换
        void TryDeadDropRecover();     ///< N44: 连续失败阈值触发 —— 死投递取新地址并换址
        void SetupInfrastructure();   ///< 装配所有Manager回调

        void StartCommandWorker();    ///< 启动命令工作线程

        void RegisterAllHandlers();   ///< 注册所有处理器
        void SetupCallbacks();        ///< 设置回调函数
        void SendFirstConnect();      ///< 发送首次连接

        /**
         * @brief 装配步骤标识(Initialize 的 6 步; 用于就绪断言与失败定位)
         * @details 顺序即 Initialize 的调用序 —— 这就是"可执行的顺序契约"本身。
         *          加新步骤必须插进这个枚举(而不是只在 Initialize 里加一行),
         *          使"步骤清单"与"断言检查"同源。
         *
         * ** 装配根治理第 3 步**: 步骤的"顺序 + 执行 + 前置检查"已收进
         *   `Initialize()` 内的单张 `kSteps[]` 表(见 .cpp)。加步骤只需:
         *     ① 本枚举加一项; ② kSteps[] 加一行; ③ RunAssembleStep 加 case。
         *   三处缺任一处, 编译器在 /W4 下以 C4062(switch 未穷举)告警, 运行期
         *   由 VerifyAssembled 兜底。
         */
        enum class AssembleStep {
            CommsLayer = 1,     ///< SetupCommsLayer: 建 chain_/network_/sink_/processor_/session_loop_/dispatcher_
            Handlers = 2,         ///< RegisterAllHandlers: 用 dispatcher_, 建 modules_ + sys_provider_
            Callbacks = 3,        ///< SetupCallbacks: 用 session_loop_/sink_/chain_
            Infrastructure = 4,   ///< SetupInfrastructure: 用 network_/chain_/session_loop_/processor_
            EventReporter = 5,    ///< event_reporter_.Install
            CommandWorker = 6,    ///< StartCommandWorker
        };

        /**
         * @brief 执行某装配步骤的**唯一分派点**
         * @details 与 `Initialize()` 的 kSteps[] 表配合: 顺序在表里, 动作在这里。
         *          `default` 故意不加 —— /W4 下新增枚举值会触发 C4062, 提醒补 case。
         */
        void RunAssembleStep(AssembleStep step);

        /**
         * @brief 登记一条"收尾注销"钩子(装配根治理第 5 步, )
         * @details
         * 本类有 ~20 处 lambda 捕获裸 `this`, 其中**持有者是永不析构的单例**
         * (JobRuntime / TacticManager / PivotManager / ModuleManager /
         * PipeSessionHub / AgentConfig)的那批, 必须在 Shutdown 时统一失效 ——
         * 否则排水超时的在飞线程在本对象析构后调用即 UAF。
         *
         * 此前靠 `Shutdown()` 里一张**手写置空表**, 而它已经漏了两项
         * (`PivotManager::SetLowBandwidthCheck`、`ModuleManager::SetRuntimeQueries`)
         *
         * 改为: **注册回调的同时登记其注销闭包**(同一行代码, 见 Setup*), Shutdown
         * 统一逆序执行。注册与注销配对出现在一处, 不再有"另一张表要同步"。
         * @param hook 注销动作(通常是把对应单例的回调 setter 置空)
         */
        void RegisterTeardown(std::function<void()> hook);

        /**
         * @brief 执行全部已登记的收尾钩子(逆序 = 与注册相反)
         * @details Shutdown 调用。用 swap 取出后执行, 故可重入(重复 Shutdown 安全)。
         */
        void RunTeardownHooks();

        /**
         * @brief 就绪断言: 检查某步的**前置条件**是否已满足, 并在失败时终止启动
         * @details 把"6 步必须按序"这条口头协议变成**可执行契约**。检查的是
         *          **产物对象是否存在**(从事实推导), 不是手工维护的 flag ——
         *          手工 flag 会漂移, 而产物对象不会撒谎。
         *
         *          失败时 LOG_ERROR 并抛 std::logic_error(经 CrashGuard 归为启动失败),
         *          而不是静默 UB 到后面某处再崩: 顺序错了要在**错的那一步**炸,
         *          带步骤名, 而不是几层之后一个空指针解引用。
         * @param step 即将执行的步骤; 本函数校验其前置
         */
        void RequireStepPrecondition(AssembleStep step) const;

        /**
         * @brief 装配完成后的自洽检查: 所有步骤的关键产物是否齐备
         * @details Initialize 末尾调用。任何一项缺失 = 某步被跳过或顺序被打乱,
         *          立即报错而非留到运行时。
         */
        void VerifyAssembled() const;

        /**
         * @brief 系统信息提供者的**唯一访问口**
         * @details
         * `sys_provider_` 是**非拥有**指针 —— 它指向 `modules_` 内 `SystemInfoModule`
         * 的值成员 `provider_`(见 Register.hpp), 生命周期 = `modules_`。
         * 此前三处裸 `sys_provider_->` 解引用, 一旦 `Shutdown()` 清空 `modules_`
         * 后误用即 UAF, 且无任何判空。
         *
         * 本访问器把"非拥有访问"收成一处: 判空 → 失败即 LOG_ERROR 并抛, 把潜在的
         * UAF 变成**确定性、可定位**的失败。Shutdown 会把它同步置空, 故"Shutdown
         * 后使用"会在此处被明确拦下, 而不是崩在某个成员函数深处。
         */
        SystemInfoProvider& SysProvider() const;

        /// 组件帧发送收口: 对绕过 FrameBuilder 的已组包 JSON/混合帧兜底注入 ts (防重放)
        bool SendData(const std::vector<uint8_t>& data);

        /// 五值 → "本帧是否已被消费"
        static bool Consumed(SendResult r);

        /// 限流被拦截时的回执: 按原帧上下文(request_id/session_id)组装错误帧,
        /// 经 `SendLimitReceipt()` 投递(A.2.3: 强制 urgent, 回调期间整体豁免)
        void SendLimitReceipt(const std::string& cmd,
            ChannelRateLimiter::Decision decision, const std::vector<uint8_t>& frame);

        /**
         * @brief 日志上报处理（从Initialize中提取）
         * @param rec 事件记录
         */




        /**
         * @brief 成员变量
         */
        // Platform network ops. Stays here because CommsManager references it.
        std::unique_ptr<INetworkOps> network_ops_;      ///< 平台网络操作对象
        std::unique_ptr<identity::IMachineIdOps> machine_id_ops_;     ///< 身份源采集
        std::unique_ptr<identity::MachineIdentity> machine_identity_; ///< 本机身份生成器

        // 通道候选表
        std::unique_ptr<Chain> chain_;                 ///< 通道链

        // Core network stack
        std::unique_ptr<CommandDispatcher> dispatcher_; ///< 命令分发器
        std::unique_ptr<CommandProcessor> processor_;   ///< 命令处理器
        std::unique_ptr<OutboundSink> sink_;          ///< 出站唯一出口
        // 析构顺序(装配根治理第 4 步, ): 本类有两条顺序约束 ——
        //   (C8) ~SessionLoop 调 network_.Disconnect() → network_ 须比 session_loop_ 晚析构;
        //   (DD) DeadDropManager 以引用持 dead_drop_http_ → dead_drop_http_ 须比 dead_drop_ 晚析构。
        // **不再依赖成员声明位置**: ~AgentContext 中显式按序 reset()(见 .cpp)。
        // 历史上曾把 C8 规则理解反 → 每次优雅退出在 ~SessionLoop 命中 UAF
        // (Debug 0xDD 必崩 + CrashGuard 静默自重启; Release 静默 UB)。显式析构序即为此设。
        std::unique_ptr<CommsManager> network_;       ///< 通信管理器(被 session_loop_ 依赖)
        std::unique_ptr<SessionLoop> session_loop_;   ///< 会话循环
        std::unique_ptr<IHttpOps> dead_drop_http_;      ///< 死投递用 HTTP 会话(被 dead_drop_ 依赖)
        std::unique_ptr<deaddrop::DeadDropManager> dead_drop_;  ///<  死投递恢复(按需启用)

        // Feature/service modules. Destroyed in reverse registration order.
        ModuleRegistry modules_;                        ///< 模块注册表

        // Non-owning convenience reference used by SendFirstConnect().
        SystemInfoProvider* sys_provider_ = nullptr;    ///< 系统信息提供者指针

        // 收尾注销钩子(装配根治理第 5 步): 注册跨单例裸 this 回调时登记其注销闭包,
        std::vector<std::function<void()>> teardown_hooks_;

        std::atomic<bool> running_{ true };             ///< 运行状态标志

        // EventReport 队列已抽至 EventReporter(诊断重构 ①): 节流/去重/有界队列/
        // 会话绑定逻辑原样迁移; 出站经 SetSender 绑定本类 SendData(SetupInfrastructure)。
        EventReporter event_reporter_;                   ///< 日志事件上报器(诊断重构 ①)

        // 低带宽通道报文级限流（DNS/ICMP）：命令类型 / 载荷大小 / 频率 三位一体，
        // 只在 AgentContext::SendData 统一收口处调用（五 Manager 出站必经）。
    };

} // namespace cleat
