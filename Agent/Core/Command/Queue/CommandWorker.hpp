// CommandWorker.hpp — 单线程顺序消费 CommandQueue，带超时护栏和并发槽位控制。
// 本类线程默认无 COM 公寓：需要 COM 的 handler 组件按调用点自管
// （Platform::ScopedComApartment，见 Common/Platform/ComApartment.hpp）。

#pragma once
#include <thread>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <memory>
#include <cstdint>

#include "Core/Command/Queue/CommandQueue.hpp"
#include "Core/Command/Queue/ExecutionTracker.hpp"

namespace cleat {

    // 命令执行线程（单例）：管理一个工作线程和一个回收线程，顺序消费命令队列。
    class CommandWorker {
    public:
        static CommandWorker& Instance();

        // 构造并绑定到指定队列（注入用）
        explicit CommandWorker(CommandQueue& queue) : queue_(queue) {}

        ~CommandWorker();

        // 设置任务执行超时（毫秒），0 表示不限时
        void SetTaskTimeoutMs(uint32_t ms);
        uint32_t GetTaskTimeoutMs() const;

        // 启动工作线程与回收线程
        void Start();

        /// 只读: 工作线程是否已启动(装配根就绪断言用; Start 的完成凭证)
        bool IsRunning() const { return running_.load(); }

        // 停止两个线程（阻塞等待退出）
        //  置位任务线程收尾开关 → join worker/reaper → 排水等待在飞任务线程
        // 归零(上限 2s)。返回后本对象成员即可安全销毁 —— 在飞线程已被开关封锁在外。
        void Stop();

        // 同时在飞的命令条数上限（默认 1 = 旧顺序语义；不是资源互斥，后者由 ResourceKey 判定）
        void SetMaxConcurrency(uint32_t n) {
            max_concurrency_.store(n == 0 ? 1 : n);
            slot_cv_.notify_all();   // 放宽上限时立刻唤醒等待名额的消费者
        }
        uint32_t MaxConcurrency() const { return max_concurrency_.load(); }

        // 观测（排障与验收）
        size_t ActiveCount() const { return tracker_->ActiveCount(); }
        size_t StrayCount() const { return tracker_->Stray(); }
        size_t InFlightCount() const { return tracker_->InFlight(); }
        uint64_t TimedOutCount() const { return tracker_->TimedOutCount(); }
        uint64_t ForcedReleaseCount() const { return tracker_->ForcedReleaseCount(); }

    private:
        CommandWorker(const CommandWorker&) = delete;
        CommandWorker& operator=(const CommandWorker&) = delete;

        // 消费者主循环：Pop → 起执行线程 → 不等待
        void WorkerMainLoop();

        // 回收线程主循环：周期 Reap，触发超时上报与强制释放
        void ReaperMainLoop();

        // 起执行线程执行带护栏的任务，立即返回
        void LaunchGuarded(const TaskItem& item);

        // 名额准入：cap 未满返回 true；被停止返回 false
        bool WaitForSlot();

        // 唤醒查询名额者
        void NotifySlotFreed();

        // 落实 reaper 产出的动作（超时上报 / 记录强制释放）
        void ReapTick();

        CommandQueue& queue_;
        std::unique_ptr<std::thread> workerThread_;
        std::unique_ptr<std::thread> reaperThread_;
        std::atomic<bool> running_{ false };
        std::atomic<uint32_t> task_timeout_ms_{ 60000 }; // 默认 60s
        std::atomic<uint32_t> max_concurrency_{ 1 };

        // 执行槽位追踪（超时/遗弃/资源持有）—— 命令类的**内核**:
        // 槽位策略随执行引擎实例隔离(夹具按用例构造多实例, 状态必须不串)。
        //  任务系统 Phase 1: 原 `guards_` 映射删除 —— 超时回调/登记信息
        // 收编进 JobRuntime(JobRecord 持回调, correlation 定位), 本类不再持有第二份记账。
        std::unique_ptr<ExecutionTracker> tracker_ = std::make_unique<ExecutionTracker>();

        // 名额释放通知
        std::mutex slot_mutex_;
        std::condition_variable slot_cv_;

        // Stop 串行化: 并发 Stop/析构时防双 join UB
        std::mutex stop_mutex_;
    };

} // namespace cleat
