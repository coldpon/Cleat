#include "Core/Concurrency/ThreadGovernor.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <utility>

#include "Core/Identity/SessionIdentity.hpp"

namespace cleat {

    struct UnitInfo {
        std::string name;
        JobKind kind = JobKind::Task;
        /// 可选: unit 自己的存活标志(P5 类别②) —— 暂停期它转 false 时 Checkpoint 立即收摊,
        /// 使"暂停期单停一个服务"的 Stop()->join 不必等窗口重开(见 Checkpoint 慢路径)。
        const std::atomic<bool>* alive = nullptr;
    };

    namespace {

        // ---- 三态状态机(P1/P2/P3) ----

        constexpr uint8_t kRunning = 0;
        constexpr uint8_t kPaused = 1;
        constexpr uint8_t kStopping = 2;   ///< 终态, 不可逆, 支配 Paused

        /**
         * 门闸: 状态字 + 事件通知。**永不析构**(堆上, 不 delete):
         * Checkpoint 可能在静态析构期被 detached 线程调用(migrate 守护), 对象必须活着
         * 静态(交付构建 /Zc:threadSafeInit- 下魔法静态既非线程安全也不可依赖, 教训)。
         */
        struct Gate {
            std::mutex m;
            std::condition_variable cv;
            std::atomic<uint8_t> state{ kRunning };
        };
        Gate* const g_gate = new Gate{};

        /// 等待兜底片: 漏通知/虚假唤醒的兜底重查周期。**它不是"暂停期限"** —— 见 Checkpoint。
        constexpr auto kWaitSlice = std::chrono::milliseconds(1000);

        /**
         * 静态析构期析构 Token —— 若名册已析构, 锁一个死掉的 mutex 即 UAF。
         */
        struct Registry {
            std::mutex m;
            std::vector<std::shared_ptr<UnitInfo>> units;
        };
        Registry* const g_registry = new Registry{};

        void UnregisterLocked(const std::shared_ptr<UnitInfo>& unit) {
            auto& units = g_registry->units;
            units.erase(std::remove(units.begin(), units.end(), unit), units.end());
        }

        /**
         * 唤醒端: 状态字已改, 这里只负责叫醒等待者。
         * 先改状态、后取锁通知 —— 等待者要么在睡下前看到新状态(不睡), 要么通知在其
         * release 锁后到达, 不漏唤醒; 另有 1s 兜底片再兜一层(含信号路径的无通知写)。
         */
        void NotifyWaiters() {
            std::lock_guard<std::mutex> lock(g_gate->m);
            g_gate->cv.notify_all();
        }

    } // namespace

    // ---------------- Token ----------------

    ThreadGovernor::Token::~Token() {
        if (!unit_) return;
        std::lock_guard<std::mutex> lock(g_registry->m);
        UnregisterLocked(unit_);
        unit_.reset();
    }

    ThreadGovernor::Token::Token(Token&& other) noexcept
        : unit_(std::move(other.unit_)) {}

    ThreadGovernor::Token& ThreadGovernor::Token::operator=(Token&& other) noexcept {
        if (this != &other) {
            if (unit_) {
                // 先注销自身注册再接管(自赋值以外的重绑场景: 换 token 不留双份)
                std::lock_guard<std::mutex> lock(g_registry->m);
                UnregisterLocked(unit_);
            }
            unit_ = std::move(other.unit_);
        }
        return *this;
    }

    bool ThreadGovernor::Token::Checkpoint() const {
        // 快路径(热路径): 一次锁自由原子读。Running 放行 / Stopping 收摊, 都无需取锁。
        const uint8_t s = g_gate->state.load(std::memory_order_relaxed);
        if (s == kRunning) return true;
        if (s == kStopping) return false;

        // 慢路径: 只在 Paused 时进入 —— 事件驱动等待。**判定纪律**(第一版伪代码的坑):
        // 只有 Running / Stopping 是判决; "仍是 Paused"(含 1s 超时)一律回到等待。
        // 超时只是兜底重查(虚假唤醒 / 信号路径的无通知写 / 漏通知), 不是"该停止" ——
        // 若把超时当结论, 任何超过 1s 的暂停都会把所有循环判成收摊, 与"暂停不确定期限"
        // 直接矛盾(也正是 CommandWorker::WaitForSlot 注释里记过的"一个 bool 承载两个结论")。
        //
        // unit 存活判决(P5 类别②): 绑定 alive 的 unit 若在暂停期自停(running_=false),
        // 必须收摊 —— 否则它的 Stop()->join 要等到窗口重开(ServiceManager 还是持锁 join,
        // 冻整个服务面)。延迟上限 = 1s 兜底片(wait_for 超时即重查)。
        const std::atomic<bool>* const alive = unit_ ? unit_->alive : nullptr;
        std::unique_lock<std::mutex> lock(g_gate->m);
        for (;;) {
            const uint8_t cur = g_gate->state.load(std::memory_order_relaxed);
            if (cur == kRunning) return true;
            if (cur == kStopping) return false;
            if (alive && !alive->load(std::memory_order_relaxed)) return false;   // unit 已自停
            g_gate->cv.wait_for(lock, kWaitSlice);   // 仍是 Paused → 继续等(超时只是重查)
        }
    }

    bool ThreadGovernor::Token::Paused() const {
        return g_gate->state.load(std::memory_order_relaxed) == kPaused;
    }

    // ---------------- 全局状态 ----------------

    ThreadGovernor::Token ThreadGovernor::Attach(const char* name, JobKind kind,
        const std::atomic<bool>* alive) {
        auto unit = std::make_shared<UnitInfo>();
        unit->name = name ? name : "unnamed";
        unit->kind = kind;
        unit->alive = alive;
        std::lock_guard<std::mutex> lock(g_registry->m);
        g_registry->units.push_back(unit);
        return Token(std::move(unit));
    }

    void ThreadGovernor::SetPaused(bool on) {
        // 唯一合法转移: Running→Paused / Paused→Running。
        // CAS 而非"读-检查-写": 信号路径可在**无锁**下直接写终态, 有锁的读-检查-写仍会
        // 与之竞争, 把刚置上的 Stopping 清回去(停止请求被吞)。CAS 失败 = 当前不是合法
        // 来源态(多半已是终态) → 忽略; 终态优先(P3)。
        const uint8_t expect = on ? kRunning : kPaused;
        const uint8_t desired = on ? kPaused : kRunning;
        uint8_t cur = expect;
        if (g_gate->state.compare_exchange_strong(cur, desired, std::memory_order_acq_rel)) {
            NotifyWaiters();
        }
    }

    bool ThreadGovernor::Paused() {
        return g_gate->state.load(std::memory_order_relaxed) == kPaused;
    }

    void ThreadGovernor::SetStopping() {
        // 普通线程入口: 置终态 + 唤醒全部等待者(常态 µs 级; 停在暂停等待里的线程立即收摊)。
        SetStoppingSignalSafe();
        NotifyWaiters();
    }

    void ThreadGovernor::SetStoppingSignalSafe() {
        // 信号处理器专用: 只有一次锁自由原子写(不 lock/notify —— 处理器内不允许)。
        // Stopping 是终态且单调(任何状态 → Stopping 都合法, 且此后不可被覆盖:
        // SetPaused 的 CAS 只认 Running/Paused), 故无需 CAS, 直接 store。
        g_gate->state.store(kStopping, std::memory_order_release);
    }

    bool ThreadGovernor::Stopping() {
        return g_gate->state.load(std::memory_order_relaxed) == kStopping;
    }

    void ThreadGovernor::RequestExit() {
        // 置终态(与本地停止同源), 再并轨 SessionIdentity::RequestExit —— 主循环/任务线程的
        // 既有判据(ShouldExit)据此自动服从。仅运行期可达 —— 静态析构期 Instance() 本身即
        // UAF(CommandWorker 同约束), 调用方遵守。
        SetStopping();
        SessionIdentity::Instance().RequestExit();
    }

    std::vector<UnitSnapshot> ThreadGovernor::List() {
        const uint8_t s = g_gate->state.load(std::memory_order_relaxed);
        const UnitState st = (s == kStopping) ? UnitState::Stopping
                           : (s == kPaused)   ? UnitState::Paused
                                              : UnitState::Running;
        std::vector<UnitSnapshot> out;
        std::lock_guard<std::mutex> lock(g_registry->m);
        out.reserve(g_registry->units.size());
        for (const auto& unit : g_registry->units) {
            out.push_back({ unit->name, unit->kind, st });
        }
        return out;
    }

    void ThreadGovernor::ResetForTests() {
        g_gate->state.store(kRunning, std::memory_order_relaxed);
        NotifyWaiters();   // 唤醒停在门闸里的测试线程(否则要等 1s 兜底片)
        // 并轨的镜像: RequestExit 同步置位了 SessionIdentity 的退出标志, 复位必须
        // 一并清掉, 否则后续用例的任务线程开工前检查被短路(实测踩坑: e2e 响应超时)。
        SessionIdentity::Instance().ResetExitForTests();
        std::lock_guard<std::mutex> lock(g_registry->m);
        g_registry->units.clear();   // 存活 Token 的析构按"查无此项"安全降级
    }

} // namespace cleat
