#include "Core/Job/JobRuntime.hpp"

#include <algorithm>
#include <mutex>
#include <set>
#include <unordered_map>
#include <utility>

#include "Common/Logger/ErrLog.hpp"
#include "Common/Logger/Logger.hpp"

namespace cleat {

    namespace {

        constexpr uint64_t kDefaultTtlMs = 10ull * 60 * 1000;
        constexpr size_t kDefaultCap = 256;

        /**
         * 线程体在静态析构期落终态(`MarkTerminal`)会锁它 —— 对象必须活着。
         * 不用函数内静态(交付构建 /Zc:threadSafeInit- 下魔法静态既非线程安全也不可依赖)。
         */
        struct ServiceCatalogEntry {
            bool high_bandwidth = false;
            JobRuntime::ServiceFactory factory;
        };

        struct Registry {
            std::mutex m;
            std::unordered_map<uint64_t, std::shared_ptr<JobRecord>> records;
            /// correlation(命令类 = ExecutionTracker 槽位号) → job id; 超时回调据此定位记录
            std::unordered_map<uint64_t, uint64_t> by_correlation;
            /// 服务目录(Phase 3): name → 条目
            std::unordered_map<std::string, ServiceCatalogEntry> services;
            /// 启动中预约(单例双开防护: 工厂在锁外跑, 该窗口内同名第二次 Start 拒绝)
            std::set<std::string> starting;
            uint64_t next_id = 1;
            uint64_t ttl_ms = kDefaultTtlMs;
            size_t cap = kDefaultCap;
        };
        Registry* const g_registry = new Registry{};

        /**
         * 任务/服务 DI 持有者(数据出口 + 低带宽检查): 同样**永不析构** ——
         * 写入只发生在装配期(启动单线程阶段), 读取在任意线程 —— 仍取锁(代价一次
         * 无竞争加锁, 换"无数据竞争"的结构保证)。
         */
        struct DiHolder {
            std::mutex m;
            JobRuntime::DataSender sender;
            JobRuntime::BoolCheck low_bandwidth_check;
            JobRuntime::BoolCheck https_forbidden;
            JobRuntime::BoolCheck network_check;
        };
        DiHolder* const g_di = new DiHolder{};

        /// 服务宿主面 shim(永不析构; 服务 job 函数把它递给需要 host 的提供者)
        struct JobHostImpl : IServiceHost {
            bool IsNetworkConnected() const override { return JobRuntime::NetworkConnected(); }
            void SendData(const std::vector<uint8_t>& data) override { JobRuntime::Emit(data); }
        };
        JobHostImpl* const g_host = new JobHostImpl{};

        JobSnapshot SnapshotOf(const std::shared_ptr<JobRecord>& rec, uint64_t now_ms) {
            JobSnapshot s;
            s.id = rec->id;
            s.name = rec->name;
            s.kind = rec->kind;
            s.state = rec->state.load(std::memory_order_relaxed);
            const uint64_t ended = rec->ended_ms.load(std::memory_order_relaxed);
            const uint64_t ref = ended != 0 ? ended : now_ms;
            s.age_ms = ref > rec->started_ms ? ref - rec->started_ms : 0;
            s.deadline_ms = rec->deadline_ms;
            s.resource = rec->key.Participates() ? rec->key.Describe() : std::string();
            s.finished = rec->finished.load(std::memory_order_relaxed);
            s.alive = rec->alive.load(std::memory_order_relaxed);
            return s;
        }

        /// 记录落终态(线程结束 / gate 拦截 / 创建失败三条路径共用)。
        /// 纪律: `TimedOut` 是更强的事实, 不覆盖; 名字册锁内只改记账, 不派发任何回调。
        void MarkTerminal(const std::shared_ptr<JobRecord>& rec, JobState end) {
            // 终态优先(一般化, 原"TimedOut 不被覆盖"): 任一终态(Completed/Killed/
            // TimedOut/Failed)已定 → 后到的落终态 no-op。会话驱动型 Finish 与线程退出
            // 的自动落终态并发时, 先到者赢 —— 二者都是事实, 不互相覆盖。
            const JobState cur = rec->state.load(std::memory_order_relaxed);
            const bool already_terminal = cur == JobState::Completed || cur == JobState::Killed
                || cur == JobState::TimedOut || cur == JobState::Failed;
            if (!already_terminal) {
                rec->state.store(end, std::memory_order_release);
            }
            rec->finished.store(true, std::memory_order_release);
            rec->ended_ms.store(JobNowMs(), std::memory_order_relaxed);
            std::lock_guard<std::mutex> lock(g_registry->m);
            if (rec->correlation != 0) {
                g_registry->by_correlation.erase(rec->correlation);
            }
        }

        /// 线程体(唯一出生口的执行面): gate → 接线门闸 → fn → 终态 → on_finished(最后)。
        /// 调用顺序是 收尾语义的保真点(见 CommandWorker.cpp 的回调构造):
        ///   * `on_finished` 用 RAII 保证**所有**退出路径都执行(含 gate 拦截与异常),
        ///     且声明在最后 → 析构最后执行 = 它是线程的最后一个动作;
        ///   * 记录落终态在 on_finished **之前**(记录是永不析构的全局对象, 收尾期也安全);
        ///   * on_finished 内部的"收尾期不得触碰派发方成员"由派发方自己守(回调里自查)。
        void RunJob(std::shared_ptr<JobRecord> rec,
            std::function<bool()> gate,
            std::function<void()> on_finished,
            JobRuntime::JobFn fn) {
            struct FinalCall {
                std::function<void()> f;
                ~FinalCall() { if (f) f(); }
            } fin{ std::move(on_finished) };

            // 门闸接线(P5 类别①②合一): 非 Command 类出生即注册进线程名册,
            // alive 绑定记录自己的取消令牌 —— 暂停期被 Cancel 也在兜底片内收摊。
            std::optional<ThreadGovernor::Token> token;
            if (rec->kind != JobKind::Command) {
                token = ThreadGovernor::Attach(rec->name.c_str(), rec->kind, &rec->alive);
            }

            try {
                if (gate && !gate()) {
                    // 收尾拦截: fn 未执行 —— 如实落 Killed(不再有"槽位滞留但状态成谜")
                    MarkTerminal(rec, JobState::Killed);
                    return;
                }
                JobContext ctx(rec, std::move(token));
                fn(ctx);
                // 终态判定: 被取消 → Killed; 业务失败(ctx.Fail) → Failed; 否则 Completed。
                // (取消优先于失败: 操作员 kill 是更强的事实)
                MarkTerminal(rec, !rec->alive.load(std::memory_order_relaxed)
                    ? JobState::Killed
                    : (rec->fail_requested.load(std::memory_order_relaxed)
                        ? JobState::Failed : JobState::Completed));
            }
            catch (const std::exception& e) {
                CLEAT_LOGE(ErrDom::Job, ErrCode::UnhandledException, "JobRuntime: job '" << rec->name << "' 执行异常: " << e.what());
                MarkTerminal(rec, JobState::Failed);
            }
            catch (...) {
                CLEAT_LOGE(ErrDom::Job, ErrCode::UnhandledException, "JobRuntime: job '" << rec->name << "' 执行未知异常");
                MarkTerminal(rec, JobState::Failed);
            }
        }

        void PruneAt(uint64_t now_ms) {
            std::lock_guard<std::mutex> lock(g_registry->m);
            auto& recs = g_registry->records;

            // ① TTL: 已终结且超过保留期 → 裁剪
            for (auto it = recs.begin(); it != recs.end();) {
                const auto& rec = it->second;
                const uint64_t ended = rec->ended_ms.load(std::memory_order_relaxed);
                if (rec->finished.load(std::memory_order_relaxed) && ended != 0 &&
                    now_ms > ended && now_ms - ended > g_registry->ttl_ms) {
                    it = recs.erase(it);
                }
                else {
                    ++it;
                }
            }

            // ② 条数上限: 超出时从**最老的终态记录**开始裁(在跑 job 永不裁剪)
            if (recs.size() > g_registry->cap) {
                std::vector<std::pair<uint64_t, uint64_t>> ended_recs;   // (ended_ms, id)
                for (const auto& [id, rec] : recs) {
                    const uint64_t ended = rec->ended_ms.load(std::memory_order_relaxed);
                    if (rec->finished.load(std::memory_order_relaxed) && ended != 0) {
                        ended_recs.emplace_back(ended, id);
                    }
                }
                std::sort(ended_recs.begin(), ended_recs.end());
                size_t over = recs.size() - g_registry->cap;
                for (const auto& [ended, id] : ended_recs) {
                    if (over == 0) break;
                    recs.erase(id);
                    --over;
                }
            }
        }

    } // namespace

    JobHandle JobRuntime::Spawn(const JobSpec& spec, JobFn fn) {
        // 收尾期拒绝出生: 不得新起循环(调用方据此回错误帧/留痕)
        if (ThreadGovernor::Stopping()) {
            LOG_WARN("JobRuntime: 收尾期拒绝出生新 job '" << spec.name << "'");
            return {};
        }

        uint64_t id = 0;
        {
            std::lock_guard<std::mutex> lock(g_registry->m);
            id = g_registry->next_id++;
        }
        auto rec = std::make_shared<JobRecord>(id, spec.name, spec.kind,
            spec.deadline_ms, spec.key, spec.correlation,
            spec.high_bandwidth, spec.attachment);
        rec->on_timeout = spec.on_timeout;
        {
            std::lock_guard<std::mutex> lock(g_registry->m);
            g_registry->records[id] = rec;
            if (spec.correlation != 0) {
                g_registry->by_correlation[spec.correlation] = id;
            }
        }

        // 起线程**之前**回填响应侧槽位(线程内读它无竞态); 状态先行置 Running ——
        // 若放在建线程之后, 快速结束的 job 可能先落 Completed 再被覆盖回 Running。
        if (spec.slot) spec.slot->record = rec;
        rec->state.store(JobState::Running, std::memory_order_release);

        try {
            std::thread th([rec, gate = spec.gate, on_finished = spec.on_finished,
                fn = std::move(fn)]() {
                    RunJob(rec, gate, on_finished, fn);
                });
            th.detach();
            LOG_DEBUG("JobRuntime: spawn #" << id << " '" << spec.name
                << "' (" << JobKindName(spec.kind) << ")");
            return JobHandle(rec);
        }
        catch (const std::exception& e) {
            CLEAT_LOGE(ErrDom::Job, ErrCode::ResourceExhausted, "JobRuntime: 创建线程失败 '" << spec.name << "': " << e.what());
            MarkTerminal(rec, JobState::Failed);
            return {};
        }
        catch (...) {
            CLEAT_LOGE(ErrDom::Job, ErrCode::UnhandledException, "JobRuntime: 创建线程未知异常 '" << spec.name << "'");
            MarkTerminal(rec, JobState::Failed);
            return {};
        }
    }

    void JobRuntime::NoteTimeout(uint64_t correlation) {
        std::shared_ptr<JobRecord> rec;
        {
            std::lock_guard<std::mutex> lock(g_registry->m);
            const auto it = g_registry->by_correlation.find(correlation);
            if (it == g_registry->by_correlation.end()) return;
            const auto rit = g_registry->records.find(it->second);
            if (rit == g_registry->records.end()) return;
            rec = rit->second;
        }

        // 状态守卫: 只有 Running/Stopping → TimedOut 才上报(恰好一次; 已终结的竞态窗口
        // 下静默 —— 与旧实现"guards_ 查不到即跳过"同语义)。
        const JobState cur = rec->state.load(std::memory_order_relaxed);
        if (cur != JobState::Running && cur != JobState::Stopping) return;

        rec->state.store(JobState::TimedOut, std::memory_order_release);
        if (rec->on_timeout) {
            try {
                rec->on_timeout();
            }
            catch (...) {
                CLEAT_LOGE(ErrDom::Job, ErrCode::UnhandledException, "JobRuntime: 超时回调异常 '" << rec->name << "'");
            }
        }
    }

    void JobRuntime::PruneTerminalHistory() {
        PruneAt(JobNowMs());
    }

    std::vector<JobSnapshot> JobRuntime::List() {
        const uint64_t now = JobNowMs();
        std::vector<JobSnapshot> out;
        std::lock_guard<std::mutex> lock(g_registry->m);
        out.reserve(g_registry->records.size());
        for (const auto& [id, rec] : g_registry->records) {
            out.push_back(SnapshotOf(rec, now));
        }
        std::sort(out.begin(), out.end(),
            [](const JobSnapshot& a, const JobSnapshot& b) { return a.id < b.id; });
        return out;
    }

    bool JobRuntime::StatusSnapshot(uint64_t id, JobSnapshot& out) {
        const uint64_t now = JobNowMs();
        std::lock_guard<std::mutex> lock(g_registry->m);
        const auto it = g_registry->records.find(id);
        if (it == g_registry->records.end()) return false;
        out = SnapshotOf(it->second, now);
        return true;
    }

    std::vector<JobSnapshot> JobRuntime::FindByName(const std::string& name) {
        const uint64_t now = JobNowMs();
        std::vector<JobSnapshot> out;
        std::lock_guard<std::mutex> lock(g_registry->m);
        for (const auto& [id, rec] : g_registry->records) {
            if (rec->name == name) out.push_back(SnapshotOf(rec, now));
        }
        std::sort(out.begin(), out.end(),
            [](const JobSnapshot& a, const JobSnapshot& b) { return a.id < b.id; });
        return out;
    }

    bool JobRuntime::Cancel(uint64_t id) {
        std::shared_ptr<JobRecord> rec;
        {
            std::lock_guard<std::mutex> lock(g_registry->m);
            const auto it = g_registry->records.find(id);
            if (it == g_registry->records.end()) return false;
            rec = it->second;
        }
        if (rec->finished.load(std::memory_order_relaxed)) return false;   // 已结束: 如实拒绝

        // 取消令牌 + 唤醒可打断等待。状态用 CAS 只认 Running → Stopping:
        // 与 MarkTerminal 的终态写并发时, 终态优先(不会把已完成写成 Stopping)。
        // 丢唤醒防线: alive 置位必须在 rec->m 锁内完成 ——
        // SleepFor 的谓词检查与"原子放锁 + 登记等待者"之间若不与置位互斥, notify 会落进
        // 该窗口被吞掉, 被取消方睡满整个剩余时长才收摊(实测: 取消后 10s 睡眠的 job 未在
        // 3s 内退出 → WaitUntil 超时)。置位后 notify 无需持锁(标准保证: 只要修改与谓词
        // 检查经同一互斥量互斥, 唤醒必达)。
        {
            std::lock_guard<std::mutex> lock(rec->m);
            rec->alive.store(false, std::memory_order_release);
        }
        JobState expected = JobState::Running;
        rec->state.compare_exchange_strong(expected, JobState::Stopping);
        rec->cv.notify_all();
        LOG_INFO("JobRuntime: 受理停止请求 #" << id << " '" << rec->name << "'(协作式)");
        return true;
    }

    int JobRuntime::CancelByName(const std::string& name) {
        std::vector<uint64_t> ids;
        {
            std::lock_guard<std::mutex> lock(g_registry->m);
            for (const auto& [id, rec] : g_registry->records) {
                if (rec->name == name && !rec->finished.load(std::memory_order_relaxed)) {
                    ids.push_back(id);
                }
            }
        }
        if (ids.empty()) return 0;
        if (ids.size() > 1) return -1;   // 歧义: 不猜"杀最新"
        return Cancel(ids[0]) ? 1 : 0;
    }

    void JobRuntime::SetDataSender(DataSender sender) {
        std::lock_guard<std::mutex> lock(g_di->m);
        g_di->sender = std::move(sender);
    }

    void JobRuntime::SetLowBandwidthCheck(BoolCheck check) {
        std::lock_guard<std::mutex> lock(g_di->m);
        g_di->low_bandwidth_check = std::move(check);
    }

    bool JobRuntime::Emit(const std::vector<uint8_t>& frame) {
        JobRuntime::DataSender sender;
        {
            std::lock_guard<std::mutex> lock(g_di->m);
            sender = g_di->sender;
        }
        if (!sender) {
            LOG_WARN("JobRuntime: 数据出口未接线, 丢弃 " << frame.size() << " 字节");
            return false;
        }
        return sender(frame);
    }

    JobRuntime::TaskStart JobRuntime::StartTask(const JobSpec& spec, JobFn fn) {
        if (spec.high_bandwidth) {
            JobRuntime::BoolCheck check;
            {
                std::lock_guard<std::mutex> lock(g_di->m);
                check = g_di->low_bandwidth_check;
            }
            if (check && check()) {
                LOG_WARN("JobRuntime: 低带宽通道, 拒绝启动高带宽任务 '" << spec.name << "'");
                return { TaskStart::Result::LowBandwidthRefused, {} };
            }
        }
        auto handle = Spawn(spec, std::move(fn));
        if (!handle) return { TaskStart::Result::Refused, {} };
        return { TaskStart::Result::Started, std::move(handle) };
    }

    std::shared_ptr<void> JobRuntime::AttachmentOf(uint64_t id) {
        std::lock_guard<std::mutex> lock(g_registry->m);
        const auto it = g_registry->records.find(id);
        if (it == g_registry->records.end()) return nullptr;
        return it->second->attachment;
    }

    bool JobRuntime::Finish(uint64_t id, JobState end) {
        std::shared_ptr<JobRecord> rec;
        {
            std::lock_guard<std::mutex> lock(g_registry->m);
            const auto it = g_registry->records.find(id);
            if (it == g_registry->records.end()) return false;
            rec = it->second;
        }
        if (rec->finished.load(std::memory_order_relaxed)) return false;   // 幂等拒绝
        MarkTerminal(rec, end);
        return true;
    }

    size_t JobRuntime::CancelAllOfKind(JobKind kind, uint32_t drain_ms) {
        std::vector<uint64_t> ids;
        {
            std::lock_guard<std::mutex> lock(g_registry->m);
            for (const auto& [id, rec] : g_registry->records) {
                if (rec->kind == kind && !rec->finished.load(std::memory_order_relaxed)) {
                    ids.push_back(id);
                }
            }
        }
        for (uint64_t id : ids) Cancel(id);

        // 有界排水: 轮询 finished(10ms 步进) —— 与 同款"宽限不是正确性机制":
        // 正确性由"取消令牌 + 永不析构的记录/出口持有者"保证, 排水只是尽力等干净。
        const auto deadline = std::chrono::steady_clock::now()
            + std::chrono::milliseconds(drain_ms);
        for (;;) {
            size_t remaining = 0;
            {
                std::lock_guard<std::mutex> lock(g_registry->m);
                for (uint64_t id : ids) {
                    const auto it = g_registry->records.find(id);
                    if (it != g_registry->records.end() &&
                        !it->second->finished.load(std::memory_order_relaxed)) {
                        ++remaining;
                    }
                }
            }
            if (remaining == 0) return 0;
            if (std::chrono::steady_clock::now() >= deadline) {
                LOG_WARN("JobRuntime: 排水超时(" << drain_ms << "ms), 仍有 " << remaining
                    << " 个 '" << JobKindName(kind) << "' job 未退出 —— 继续收停"
                    << "(它们已持有取消令牌)");
                return remaining;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }

    size_t JobRuntime::CancelHighBandwidthOfKind(JobKind kind) {
        std::vector<uint64_t> ids;
        {
            std::lock_guard<std::mutex> lock(g_registry->m);
            for (const auto& [id, rec] : g_registry->records) {
                if (rec->kind == kind && rec->high_bandwidth &&
                    !rec->finished.load(std::memory_order_relaxed)) {
                    ids.push_back(id);
                }
            }
        }
        for (uint64_t id : ids) Cancel(id);
        return ids.size();
    }

    void JobRuntime::RegisterService(const std::string& name, bool high_bandwidth,
        ServiceFactory factory) {
        std::lock_guard<std::mutex> lock(g_registry->m);
        g_registry->services[name] = ServiceCatalogEntry{ high_bandwidth, std::move(factory) };
        LOG_DEBUG("JobRuntime: 服务目录登记 '" << name << "'"
            << (high_bandwidth ? " (high_bandwidth)" : ""));
    }

    JobRuntime::TaskStart JobRuntime::ServiceStart(const std::string& name,
        const nlohmann::json& params) {
        using R = TaskStart::Result;

        // ① 目录查找 + 单例检查 + **预约**(同一临界区 —— 实测 
        // 原实现"检查"与"插入 starting"是两段锁, 并发双开可双双通过检查再各自
        // 预约成功, 单例防线形同虚设; 现检查与预约原子, 锁外跑工厂的纪律不变)
        ServiceCatalogEntry entry;
        {
            std::lock_guard<std::mutex> lock(g_registry->m);
            const auto cit = g_registry->services.find(name);
            if (cit == g_registry->services.end()) {
                LOG_WARN("JobRuntime: 服务未注册: " << name);
                return { R::NotFound, {} };
            }
            entry = cit->second;
            for (const auto& [id, rec] : g_registry->records) {
                if (rec->kind == JobKind::Service && rec->name == name &&
                    !rec->finished.load(std::memory_order_relaxed)) {
                    LOG_WARN("JobRuntime: 服务已在运行: " << name);
                    return { R::AlreadyRunning, {} };
                }
            }
            if (g_registry->starting.count(name)) {
                LOG_WARN("JobRuntime: 服务启动中(并发双开拒绝): " << name);
                return { R::AlreadyRunning, {} };
            }
            g_registry->starting.insert(name);
        }
        // 预约清理 RAII 必须紧跟预约(先于双门): LowBandwidthRefused/HttpsRefused 的
        // 提前返回也要清预约, 否则下一条 Start 会被"启动中"误拒(实测被
        // Phase3_服务双门用例抓住 —— 实测 修复的首版回归)。
        struct ClearStarting {
            ~ClearStarting() {
                std::lock_guard<std::mutex> lock(g_registry->m);
                g_registry->starting.erase(name_);
            }
            const std::string& name_;
        } clear_starting{ name };

        // ② 高带宽双门(锁外调检查 —— 检查会进 Chain, 不得持名册锁, BandwidthTxn 同纪律)
        if (entry.high_bandwidth) {
            JobRuntime::BoolCheck low_bw, https;
            {
                std::lock_guard<std::mutex> lock(g_di->m);
                low_bw = g_di->low_bandwidth_check;
                https = g_di->https_forbidden;
            }
            if (low_bw && low_bw()) {
                LOG_WARN("JobRuntime: 低带宽通道, 拒绝启动流式服务 " << name);
                return { R::LowBandwidthRefused, {} };
            }
            if (https && https()) {
                LOG_WARN("JobRuntime: https 传输, 拒绝启动流式服务 " << name);
                return { R::HttpsRefused, {} };
            }
        }

        // ③ 工厂(锁外; 预约已在①原子完成, ClearStarting RAII 覆盖其后所有退出路径)
        ServiceJob sj;
        try {
            sj = entry.factory(params);
        }
        catch (const std::exception& e) {
            CLEAT_LOGE(ErrDom::Job, ErrCode::UnhandledException, "JobRuntime: 服务工厂异常 '" << name << "': " << e.what());
            return { R::StartFailed, {} };
        }
        catch (...) {
            CLEAT_LOGE(ErrDom::Job, ErrCode::UnhandledException, "JobRuntime: 服务工厂未知异常 '" << name << "'");
            return { R::StartFailed, {} };
        }
        if (!sj.fn) return { R::StartFailed, {} };

        // ④ 出生(kind=Service → 自动进线程名册; roster_mismatch 对账生效)
        JobSpec spec;
        spec.name = name;
        spec.kind = JobKind::Service;
        spec.high_bandwidth = entry.high_bandwidth;
        spec.attachment = sj.attachment;
        auto handle = Spawn(spec, std::move(sj.fn));
        if (!handle) return { R::Refused, {} };
        LOG_INFO("JobRuntime: 服务启动成功 '" << name << "' (#" << handle.id() << ")");
        return { R::Started, std::move(handle) };
    }

    bool JobRuntime::ServiceStop(const std::string& name) {
        std::vector<uint64_t> ids;
        {
            std::lock_guard<std::mutex> lock(g_registry->m);
            for (const auto& [id, rec] : g_registry->records) {
                if (rec->kind == JobKind::Service && rec->name == name &&
                    !rec->finished.load(std::memory_order_relaxed)) {
                    ids.push_back(id);
                }
            }
        }
        if (ids.empty()) {
            LOG_WARN("JobRuntime: 服务未运行: " << name);
            return false;
        }
        for (uint64_t id : ids) Cancel(id);   // 单例约束下至多 1 条; 防御性全收
        return true;
    }

    size_t JobRuntime::StopStreamServices() {
        //  修正(恢复  修复的原始语义): **不做低带宽/https 门检查**。
        // 本函数由"带宽观察者(进入 Low)"与"会话重连观察者(离开有链路态)"共用 ——
        // (8127a92)曾给它加 (low_bw || https) 门, 而该门在 TCP/High 断链
        // 路径恒为 false → **什么都不停**: 桌面/摄像头/录音在服务器掉线期间继续采集
        // 编码(白白烧 CPU、塞满出站队列), 恢复重连后又自动续播(操作员未重启即可
        // 再次收到桌面帧)—— 与  修复(7fcdec2: 断链即停、不自动重启)意图相悖,
        // 也与任务侧(无条件取消)不对称。
        // 停止**不设门**是对的: "能不能启动"由 ServiceStart 双门在启动时裁决;
        // "链路不可用该不该停"的答案恒为停。
        std::vector<uint64_t> ids;
        {
            std::lock_guard<std::mutex> lock(g_registry->m);
            for (const auto& [id, rec] : g_registry->records) {
                if (rec->kind == JobKind::Service && rec->high_bandwidth &&
                    !rec->finished.load(std::memory_order_relaxed)) {
                    ids.push_back(id);
                }
            }
        }
        for (uint64_t id : ids) Cancel(id);
        return ids.size();
    }

    std::shared_ptr<void> JobRuntime::RunningServiceAttachment(const std::string& name) {
        std::lock_guard<std::mutex> lock(g_registry->m);
        for (const auto& [id, rec] : g_registry->records) {
            if (rec->kind == JobKind::Service && rec->name == name &&
                !rec->finished.load(std::memory_order_relaxed)) {
                return rec->attachment;
            }
        }
        return nullptr;
    }

    void JobRuntime::SetHttpsStreamForbiddenCheck(BoolCheck check) {
        std::lock_guard<std::mutex> lock(g_di->m);
        g_di->https_forbidden = std::move(check);
    }

    bool JobRuntime::HttpsStreamForbidden() {
        JobRuntime::BoolCheck check;
        {
            std::lock_guard<std::mutex> lock(g_di->m);
            check = g_di->https_forbidden;
        }
        return check ? check() : false;
    }

    void JobRuntime::SetNetworkCheck(BoolCheck check) {
        std::lock_guard<std::mutex> lock(g_di->m);
        g_di->network_check = std::move(check);
    }

    bool JobRuntime::NetworkConnected() {
        JobRuntime::BoolCheck check;
        {
            std::lock_guard<std::mutex> lock(g_di->m);
            check = g_di->network_check;
        }
        return check ? check() : false;   // 未接线 = 未连接(与旧 ServiceManager 同语义, 不谎报)
    }

    IServiceHost& JobRuntime::Host() { return *g_host; }

    void JobRuntime::ResetForTests() {
        {
            std::lock_guard<std::mutex> lock(g_di->m);
            g_di->sender = nullptr;
            g_di->low_bandwidth_check = nullptr;
            g_di->https_forbidden = nullptr;
            g_di->network_check = nullptr;
        }
        std::lock_guard<std::mutex> lock(g_registry->m);
        // 只清登记; 在跑线程持有的 shared_ptr 仍有效(其落终态照常, 只是不再被查询到)
        g_registry->records.clear();
        g_registry->by_correlation.clear();
        g_registry->services.clear();
        g_registry->starting.clear();
        g_registry->ttl_ms = kDefaultTtlMs;
        g_registry->cap = kDefaultCap;
    }

    void JobRuntime::SetHistoryLimitsForTests(uint64_t ttl_ms, size_t cap) {
        std::lock_guard<std::mutex> lock(g_registry->m);
        g_registry->ttl_ms = ttl_ms == 0 ? kDefaultTtlMs : ttl_ms;
        g_registry->cap = cap == 0 ? kDefaultCap : cap;
    }

} // namespace cleat
