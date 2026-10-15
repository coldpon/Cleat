/**
 * @file ExecutionTracker.cpp
 * @brief 执行槽位追踪与回收决策实现
 */

#include "Core/Command/Queue/ExecutionTracker.hpp"

#include "Common/Logger/ErrLog.hpp"
#include "Common/Logger/Logger.hpp"

namespace cleat {

    uint32_t MaxHoldMs(ResourceKind kind) {
        // 用户  裁定(资源清单); 0 = 不设限
        switch (kind) {
        case ResourceKind::File: return 10u * 60 * 1000;         // 10 min
        case ResourceKind::Registry: return 5u * 60 * 1000;      // 5 min
        case ResourceKind::Clipboard: return 2u * 60 * 1000;     // 2 min
        case ResourceKind::Pivot: return 30u * 60 * 1000;        // 正常释放走 Close; 30 min 为兜底上限
        case ResourceKind::Process: return 5u * 60 * 1000;       // 注入类 5 min
        case ResourceKind::Module: return 10u * 60 * 1000;       // 加载 10 min
        case ResourceKind::Capture: return 30u * 1000;           // 单次采集 30 s(流式走 Stop)
        case ResourceKind::InputHook: return 0;                  // 显式 Stop 才是正常释放路径
        case ResourceKind::FileMonitor: return 0;                // 同上
        case ResourceKind::Terminal: return 0;                   // 同上
        case ResourceKind::None: return 0;
        }
        return 0;
    }

    uint64_t ExecutionTracker::Begin(const ResourceKey& key, uint32_t timeout_ms, TimePoint now) {
        std::lock_guard<std::mutex> lock(mutex_);
        const uint64_t id = next_id_++;
        ExecutionSlot s;
        s.task_id = id;
        s.key = key;
        s.timeout_ms = timeout_ms;
        s.started_at = now;
        // timeout_ms == 0 = 不限时(与旧语义一致): 用一个"永不到达"的 deadline 表达,
        // 而不是另设分支 —— 分支越少, "什么情况下会超时"就越容易一眼看全。
        s.deadline = (timeout_ms == 0)
            ? TimePoint::max()
            : now + std::chrono::milliseconds(timeout_ms);
        slots_.emplace(id, std::move(s));
        return id;
    }

    bool ExecutionTracker::MarkFinished(uint64_t task_id) {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = slots_.find(task_id);
        if (it == slots_.end()) return false;
        it->second.finished = true;
        return true;
    }

    void ExecutionTracker::EraseSlot(uint64_t task_id) {
        slots_.erase(task_id);   // 调用方已持锁(Reap 专用)
    }

    std::vector<ReapAction> ExecutionTracker::Reap(TimePoint now) {
        // 全程持锁(): 与 Begin 的 rehash、MarkFinished 的 find 并发时迭代器必须有效。
        // 动作收集进返回值, 锁随返回释放 —— 调用方落实动作(回调/发帧)时不持本锁。
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<ReapAction> actions;

        // ① 超时上报: **恰好一次**(否则同一 request_id 会收到多个超时响应)
        for (auto& kv : slots_) {
            ExecutionSlot& s = kv.second;
            if (s.timed_out || s.finished || s.timeout_ms == 0) continue;
            if (now < s.deadline) continue;
            s.timed_out = true;
            ++timed_out_count_;
            ReapAction a;
            a.kind = ReapActionKind::FireTimeout;
            a.task_id = s.task_id;
            a.held_ms = static_cast<uint32_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(now - s.started_at).count());
            actions.push_back(a);
        }

        // ② 释放**: 只有线程真正结束**才释放 key(被遗弃的线程仍在改资源, 提前释放会让两个
        //    线程同时改同一资源 —— 比不互斥更糟), 并在同一轮里收尾该槽位
        std::vector<uint64_t> done;
        for (auto& kv : slots_) {
            ExecutionSlot& s = kv.second;
            if (!s.finished) continue;
            if (s.key.Participates() && !s.key_released) {
                s.key_released = true;
                ReapAction a;
                a.kind = ReapActionKind::ReleaseKey;
                a.task_id = s.task_id;
                a.key = s.key;
                a.held_ms = static_cast<uint32_t>(
                    std::chrono::duration_cast<std::chrono::milliseconds>(now - s.started_at).count());
                actions.push_back(a);
            }
            done.push_back(s.task_id);
        }
        for (uint64_t id : done) EraseSlot(id);

        // ③ 最长持有 + 强制释放: 到期后**必须放行**(否则一条永久卡死的命令会让该资源永久不可用),
        //    并留下可观测的痕迹(计数 + 告警), 让 operator 知道"这里出过事"。
        for (auto& kv : slots_) {
            ExecutionSlot& s = kv.second;
            if (!s.key.Participates() || s.key_released) continue;
            const uint32_t max_hold = MaxHoldMs(s.key.kind);
            if (max_hold == 0) continue;                       // 该类不设上限(显式 Stop 才是出口)
            const auto held = std::chrono::duration_cast<std::chrono::milliseconds>(
                now - s.started_at).count();
            if (held < static_cast<int64_t>(max_hold)) continue;
            s.key_released = true;
            ++forced_release_count_;
            CLEAT_LOGE(ErrDom::Cmd, ErrCode::ResourceExhausted, "ExecutionTracker: 资源 " << s.key.Describe() << " 超过最长持有 "
                << max_hold << "ms(已 " << held << "ms), 强制放行 task=" << s.task_id
                << " —— 该资源在此之前对新命令不可用, 请排查是否存在永久卡死的执行线程");
            ReapAction a;
            a.kind = ReapActionKind::ForcedReleaseKey;
            a.task_id = s.task_id;
            a.key = s.key;
            a.held_ms = static_cast<uint32_t>(held);
            actions.push_back(a);
        }

        return actions;
    }

    bool ExecutionTracker::TryAdmit(const ResourceKey& key) const {
        if (!key.Participates()) return true;      // 不参与互斥
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& kv : slots_) {
            const ExecutionSlot& s = kv.second;
            if (!s.key.Participates() || s.key_released) continue;
            if (s.key == key) return false;        // 已被占用(含被遗弃但仍在跑的)
        }
        return true;
    }

    bool ExecutionTracker::Holds(const ResourceKey& key) const {
        return !TryAdmit(key);
    }

    size_t ExecutionTracker::InFlight() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return slots_.size();
    }

    size_t ExecutionTracker::ActiveCount() const {
        std::lock_guard<std::mutex> lock(mutex_);
        size_t n = 0;
        for (const auto& kv : slots_) {
            // 不占名额的两种情形:
            //   ① 已上报超时 —— 旧实现 detach 后立刻取下一条, 名额在超时那一刻就释放;
            //   ② 已完成但尚未被 Reap 收尾 —— 槽位保留只为释放 key 与统计, 若还算名额,
            //      每条命令之间都要白等一个 reaper 节拍。
            if (kv.second.timed_out || kv.second.finished) continue;
            ++n;
        }
        return n;
    }

    size_t ExecutionTracker::Stray() const {
        std::lock_guard<std::mutex> lock(mutex_);
        size_t n = 0;
        for (const auto& kv : slots_) {
            // 已上报超时但线程仍未结束 = 被遗弃仍在跑(= 旧实现的 stray_threads_)
            if (kv.second.timed_out && !kv.second.finished) ++n;
        }
        return n;
    }

    uint64_t ExecutionTracker::TimedOutCount() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return timed_out_count_;
    }

    uint64_t ExecutionTracker::ForcedReleaseCount() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return forced_release_count_;
    }

} // namespace cleat
