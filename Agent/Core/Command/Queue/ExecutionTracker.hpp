/**
 * @file ExecutionTracker.hpp
 * @brief 执行槽位追踪与回收决策
 * @details
 * **存在理由**: 今天(合一前)的超时由"**等待者**"触发 —— `CommandWorker` 在
 * `cv.wait_for` 上阻塞到超时, 然后自己置 `expired` 并调 `on_timeout()`。这带来两个后果:
 *   ① **并发度恒为 1**: worker 阻塞在等待里, 队列里下一条命令要等这条超时/结束;
 *   ② **被遗弃的线程无人追踪**: 超时后线程被丢下继续跑, "它何时真正结束"没有任何记录 ——
 *      两个线程同时改同一资源(比不互斥更糟)。
 *
 * 本类是 reaper 的**决策状态机**: 只记账与产出"该做什么", **不持线程、不睡眠、不 join** ——
 * 与 `SessionRunner` 同法(时间由调用方传入), 故全部策略都可用假时钟确定性地断言。
 *
 * **与 JobRuntime 的分工(不重复)**: 本类是**命令执行引擎的槽位策略**(互斥判据/超时判定/
 * 强制释放——随引擎实例隔离, 夹具多实例的硬约束); 超时回调的落实与登记/查询在
 * `JobRuntime`(经 `correlation` 对齐;  任务系统 Phase 1)。迟到结果丢弃判据 =
 * `JobRecord.state == TimedOut`(单一权威, 原 `TaskGuard::expired` 副本已删除)。
 *
 * **策略来源**:
 *   * 超时上报**恰好一次**(否则同一 request_id 会收到多个超时响应);
 *   * 被遗弃槽位的 key **保持占用直到线程真正结束**(`MarkFinished`), 那时才 `ReleaseKey`;
 *   * **最长持有 + 强制释放**: 每类资源有上限, 到期强制放行并大声告警 —— 否则一条永久卡死的
 *     命令会让该资源**永久不可用**; 上限为 0 = 不设限(仅在"显式 Stop 才是正常释放路径"的类上,
 *     如键盘钩子/终端/流式采集)。
 */
#pragma once

#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "Core/Command/Handlers/ResourceKey.hpp"

namespace cleat {

    /// reaper 在某一时刻应当执行的动作(纯数据; 由驱动方落实为真实动作)
    enum class ReapActionKind {
        FireTimeout = 0,      ///< 触发一次超时上报(投递方据此回超时错误)
        ReleaseKey,           ///< 释放槽位资源(线程已结束, 或强制释放)
        ForcedReleaseKey,     ///< **强制**释放(超过最长持有; 必须告警 + 计入统计)
    };

    inline const char* ToString(ReapActionKind k) {
        switch (k) {
        case ReapActionKind::FireTimeout: return "FireTimeout";
        case ReapActionKind::ReleaseKey: return "ReleaseKey";
        case ReapActionKind::ForcedReleaseKey: return "ForcedReleaseKey";
        }
        return "?";
    }

    struct ReapAction {
        ReapActionKind kind = ReapActionKind::FireTimeout;
        uint64_t task_id = 0;
        ResourceKey key;              ///< 仅 ReleaseKey/ForcedReleaseKey 有意义
        uint32_t held_ms = 0;         ///< 已持有毫秒数(告警与排障用)
    };

    /**
     * @brief 一类资源的"最长持有"上限(0 = 不设限)
     * @details 数值取自用户裁定的资源清单; 改这里就是改策略, 不要在别处再写一份。
     */
    uint32_t MaxHoldMs(ResourceKind kind);

    /// 一个正在执行(或已被遗弃但仍在跑)的槽位
    struct ExecutionSlot {
        uint64_t task_id = 0;
        ResourceKey key;                     ///< 可能不参与互斥(kind == None / 未声明 id)
        uint32_t timeout_ms = 0;             ///< 0 = 不限时
        std::chrono::steady_clock::time_point started_at{};
        std::chrono::steady_clock::time_point deadline{};
        bool finished = false;               ///< 任务线程已结束(由执行角色置)
        bool timed_out = false;              ///< 超时已上报(**恰好一次**)
        bool key_released = false;           ///< 资源已释放(幂等)
    };

    /**
     * @brief 执行槽位追踪器(reaper 的被动内核)
     * @details
     * **并发契约(第十七轮 )**: 本类被三个线程并发调用 —— worker(`TryAdmit`/`ActiveCount`/
     * `Begin`)、每任务一个的 detach 任务线程(`MarkFinished`)、reaper(`Reap`/`Stray`)。
     * 并发安全**落在类型里**(每个公开方法自带锁), 不靠调用方自觉 —— 首版把锁留在外面,
     * `CommandWorker::slot_mutex_` 又只护条件变量不护本类, `Begin` 的 rehash 与 `Reap`
     * 的遍历相撞即迭代器失效(随机崩溃且被自重启掩盖, 极难定位)。
     * 注意: `Reap` 返回动作列表后锁已释放, 动作落实(回调/发帧)不持本锁 —— 回调若再进
     * 本类(现无此路径)也不会死锁。
     */
    class ExecutionTracker {
    public:
        using Clock = std::chrono::steady_clock;
        using TimePoint = Clock::time_point;

        /**
         * @brief 登记一个开始执行的命令
         * @param key        该命令独占的资源(默认空 = 不参与互斥)
         * @param timeout_ms 超时毫秒(0 = 不限时, 与旧语义一致)
         * @param now        起始时刻
         * @return 槽位 id(单调递增)
         */
        uint64_t Begin(const ResourceKey& key, uint32_t timeout_ms, TimePoint now);

        /// 标记任务线程**真正结束**(被遗弃的线程结束时也必须调用, 否则 key 永不释放)
        bool MarkFinished(uint64_t task_id);

        /**
         * @brief 推进时间, 产出"现在该做什么"(不改任何线程状态)
         * @details 产出顺序: 先 `FireTimeout`(每槽位至多一次), 再释放已结束槽位的 key,
         *          最后对超过最长持有者产出 `ForcedReleaseKey`。已收尾的槽位从表中移除。
         */
        std::vector<ReapAction> Reap(TimePoint now);

        /**
         * @brief 互斥判据: 该资源现在是否可被新命令占用
         * @details 同时看两件事: ①是否有在跑(含被遗弃)的命令持有同一 key;
         *          ②该 key 是否已被强制释放(强制释放后语义上仍被旧线程占用, 但策略上放行 ——
         *          放行是**故意的**, 因为不放行就等于该资源永久不可用)。
         */
        bool TryAdmit(const ResourceKey& key) const;

        // ---------------- 观测(排障与验收) ----------------

        size_t InFlight() const;

        /**
         * @brief 占用**并发槽位**的条数(超时即释放; 与"资源 key"是两件事)
         * @details 这条区分来自旧实现的实测语义: 超时后线程被 `detach()` 出去继续跑,
         *          而消费者**立刻取了下一条** —— 即"并发槽位在超时那一刻就释放, 被遗弃的
         *          线程仍占着它的资源 key"。若把二者混为一谈: 要么并发度被遗弃线程永久吃掉
         *          (cap=1 时后续命令永不执行), 要么资源被提前放行(两个线程同时改同一资源)。
         */
        size_t ActiveCount() const;

        /// 已被遗弃但仍在跑的槽位数(= 旧实现的 `stray_threads_` 概念)
        size_t Stray() const;
        uint64_t TimedOutCount() const;
        uint64_t ForcedReleaseCount() const;
        /// 当前是否有命令持有该资源(测试/排障用)
        bool Holds(const ResourceKey& key) const;

    private:
        void EraseSlot(uint64_t task_id);

        mutable std::mutex mutex_;   ///<  保护以下全部成员(并发契约落在类型里)
        std::unordered_map<uint64_t, ExecutionSlot> slots_;
        uint64_t next_id_ = 1;
        uint64_t timed_out_count_ = 0;
        uint64_t forced_release_count_ = 0;
    };

} // namespace cleat
