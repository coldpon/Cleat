/**
 * @file SessionLoop.hpp
 * @brief 会话循环(I/O 引擎): 唯一驱动 `SessionRunner` 的地方
 * @details
 * 承接原 `ConnectionManager` 的**全部循环职责** —— 首次连接、重连退避、心跳与 Poll 的
 * 节奏、EventReport 刷出、帧路由、启动随机延时(防惊群); 但**状态判定全部委托给
 * `SessionRunner`**: 本类只做 I/O 与时间(Connect / Disconnect / Send / Sleep), 不再
 * 持有任何会话标志位。
 *
 * **为什么是独立的一类, 而不是把循环塞进 `SessionRunner`**(计划文本"职责分归 Runner"
 * 的等价落地): ()把"被动"定为状态机的**契约** —— 不持线程、不睡眠、不读
 * 配置, 状态推进才可能脱离 I/O 单测。把线程、睡眠与全局配置
 * 塞回 Runner, 会当场推翻该契约。故切成两块, 各自只有一个所有者:
 *   * `SessionRunner` —— 会话状态 / 事件 / 查询(被动, 可无 I/O 单测);
 *   * `SessionLoop`   —— 循环与 I/O(全进程唯一调用 Runner 的地方)。
 * 这正是 A "把 I/O 从主循环拆出"的目标形态, 只是提前落地。
 *
 * **相对旧实现的两处有意差异**(均为严格更优, 不改变协议语义):
 *   ① `Stop()` 延迟从"最长一个退避(可达 30s)"降到 ≤10ms —— 旧实现用不可中断的
 *      `sleep_for`, 停止要等退避结束; 现在等待按 `kSleepSliceMs` 分片并检查停止标志。
 *   ② 启动随机延时同样分片(旧实现用条件变量精确唤醒); 差异仅 ≤10ms, 防惊群语义不变。
 */
#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <random>
#include <string>

#include "Core/Comms/Session/Schedulers.hpp"
#include "Core/Comms/Session/SessionRunner.hpp"

namespace cleat {

    class CommsManager;
    class CommandProcessor;
    class OutboundSink;

    /**
     * @brief 会话循环(I/O 引擎)
     */
    class SessionLoop {
    public:
        using Clock = std::chrono::steady_clock;

        /// 心跳回调: 返回 true = 本轮已发出(失败会被留痕, R2)
        using HeartbeatCallback = std::function<bool()>;
        /// Poll 拉取回调: 返回 true = 本轮已发出
        /// 连接成功回调(首次 / 每次重连)
        using OnConnectedCallback = std::function<void()>;
        /// EventReport 队列刷出(每轮; 发送在锁外)
        using EventReportFlusher = std::function<void()>;
        /// 当前通道是否 Push 型(节拍合一 A3.1: Poll 通道的保活=GET, 心跳分支整体不参与)
        using IsPushChannel = std::function<bool()>;
        /// N44: 连续失败达到阈值时触发一轮死投递恢复(回调内部自行换址/节流)
        using AddressRecoveryCallback = std::function<void()>;

        SessionLoop(CommsManager& network, OutboundSink& sink, CommandProcessor& processor);
        ~SessionLoop();

        SessionLoop(const SessionLoop&) = delete;
        SessionLoop& operator=(const SessionLoop&) = delete;

        // ---------------- 装配(与旧 ConnectionManager 同名, 装配方改动面最小) ----------------
        void SetHeartbeatCallback(HeartbeatCallback cb);
        void SetIsPushChannel(IsPushChannel cb);
        void SetAddressRecoveryCallback(AddressRecoveryCallback cb);
        void SetOnFirstConnectCallback(OnConnectedCallback cb);
        void SetOnReconnectCallback(OnConnectedCallback cb);
        void SetEventReportFlusher(EventReportFlusher cb);
        /// 换链事件订阅
        void AddChannelsReloadObserver(SessionRunner::ChannelsReloadObserver obs);
        /// 从 AgentConfig 的 reconnect 域重配退避(B3-7 收口): 构造时与 SetConfig
        /// 下发 reconnect 域各调一次 —— 之前该域在 AgentConfig 之外**零消费**
        /// (SessionRunner 跑的是自己的默认值, 配置形同虚设)。
        void ApplySessionConfig();
        /// 外部活动通知(除命令外的活动信号, 如任务运行)
        void NotifyActivity();

        // ---------------- 运行 ----------------
        /**
         * @brief 退出原因
         * @details 本循环**不存在异常退出路径** —— 未捕获异常由 RunAgent catch(退出码 1),
         *          硬件异常走 CrashGuard; 这里如实报告的是"哪一类计划内终止", 供外部
         *          (启动器/守护脚本)区分。旧实现 Run 返回 void, 一切终止在上层都塌成 0。
         */
        enum class ExitKind {
            Stopped,        ///< 外部停止: Stop() / 收尾信号(门闸 Stopping 并轨)
            ExitRequested,  ///< 退出请求: 服务端 ExitProgram / 自提权·迁移交接
            Lifecycle,      ///< killdate / 窗外退出策略到点(计划内终止, 守护脚本勿重启)
        };

        /**
         * @brief 阻塞式运行: 建链 → 注册 → 稳态(心跳/Poll/路由) → 断链重连, 直到 Stop()
         * @details
         * 循环骨架:
         * ```
         * 启动随机延时 → runner_.Start()
         * loop:
         *   退出请求? → 结束
         *   读配置修订 → runner_.NoteChannelsRevision()   // 换链: 去重与派发在 Runner 内
         *   Reconnecting? → 睡退避 → BackoffElapsed
         *   Opening?      → Connect + (KE) + 注册帧
         *   链已死?       → runner_.OnChannelDown()
         *   否则          → runner_.Tick(收帧回调)  // 经 Chain::Recv(唯一入口)→Classify→Feed
         *                   路由帧 → 等确认 / 退避 / 稳态
         * ```
         * @return 退出原因(见 ExitKind)
         */
        ExitKind Run(const std::string& host, uint16_t port);

        /// 停止: 置停止标志并断开网络(睡眠分片 ≤10ms 内退出)
        void Stop();

        // ---------------- 观测(排障与端到端冒烟) ----------------
        SessionState State() const { return runner_.State(); }
        uint64_t Epoch() const { return runner_.Epoch(); }
        SessionRunner& Runner() { return runner_; }
        const SessionRunner& Runner() const { return runner_; }

    private:
        /// 停止标志(不持 mutex: 等待被分片化, 无需唤醒机制)
        bool StopRequested() const { return !running_.load(); }

        /// 分片睡眠(每片检查停止标志); 返回 false = 被停止中断
        bool SleepMs(uint32_t ms);

        /// 启动随机延时(防惊群; 上限来自 AgentConfig, 0 = 不延时)
        void StartUpDelay();

        /// 建链 + 注册序列(KE → FirstConnect), 等价旧实现的 Arm/ClearOutbound/KE/FC 次序
        void OpenAndRegister(const std::string& host, uint16_t port);

        /// 稳态工作: 心跳 / Poll / EventReport 刷出
        void SteadyStateTasks();
        /// 上一轮稳态工作是否已启动过(调度器只在进入 Ready 时复位一次, 与旧实现一致)
        void EnsureSteadyStateStarted();

        /// 把 Tick 收到的帧交给业务侧执行副作用(分类已在 Tick 内完成, 不重复分类)
        void RouteFrames(const SessionRunner::TickResult& res);

        /// 发送失败留痕(10s 节流, 防断线瞬间刷屏; R2)
        void LogSendFailure(Clock::time_point& lastFailAt, const char* what);

        CommsManager& network_;
        OutboundSink& sink_;      ///< 出站唯一出口(KE 也走它, 但显式标记为握手帧)
        CommandProcessor& processor_;
        std::atomic<bool> running_{ true };

        SessionRunner runner_;                       ///< 会话状态的唯一持有者
        HeartbeatScheduler heartbeat_sched_;          ///< 心跳节奏
        bool steady_state_started_ = false;           ///< 调度器是否已复位(进入 Ready 时一次)
        bool ready_announced_ = false;                ///< "进入 Ready"是否已留痕(离开 Ready 即复位)
        bool lifecycle_pause_ = false;                ///< working hours 窗外暂停闩(断链只做一次)

        HeartbeatCallback heartbeat_callback_;
        IsPushChannel is_push_channel_;
        AddressRecoveryCallback address_recovery_cb_;
        uint32_t recover_fail_streak_ = 0;   ///< 连续重连失败计数(进入稳态清零; 仅首次触发阶段使用)
        // ---- 死投递重试节奏(两阶段: 计数首次 + 时间重试,  天级化) ----
        bool dd_time_mode_ = false;                    ///< 首次触发后进入时间模式(独立指数退避)
        uint32_t dd_retry_round_ = 0;                  ///< 时间模式轮次(等待 = min×2^round 封顶 max)
        Clock::time_point dd_next_attempt_{};          ///< 下一轮死投递恢复的最早时刻
        EventReportFlusher event_report_flusher_;
        OnConnectedCallback on_first_connect_callback_;
        OnConnectedCallback on_reconnect_callback_;

        Clock::time_point last_hb_fail_at_{};          ///< 上次心跳失败(节流用)
        std::mt19937 rng_{ std::random_device{}() };   ///< 启动随机延时用
    };

} // namespace cleat
