#pragma once
/**
 * @file ThreadGovernor.hpp
 * @brief 后台活动统一治理：注册名册 + 协作式静默/退出门闸
 * @details
 * 定位是**静默门闸**，不是线程生命周期的管理者：不起线程、不 join、不杀、不唤醒，
 * 只回答三个问题 —— 谁在跑（List）、现在该不该干活（Checkpoint）、要不要收摊（Stopping）。
 * 每个后台循环自己决定怎么服从（粗睡 / continue / 退出），与 SessionMachine /
 * ExecutionTracker 的"每拍问一句"哲学一致。
 * OPSEC 不变量**不在本类**：窗外"零出站"由链路断开 + `OutboundSink` 唯一出口 +
 * R7（重连握手前作废积压帧）保证（《ThreadGovernor设计复盘与改造方案(第二版)》）。
 *
 * 状态机（唯一真源 = 一个三态字，见 cpp 的 Gate）：
 *   Running  → Checkpoint 放行；
 *   Paused   → 在门闸内**事件等待**（恢复→放行 / 收尾→收摊 / 1s 兜底片重查），暂停不确定期限；
 *   Stopping → **终态**，不可逆，且**支配 Paused** —— 暂停不可能残留进收尾（P3）。
 * 转移只有两个写点：主循环 `SetPaused`；停止入口 `SetStopping` / `RequestExit`。
 * 转移用 CAS 实现（只认合法来源态）—— 信号处理器在无锁下直接写终态，有锁的
 * "读-检查-写"会与之竞争，把刚置上的终态清回去（停止请求被静默吞掉）。
 *
 *
 * 信号安全：`SetStoppingSignalSafe()` 只做一次锁自由原子写，**信号处理器专用**
 * （不 lock / 不 notify）；等待者由 1s 兜底片、或主循环并轨的 notify 唤醒 ——
 *
 * 静态析构期析构 Token，锁一个已析构的 mutex 即 UAF）；热路径标志是锁自由原子，
 * 不依赖任何函数内静态单例的存活（CommandWorker 同款理由；交付构建的
 * /Zc:threadSafeInit- 下魔法静态既非线程安全也不可依赖）。故本类全部走**静态方法**，
 * 不设 Instance()。
 */

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace cleat {

    struct UnitInfo;   // 注册项（cpp 内定义；Token 类型擦除持有）

    /// 后台活动分类（审计用；决定服从义务：Ephemeral 只登记不服从）
    enum class JobKind : uint8_t {
        Main,           ///< 主循环（SessionLoop）
        Infrastructure, ///< 框架常驻（cmd worker/reaper、rportfwd accept、pivot 泵）
        Service,        ///< 常驻服务（剪贴板/键盘/文件监控/流媒体/shell）
        Task,           ///< 任务型（文件分块下载等）
        Ephemeral,      ///< 一次性 detached（migrate 守护）——只登记，允许自然结束
        /// 命令执行线程（JobCenter 登记用；**不进本名册**）——同步 handler 无协作停车点，
        /// 不接 Checkpoint：静默由"派发循环不开新工 + OutboundSink 出口拒发 + deadline"承担
        /// 。
        Command,
    };

    /// 审计快照里的 unit 状态（全局门闸的投影，非线程阻塞位置）
    enum class UnitState : uint8_t { Running, Paused, Stopping };

    struct UnitSnapshot {
        std::string name;
        JobKind kind;
        UnitState state;
    };

    class ThreadGovernor {
    public:
        /// RAII 注册句柄：构造即入册，析构即注销。只移动（防意外拷贝延长注册期）。
        class Token {
        public:
            ~Token();
            Token(Token&& other) noexcept;
            Token& operator=(Token&& other) noexcept;
            Token(const Token&) = delete;
            Token& operator=(const Token&) = delete;

            /**
             * @brief 每拍调用（循环顶部）。
             * @return false = 已请求收摊（Stopping，或本 unit 已自停），循环须立即 break；
             *         true = 继续本拍工作。
             * @details 暂停期在门闸内**事件等待**（cv + 谓词）；恢复后返回 true，收尾则返回 false。
             *          **判定纪律**：只有 Running / Stopping 是判决 —— "仍是 Paused"（含 1s
             *          兜底片超时）一律回到等待。超时只是兜底重查，不是"该停止"。
             *          快路径（Running/Stopping）无锁；只有 Paused 进慢路径（取锁等待）。
             *          **存活绑定**（见 `Attach` 的 `alive`）：暂停期该 unit 自停（alive=false）
             *          也会在兜底片内返回 false —— "暂停期单停一个服务"的 Stop()->join
             *          不必等到窗口重开。
             */
            bool Checkpoint() const;

            /// 纯查询不阻塞：暂停中返回 true。丢弃型接入点据此丢弃采集（不积不存）。
            bool Paused() const;

        private:
            friend class ThreadGovernor;
            Token() = default;
            explicit Token(std::shared_ptr<UnitInfo> unit) : unit_(std::move(unit)) {}
            std::shared_ptr<UnitInfo> unit_;   ///< 空 = 已注销 / moved-from
        };

        /**
         * @brief 对象式接入（P7）：一个对象同时承载"注册 + 存活绑定 + 每拍判决"。
         * @details 新接入一律用本对象（取代"裸 Attach + 手写 Checkpoint"组合）：
         *            ThreadGovernor::GovLoop gov("svc-audio", JobKind::Service, &running_);
         *            while (running_.load()) {
         *                if (!gov.Tick()) break;
         *                ...
         *            }
         *          收益：接入动作是"**必须持有这个对象**"（Tick 不可能脱离注册）；unit 自己的
         *          存活标志随构造绑定，暂停期自停也会被 `Tick()` 收摊（见 `Attach` 的 `alive`）。
         */
        class GovLoop {
        public:
            GovLoop(const char* name, JobKind kind, const std::atomic<bool>* alive = nullptr)
                : token_(ThreadGovernor::Attach(name, kind, alive)) {}

            GovLoop(GovLoop&&) = default;
            GovLoop& operator=(GovLoop&&) = default;
            GovLoop(const GovLoop&) = delete;
            GovLoop& operator=(const GovLoop&) = delete;

            /// 每拍判决（等价 `Token::Checkpoint()`）：false = 收摊，循环须立即 break。
            bool Tick() const { return token_.Checkpoint(); }

            /// 纯查询不阻塞（等价 `Token::Paused()`）。
            bool Paused() const { return token_.Paused(); }

        private:
            Token token_;
        };

        // ---- 全局状态（静态方法；状态为文件级"永不析构"对象，见文件头说明） ----

        /**
         * @brief 注册当前后台活动。name 供 List/审计；kind 分类。
         * @param alive 可选：unit 自己的存活标志（如服务的 `running_`）—— 绑定后，暂停期
         *              该标志转 false 会让 `Checkpoint()` 在兜底片内收摊（P5 类别②）。
         *              为什么必须有它：`ServiceManager::StopService` 是**持锁 join**
         *              （持 `mutex_` 调 `service->Stop()` 再 join worker），若 worker 正停在
         *              门闸的暂停等待里，join 要等窗口重开 —— 冻的不是一个线程，是整个
         *              ServiceManager 互斥面。绑定点后该场景的延迟上限 = 1s 兜底片。
         *              指针须在 Token 生命周期内有效（通常指向对象的成员原子，天然满足）；
         *              nullptr = 不绑定（旧行为）。
         */
        static Token Attach(const char* name, JobKind kind,
            const std::atomic<bool>* alive = nullptr);

        /**
         * @brief 置全局暂停（唯一合法转移：Running↔Paused；对 Stopping 一律忽略）。
         * @details **仅主循环的 LifecycleGate 消费点调用**。
         *          转移用 CAS：信号路径可以在无锁下直接写终态，有锁的"读-检查-写"
         *          会与之竞争把终态清回去 —— 终态优先（P3）必须由原子 RMW 保证。
         */
        static void SetPaused(bool on);
        static bool Paused();

        /**
         * @brief 置"收尾中"终态（普通线程入口）：所有 `Checkpoint()` 立即收摊/立即返回。
         * @details 本地停止路径（`AgentContext::Stop/Shutdown`）必须在 **join 任何后台线程
         *          本函数 = 写状态字 + notify（唤醒全部等待者，常态 µs 级）。
         */
        static void SetStopping();

        /**
         * @brief `SetStopping` 的信号安全形态：**只做一次锁自由原子写**，不 lock / 不 notify。
         * @details 信号处理器专用（`EntryExe.cpp`）。三态字是锁自由原子（与旧 `g_stopping.store`
         *          或主循环并轨的 notify 唤醒 —— 正确性不依赖并轨。
         */
        static void SetStoppingSignalSafe();
        static bool Stopping();

        /**
         * @brief 请求全进程退出：置门闸终态**并并轨** SessionIdentity::RequestExit。
         * @details 主循环的既有 ShouldExit 检查与任务线程的开工前检查因此自动服从；
         *          ExitProgram 命令与 killdate（批B LifecycleGate）都走这里。
         *          仅运行期调用（静态析构期不可达 —— 与 CommandWorker 同约束）。
         */
        static void RequestExit();

        static std::vector<UnitSnapshot> List();

        /// 单测进程内全局状态复位（测试单进程共享全局；生产禁止调用）
        static void ResetForTests();

    private:
        ThreadGovernor() = delete;
    };

} // namespace cleat
