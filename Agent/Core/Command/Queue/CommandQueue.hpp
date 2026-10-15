/**
 * @file CommandQueue.hpp
 * @brief 命令队列头文件
 * @details 定义基于 std::multiset 的优先级任务队列，支持普通任务和带超时护栏的任务。
 */

#pragma once
#include <set>
#include <mutex>
#include <condition_variable>
#include <functional>
#include <vector>
#include <atomic>
#include <memory>

#include "Core/Command/Queue/CommandTaskGuard.hpp"
#include "Core/Command/Handlers/ResourceKey.hpp"

namespace cleat {

    /**
     * @brief 队列任务项结构
     *
     * ticket 为空表示普通任务（内部投递，不参与超时护栏与 job 登记）。
     */
    struct TaskItem {
        int priority{};                           ///< 优先级（数值越小越先执行）
        uint64_t seq{};                           ///< 单调递增入队序号( 保证等价优先级 FIFO)
        std::function<void()> func;             ///< 任务可调用对象
        /// 命令投递票据（可选; 超时/拒绝回调 + 出生回填槽, 见 CommandTaskGuard.hpp）。
        ///  任务系统 Phase 1: 取代旧 TaskGuard —— 超时状态副本(expired)删除,
        /// 迟到响应判据改查 JobRecord.state。
        std::shared_ptr<JobTicket> ticket;
        ///  该命令独占的资源(由投递方取自 handler 的 `ResourceOf()`);
        /// 默认空 = 不参与互斥, 与今天无互斥的行为一致
        ResourceKey key;
        /// 本任务超时覆盖: >0 时替代全局 worker 超时
        /// (下限 1s, 由投递方钳制); 0 = 用全局 `task_timeout_ms_`。
        uint32_t timeout_ms{ 0 };
    };

    /**
     * @brief 命令队列类（单例）
     *
     * 基于 std::multiset 实现优先级队列，支持线程安全的任务投递和取出。
     * 使用条件变量实现消费者阻塞等待。
     */
    class CommandQueue {
    public:
        /// 单例实例(生产路径); 测试/链路批可自行构造注入
        static CommandQueue& Instance();

        /**
         * @brief 构造函数(公开以便注入)
         * @details (): 由"仅单例"改为"可构造注入", 使 SessionHarness 能拥有
         *          独立的队列实例 —— 单例的 `Stop()` 不可逆(stopped_ 无复位路径),
         *          跨用例复用会造成"任务入队但无人消费"的静默丢失。
         */
        CommandQueue() = default;
        ~CommandQueue() = default;

        /**
         * @brief 投递普通任务（无超时护栏）
         * @param priority 优先级（数值越小越先执行）
         * @param task     可调用任务对象
         */
        void Push(int priority, std::function<void()> task);

        /**
         * @brief 投递带投递票据的任务（票据承载超时/拒绝回调与出生回填槽）
         * @param priority 优先级（数值越小越先执行）
         * @param task     任务可调用对象
         * @param ticket   命令投递票据（见 CommandTaskGuard.hpp）
         */
        bool Push(int priority, std::function<void()> task, std::shared_ptr<JobTicket> ticket);

        /**
         * @brief 投递带票据 + 资源声明的任务
         * @param key 该命令独占的资源(空 = 不参与互斥)
         * @param timeout_ms 本任务超时覆盖(0 = 用全局 worker 超时; BofExec 等慢命令用)
         * @details 与上一个重载分开是为了**不改既有调用点** —— 资源声明是增量能力,
         *          没声明的命令行为与今天完全一致。timeout_ms 走默认参数同理。
         */
        bool Push(int priority, std::function<void()> task, std::shared_ptr<JobTicket> ticket,
            ResourceKey key, uint32_t timeout_ms = 0);

        /**
         * @brief 待执行队列上限
         * @details 出站侧早有上限(`OutboundQueue`), 而**执行侧此前完全无界** —— 命令到达快于
         *          执行时(operator 脚本连发 / 服务端异常刷命令)队列会无限增长, 表现为内存增长
         *          与"命令延迟越来越长"且无人知晓。超限时 `Push` 返回 false, 由投递方回一个
         *          **可区分**的错误(`QUEUE_FULL`)—— 不静默丢, 也不无限排队。
         */
        void SetMaxPending(size_t max_pending) { max_pending_.store(max_pending == 0 ? 1 : max_pending); }
        size_t MaxPending() const { return max_pending_.load(); }
        /// 当前待执行条数(背压观测)
        size_t PendingCount() const;

        /**
         * @brief 阻塞取出一个任务
         * @param[out] item 存放取出的任务项
         * @return 成功取出返回 true；false 的唯一语义 = **队列已停止**
         *         
         */
        bool Pop(TaskItem& item);

        /// Pop 的三态结果(有界等待变体专用): Timeout ≠ 停止, 队列仍然活着
        enum class PopStatus { Got, Timeout, Stopped };

        /**
         * @brief 有界等待取出(睡眠掩码停靠接入, )
         * @details 与 Pop 的差异只在"队列空"时: 至多等 timeout_ms 即返回 Timeout。
         *          命令到达与停止仍**即时唤醒**(Push/Stop 的 notify 不变) —— 本函数
         *          存在的理由是空闲期的消费者必须能回到循环顶部应答停靠请求
         *          (MaskPark::ParticipantCheckpoint): Pop 的无限期等待会让 worker
         *          永远到不了循环顶, 停靠协议收不齐 ack, 掩码窗口在空闲态(最该掩的
         *          状态)反而永远无法成立。超时片只影响"空闲时检查点的节拍",
         *          不引入命令延迟。
         */
        PopStatus PopFor(TaskItem& item, uint32_t timeout_ms);

        /// 停止队列并唤醒所有等待线程
        void Stop();

        /// 检查队列是否已被停止
        bool IsStopped() const;

    private:
        /**
         * @brief 比较器: **升序**排列, `begin()` = 最先执行者
         * @details
         * ** 订正**: 原文写作 `a.priority > b.priority` 并注释"小的先出(最小堆)"
         * —— 两处都错, 且互相掩盖:
         *   * 这里是 `std::multiset`(按比较器**升序**排列, `begin()` 是最小者), **不是**
         *     `std::priority_queue`("最小堆"那句措辞是从后者搬来的);
         *   * 对升序容器来说, "谁先出"由 `a < b` 决定 —— 写 `>` 等于把顺序**整个颠倒**:
         *     优先级数值大(低优先级)的先执行, 同档**后入队**的先执行。
         * 实测证据: 注册 `High=0`/`Low=4` 两条并按 Low→High
         * 入队, 实际执行为 **low → high**; 同档四条按 1,2,3,4 入队, 实际执行为 **4,3,2,1**。
         * 影响面: 生存类命令(Survival=0)被排到所有命令之后; 同档命令反向执行 ——
         * operator 按序下发的命令序列会被反过来执行(如"先读后写"变成"先写后读")。
         * 该缺陷自 `199b5a9` 起存在(在 `last-known-good` 基线中同样存在), 非本次重构引入。
         */
        struct Compare {
            bool operator()(const TaskItem& a, const TaskItem& b) const {
                if (a.priority != b.priority) {
                    return a.priority < b.priority;   // 数值小(优先级高)先出
                }
                return a.seq < b.seq;                 // 同档: 先入队先出(FIFO)
            }
        };

        std::multiset<TaskItem, Compare> queue_;
        std::atomic<uint64_t> next_seq_{ 0 };   ///< 入队序号(C9 FIFO)
        std::atomic<size_t> max_pending_{ 512 };  ///< 待执行上限
        std::atomic<uint64_t> rejected_{ 0 };     ///< 因队列满被拒的条数(观测)
        mutable std::mutex mutex_;
        std::condition_variable cv_;
        std::atomic<bool> stopped_{ false };
    };

} // namespace cleat