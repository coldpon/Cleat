/**
 * @file CommandWorker.cpp
 * @brief 命令执行线程实现
 * @details
 * 单线程顺序消费 CommandQueue 中的任务。本类线程**默认无 COM 公寓**（框架不代管，
 * 需要 COM 的 handler 组件按调用点自备，工具：Platform::ScopedComApartment）。
 * 带投递票据（JobTicket）的任务（网络命令）经 `JobRuntime::Spawn` 出生为
 * kind=Command 的 job，在子线程执行并受超时护栏约束：超时后遗弃任务线程、
 * 回超时错误，队列不会被单个卡死的 handler 堵死。
 *
 * 任务系统 Phase 1把本类从"自持记账"改为"委托记账"：
 *   * 线程出生 → `JobRuntime::Spawn`（唯一出生口; 登记/门闸接线/终态落定在内）；
 *   * `guards_` 映射删除 —— 超时回调收编进 JobRecord（经 correlation 定位）；
 *   * 超时落实 → `JobRuntime::NoteTimeout`（恰好一次 + 状态守卫）；
 *     **槽位策略内核**(ExecutionTracker, 随实例隔离 —— 夹具多实例的硬约束)。
 */

#include "CommandWorker.hpp"
#include "CommandQueue.hpp"
#include "Common/Logger/ErrLog.hpp"
#include "Common/Logger/Logger.hpp"
#include "Core/Concurrency/ThreadGovernor.hpp"
#include "Core/Identity/SessionIdentity.hpp"
#include "Core/CrashGuard/CrashGuard.hpp"
#include "Core/Job/JobDefs.hpp"
#include "Core/Job/JobRuntime.hpp"
#include "Core/Mask/MaskPark.hpp"
#include <algorithm>

namespace cleat {

    namespace {
        /// reaper 节拍: 决定"超时上报"的最坏延迟(旧实现恰在 deadline; 新实现 ≤ 本值)
        constexpr uint32_t kReapTickMs = 10;

        /// 对"等名额"的打断延迟上限 = 本值。
        constexpr uint32_t kSlotPollMs = 20;

        /// 空闲 Pop 有界等待片(睡眠掩码停靠接入, ): PopFor 的超时上界。
        /// 空闲期消费者必须能周期性回到循环顶(MaskPark::ParticipantCheckpoint),
        /// 否则停靠请求收不齐 ack, 掩码窗口在最该掩的空闲态永远无法成立。
        /// 命令到达/停止仍即时唤醒(notify 不变), 本值不引入命令延迟。
        constexpr uint32_t kIdleParkSliceMs = 100;

        /// 关停排水(): Stop 在 join 完 worker/reaper 后, 给在飞任务线程的宽限上限。
        /// **这只是宽限, 不是正确性机制** —— 正确性由 g_worker_stopped 保证(见下)。
        /// 超限即继续收停: 被遗弃(已超时)的线程可能永不结束, 不能无限等( 同族的已知取舍)。
        constexpr uint32_t kStopDrainTimeoutMs = 2000;
        constexpr uint32_t kStopDrainPollMs = 20;

        /**
         *  任务线程的"收尾"开关。**必须是平凡可析构的文件级 atomic**, 理由有二:
         *   ① 函数内静态单例(CommandWorker/SessionIdentity/CommandQueue)在静态析构期
         *     可能已析构, 此时读它们的成员本身就是 UAF —— 旧实现的
         *     `SessionIdentity::Instance().ShouldExit()` 判定正有此患;
         *   ② 平凡可析构类型没有注册析构, 静态析构的任意时刻读写都安全。
         * 语义: Stop() 置位后, 一律 detach 的任务线程**不得再碰本对象的任何成员**
         * (tracker_/slot_cv_), 收尾动作也不例外 —— 旧实现的注释声称已防, 实际只包住了
         * fn(), MarkFinished/notify 裸奔在判定外。 任务系统 Phase 1 后该判定
         * 落在两个位置: Spawn 的 gate(挡 fn)与 on_finished 回调(挡 tracker_/slot_cv_);
         * 记录落终态走永不析构的 JobRuntime, 收尾期也安全(无需判定)。
         */
        std::atomic<bool> g_worker_stopped{ false };

        /**
         *  在飞任务线程计数(仍可能回来碰本对象的线程数)。同样必须平凡可析构:
         * 它是排水等待的信号, 而排水发生时其它对象成员还"活着但即将销毁" ——
         * 信号本身绝不能依赖它们。约定: LaunchGuarded 在建线程前自增; 任务线程把
         * 自减作为**最后一个动作**(此后不再碰任何共享状态); Stop 排水等到归零
         * (带上限)。因实例互斥地起停(单例 / 顺序执行的夹具), 全局一份即够。
         */
        std::atomic<int> g_active_task_threads{ 0 };
    } // namespace

    /**
     * @brief 获取命令工作线程单例实例(绑定全局命令队列)
     * @return CommandWorker& 静态单例引用
     */
    CommandWorker& CommandWorker::Instance() {
        static CommandWorker instance{ CommandQueue::Instance() };
        return instance;
    }

    /**
     * @brief 析构函数，确保线程停止
     */
    CommandWorker::~CommandWorker() { Stop(); }

    /**
     * @brief 设置任务执行超时时间
     * @param ms 超时毫秒数；0 表示不限时，其他值最小为 1000ms
     *
     * 若 ms 不为 0，则取 max(ms, 1000) 作为有效超时值。
     */
    void CommandWorker::SetTaskTimeoutMs(uint32_t ms) {
        // 0 = 不限时；其余下限 1000ms（护栏要有意义的最小粒度）
        task_timeout_ms_.store(ms == 0 ? 0 : std::max<uint32_t>(ms, 1000));
        LOG_INFO("CommandWorker: 任务超时设置为 " << task_timeout_ms_.load() << "ms");
    }

    /**
     * @brief 获取当前任务超时时间
     * @return uint32_t 超时毫秒数；0 表示不限时
     */
    uint32_t CommandWorker::GetTaskTimeoutMs() const {
        return task_timeout_ms_.load();
    }

    /**
     * @brief 启动工作线程
     *
     * 如果线程已运行则不做任何操作并记录警告日志。
     */
    void CommandWorker::Start() {
        if (running_.load()) {
            LOG_WARN("CommandWorker: 已经运行");
            return;
        }
        // 夹具会按用例起停实例: 重开时必须解除上一轮的收尾封锁,
        // 否则新实例的任务线程会看到旧标志而全部空转。生产路径只启动一次, 无此交叉。
        g_worker_stopped.store(false);
        running_.store(true);
        //  reaper 独立线程 —— 超时上报不再依赖"有人正好在等"(空闲期也必须能上报)
        reaperThread_ = std::make_unique<std::thread>(&CommandWorker::ReaperMainLoop, this);
        workerThread_ = std::make_unique<std::thread>(&CommandWorker::WorkerMainLoop, this);
        LOG_DEBUG("CommandWorker: 已启动(消费者 + reaper)");
    }

    /**
     * @brief 停止工作线程（阻塞等待退出）
     *
     * 若任务卡死，最坏等待一个超时周期。先停止队列唤醒可能阻塞的 Pop，
     * 然后等待工作线程结束。
     *
     *  本函数返回后调用方(析构或 Shutdown)将继续销毁本对象与各模块 ——
     * 因此①置位 g_worker_stopped 让在飞任务线程自检退出(正确性机制);
     * ②排水等待在飞计数归零, 给"正在正常执行"的任务一个收尾宽限(尽力而为,
     * 带上限; 超限的残余风险与旧行为一致 —— 旧实现在此**一个字节都不等**)。
     */
    void CommandWorker::Stop() {
        // 竞争)时原"joinable 检查"防不住两线程同时通过检查后双双 join(UB); 先锁后查
        // 串行化, 后到者看到 running_==false 直接返回。
        std::lock_guard<std::mutex> lock(stop_mutex_);
        if (!running_.load()) {
            return;
        }
        // 先立收尾标志(先于一切): 此刻起任务线程不得再碰本对象成员。
        // worker/reaper 由下面的 join 照管; 任务线程只能靠这个原子开关自检。
        g_worker_stopped.store(true);
        running_.store(false);
        queue_.Stop();      // 唤醒可能阻塞的 Pop
        slot_cv_.notify_all();   // 唤醒可能阻塞在名额准入上的消费者
        if (workerThread_ && workerThread_->joinable()) {
            workerThread_->join();
        }
        if (reaperThread_ && reaperThread_->joinable()) {
            reaperThread_->join();   // 最坏等一个 kReapTickMs(10ms)
        }
        // 排水: 等"还会回来碰成员"的在飞任务线程归零(计数自减是线程最后一个
        // 动作, 归零 = 已无线程会再访问本对象)。注: 被收尾开关挡住的线程不再
        // MarkFinished, 槽位会滞留 —— 有意取舍, 故排水看计数、不看 UnfinishedCount。
        const auto drain_deadline = std::chrono::steady_clock::now()
            + std::chrono::milliseconds(kStopDrainTimeoutMs);
        int active = g_active_task_threads.load();
        while (active > 0) {
            if (std::chrono::steady_clock::now() >= drain_deadline) {
                LOG_WARN("CommandWorker: 关停排水超时, 仍有 " << active
                    << " 个在飞任务线程(可能被遗弃/卡死) —— 继续收停, 它们已被封锁在外");
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(kStopDrainPollMs));
            active = g_active_task_threads.load();
        }
        LOG_DEBUG("CommandWorker: 已停止");
    }

    /**
     * @brief 工作线程主循环
     *
     * 包含线程级崩溃兜底（硬件异常保护），并在循环内部完成平台初始化（COM等）。
     * 循环中从队列取出任务，根据是否有 guard 分别走带超时护栏或普通执行路径。
     */
    void CommandWorker::WorkerMainLoop() {
        // 线程级崩溃兜底：任务内硬件异常（访问违例/栈溢出等）不返回，
        // 走崩溃路径（minidump + 自重启），防止单个 handler 野指针带走整个进程。
        // 平台细节封装在 Core/CrashGuard（Windows SEH / POSIX 信号），Core 零平台代码。
        Platform::RunWorkerGuarded([this]() {

            // 本线程无 COM 公寓：
            // timeout=0 的就地执行与内部投递若有 COM 需求，由 handler 组件按调用点
            // 自备（Platform::ScopedComApartment）。全仓 COM 组件
            // （CameraShot/CameraStream/EdrRecon/Persistence）均已自管。

            // 线程治理: 注册进全局名册(审计可见), 每拍查暂停/退出 —— 窗外暂停期
            // 不取件(已到的命令留队列, 恢复后执行), 退出请求则立即收摊。
            // 绑定 running_: 暂停期 Stop() 时立即收摊, join 不必等窗口重开。
            ThreadGovernor::GovLoop gov("cmd-worker", JobKind::Infrastructure, &running_);

            // 睡眠掩码停靠(设计): 入册 + 循环顶检查点。掩码开关 OFF 时两者皆空实现。
            MaskPark::Registration park_reg;
            (void)park_reg;

            LOG_DEBUG("CommandWorker: 工作线程开始");
            while (running_.load()) {
                // 停靠检查点: 必须在 gov.Tick 之前 ——
                // dormant 暂停期门闸 Tick 是阻塞等待, 参与者冻在门闸内, 停靠 ack
                // 永远收不齐(实测 dormant 掩码 100% park-enter-timeout)。
                MaskPark::ParticipantCheckpoint();
                if (ThreadGovernor::Paused()) {
                    // 暂停期保持循环节奏(不进阻塞门闸): 每 kIdleParkSliceMs 回到
                    // 循环顶一次, 停靠请求随时可达; 不取件、不消费 —— 门闸的暂停
                    // 语义(不干活)完整保留, 只是"等待"从门闸内移到循环顶。
                    std::this_thread::sleep_for(std::chrono::milliseconds(kIdleParkSliceMs));
                    continue;   // running_/Stopping 由循环条件与下一拍判定
                }
                if (!gov.Tick()) break;
                // 取件**先于**名额等待: 资源准入判定必须在"等待名额"之前发生 ——
                // 否则 cap=1 时消费者阻塞在名额上, 同 key 的第二条命令只会**排队**,
                TaskItem item;
                const auto popped = queue_.PopFor(item, kIdleParkSliceMs);
                if (popped == CommandQueue::PopStatus::Stopped) {
                    break;   // 被停止
                }
                if (popped == CommandQueue::PopStatus::Got) {
                    if (item.ticket) {
                        // ① 资源准入(**先于**名额等待): 被占用则按用户裁定"拒绝而非排队" ——
                        //    若放在名额等待之后, cap=1 时同 key 的第二条只会排队, 等占用者结束后
                        if (item.key.Participates() && !tracker_->TryAdmit(item.key)) {
                            LOG_WARN("CommandWorker: 资源 " << item.key.Describe()
                                << " 正被占用, 拒绝执行(不排队; 排队会让它在稍后突然生效, 更难排查)");
                            if (item.ticket->on_rejected) {
                                try {
                                    item.ticket->on_rejected();
                                }
                                catch (...) {
                                    CLEAT_LOGE(ErrDom::Cmd, ErrCode::UnhandledException, "CommandWorker: 资源拒绝回调异常");
                                }
                            }
                            else if (item.ticket->on_timeout) {
                                try {
                                    item.ticket->on_timeout();
                                }
                                catch (...) {
                                    CLEAT_LOGE(ErrDom::Cmd, ErrCode::UnhandledException, "CommandWorker: 超时回调异常");
                                }
                            }
                            continue;
                        }
                        // ② 名额准入: cap 满则等名额释放(默认 1 = 旧顺序语义)。
                        //    代价(已知): 任务在 Pop 时即被认领, 故等待期间新到的高优先级命令排在其后;
                        //    这是为"资源被占用立刻拒绝"接受的次序细节, 只在 cap 满且有积压时可见。
                        if (!WaitForSlot()) break;
                        // ③ 起执行线程后**立即返回**(消费者不再等待), 超时由 reaper 上报
                        LaunchGuarded(item);
                    }
                    else {
                        // 普通任务（内部投递）：保持原有语义
                        try {
                            item.func();
                        }
                        catch (const std::exception& e) {
                            CLEAT_LOGE(ErrDom::Cmd, ErrCode::UnhandledException, "CommandWorker: 任务执行异常: " << e.what());
                        }
                        catch (...) {
                            CLEAT_LOGE(ErrDom::Cmd, ErrCode::UnhandledException, "CommandWorker: 任务执行未知异常");
                        }
                    }
                }
                // Timeout: 空闲片到点 → 回循环顶(gov 判决 + 停靠检查点), 无任务交付
            }

            LOG_DEBUG("CommandWorker: 工作线程结束");
            });
    }

    /**
     * @brief 起始名额准入: cap 未满即可取新任务
     * @details 名额在**任务超时那一刻**即释放(与旧实现 detach + 立刻取下一条一致), 而资源 key
     *          要等线程真正结束 —— 二者是两件事, 见 `ExecutionTracker::ActiveCount`。
     */
    bool CommandWorker::WaitForSlot() {
        std::unique_lock<std::mutex> lock(slot_mutex_);
        // **循环**等到"可开工"或"该收摊"为止。首版写成"一次 wait_for 的返回值即结论":
        // 谓词为假时 wait_for 返回 false, 而调用方把 false 当作"该停止" → 起完第一条命令
        // 消费者线程就退出了, 后续命令永不执行(实测被"第二条命令"用例抓住)。
        // 教训: "条件未满足"与"该停止"是两个结论, 不能让一个 bool 同时承载。
        //
        // 之前还有一段空窗(取件 → 资源准入 → 等名额), 暂停恰落在其中时, 名额释放仍会
        // "就地开工"。故: 收尾(Stopping) → 收摊; **暂停(Paused) → 等恢复再放行, 不得返回
        // false**(返回 false 会被调用方当作收摊把 worker 弄死 —— 暂停不是停止)。
        // 唤醒来源: 门闸的 notify 到不了本类的 slot_cv_, 打断靠下列 20ms 轮询兜底。
        for (;;) {
            if (!running_.load()) return false;                    // 本地停: 收摊
            if (ThreadGovernor::Stopping()) return false;          // 收尾门: 收摊
            if (ThreadGovernor::Paused()) {                        // 暂停: 不得开工, 等窗口
                slot_cv_.wait_for(lock, std::chrono::milliseconds(kSlotPollMs));
                continue;
            }
            if (tracker_->ActiveCount() < max_concurrency_.load()) return true;   // 有名额
            slot_cv_.wait_for(lock, std::chrono::milliseconds(kSlotPollMs));
        }
    }

    void CommandWorker::NotifySlotFreed() {
        slot_cv_.notify_all();
    }

    /**
     * @brief 经唯一出生口执行带超时护栏的任务并立即返回
     * @details
     * 与旧实现的差异(**逐条在提交信息里登记**):
     *   ① 消费者**不再等待** —— 并发度不再被"等待"锁死为 1(不变);
     *   ② 线程经 `JobRuntime::Spawn` 出生(kind=Command; 登记/门闸接线/终态落定在内),
     *      结束由 on_finished 回调上报 —— 本类不再持 `guards_` 记账(Phase 1 收编);
     *   ③ 超时**不再由等待者触发**, 改由 reaper 线程(见 `ReaperMainLoop`)。
     * 收尾语义**逐字保留**: gate 挡"收尾开始后 fn 不得执行"(先查 g_worker_stopped
     * 再查 SessionIdentity, 短路次序即是理由); on_finished 在收尾期不碰本类成员,
     * 但排水计数自减**无条件**且是线程最后一个动作(否则 Stop 的排水永不等不到归零)。
     */
    void CommandWorker::LaunchGuarded(const TaskItem& item) {
        std::shared_ptr<JobTicket> ticket = item.ticket;
        // per-task 超时覆盖: item.timeout_ms > 0 优先, 否则全局
        const uint32_t timeoutMs = item.timeout_ms != 0 ? item.timeout_ms : task_timeout_ms_.load();

        if (timeoutMs == 0) {
            // 不限时: 与旧实现一致, 直接在本线程执行(不占槽位、不参与超时)
            try {
                item.func();
            }
            catch (const std::exception& e) {
                CLEAT_LOGE(ErrDom::Cmd, ErrCode::UnhandledException, "CommandWorker: 任务执行异常: " << e.what());
            }
            catch (...) {
                CLEAT_LOGE(ErrDom::Cmd, ErrCode::UnhandledException, "CommandWorker: 任务执行未知异常");
            }
            return;
        }

        // 槽位登记
        const uint64_t slot = tracker_->Begin(item.key, timeoutMs, ExecutionTracker::Clock::now());

        //  在飞计数先行自增(出生失败时在下方补自减) —— 保证线程的自减
        // 永不落在自增之前, 排水等待的归零才可信。
        g_active_task_threads.fetch_add(1);

        // 出生规格: 登记 + 门闸接线 + 终态落定都在 Spawn 内;
        // 本处只组装命令类特有的三件 —— gate(第一次判定)、on_finished(第二次判定 +
        // 排水计数)、超时回调(ticket → 记录, 由 NoteTimeout 在状态守卫下恰好一次调用)。
        JobSpec spec;
        spec.name = ticket && !ticket->name.empty() ? ticket->name : std::string("cmd:unknown");
        spec.kind = JobKind::Command;
        spec.deadline_ms = timeoutMs;
        spec.key = item.key;
        spec.correlation = slot;          // 超时回调据此定位记录(JobRuntime::NoteTimeout)
        if (ticket) {
            spec.on_timeout = ticket->on_timeout;
            spec.slot = ticket->slot;     // 出生前回填(迟到响应判据 = 记录状态, D2)
        }
        // gate(第一次判定): **先**查 g_worker_stopped(平凡可析构, 静态析构期读写安全),
        // 再查 SessionIdentity(函数内静态单例, 静态析构期 Instance() 本身即 UAF —— 短路保证
        // 后者在析构期不可达)。false = fn 不执行(记录按 Killed 落终态留痕)。
        spec.gate = []() {
            return !g_worker_stopped.load() &&
                !cleat::SessionIdentity::Instance().ShouldExit();
            };
        // on_finished(Spawn 线程体的**最后一个**动作; 此刻记录已落终态):
        // 收尾期不得再碰 tracker_/slot_cv_(第二次判定); 但排水计数自减必须**无条件**
        // 执行(已被收尾拦住也不能漏, 否则 Stop 的排水等不到归零)。
        spec.on_finished = [this, slot]() {
            if (!g_worker_stopped.load()) {
                tracker_->MarkFinished(slot);   // 线程**真正结束**才上报 = 资源 key 释放判据
                NotifySlotFreed();
            }
            g_active_task_threads.fetch_sub(1);
            };

        auto job = JobRuntime::Spawn(spec, [fn = item.func](JobContext&) {
            // 本线程无 COM 公寓（框架不代管）: 需要 COM 的 handler 组件按调用点自备
            // （Platform::ScopedComApartment），见 JobRuntime::Spawn 的 COM 契约注释。
            Platform::RunWorkerGuarded([&fn]() {
                try {
                    fn();
                }
                catch (const std::exception& e) {
                    CLEAT_LOGE(ErrDom::Cmd, ErrCode::UnhandledException, "CommandWorker: 任务执行异常: " << e.what());
                }
                catch (...) {
                    CLEAT_LOGE(ErrDom::Cmd, ErrCode::UnhandledException, "CommandWorker: 任务执行未知异常");
                }
                });
            });
        if (!job) {
            // 出生失败(线程创建资源耗尽 / 收尾期拒绝): 立刻如实报失败 + 释放槽位, 不留孤儿
            // (与旧 catch 分支逐条对应; 记录已由 Spawn 按 Failed 落终态留痕)
            CLEAT_LOGE(ErrDom::Cmd, ErrCode::ResourceExhausted, "CommandWorker: 任务出生失败(线程创建失败或收尾期拒绝), 立即上报");
            g_active_task_threads.fetch_sub(1);
            tracker_->MarkFinished(slot);
            if (ticket && ticket->on_timeout) {
                try {
                    ticket->on_timeout();
                }
                catch (...) {
                    CLEAT_LOGE(ErrDom::Cmd, ErrCode::UnhandledException, "CommandWorker: 超时回调异常");
                }
            }
            NotifySlotFreed();
            return;
        }
    }

    /**
     * @brief reaper: 周期回收 + janitor 拍(终态历史裁剪;  Phase 1 扩权)
     * @details 这是"超时触发者"从等待者移交到 reaper 的落点。产出的动作在此落实:
     *          `FireTimeout` → `JobRuntime::NoteTimeout`(按 correlation 定位记录 →
     *          置 TimedOut + 调超时回调, 恰好一次; 迟到响应判据 = 记录状态, 不再有
     *          `expired` 副本); `ForcedReleaseKey` → 内核已打日志, 此处只需唤醒等待
     *          名额的消费者。裁剪每拍顺带执行(终态历史 TTL + 条数上限, 开销为一次
     *          小表扫描; janitor 与 reaper 是**同一个线程**, 不加新线程)。
     */
    void CommandWorker::ReapTick() {
        const auto actions = tracker_->Reap(ExecutionTracker::Clock::now());
        for (const ReapAction& a : actions) {
            if (a.kind == ReapActionKind::FireTimeout) {
                JobRuntime::NoteTimeout(a.task_id);
                CLEAT_LOGE(ErrDom::Cmd, ErrCode::ResourceExhausted, "CommandWorker: 任务执行超时, 已遗弃任务线程(累计遗弃 "
                    << tracker_->Stray() << ", 已超时 " << tracker_->TimedOutCount() << ")");
            }
        }
        if (!actions.empty()) {
            NotifySlotFreed();   // 名额可能因超时而释放
        }
        JobRuntime::PruneTerminalHistory();
    }

    void CommandWorker::ReaperMainLoop() {
        LOG_DEBUG("CommandWorker: reaper 线程开始");
        // 线程治理: 随 worker 一起注册; 暂停期 reaper 同停 —— 在飞任务自身已被
        // Checkpoint 拦在分块/循环边界, 暂停中不产生新的超时上报(否则上报帧会
        // 在静默期出站)。恢复后一次性补算: 长暂停跨过超时的任务立即超时, 有意取舍。
        ThreadGovernor::GovLoop gov("cmd-reaper", JobKind::Infrastructure, &running_);

        // 睡眠掩码停靠(设计): reaper 与 worker 同为停靠参与者(节拍 10ms, 应答更快)
        MaskPark::Registration park_reg;
        (void)park_reg;

        while (running_.load()) {
            // 停靠检查点最高优先(同 worker 循环的修正注释; reaper 节拍 10ms 应答更快)。
            MaskPark::ParticipantCheckpoint();
            if (ThreadGovernor::Paused()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(kReapTickMs));
                continue;
            }
            if (!gov.Tick()) break;
            ReapTick();
            std::this_thread::sleep_for(std::chrono::milliseconds(kReapTickMs));
        }
        LOG_DEBUG("CommandWorker: reaper 线程结束");
    }

} // namespace cleat
