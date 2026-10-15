/**
 * @file SessionMachine.hpp
 * @brief 会话状态机(被动: 纯状态 + 转移表, 无 I/O)
 * @details
 * **被动状态机**(): 只接受事件、更新状态, 不做任何 I/O、不持定时器。
 * 由 `SessionRunner`驱动 —— 这样状态机可脱离 I/O 单测, I/O 引擎也才能在
 * 后续独立拆分( A)。
 *
 * 职责边界:
 *   * **接受/拒绝转移**: 非法组合一律拒绝并计数(`RejectedTransitions()`), 不静默吞掉;
 *   * **代际(epoch)**: 每次进入 `Registering` 递增; 供响应绑定与陈旧帧识别;
 *   * **归属校验**: `Confirm` 事件携带"接收时代际", 与当前代际不符即为陈旧帧 → 丢弃。
 *     这是 那条"任何带 session_id 的帧都算确认"的根治: 确认只在 `Registering`
 *     态被接受, 且代际必须匹配。
 *   * **状态变化通知**: 供 (降级动作事件化)订阅 —— 取代"每轮轮询回调"
 *     (轮询无法表达"**刚进入**降级"这一时刻语义)。
 */
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "Core/Comms/Session/SessionState.hpp"

namespace cleat {

    class SessionMachine {
    public:
        using StateObserver = std::function<void(SessionState from, SessionState to,
            const SessionEvent& ev)>;

        /**
         * @brief 意外确认观察者(服务端异常信号)
         * @param state 收到该确认时本机所处的状态(必非 Registering)
         * @param sid   确认帧携带的 session_id(服务端**新建**的那个会话)
         * @details
         * `Ready` 态收到确认 = 服务端为一个**本该复用**的帧新建了会话 —— 这是两端契约
         * 破裂的可靠信号(实测: HTTPS poll 下流数据不带 session_id, 服务端按"非注册
         * 首帧"匿名注册, 一帧一会话, 一次远程桌面开出 20+)。
         *
         * 此前这里只有一条 `LOG_WARN` —— 而 agent 是无人值守的, 本地日志没人看。
         * 故增加本回调, 由装配方接到**既有上报通道**(`EventReport`), 让 operator 在
         * 控制台上能看见"agent 正在被服务端当新会话对待"。
         */
        using AnomalyObserver = std::function<void(SessionState state, uint64_t sid)>;

        SessionMachine() = default;
        SessionMachine(const SessionMachine&) = delete;
        SessionMachine& operator=(const SessionMachine&) = delete;

        /// 当前状态
        SessionState State() const { return state_; }

        /// 当前连接代际( 对外只读, 供链路与模块查询)
        uint64_t CurrentEpoch() const { return epoch_; }

        /// 已拒绝的非法转移计数(供夹具断言与排障)
        uint64_t RejectedTransitions() const { return rejected_; }

        /// 注册状态变化观察者
        void AddStateObserver(StateObserver obs) { observers_.push_back(std::move(obs)); }

        /// 注册意外确认观察者(可多个; 见 AnomalyObserver)
        void AddAnomalyObserver(AnomalyObserver obs) { anomaly_observers_.push_back(std::move(obs)); }

        /**
         * @brief 喂入事件(唯一入口)
         * @return true = 转移被接受; false = 非法转移, 状态不变
         * @details 拒绝时记录 WARN + 计数。**不抛异常**: 状态机在 I/O 路径上被调用,
         *          异常逃逸会破坏会话循环的收敛性。
         *
         * **会话身份的唯一所有者**: `Confirm` 被接受的转移(`Registering → Ready`)里
         * 由本类**唯一一次**调用 `SessionIdentity::SetSessionId` 写入身份; 与之对称地, 被接受
         * 的 `SessionLost`(`SESSION_UNKNOWN`)里由本类**唯一一次**清除身份(sid → 0)。
         * 此前写入副作用在 `CommandProcessor::HandleClassified` 的分类分支里**先于**状态
         * 判定执行, 导致 `Ready` 态的意外确认照样污染本机身份(状态机挡住了状态迁移, 却
         * 没挡住身份覆盖) —— 后续所有上行帧被引到服务端新建的野会话上。分类层不碰状态,
         * 身份归状态机: 写入与清除都由同一段"被接受的转移"代码负责。
         */
        bool Feed(const SessionEvent& ev);

        /// 便利: 构造常用事件
        bool Feed(SessionEventKind kind, const char* why = nullptr) {
            return Feed(SessionEvent::Make(kind, why));
        }

        /// 状态轨迹(供夹具断言"状态推进"序列); **有界**: 只保留最近 kMaxTraceEntries 条
        struct Transition {
            SessionState from;
            SessionState to;
            SessionEventKind event;
            uint64_t epoch;
        };
        static constexpr size_t kMaxTraceEntries = 64;
        const std::vector<Transition>& Trace() const { return trace_; }
        void ClearTrace() { trace_.clear(); }

        /// 轨迹 → 紧凑字符串序列(如 "Idle>Opening", "Opening>Registering")
        std::vector<std::string> TraceStrings() const;

    private:
        void Notify(SessionState from, SessionState to, const SessionEvent& ev);
        void NotifyAnomaly(SessionState state, uint64_t sid);

        SessionState state_ = SessionState::Idle;
        uint64_t epoch_ = 0;
        uint64_t rejected_ = 0;
        std::vector<StateObserver> observers_;
        std::vector<AnomalyObserver> anomaly_observers_;
        std::vector<Transition> trace_;
    };

} // namespace cleat
