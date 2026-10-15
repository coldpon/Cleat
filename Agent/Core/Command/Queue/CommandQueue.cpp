/**
 * @file CommandQueue.cpp
 * @brief 命令队列实现
 * @details 提供优先级队列用于任务调度，支持普通任务和带超时护栏的任务。
 */

#include "CommandQueue.hpp"
#include "Common/Logger/Logger.hpp"

namespace cleat {

    /**
     * @brief 获取命令队列单例实例
     * @return CommandQueue& 静态单例引用
     */
    CommandQueue& CommandQueue::Instance() {
        static CommandQueue instance;
        return instance;
    }

    /**
     * @brief 投递普通任务（无超时护栏）
     * @param priority 优先级（数值越小越先执行）
     * @param task     可调用任务对象
     */
    void CommandQueue::Push(int priority, std::function<void()> task) {
        Push(priority, std::move(task), nullptr);
    }

    /**
     * @brief 投递带投递票据的任务
     * @param priority 优先级（数值越小越先执行）
     * @param task     任务可调用对象
     * @param ticket   命令投递票据（超时/拒绝回调 + 出生回填槽）
     *
     * 如果队列已停止，任务会被丢弃并记录警告日志。
     */
    bool CommandQueue::Push(int priority, std::function<void()> task, std::shared_ptr<JobTicket> ticket) {
        return Push(priority, std::move(task), std::move(ticket), ResourceKey{});
    }

    size_t CommandQueue::PendingCount() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return queue_.size();
    }

    bool CommandQueue::Push(int priority, std::function<void()> task,
        std::shared_ptr<JobTicket> ticket, ResourceKey key, uint32_t timeout_ms) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            //  stopped_ 判定必须在锁内 —— 判定在锁外时, 「Push 通过判定 → Stop 置位
            // 并清空队列 → Push 持锁入队」的交错会让任务进入**已停止**的队列: worker 已
            // 退出, 该命令永不执行也永不回响应(静默丢失), 且任务捕获的
            // CommandProcessor::this(C7)被留到队列析构。判定与入队同临界区后, 与 Stop()
            // 的清队互斥, 要么判定看到停止(拒绝), 要么入队先于清队(被 Stop 清掉, 有据可查)。
            if (stopped_.load()) {
                LOG_WARN("CommandQueue: 已停止，任务被丢弃");
                return false;
            }
            // 背压: 上限内的等待是可接受的延迟; 超过则**拒绝**(由投递方回可区分错误),
            // 既不静默丢(operator 无从得知), 也不无限排队(内存增长 + 延迟失控)。
            if (queue_.size() >= max_pending_.load()) {
                ++rejected_;
                LOG_WARN("CommandQueue: 待执行队列已满(" << queue_.size() << "/"
                    << max_pending_.load() << "), 拒绝新任务(累计 " << rejected_.load() << ")");
                return false;
            }
            //  入队序号单调递增, 保证同优先级 FIFO
            const uint64_t seq = next_seq_.fetch_add(1);
            queue_.insert({ priority, seq, std::move(task), std::move(ticket), std::move(key),
                timeout_ms });
        }
        cv_.notify_one();
        return true;
    }

    /**
     * @brief 阻塞式取出一个任务
     * @param[out] item 存放取出的任务项
     * @return 成功取出返回 true，队列被停止且无任务时返回 false
     *
     * 内部使用条件变量等待，直到队列非空或收到停止信号。
     * 取出时使用 multiset 的 extract 移动语义，避免拷贝。
     */
    bool CommandQueue::Pop(TaskItem& item) {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait(lock, [this]() { return !queue_.empty() || stopped_.load(); });
        // `stopped_ && empty` 在"stopped_.store(true) 已执行、clear 尚未拿到锁"的
        // 窗口会放行残留任务, 打破 C7"worker 只会 Pop 到空队列退出"的承诺(残留任务
        // 捕获的 processor/上下文可能正被收停)。残留任务由 Stop() 的清队兜底丢弃。
        if (stopped_.load()) {
            return false;
        }
        auto it = queue_.begin();
        // 提取节点（无需拷贝）
        auto node = queue_.extract(it);
        item = std::move(node.value());  // 真正的移动
        return true;
    }

    CommandQueue::PopStatus CommandQueue::PopFor(TaskItem& item, uint32_t timeout_ms) {
        std::unique_lock<std::mutex> lock(mutex_);
        const bool got = cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms),
            [this]() { return !queue_.empty() || stopped_.load(); });
        // 交付纪律与 Pop 逐字一致: 停止后一律不再交付
        if (!got) {
            return PopStatus::Timeout;
        }
        if (stopped_.load()) {
            return PopStatus::Stopped;
        }
        auto it = queue_.begin();
        auto node = queue_.extract(it);
        item = std::move(node.value());
        return PopStatus::Got;
    }

    /**
     * @brief 停止队列并唤醒所有等待线程
     *
     * 设置停止标志，并通知所有阻塞在 Pop 上的消费者线程。
     */
    void CommandQueue::Stop() {
        stopped_.store(true);
        {
            //  清空残留任务 —— 队列任务捕获 CommandProcessor::this
            // 等对象, 若被 Pop 执行时对象已析构则 UAF。停止时丢弃所有未执行任务,
            // 保证 worker 只会 Pop 到空队列退出。
            std::lock_guard<std::mutex> lock(mutex_);
            queue_.clear();
        }
        cv_.notify_all();
    }

    /**
     * @brief 检查队列是否已被停止
     * @return true 表示已停止，false 表示仍在运行
     */
    bool CommandQueue::IsStopped() const {
        return stopped_.load();
    }

} // namespace cleat