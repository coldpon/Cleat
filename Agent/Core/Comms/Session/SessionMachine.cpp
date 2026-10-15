/**
 * @file SessionMachine.cpp
 * @brief 会话状态机实现(被动: 无 I/O、无定时器)
 */

#include "Core/Comms/Session/SessionMachine.hpp"

#include "Common/Logger/Logger.hpp"
#include "Core/Identity/SessionIdentity.hpp"

namespace cleat {

    bool SessionMachine::Feed(const SessionEvent& ev) {
        // 归属校验( 的根治): Confirm 只在 Registering 态被接受, 且**接收时代际**
        // 必须等于当前代际 —— 否则是陈旧帧(旧链路残留/迟到确认), 丢弃并留痕。
        if (ev.kind == SessionEventKind::Confirm) {
            if (state_ != SessionState::Registering) {
                // 非注册窗口的确认 = **服务端为一个本该复用的帧新建了会话**(两端契约
                // 破裂的可靠信号; 实测 HTTPS poll 下流数据不带 session_id → 一帧一会话)。
                // 这里必须继续拒绝, 且**不得覆盖本机 session_id** —— 否则后续所有上行帧
                // 都会被引到那个野会话上(比"多出一个匿名会话"更隐蔽的二级故障)。
                LOG_WARN("SessionMachine: 丢弃确认帧(当前状态 " << ToString(state_)
                    << ", 非 Registering) —— 陈旧/意外确认不得改变会话状态");
                ++rejected_;
                NotifyAnomaly(state_, ev.session_id);   // 上报, 不只在本地留 WARN
                return false;
            }
            if (ev.epoch != epoch_) {
                LOG_WARN("SessionMachine: 丢弃陈旧代际确认帧(帧代际 " << ev.epoch
                    << " != 当前 " << epoch_ << ")");
                ++rejected_;
                return false;
            }
            if (ev.session_id == 0) {
                LOG_WARN("SessionMachine: 丢弃 session_id=0 的确认帧(非法)");
                ++rejected_;
                return false;
            }
        }

        // 失效通知的**归属校验**(与 Confirm 的代际校验同构): 只受理"就是本机当前
        // sid"的通知。清身份前已发出的请求(如 Poll/心跳)会带回**迟到**的失效通知 ——
        // 若无此校验, 它会在重注册成功、拿到新 sid 之后又把这个**新**身份清掉, 把链路
        // 拖进反复重注册(与 DENY_BUSY 的"迟到拒绝"同族, 见 SessionRunner::Tick)。
        // 回带 sid=0 表示服务端连该帧的 sid 都没解析出来(无从路由), 此时无从归属, 照常受理。
        if (ev.kind == SessionEventKind::SessionLost && ev.session_id != 0
            && ev.session_id != SessionIdentity::Instance().GetSessionId()) {
            LOG_WARN("SessionMachine: 丢弃非本机会话的失效通知(通知 sid " << ev.session_id
                << " != 本机 " << SessionIdentity::Instance().GetSessionId()
                << ") —— 属上一次会话的迟到通知, 不得清掉当前身份");
            ++rejected_;
            return false;
        }

        const int next = NextState(state_, ev);
        if (next < 0) {
            LOG_WARN("SessionMachine: 非法转移被拒 " << ToString(state_) << " --"
                << ToString(ev.kind) << "--> ?"
                << (ev.why ? std::string(" (") + ev.why + ")" : std::string()));
            ++rejected_;
            return false;
        }

        const SessionState from = state_;
        state_ = static_cast<SessionState>(next);

        // **会话身份的唯一写入点**(见头文件): 只有被接受的注册确认
        // (Registering --Confirm--> Ready)才允许改本机 session_id。
        // 此前该副作用在 CommandProcessor 的分类分支里**先于**状态判定执行, Ready 态的
        // 意外确认照样污染身份 —— 移到此处后, "拒绝转移"与"不污染身份"由同一段代码保证。
        if (ev.kind == SessionEventKind::Confirm && from == SessionState::Registering
            && state_ == SessionState::Ready) {
            SessionIdentity::Instance().SetSessionId(ev.session_id);
        }

        // **与上面对称的唯一清除点**: 身份失效只由被接受的 SessionLost 触发。
        // 写在状态机里的理由与写入点时相同 —— 分类层与业务层不得碰身份, "拒绝转移"
        // 与"不污染身份"由同一段代码保证; 今天被拒的失效通知(上一段)因此不会清身份。
        if (ev.kind == SessionEventKind::SessionLost) {
            SessionIdentity::Instance().SetSessionId(0);
            LOG_WARN("SessionMachine: 服务端不认得本机会话(通知 sid=" << ev.session_id
                << ") —— 已清空本地 session_id, 将由重连路径带 machine_id 重新注册");
        }

        // 代际递增: 每次进入 Registering = 一次新的 FirstConnect 尝试()
        if (state_ == SessionState::Registering) {
            ++epoch_;
        }

        trace_.push_back(Transition{ from, state_, ev.kind, epoch_ });
        // 轨迹有界(N27): 长驻进程每小时可能发生数次重连, 无上限会持续增长
        // (实测心算: 每 30s 一次重连 × 一个月 ≈ 百万级条目)。只保留最近若干条 ——
        // 轨迹的用途是"排障时看最近发生了什么", 不是审计全量历史。
        if (trace_.size() > kMaxTraceEntries) {
            trace_.erase(trace_.begin(), trace_.begin() +
                static_cast<ptrdiff_t>(trace_.size() - kMaxTraceEntries));
        }
        LOG_DEBUG("SessionMachine: " << ToString(from) << " --"
            << ToString(ev.kind) << "--> " << ToString(state_)
            << " (epoch=" << epoch_ << ")");
        Notify(from, state_, ev);
        return true;
    }

    void SessionMachine::Notify(SessionState from, SessionState to, const SessionEvent& ev) {
        for (auto& obs : observers_) {
            if (obs) obs(from, to, ev);
        }
    }

    void SessionMachine::NotifyAnomaly(SessionState state, uint64_t sid) {
        for (auto& obs : anomaly_observers_) {
            if (obs) obs(state, sid);
        }
    }

    std::vector<std::string> SessionMachine::TraceStrings() const {
        std::vector<std::string> out;
        out.reserve(trace_.size());
        for (const Transition& t : trace_) {
            out.push_back(std::string(ToString(t.from)) + ">" + ToString(t.to));
        }
        return out;
    }

} // namespace cleat
