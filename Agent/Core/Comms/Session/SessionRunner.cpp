/**
 * @file SessionRunner.cpp
 * @brief 会话运行器实现
 */

#include "Core/Comms/Session/SessionRunner.hpp"

#include "Common/Logger/Logger.hpp"
#include "Core/Comms/Channel/IChannel.hpp"

#include <utility>

namespace cleat {

    SessionRunner::SessionRunner() : SessionRunner(SessionRunnerConfig{}) {}

    SessionRunner::SessionRunner(const SessionRunnerConfig& cfg)
        : first_connect_backoff_(cfg.first_connect_base_ms, cfg.first_connect_max_ms,
            cfg.jitter_permille, cfg.rng_seed),
        reconnect_backoff_(cfg.reconnect_base_ms, cfg.reconnect_max_ms,
            cfg.jitter_permille, cfg.rng_seed),
        cfg_(cfg) {
        LOG_INFO("SessionRunner: 确认超时 " << cfg_.confirm_timeout_ms
            << "ms, 首连退避 " << cfg_.first_connect_base_ms << "-"
            << cfg_.first_connect_max_ms << "ms, 重连退避 " << cfg_.reconnect_base_ms
            << "-" << cfg_.reconnect_max_ms << "ms, 抖动 +-" << cfg_.jitter_permille << "permille");
    }

    void SessionRunner::Configure(const SessionRunnerConfig& cfg) {
        cfg_ = cfg;
        // 退避策略重建(相位归零) —— 与合一前"进入首连/重连函数时按配置构造"一致
        first_connect_backoff_ = Backoff(cfg.first_connect_base_ms, cfg.first_connect_max_ms,
            cfg.jitter_permille, cfg.rng_seed);
        reconnect_backoff_ = Backoff(cfg.reconnect_base_ms, cfg.reconnect_max_ms,
            cfg.jitter_permille, cfg.rng_seed);
        LOG_INFO("SessionRunner: 配置更新(确认超时 " << cfg_.confirm_timeout_ms
            << "ms, 重连退避 " << cfg_.reconnect_base_ms << "-" << cfg_.reconnect_max_ms
            << "ms, 抖动 +-" << cfg_.jitter_permille << "permille)");
    }

    bool SessionRunner::FeedRelevant(const SessionEvent& ev) {
        // "无定义"与"非法"分开: 前者是该事件在此态**根本没有语义**(如 Idle 态收到
        // 换链事件 —— 此时并无旧链可失效), 属正常时序, 只留 DEBUG; 后者是真实病态,
        // 由 SessionMachine 自己 WARN + 计数。
        if (!IsTransitionLegal(machine_.State(), ev)) {
            LOG_DEBUG("SessionRunner: 事件 " << ToString(ev.kind) << " 在 "
                << ToString(machine_.State()) << " 态无定义, 忽略"
                << (ev.why ? std::string(" (") + ev.why + ")" : std::string()));
            return false;
        }
        return machine_.Feed(ev);
    }

    bool SessionRunner::InRegistrationWindow() const {
        const SessionState s = machine_.State();
        return s == SessionState::Handshaking || s == SessionState::Registering;
    }

    bool SessionRunner::AwaitingConfirm() const {
        return InRegistrationWindow();
    }

    void SessionRunner::Start() {
        if (machine_.State() != SessionState::Idle) {
            LOG_DEBUG("SessionRunner: Start 在 " << ToString(machine_.State()) << " 态被忽略");
            return;
        }
        machine_.Feed(SessionEvent::Make(SessionEventKind::OpenRequested, "start"));
    }

    void SessionRunner::BackoffElapsed() {
        FeedRelevant(SessionEvent::Make(SessionEventKind::BackoffElapsed, "backoff elapsed"));
    }

    void SessionRunner::Stop() {
        // Stop 对任意状态合法且幂等(转移表内的唯一全局事件)
        if (machine_.State() == SessionState::Stopping) return;
        machine_.Feed(SessionEvent::Make(SessionEventKind::Stop, "stop"));
        confirm_deadline_armed_ = false;
    }

    void SessionRunner::OnChannelResult(bool ok, bool handshake_required, TimePoint now) {
        SessionEvent ev;
        if (ok) {
            ev.kind = SessionEventKind::ChannelUp;
            ev.handshake_required = handshake_required;
            ev.why = handshake_required ? "channel up, handshake required"
                : "channel up, no handshake";
        }
        else {
            ev.kind = SessionEventKind::OpenFailed;
            ev.why = "connect failed";
        }
        if (!FeedRelevant(ev)) return;

        if (ok) {
            // 确认超时窗口起点 = 通道就绪时刻(等价于合一前的 ArmSessionConfirm():
            // 它在 Connect 成功后立即记录 connect_start_, 之后才发 KE 与注册帧)
            confirm_deadline_ = now + std::chrono::milliseconds(cfg_.confirm_timeout_ms);
            confirm_deadline_armed_ = true;
            LOG_INFO("SessionRunner: 通道就绪(" << (handshake_required ? "需握手" : "免握手")
                << "), 会话确认超时 " << cfg_.confirm_timeout_ms << "ms 起算");
        }
        else {
            confirm_deadline_armed_ = false;
        }
    }

    void SessionRunner::OnHandshakeSent() {
        FeedRelevant(SessionEvent::Make(SessionEventKind::HandshakeSent, "handshake sent"));
    }

    void SessionRunner::OnChannelDown() {
        confirm_deadline_armed_ = false;
        FeedRelevant(SessionEvent::Make(SessionEventKind::ChannelDown, "channel down"));
    }

    void SessionRunner::OnChannelsReconfigured() {
        FeedRelevant(SessionEvent::Make(SessionEventKind::ChannelsReconfigured,
            "channels reloaded"));
    }

    bool SessionRunner::NoteChannelsRevision(uint64_t rev) {
        if (rev == applied_channels_rev_) return false;   // 去重: 同一修订号只派发一次
        applied_channels_rev_ = rev;
        LOG_WARN("SessionRunner: 通道链 revision=" << rev << " 变化, 派发换链事件(会话状态 "
            << ToString(machine_.State()) << ")");

        // 动作订阅者先于状态事件执行: 换链必须**真的发生** —— 与当前会话状态无关
        // (Idle/Opening 下没有旧链可失效, 但链本身仍需重建)。
        for (auto& obs : reload_observers_) {
            if (obs) obs(rev);
        }

        const bool moved = FeedRelevant(SessionEvent::Make(SessionEventKind::ChannelsReconfigured,
            "channels revision changed"));
        if (moved && machine_.State() == SessionState::Reconnecting) {
            // 配置驱动而非链路失败: 复位退避并立即回到 Opening(等价旧实现"换链后当轮即连")
            reconnect_backoff_.Reset();
            FeedRelevant(SessionEvent::Make(SessionEventKind::BackoffElapsed,
                "reload immediate"));
        }
        return true;
    }

    uint32_t SessionRunner::NextBackoffMs() {
        // 首连完成后改用重连退避 —— 与合一前的两处调用点分工一致
        return first_connect_done_ ? reconnect_backoff_.NextDelayMs()
            : first_connect_backoff_.NextDelayMs();
    }

    void SessionRunner::NoteHealthy() {
        reconnect_backoff_.Reset();
    }

    SessionRunner::TickResult SessionRunner::Tick(const RecvFn& recv, TimePoint now) {
        TickResult out;

        // ---- (1) 时间推进: 注册窗口内的确认超时(先于收包, 与 WaitSessionConfirm 同序) ----
        if (confirm_deadline_armed_ && InRegistrationWindow()) {
            if (now >= confirm_deadline_) {
                confirm_deadline_armed_ = false;
                LOG_WARN("SessionRunner: 会话确认超时(>" << cfg_.confirm_timeout_ms
                    << "ms 未收到 FirstConnect 确认) —— 判为假成功, 断开重连");
                // 复用 ChannelDown 而非新增事件: 语义同为"当前链路不可用 → 重连",
                // 且冻结的转移表不必为一种失败原因扩容; 原因由 why 与日志承载。
                if (FeedRelevant(SessionEvent::Make(SessionEventKind::ChannelDown,
                    "confirm timeout"))) {
                    out.confirm_timeout = true;
                }
            }
        }

        // ---- (2) 接收时代际快照(一次 Pump 批次共用一个, 见头文件) ----
        const uint64_t recv_epoch = machine_.CurrentEpoch();
        out.recv_epoch = recv_epoch;

        // ---- (3) 收帧 → Classify → Feed(收帧函数由驱动方注入, 收包入口只有一个) ----
        if (recv) {
            std::vector<uint8_t> raw;
            while (recv(raw)) {
                ClassifiedFrame f = ClassifyFrame(raw);

                if (f.kind == FrameKind::SessionConfirm) {
                    // 直接喂入(不经 FeedRelevant): 代际/状态不符正是要**响亮拒绝**的病态
                    SessionEvent ev = SessionEvent::Make(SessionEventKind::Confirm, "confirm");
                    ev.session_id = f.session_id;
                    ev.epoch = recv_epoch;   //  归属标记取接收时代际
                    if (machine_.Feed(ev)) {
                        out.ready_entered = true;
                        first_connect_done_ = true;
                        confirm_deadline_armed_ = false;
                    }
                }
                else if (f.kind == FrameKind::Denied) {
                    // 与 Confirm 对称的约束: 拒绝只对**当前注册尝试**有效。Ready 态收到的
                    // DENY_BUSY 属于上一次尝试(其失败早已由超时/退避路径处理) —— 若在此
                    // 生效会打断正常会话并触发重连风暴, 故仅留痕。
                    if (InRegistrationWindow()) {
                        if (FeedRelevant(SessionEvent::Make(SessionEventKind::Denied,
                            "DENY_BUSY"))) {
                            out.denied = true;
                            confirm_deadline_armed_ = false;
                        }
                    }
                    else {
                        LOG_WARN("SessionRunner: 会话已就绪却收到 DENY_BUSY —— 属上一次注册"
                            "尝试的迟到拒绝, 不改变会话状态(不触发重连)");
                    }
                }

                else if (f.kind == FrameKind::SessionUnknown) {
                    // **与 Denied 不同, 本分支必须在 `Ready` 态也生效** —— 它到达的主场
                    // 恰恰是 Ready: Agent 以为自己已注册、照常发 Poll/心跳, 而服务端重启
                    // 后连持久记录也没了(`store.CleanupExpired` 清掉 / 库被擦) -> 回
                    // SESSION_UNKNOWN。若不处理(分类层旧行为是归入 Ignored 静默丢弃), Agent
                    // 会永远带着一个服务端不认的 sid 空转 —— **永久失联且无任何报错**。
                    //
                    // 归属校验在 `SessionMachine::Feed` 内(只受理通知 sid == 本机 sid);
                    // 此处不另设"仅注册窗口内"的门 —— 那道门正是 Denied 为防迟到拒绝
                    // 打断正常会话而设, 用途完全不同。
                    SessionEvent ev = SessionEvent::Make(SessionEventKind::SessionLost,
                        "SESSION_UNKNOWN");
                    ev.session_id = f.session_id;
                    if (FeedRelevant(ev)) {
                        out.session_lost = true;
                        confirm_deadline_armed_ = false;
                    }
                }

                // 活动判定口径: "会派发命令"的帧(Command, 或捎带命令的 Poll 响应)
                if (!f.cmd.empty()) ++out.command_frames;

                out.frames.push_back(std::move(f));
                raw.clear();
            }
        }

        return out;
    }

} // namespace cleat
