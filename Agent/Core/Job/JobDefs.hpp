#pragma once
/**
 * @file JobDefs.hpp
 * @brief 任务系统基础定义
 * @details
 * 本文件是任务系统的**类型契约**:
 *   * `JobKind` 复用 ThreadGovernor 的分类(单一分类, 不许两份);
 *   * `JobRecord` 是单个工作单元状态的**唯一权威**(取代 IsRunning()/expired/完成回调
 *   * `JobSpec` 是出生口的输入(唯一出生口见 JobRuntime::Spawn);
 *   * `JobSnapshot` 是查询面的只读投影(命令面/审计消费, 不暴露内部锁与回调)。
 * 本文件只定义数据形状, 不含策略 —— 策略在 JobRuntime(登记/回收/取消)与各消费方。
 */

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>

#include "Core/Command/Handlers/ResourceKey.hpp"
#include "Core/Concurrency/ThreadGovernor.hpp"

namespace cleat {

    /**
     * @brief 工作单元状态(JobRecord 的单一权威状态字)
     * @details 转移纪律(与 ThreadGovernor 三态门闸同法, 只增不减语义):
     *   Starting --Spawn 成功建线程--> Running
     *   Running  --Cancel 受理-->      Stopping(协作式: 线程在安全点退出)
     *   Running/Stopping --deadline 到--> TimedOut(**终态**; 线程可仍在跑 = 遗弃)
     *   其余运行态 --线程结束-->        Completed / Killed / Failed
     * 终态之间不互转; `TimedOut` 是**更强的事实**(超时上报已发生), 线程随后结束不覆盖它。
     */
    enum class JobState : uint8_t {
        Starting = 0,   ///< 已登记, 线程尚未起跑(极短窗口)
        Running,        ///< 在跑
        Stopping,       ///< 已受理停止请求(协作式), 线程在安全点退出前保持
        Completed,      ///< 正常结束
        Killed,         ///< 被取消后结束(操作员 kill / 收尾拦截)
        TimedOut,       ///< 超时上报已发生(线程可能仍在跑 = 被遗弃, 见 finished 字段)
        Failed,         ///< 异常结束(线程创建失败 / 未捕获异常)
    };

    inline const char* ToString(JobState s) {
        switch (s) {
        case JobState::Starting:  return "Starting";
        case JobState::Running:   return "Running";
        case JobState::Stopping:  return "Stopping";
        case JobState::Completed: return "Completed";
        case JobState::Killed:    return "Killed";
        case JobState::TimedOut:  return "TimedOut";
        case JobState::Failed:    return "Failed";
        }
        return "?";
    }

    /// JobKind → 机器可读名(命令面 JSON 用; 与线程名册同源单一分类)
    inline const char* JobKindName(JobKind k) {
        switch (k) {
        case JobKind::Main:           return "Main";
        case JobKind::Infrastructure: return "Infrastructure";
        case JobKind::Service:        return "Service";
        case JobKind::Task:           return "Task";
        case JobKind::Ephemeral:      return "Ephemeral";
        case JobKind::Command:        return "Command";
        }
        return "?";
    }

    /// 单调毫秒(steady_clock 纪元; 仅用于存活时长/裁剪, 与墙钟无关)
    inline uint64_t JobNowMs() {
        return static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count());
    }

    /**
     * @brief 单个工作单元的记录(登记表条目)
     * @details **唯一权威**: 状态/取消令牌/完成事实都只在这里。跨线程访问纪律:
     *   * `state`/`alive`/`finished`/`ended_ms` 是锁自由原子(热路径零锁);
     *   * `m`/`cv` 只服务"可打断睡眠"(`JobContext::SleepFor`)与取消唤醒;
     *   * 构造后只读的字段(id/name/kind/...)以 const 表达, 由 Spawn 一次写定。
     * 生命周期: 由登记表持有 shared_ptr; 线程体另持一份 —— 裁剪先行也不会让
     * 在跑线程持有悬垂引用(命令侧的迟到响应判据正依赖这一点)。
     */
    struct JobRecord {
        const uint64_t id;
        const std::string name;
        const JobKind kind;
        const uint64_t started_ms;
        const uint32_t deadline_ms;      ///< 0 = 不限时
        const ResourceKey key;           ///< 资源声明(命令类互斥判据; 其余类留空)
        /// 高带宽声明(Phase 2/3: 低带宽通道拒绝启动 / 降级时被批量取消)
        const bool high_bandwidth;
        /// 会话附件(类型擦除): 任务类 job 把"handler 与 job 函数共享的会话对象"
        /// 挂在记录上 —— 单一来源(取代 TaskManager::GetTask 的第二份 map),
        /// 查询方经 JobRuntime::AttachmentOf 取回。
        const std::shared_ptr<void> attachment;
        /// 命令类专属: ExecutionTracker 槽位号 —— 超时回调经它定位记录
        /// (`JobRuntime::NoteTimeout`); 非命令类为 0。
        const uint64_t correlation;

        std::atomic<JobState> state{ JobState::Starting };
        /// 取消令牌(false = 已请求停止)。同时是 ThreadGovernor 的 alive 绑定目标:
        /// 暂停期被 Cancel 的 unit 在门闸兜底片内收摊(避免 join 等到窗口重开)。
        std::atomic<bool> alive{ true };
        std::atomic<bool> finished{ false };
        /// 业务失败请求(job 函数经 ctx.Fail() 置位): fn 正常返回但业务已失败 → Failed
        /// (真机冒烟发现: "文件不可读→error帧→return" 曾被落成 Completed)
        std::atomic<bool> fail_requested{ false };
        /// 终态时刻(裁剪用; 0 = 未终结)
        std::atomic<uint64_t> ended_ms{ 0 };

        /// 超时回调(命令类; 由 Spawn 从 JobSpec 拷入) —— 每 job 至多调用一次,
        /// 由内核(ExecutionTracker)的"恰好一次"保证 + NoteTimeout 的状态守卫双保险。
        std::function<void()> on_timeout;

        /// SleepFor/取消唤醒用: 护"取消方置 alive ↔ 等待方谓词检查"的握手(防丢唤醒,
        /// 见 JobRuntime::Cancel 的锁内置位纪律); JobState 状态字本身是锁自由原子, 不经它。
        std::mutex m;
        std::condition_variable cv;

        JobRecord(uint64_t id_, std::string name_, JobKind kind_, uint32_t deadline_ms_,
            ResourceKey key_, uint64_t correlation_, bool high_bandwidth_,
            std::shared_ptr<void> attachment_)
            : id(id_), name(std::move(name_)), kind(kind_),
              started_ms(JobNowMs()), deadline_ms(deadline_ms_),
              key(std::move(key_)), correlation(correlation_),
              high_bandwidth(high_bandwidth_), attachment(std::move(attachment_)) {}
    };

    /// 出生前回填槽: Spawn 在**起线程之前**写入 record —— 响应侧(线程内)读它无竞态
    /// 。
    struct JobRecordSlot {
        std::shared_ptr<JobRecord> record;
    };

    /**
     * @brief 出生口的输入(唯一出生口 = JobRuntime::Spawn)
     * @details 回调全部是**可选**的: 命令类用 on_timeout/on_rejected/slot/gate;
     * 任务/服务类(Phase 2/3)主要用 name/kind。gate 与 on_finished 的调用位置由
     * Spawn 的线程体严格定义(见 JobRuntime.cpp), 命令派发循环的 收尾语义靠它们保真。
     */
    struct JobSpec {
        std::string name;                ///< 登记名("cmd:<命令名>" / 服务名; 操作员寻址用)
        JobKind kind = JobKind::Command;
        uint32_t deadline_ms = 0;        ///< 0 = 不限时
        ResourceKey key{};
        uint64_t correlation = 0;        ///< 命令类: ExecutionTracker 槽位号(见 JobRecord)
        bool high_bandwidth = false;     ///< 高带宽声明(低带宽通道拒绝启动; 降级时被取消)
        std::shared_ptr<void> attachment;  ///< 会话附件(经 JobRuntime::AttachmentOf 取回)

        std::function<void()> on_timeout;    ///< 超时上报(命令类: 回超时错误帧; 恰好一次)
        std::function<void()> on_rejected;   ///< 资源占用拒绝(命令类: worker 准入路径消费)
        /// 线程起跑前的放行判定(false = **不执行 fn**, 记录按 Killed 落终态)。
        /// 命令类用它保留 的"收尾开始后 fn 不得执行"语义(见 CommandWorker.cpp)。
        std::function<bool()> gate;
        /// 线程**最后**动作(记录已落终态之后; 命令类的排水计数自减靠它保真)。
        std::function<void()> on_finished;
        std::shared_ptr<JobRecordSlot> slot;   ///< 可选: 出生前回填 record
    };

    /// 查询面的只读投影(命令面/审计消费; 不含回调与内部锁)
    struct JobSnapshot {
        uint64_t id = 0;
        std::string name;
        JobKind kind = JobKind::Command;
        JobState state = JobState::Starting;
        uint64_t age_ms = 0;
        uint32_t deadline_ms = 0;
        std::string resource;            ///< ResourceKey::Describe()(空 key → 空串)
        bool finished = false;           ///< 线程是否真正结束(与 state 分开: TimedOut 可能是遗弃)
        bool alive = true;               ///< 取消令牌(未受理停止 = true)
    };

    /// 出生口的返回句柄(无效句柄 = 未出生: 收尾期拒绝或线程创建失败)
    class JobHandle {
    public:
        JobHandle() = default;

        explicit operator bool() const { return static_cast<bool>(record_); }
        uint64_t id() const { return record_ ? record_->id : 0; }
        std::shared_ptr<JobRecord> record() const { return record_; }

    private:
        friend class JobRuntime;
        explicit JobHandle(std::shared_ptr<JobRecord> rec) : record_(std::move(rec)) {}
        std::shared_ptr<JobRecord> record_;
    };

} // namespace cleat
