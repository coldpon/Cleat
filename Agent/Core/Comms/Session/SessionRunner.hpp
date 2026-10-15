/**
 * @file SessionRunner.hpp
 * @brief 会话运行器: 会话状态的唯一持有者 + 被动驱动
 * @details
 * **存在理由**: 会话状态此前摊在 `ConnectionManager` 的标志位上, 且**没有主人** ——
 * `session_confirm_ok_` / `session_denied_` / `awaiting_confirm_` / `first_connect_done_`
 * 由四个不同位置读写, 两个循环(首连/重连)各写一份 `retry_delay`, 而"确认"这一事件
 * 是**跨线程**从 `CommandProcessor` 回调里塞进原子 bool 的。任何一处改动都要在四处
 * 对齐, 于是长出"假成功"与状态错配。
 *
 * 本类把它收敛为: **一份状态机(`SessionMachine`) + 一次 Tick 的输入输出**。
 *
 * ```
 * Tick(recv, now)
 *   ├─ 接收时代际快照()            ← 归属标记必须在"到达时刻"确定
 *   ├─ Pump:  recv(raw)                ← 由驱动方注入: 生产 = Chain::Recv(**唯一收包入口**,
 *   │                                     含"降级时回探更高优先候选"); I/O 与解密在通道层完成
 *   ├─ Classify: ClassifyFrame(raw)      ← 纯函数
 *   └─ Feed:   SessionMachine::Feed(ev)  ← 唯一的状态推进入口
 * ```
 *
 * **被动性质(硬约束)**: 本类**不持线程、不睡眠、不自己读配置**。时间由调用方以
 * `TimePoint` 传入, 配置由 `SessionRunnerConfig` 传入 —— 于是全部节奏与状态推进都能
 * 在夹具里用假时钟确定性地断言。I/O 动作(建链、发 KE、发注册)
 * 由驱动方按本类的查询结果执行, 直至 把主循环整体搬进来。
 *
 * **收帧以回调注入(N32/L-1)**: 早前这里直接收 `IChannel*` 并自己 `Pump`, 于是生产走
 * "直取通道"、夹具走 `Chain::Recv` —— 两条收包路径分叉, 而回探/收侧失败计数只挂在后者,
 * 生产里从不回探。现由驱动方注入收帧函数, 收包入口**只剩一个**(`Chain::Recv`)。
 *
 * **同时代际归属()**: 一次 Pump 批次**共用一个接收时代际**。同批帧可视为同一
 * 时刻到达; 若让本批前序帧改变了状态、后续帧改用新代际, 会把"同一时刻到达"割裂成
 * 不同归属, 反而制造误判。
 */
#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "Core/Comms/Session/Backoff.hpp"
#include "Core/Comms/Session/FrameClassifier.hpp"
#include "Core/Comms/Session/SessionMachine.hpp"
#include "Core/Comms/Session/SessionState.hpp"

namespace cleat {

    class IChannel;

    /**
     * @brief 会话运行器配置(纯数据)
     * @details 不依赖 `AgentConfig` —— 装配方从配置系统映射进来, 夹具直接构造。
     *          这是"节奏可断言"的前提: 读全局单例的对象无法在夹具里换参数。
     */
    struct SessionRunnerConfig {
        uint32_t first_connect_base_ms = 1000;   ///< 首连退避起点
        uint32_t first_connect_max_ms = 30000;   ///< 首连退避上限
        uint32_t reconnect_base_ms = 1000;       ///< 重连退避起点
        uint32_t reconnect_max_ms = 30000;       ///< 重连退避上限
        uint32_t jitter_permille = 300;          ///< 退避抖动(±千分比)
        uint32_t confirm_timeout_ms = 8000;      ///< 确认超时(合一前 kSessionConfirmTimeoutMs)
        uint32_t rng_seed = 0;                   ///< 0 = 系统随机源(生产); 非 0 = 确定性(夹具)
    };

    /**
     * @brief 会话运行器
     */
    class SessionRunner {
    public:
        using Clock = std::chrono::steady_clock;
        using TimePoint = Clock::time_point;

        /// 一次 Tick 的产出(驱动方据此路由帧与记录日志)
        struct TickResult {
            std::vector<ClassifiedFrame> frames; ///< 本 Tick 收到的帧(全部, 含会话帧)
            size_t command_frames = 0;    ///< 其中"会派发命令"的帧数(活动判定口径)
            /// 本批帧的**接收时代际**(): 驱动方路由帧时把它绑到命令上 —— 迟到响应据此丢弃
            uint64_t recv_epoch = 0;
            bool ready_entered = false;   ///< 本 Tick 确认成功 → Ready
            bool denied = false;          ///< 本 Tick 在注册窗口内被服务端拒绝
            bool confirm_timeout = false; ///< 本 Tick 确认超时("假成功")
            bool channel_down = false;    ///< 本 Tick 通道自报故障
            /// 本 Tick 收到"服务端不认得本机 sid"(SESSION_UNKNOWN) → 已清身份转入重连
            bool session_lost = false;
            /// 上述任一为真(驱动方的日志/重连分支用)
            bool session_changed() const {
                return ready_entered || denied || confirm_timeout || channel_down
                    || session_lost;
            }
        };

        SessionRunner();
        explicit SessionRunner(const SessionRunnerConfig& cfg);

        /// 重建退避策略(不改变当前会话状态; 装配方/SetConfig 下发后调用)
        void Configure(const SessionRunnerConfig& cfg);

        // ---------------- 生命周期 ----------------

        /// 请求建链(Idle → Opening)。幂等: 非 Idle 态忽略并留痕
        void Start();

        /// 退避到点(Reconnecting → Opening)
        void BackoffElapsed();

        /// 停止(任意态 → Stopping; 幂等)
        void Stop();

        // ---------------- I/O 事件(由驱动方喂入) ----------------

        /**
         * @brief 通道打开结果(驱动方在 `Chain::Connect` 返回后**立即**调用)
         * @param ok               通道是否就绪
         * @param handshake_required 是否需要握手步骤(PSK 模式为 false → 直接 Registering)
         * @param now              当前时间(确认超时**从此刻起算** —— 等价于合一前的
         *                         `ArmSessionConfirm()` 在 Connect 成功后记录 connect_start_)
         */
        void OnChannelResult(bool ok, bool handshake_required, TimePoint now);

        /// 握手帧已发出(Handshaking → Registering; 仅 handshake_required 时有意义)
        void OnHandshakeSent();

        /// 通道自报故障(如心跳/Poll 发送返回 ChannelFailure)
        void OnChannelDown();

        /// SetConfig 下发新通道链(旧链已失效)。仅在"有旧链可用"的状态下改变会话状态
        void OnChannelsReconfigured();

        /**
         * @brief 轮询配置修订号: 变化则**恰好一次**地产生换链事件
         * @param rev 当前 channels 域修订号(来源 `AgentConfig::GetChannelsRevision()`)
         * @return true = 本次检测到变化(已派发)
         * @details
         * 合一前是"每轮读 revision + 当轮立刻调换链回调"(`ConnectionManager::CheckChannelsReload`),
         * "检测"与"动作"混在一处, 既无法断言"只发生一次", 也无法表达"这是配置驱动而非链路失败"。
         * 此处只负责**去重与派发**: 换链动作由订阅者执行(装配方把 `AgentContext::ReloadChannels`
         *
         * 初始修订号视为 **0**(与启动时的初始链一致), 故启动不产生换链事件。
         * 换链是**配置驱动而非失败**: 事件后立即复位退避并推进到 `Opening` —— 等价于旧实现
         * "换链后当轮即调 Connect, 不等退避"。
         */
        bool NoteChannelsRevision(uint64_t rev);

        /// 已应用的配置修订号(观测用)
        uint64_t AppliedChannelsRevision() const { return applied_channels_rev_; }

        // ---------------- 驱动 ----------------

        /// 收帧回调: 取一帧返回 true, 无帧返回 false(非错误)。生产传 `Chain::Recv`(唯一入口)
        using RecvFn = std::function<bool(std::vector<uint8_t>&)>;

        /**
         * @brief 一次驱动: 时间推进 → 收帧 → Classify → Feed(不含命令执行)
         * @param recv 收帧回调(空 = 只做时间推进, 夹具可无通道驱动); 由驱动方注入,
         *             故收包入口只有一个(`Chain::Recv` 内含"降级时回探更高优先候选")
         * @param now  当前时间(确认超时判定用; 全部节奏由它决定, 本类不读真实时钟)
         * @details
         * 次序与合一前的 `WaitSessionConfirm` **逐条一致**: 先判"是否已超时", 再驱动
         * 收包 —— 于是"同一轮里既超时又收到确认"时**超时获胜**(与旧实现相同)。
         * 收到的帧**全部**返回给驱动方路由: 会话帧由本类消费(状态推进), 业务帧交
         * 命令处理器 —— 本类只读会话相关字段, 不重复处理业务。
         */
        TickResult Tick(const RecvFn& recv, TimePoint now);

        // ---------------- 查询(驱动方据此执行 I/O) ----------------

        SessionState State() const { return machine_.State(); }
        uint64_t Epoch() const { return machine_.CurrentEpoch(); }
        bool Stopping() const { return machine_.State() == SessionState::Stopping; }

        /// 是否需要调用 `Connect`(Opening 态)
        bool NeedOpen() const { return machine_.State() == SessionState::Opening; }

        /// 是否处于注册窗口(Handshaking/Registering): 期间不发心跳/Poll
        bool AwaitingConfirm() const;

        /// 会话就绪(已注册)
        bool Ready() const { return machine_.State() == SessionState::Ready; }

        /// 是否该发握手帧(Handshaking 态 = 通道要求握手且尚未发出)
        bool NeedHandshakeFrame() const { return machine_.State() == SessionState::Handshaking; }

        /// 首次连接是否已完成(决定用"首连"还是"重连"的注册回调)
        bool FirstConnectDone() const { return first_connect_done_; }

        /// 最近的通道打开是否失败(驱动方据此决定是否退避等待)
        bool NeedBackoff() const { return machine_.State() == SessionState::Reconnecting; }

        /**
         * @brief 取本次退避等待时长(带抖动)并推进内部基准
         * @details 未完成首连时用首连退避, 之后用重连退避 —— 与合一前的两处调用一致。
         */
        uint32_t NextBackoffMs();

        /// 会话健康(连接正常且已就绪/注册中) → 退避回到最短(合一前每轮循环调用)
        void NoteHealthy();

        // ---------------- 观测 ----------------

        const SessionMachine& Machine() const { return machine_; }
        uint64_t RejectedTransitions() const { return machine_.RejectedTransitions(); }
        std::vector<std::string> TraceStrings() const { return machine_.TraceStrings(); }
        void AddStateObserver(SessionMachine::StateObserver obs) {
            machine_.AddStateObserver(std::move(obs));
        }

        /**
         * @brief 注册意外确认观察者(转发给状态机)
         * @details 与 `AddStateObserver` 同构 —— `Machine()` 只给 const 引用, 故新增观察者
         *          必须在此开一个转发口, 不能让装配方绕过 Runner 去改状态机。
         */
        void AddAnomalyObserver(SessionMachine::AnomalyObserver obs) {
            machine_.AddAnomalyObserver(std::move(obs));
        }

        /// 换链事件观察者(= 实际换链动作的执行者; 每次 revision 变化恰好回调一次)
        using ChannelsReloadObserver = std::function<void(uint64_t rev)>;
        void AddChannelsReloadObserver(ChannelsReloadObserver obs) {
            reload_observers_.push_back(std::move(obs));
        }

    private:
        /// 仅在当前态对该事件有定义时喂入(否则 DEBUG 忽略) —— 用于"无意义的重复/过早事件"
        bool FeedRelevant(const SessionEvent& ev);

        /// 是否处于注册窗口(Handshaking/Registering)
        bool InRegistrationWindow() const;

        SessionMachine machine_;
        Backoff first_connect_backoff_;
        Backoff reconnect_backoff_;
        SessionRunnerConfig cfg_;
        bool first_connect_done_ = false;
        TimePoint confirm_deadline_{};            ///< 注册窗口内的确认截止时刻
        bool confirm_deadline_armed_ = false;
        uint64_t applied_channels_rev_ = 0;       ///< 已应用的 channels 修订号(启动=0 与初始链一致)
        std::vector<ChannelsReloadObserver> reload_observers_;   ///< 换链动作订阅者
    };

} // namespace cleat
