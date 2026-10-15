#pragma once
/**
 * @file JobRuntime.hpp
 * @brief 任务运行时: 唯一出生口 + 登记表 + 查询/取消面
 * @details
 * 定位(三面模型的**控制面**):
 *   * 政策面(ThreadGovernor): "该不该干活/要不要收摊" —— 不动;
 *   * 控制面(本类): "存在哪些工作单元/状态/取消" —— 登记表 + 命令面;
 *   * 执行面(Spawn 的线程体): 出生即接线(门闸/取消/登记), 业务循环只见 JobContext。
 *
 * 职责边界:
 *   * **登记**: 所有经 Spawn 出生的 job 进一张表(单一口径, 取代 guards_ + 各 Manager map);
 *   * **驱动**: 终态落定由**线程自己**上报(不轮询); deadline/强制释放等**命令类槽位策略
 *     仍归其执行引擎(CommandWorker 自持的 ExecutionTracker)** —— 夹具按用例构造多实例,
 *     Phase 1 的边界注记);
 *
 * 线程体在静态析构期落终态/裁剪都安全。全部静态方法, 不设 Instance()。
 */

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "Common/Protocol/IServiceHost.hpp"
#include "Common/ThirdParty/json.hpp"

#include "Core/Job/JobContext.hpp"
#include "Core/Job/JobDefs.hpp"

namespace cleat {

    class JobRuntime {
    public:
        using JobFn = std::function<void(JobContext&)>;
        /// 任务/服务数据出口(装配注入; bool = 背压契约 B2-6: false = 未发出/积压)
        using DataSender = std::function<bool(const std::vector<uint8_t>&)>;
        using BoolCheck = std::function<bool()>;

        /// 启动结果(拒绝原因**可区分** ——  的"有损 bool"在此类被消灭)
        struct TaskStart {
            enum class Result {
                Started,              ///< 已出生
                NotFound,             ///< 服务目录查无此名
                AlreadyRunning,       ///< 同名单例已在跑(含启动中预约)
                LowBandwidthRefused,  ///< 高带宽 + 低带宽通道
                HttpsRefused,         ///< 高带宽服务 + https 传输(N44)
                StartFailed,          ///< 工厂/提供者启动失败(同步上报)
                Refused,              ///< 其余(收尾期拒绝出生等)
            };
            Result result = Result::Refused;
            JobHandle handle;
        };

        /**
         * @brief 服务工厂(Phase 3): 参数解析 + 提供者启动(在**命令线程**同步完成,
         *        失败返回空 fn → StartFailed —— 与旧 service->Start() 的同步 bool 语义一致)。
         * @details 返回的 attachment 是"服务句柄槽"(如 Shell 的命令入口), 操作员经
         *          `RunningServiceAttachment` 取回 —— 取代 GetService+static_cast。
         */
        struct ServiceJob {
            JobFn fn;
            std::shared_ptr<void> attachment;
        };
        using ServiceFactory = std::function<ServiceJob(const nlohmann::json& params)>;

        /**
         * @brief 唯一出生口: 登记 + 线程体接线(门闸/gate/终态/on_finished) + 建线程(detach)。
         * @param spec 出生规格(见 JobDefs.hpp; slot 在起线程**之前**回填)
         * @param fn   任务函数(非 Command 类的标准循环形见 JobContext.hpp)
         * @return 有效句柄 = 已出生; 空句柄 = 未出生(全局收尾期拒绝 / 线程创建失败,
         *         后者记录按 Failed 落终态留痕)。**不抛异常**(线程创建异常就地收口)。
         * @note COM 契约(Windows): 本口出生的线程一律**无 COM 公寓** —— 框架不代管,
         *       ThreadInitGuard 已于  删除, 全仓不存在"出生自带公寓"的特例。
         *       需要 COM 的 job 必须在调用点自备: 优先用 Platform::ScopedComApartment
         *       (Common/Platform/ComApartment.hpp, 单次调用作用域, 析构按判据配对释放);
         *       需要 MTA 的组件自行 CoInitializeEx(MTA) 并配对(范例 PlatformWin_EdrRecon)。
         *       公寓按线程生效, "别的线程已初始化过"在本口不构成任何保证。
         */
        static JobHandle Spawn(const JobSpec& spec, JobFn fn);

        // ---------------- 任务/服务 DI(Phase 2/3; 装配唯一注入点) ----------------

        /// 数据出口注入(AgentContextAssemble; 取代 TaskManager::SetGlobalDataCallback)。
        /// 出口对象随 AgentContext 生命周期 —— 任务线程的收尾由 CancelAllOfKind 的
        /// 有界排水兜住(), 与 同款取舍。
        static void SetDataSender(DataSender sender);

        /// 低带宽检查注入(取代 TaskManager::SetLowBandwidthCheck)。
        static void SetLowBandwidthCheck(BoolCheck check);

        /**
         * @brief 任务/服务的出站唯一口(ctx.Send 与会话对象都走这里)。
         * @return 出口的 bool(背压契约); 出口未接线 = false + 告警。
         */
        static bool Emit(const std::vector<uint8_t>& frame);

        // ---------------- 服务目录(Phase 3; 取代 ServiceManager) ----------------

        /**
         * @brief 登记具名服务(装配期一次; 取代 ServiceManager::RegisterService)。
         * @param high_bandwidth 流式服务(桌面/摄像头/录音): 低带宽通道拒绝启动 +
         *        降级时批量取消 + https 下经 ctx.Send 第一帧自停(N44)
         */
        static void RegisterService(const std::string& name, bool high_bandwidth,
            ServiceFactory factory);

        /**
         * @brief 启动具名服务(单例 + 双门 + 工厂同步启动)。
         * @details 顺序与旧 ServiceManager::StartService 一致: 查目录 → 单例检查 →
         *          高带宽双门 → 工厂(提供者启动, 失败 = StartFailed) → Spawn。
         *          单例用"启动中预约"防并发双开(工厂在锁外跑, 预约覆盖该窗口)。
         */
        static TaskStart ServiceStart(const std::string& name, const nlohmann::json& params);

        /// 停止具名服务(协作式取消; 未在跑返回 false —— 与旧 StopService 同语义)
        static bool ServiceStop(const std::string& name);

        /// 取运行中服务的句柄槽(未在跑 → 空; 终态历史不可用 —— 与"服务已停"同义)
        static std::shared_ptr<void> RunningServiceAttachment(const std::string& name);

        /**
         * @brief 流服务收停(**无条件**): 停止全部在跑的高带宽服务 —— 带宽观察者(Low)与
         *        会话重连观察者(离开有链路态)共用; 不自动重启(由操作员显式启动)。
         * @return 受理取消的条数。
         * @details  修正: 停止**不设门**(旧 ServiceManager::StopHighBandwidthServices
         *          在此等价语义下恢复)。此处曾按"低带宽或 https"做门检查, 使 TCP/High
         *          重连路径不停任何流服务( 修复被抵消): 桌面/摄像头/录音断链期间继续
         *          产出、恢复后自动续播。启动的裁决在 ServiceStart 的双门( 语义不受
         *          影响); 链路不可用时, 高带宽生产者没有继续产出的理由。
         */
        static size_t StopStreamServices();

        /// https 流式禁止检查注入(N44; 取代 ServiceManager::SetHttpsStreamForbiddenCheck)
        static void SetHttpsStreamForbiddenCheck(BoolCheck check);
        static bool HttpsStreamForbidden();

        /// 网络状态检查注入(取代 ServiceManager::SetNetworkCheck)
        static void SetNetworkCheck(BoolCheck check);
        static bool NetworkConnected();

        /**
         * @brief 服务可用的宿主面(IServiceHost shim; 取代"服务类持有 ServiceManager&")。
         * @details IsNetworkConnected → NetworkConnected(); SendData → Emit()。
         *          提供者代码零改动 —— 服务 job 函数把它递给需要 host 的提供者。
         */
        static IServiceHost& Host();

        /**
         * @brief 任务类出生口(高带宽门 + Spawn)。
         * @details 高带宽 job 在低带宽通道(DNS/ICMP)下拒绝启动 —— 与旧
         *          TaskManager::StartTask 同语义, 但拒绝**可区分**(N20: 旧实现返回
         *          空串被 handler 折叠成假成功)。其余拒绝(收尾期) = Refused。
         */
        static TaskStart StartTask(const JobSpec& spec, JobFn fn);

        /// 取回会话附件(未找到/无附件 → 空)。static_pointer_cast 在消费方完成。
        static std::shared_ptr<void> AttachmentOf(uint64_t id);

        /**
         * @brief 会话驱动型 job 的终态上报(upload 类: 完成发生在命令线程而非 job 线程)。
         * @return true = 已落终态; false = 已终结(幂等拒绝)。
         * @details 之后 job 线程退出时 RunJob 的自动落终态看到终态已定 → no-op
         *          (终态权威仍是登记表, 只是上报者换成知道事实的一方)。
         */
        static bool Finish(uint64_t id, JobState end);

        /**
         * @brief 取消某 kind 的全部在跑 job 并**有界排水**(关机链用;)。
         * @return 排水超时后仍未退出的 job 数(0 = 全部收干净)。
         * @details 取消是协作置位; 排水轮询 finished 标志(10ms 步进, 上限 drain_ms)。
         *          与 同款取舍: 超限者"继续收停 + 大声告警", 不引入无限等待。
         */
        static size_t CancelAllOfKind(JobKind kind, uint32_t drain_ms);

        /**
         * @brief 批量取消某 kind 的**高带宽**在跑 job(带宽观察者与会话重连观察者共用;
         *        不排水 —— 降级路径是异步的, job 在分块边界自停)。
         * @return 受理取消的条数。
         */
        static size_t CancelHighBandwidthOfKind(JobKind kind);

        /**
         * @brief 超时上报落实(reaper 侧调用)。
         * @param correlation JobSpec.correlation(命令类 = ExecutionTracker 槽位号)。
         * @details 只有 Running/Stopping → TimedOut 的转移会调 `on_timeout`(**恰好一次**;
         *          内核保证每槽至多一次, 此处状态守卫是第二道保险); 回调在名册锁**外**派发
         */
        static void NoteTimeout(uint64_t correlation);

        /// janitor 拍: 终态历史裁剪(TTL + 条数上限; 只动已终结记录, 在跑 job 永不裁剪)
        static void PruneTerminalHistory();

        // ---------------- 查询与操作(命令面消费) ----------------

        /// 全量快照(按 id 升序; 含终态历史)
        static std::vector<JobSnapshot> List();

        /// 按 id 取快照; 不存在返回 false
        static bool StatusSnapshot(uint64_t id, JobSnapshot& out);

        /// 按名取全部匹配(含终态; 调用方按需过滤)
        static std::vector<JobSnapshot> FindByName(const std::string& name);

        /**
         * @brief 受理停止请求(协作式): 置取消令牌 + 唤醒可打断等待 + Running→Stopping。
         * @return true = 已受理(含"已超时但线程仍在跑"的遗弃槽位); false = 未找到 / 已结束。
         * @details 不做强杀: 线程在安全点(Tick/SleepFor)自觉退出;
         *          状态与"线程是否已消亡"分开呈现(`JobSnapshot.finished` / draining)。
         */
        static bool Cancel(uint64_t id);

        /**
         * @brief 按名受理停止请求。
         * @return 1 = 已受理; 0 = 未找到(或无在跑job); -1 = **同名多个在跑 → 歧义拒绝**
         *         (消歧交给操作员用 id, 不猜"杀最新")。
         */
        static int CancelByName(const std::string& name);

        // ---------------- 单测专用(生产禁止调用) ----------------

        /// 进程内全局状态复位(仅当无在飞 job 时使用; 复用测试进程的用例间隔离)
        static void ResetForTests();

        /// 覆写终态裁剪策略(0, 0 = 恢复默认 10min / 256 条)
        static void SetHistoryLimitsForTests(uint64_t ttl_ms, size_t cap);

    private:
        JobRuntime() = delete;
    };

} // namespace cleat
