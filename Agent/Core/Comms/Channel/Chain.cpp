/**
 * @file Chain.cpp
 * @brief 通道链实现
 */

#include "Core/Comms/Channel/Chain.hpp"

#include "Common/Logger/Logger.hpp"
#include "Core/Config/AgentConfig.hpp"

#include <utility>

namespace cleat {

    void Chain::AddCandidate(std::unique_ptr<IChannel> channel, TransportConfig cfg) {
        candidates_.push_back(Candidate{ std::move(channel), std::move(cfg) });
    }

    void Chain::SetCandidates(ChannelCandidates cands) {
        // 事务先于锁构造 ⇒ 观察者在**锁释放之后**才被调用(); 下同
        BandwidthTxn bw_txn(*this);
        // 锁序(硬约束): probe_mutex_ 先于 mutex_。等回探结束再 Close/替换候选 ——
        // 否则会把"正在锁外 Open 的候选"关闭/析构掉。
        std::lock_guard<std::mutex> probe_lock(probe_mutex_);
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        // 先关闭全部旧候选: 释放 socket / 会话, 防句柄与连接池悬挂
        for (auto& c : candidates_) {
            if (c.channel) c.channel->Close();
        }
        candidates_.clear();
        for (auto& p : cands) {
            candidates_.push_back(Candidate{ std::move(p.first), std::move(p.second) });
        }
        current_ = nullptr;
        attempt_index_ = 0;
        current_index_ = 0;
        fail_count_ = 0;
        connected_at_ = {};
        last_probe_at_ = {};
        NoteBandwidthChangeLocked();   //  无当前通道 -> High(与 Bandwidth() 约定一致)
        LOG_WARN("Chain: 候选链已热替换 count=" << candidates_.size()
            << " (由会话层走重连路径重建会话)");
    }

    bool Chain::Connect(const std::string& host, uint16_t port) {
        BandwidthTxn bw_txn(*this);
        // 锁序(硬约束): probe_mutex_ 先于 mutex_。Connect 也会对候选做 Open/Close,
        // 与回探(probe_mutex_ 保护段)串行化, 防同一候选对象上的并发建链。
        std::lock_guard<std::mutex> probe_lock(probe_mutex_);
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        last_host_ = host;
        last_port_ = port;
        // 清理旧通道: **必须无条件 Close** —— HTTPS 断开后 Health 已为 Dead,
        // 若按"仅当健康时关闭"的条件跳过, 其底层连接池(指向死连接的 keep-alive)
        // 不会释放, 后续回探/重连永远复用死连接而失败(实测)。
        // Close 幂等(已关闭再关无副作用), 同时释放连接池供重建。
        if (current_) {
            current_->Close();
        }
        current_ = nullptr;
        // 关旧通道 = "暂时无通道": 同步记账(否则本窗口内 `Bandwidth()` 与记账不一致 ——
        NoteBandwidthChangeLocked();

        const size_t n = candidates_.size();
        if (n == 0) {
            return false;
        }

        // 从 attempt_index_ 起依次尝试(回绕): 成功即用; 全失败回到最高优先候选。
        // 起始下标在循环入口**快照**: record_failure() 会更新 attempt_index_(供下一次
        // Connect 使用), 若在循环内实时读取, fail_threshold=1 时下标会在遍历中途被改写
        // —— 实测会反复试同一个候选而永远轮不到下一个(即使它是好的)。
        const size_t start = attempt_index_;
        for (size_t k = 0; k < n; ++k) {
            const size_t idx = (start + k) % n;
            if (try_connect_at(idx, host, port)) {
                current_index_ = idx;
                fail_count_ = 0;
                connected_at_ = std::chrono::steady_clock::now();
                last_probe_at_ = connected_at_;
                LOG_INFO("Chain: 通道就绪 type="
                    << static_cast<int>(candidates_[idx].channel->Type())
                    << " bandwidth=" << ToString(candidates_[idx].channel->Caps().bandwidth)
                    << " index=" << idx);
                NoteBandwidthChangeLocked();   //  档位可能随候选变化
                return true;
            }
            record_failure();
        }
        // 全部失败: 回到最高优先候选, 整体退避由会话层承担
        attempt_index_ = 0;
        fail_count_ = 0;
        return false;
    }

    bool Chain::try_connect_at(size_t idx, const std::string& host, uint16_t port) {
        auto& cand = candidates_[idx];
        if (!cand.channel) return false;
        // 端点注入规则(与旧实现一致):
        //  - url(HTTPS)或 host(通道规格 TCP)已自带端点 → 不注入;
        //  - 都没有(legacy TCP 候选) → 注入会话给的 host/port;
        //  - 只给了 host 未给 port → 用参数 port 兜底。
        if (cand.cfg.url.empty() && cand.cfg.host.empty()) {
            cand.cfg.host = host;
            cand.cfg.port = port;
        }
        else if (cand.cfg.url.empty() && cand.cfg.port == 0) {
            cand.cfg.port = port;
        }
        if (cand.channel->Open(cand.cfg)) {
            current_ = cand.channel.get();
            return true;
        }
        cand.channel->Close();   // 失败清理, 供下次尝试(与旧 Reset() 同义)
        return false;
    }

    void Chain::record_failure() {
        ++fail_count_;
        const uint32_t threshold =
            AgentConfig::Instance().GetTransportFailover().fail_threshold;
        if (fail_count_ >= threshold && candidates_.size() > 1) {
            attempt_index_ = (current_index_ + 1) % candidates_.size();
            fail_count_ = 0;
            LOG_WARN("Chain: 候选连续失败 " << threshold
                << " 次, 切换到 index=" << attempt_index_);
        }
    }

    bool Chain::ProbeDueLocked() const {
        // 仅降级运行时(当前索引 > 0)回探最高优先候选
        if (candidates_.size() < 2 || current_index_ == 0) {
            return false;
        }
        const TransportFailoverConfig tf =
            AgentConfig::Instance().GetTransportFailover();
        const auto now = std::chrono::steady_clock::now();
        // 防抖: 切换后 min_hold 内不回探
        if (now - connected_at_ < std::chrono::milliseconds(tf.min_hold_ms)) {
            return false;
        }
        if (last_probe_at_.time_since_epoch().count() != 0 &&
            now - last_probe_at_ < std::chrono::milliseconds(tf.probe_interval_ms)) {
            return false;
        }
        return true;
    }

    void Chain::ProbeBetter() {
        // 锁序(硬约束): probe_mutex_ 先于 mutex_ —— SetCandidates/Connect/Disconnect
        // 同序。回探的 Open 在 mutex_ 外执行, 但全程持 probe_mutex_ 挡住候选对象被
        // 替换/关闭(实测: 旧实现持 mutex_ 做 Open, 降级期每次回探让全部
        // 出站停摆数秒; 直接挪到锁外而不加本锁, 会引入"热切换 Close 正在 Open 的候选"竞争)。
        std::lock_guard<std::mutex> probe_lock(probe_mutex_);

        constexpr size_t idx = 0;   // 只探最高优先候选
        IChannel* cand_channel = nullptr;
        TransportConfig cand_cfg;

        {
            std::lock_guard<std::recursive_mutex> lock(mutex_);
            // 双重检查: 并发的第二个检测者(夹具/未来多线程)可能已探过并刷新了
            // last_probe_at_; 或期间已回切/断链(Disconnect/Connect 可与本函数
            // 交错于 probe_mutex_ 获取点)。
            if (!ProbeDueLocked()) {
                return;
            }
            if (!current_ || current_->HealthNow() == ChannelHealth::Dead) {
                return;
            }
            // 记录探测时刻(与旧 maybe_probe_better 一致: 无论成败, 下次探测按间隔递推)
            last_probe_at_ = std::chrono::steady_clock::now();

            auto& cand = candidates_[idx];
            if (!cand.channel) {
                return;
            }
            // 端点注入规则(原 try_connect_at 的 :98-104): 副作用写入 cfg, 锁内完成
            if (cand.cfg.url.empty() && cand.cfg.host.empty()) {
                cand.cfg.host = last_host_;
                cand.cfg.port = last_port_;
            }
            else if (cand.cfg.url.empty() && cand.cfg.port == 0) {
                cand.cfg.port = last_port_;
            }
            cand_channel = cand.channel.get();
            cand_cfg = cand.cfg;   // 拷贝: 锁外的 Open 引用它(对象在 probe_mutex_ 保护下)
        }

        LOG_INFO("Chain: 回探更高优先候选 index=0(锁外 Open)");
        if (!cand_channel->Open(cand_cfg)) {
            cand_channel->Close();   // 失败清理, 供下次尝试(与旧 Reset() 同义)
            LOG_DEBUG("Chain: 回探失败, 维持当前通道");
            return;
        }

        std::lock_guard<std::recursive_mutex> lock(mutex_);
        // 候选在探测期间受 probe_mutex_ 保护, 此处指针复验是廉价保险(防夹具/未来
        // 多线程误用): 候选已被替换/清空 → 作废本次新连接, 不切换。
        if (idx >= candidates_.size()
            || candidates_[idx].channel.get() != cand_channel) {
            cand_channel->Close();
            LOG_DEBUG("Chain: 回探期间候选被替换, 作废本次回切");
            return;
        }
        current_ = cand_channel;
        // 再切索引; 该顺序现在只存在于 SwapTo 一处。
        SwapTo(idx);
        LOG_INFO("Chain: 回探成功, 回切主通道 index=0");
    }

    void Chain::SwapTo(size_t idx) {
        // 调用方(try_connect_at 成功路径 / ProbeBetter 提交)已把 current_ 指向新通道;
        const size_t old_idx = current_index_;
        if (old_idx < candidates_.size() && old_idx != idx &&
            candidates_[old_idx].channel) {
            candidates_[old_idx].channel->Close();   // 作废其链路级状态并释放 socket
        }
        current_index_ = idx;
        attempt_index_ = 0;       // 复位游标: 否则后续重连仍从降级候选起步( 之二)
        fail_count_ = 0;
        connected_at_ = std::chrono::steady_clock::now();
        NoteBandwidthChangeLocked();   //  换通道可能改变带宽档(锁内只记账, 最外层事务派发)
    }

    void Chain::Disconnect() {
        BandwidthTxn bw_txn(*this);
        // 锁序(硬约束): probe_mutex_ 先于 mutex_。等回探(含其提交)结束再作废 ——
        // 否则回探可能在 Disconnect 之后把新连接提交为 current_(静默窗口/停止期
        // 出现一条"计划外恢复"的活连接; 实测)。
        std::lock_guard<std::mutex> probe_lock(probe_mutex_);
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        if (current_) {
            current_->Close();
            current_ = nullptr;
            NoteBandwidthChangeLocked();   //  断链 -> 档位回到 High(无通道)
        }
    }

    bool Chain::IsConnected() const {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        return current_ ? current_->HealthNow() != ChannelHealth::Dead : false;
    }

    ChannelHealth Chain::HealthNow() const {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        return current_ ? current_->HealthNow() : ChannelHealth::Dead;
    }

    SendResult Chain::Send(const std::vector<uint8_t>& frame) {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        if (!current_ || current_->HealthNow() == ChannelHealth::Dead) {
            // 无可用通道: 本轮发不出去 —— 不改候选、不计失败(与旧实现一致)
            return SendResult::Rejected;
        }
        const SendResult r = current_->Write(frame);
        // 硬约束: **只有** ChannelFailure 计失败并可能换候选
        if (r == SendResult::ChannelFailure) {
            record_failure();
        }
        return r;
    }

    bool Chain::Recv(std::vector<uint8_t>& frame, int /*timeout_ms*/) {
        BandwidthTxn bw_txn(*this);   // 回探可能换通道 -> 档位变化需在锁外派发

        // 阶段1(锁内): 快照判定"是否该回探"。真正的回探在锁外执行(ProbeBetter),
        // 期间出站(Send 取 mutex_)不再被回探的建链 I/O 阻塞。
        bool probe_due = false;
        {
            std::lock_guard<std::recursive_mutex> lock(mutex_);
            if (!current_ || current_->HealthNow() == ChannelHealth::Dead) {
                return false;
            }
            probe_due = ProbeDueLocked();
        }
        // 降级时周期回探更高优先候选 —— 本函数是唯一收包入口(会话循环与夹具都经此),
        // 故回探在生产中必然被执行
        if (probe_due) {
            ProbeBetter();
        }

        std::lock_guard<std::recursive_mutex> lock(mutex_);
        if (!current_ || current_->HealthNow() == ChannelHealth::Dead) {
            return false;
        }
        const bool ok = current_->Pump(frame);
        if (!ok && current_->HealthNow() == ChannelHealth::Dead) {
            record_failure();
        }
        return ok;
    }


    BandwidthClass Chain::Bandwidth() const {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        if (!current_) return BandwidthClass::High;   // 无通道时与旧实现一致(非低带宽)
        return current_->Caps().bandwidth;
    }

    void Chain::NoteBandwidthChangeLocked() {
        const BandwidthClass now = current_ ? current_->Caps().bandwidth : BandwidthClass::High;
        if (now == last_notified_bandwidth_) return;
        const BandwidthClass from = last_notified_bandwidth_;
        last_notified_bandwidth_ = now;
        LOG_INFO("Chain: 带宽档 " << ToString(from) << " -> " << ToString(now));
        // 只写槽位, 不回调: 观察者会 join 高带宽工作线程, 而它们要取本对象的锁()。
        // 同一帧内多次变化时**覆盖**为末次 —— 动作只关心最终档位, 且 from 取自被即时更新的
        // `last_notified_bandwidth_`, 故覆盖后仍是"上次已派发档位 -> 当前档位"的诚实描述。
        pending_bw_.valid = true;
        pending_bw_.from = from;
        pending_bw_.to = now;
    }

    void Chain::DispatchBandwidth() {
        BandwidthChange ch;
        BandwidthObserver obs;
        {
            std::lock_guard<std::recursive_mutex> lock(mutex_);
            if (!pending_bw_.valid) return;
            ch = pending_bw_;
            pending_bw_ = BandwidthChange{};   // 取走即清空 -> 至多派发一次
            obs = bandwidth_obs_;              // 拷贝后锁外调用(观察者可能长时间阻塞)
        }
        if (obs) obs(ch.from, ch.to);
    }

    IChannel* Chain::Current() {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        return current_;
    }

    uint32_t Chain::NextPollDueInMs() const {
        // 与 Bandwidth() 同款: 持锁取值并调用 —— current_ 生命周期由候选链持有,
        // 热替换在同一把锁下换指针, 锁内调用即既有房规。
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        return current_ ? current_->NextPollDueInMs() : 0;
    }

    uint32_t Chain::PendingUplinkBytes() const {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        return current_ ? current_->PendingUplinkBytes() : 0;
    }

    size_t Chain::CandidateCount() const {
        // 必须加锁: candidates_ 会被 SetCandidates(热切换线程)整体替换,
        // 而该 getter 常从日志参数中被调用 —— 无锁读与写构成数据竞争。
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        return candidates_.size();
    }

    size_t Chain::CurrentIndex() const {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        return current_index_;
    }

    size_t Chain::AttemptIndex() const {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        return attempt_index_;
    }

    size_t Chain::FailCount() const {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        return fail_count_;
    }

} // namespace cleat
