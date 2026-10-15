/**
 * @file OutboundSink.hpp
 * @brief 出站唯一出口
 * @details
 * **存在理由**: 合一前出站有**六条旁路**,
 * 每条自己决定"加不加 ts、过不过限流、失败了怎么办" —— 于是:
 *   * `ChannelRateLimiter` 写好了却**全仓零调用者**(不是"响应绕过了限流", 而是整条链路
 *     从未装上限流): 因为"统一收口处"(`AgentContext::SendData`)从来不是唯一入口;
 *   * 队列、重发、`SendResult` 语义散在 `CommsManager` 里, 而响应三路(解析错误/超时/
 *     正常响应)直连 `network_.Send`, 绕过了本该生效的闸门(N3)。
 *
 * 本类把这些收成**一处**: ts 的入口契约、低带宽限流、有界队列、`SendResult` 收口。
 *
 * **归属(决策一)**: 队列内聚在这里, `CommsManager` 退化为**纯传输门面**
 * (只持 `Chain&`, 提供 Connect/Disconnect/IsConnected 透传)。于是"改队列策略"只改一处,
 * "加通道"只加 `IChannel` 实现 —— 两条正交的演进轴不再互相牵扯。
 *
 * **不做帧体解析(决策二)**: 限流依据是调用方声明的 `SendOptions::cmd`, 而不是出口去解析
 * JSON。同理 `ts` 由 `FrameBuilder::Build` 在**入口侧**注入(sink 不碰帧体), 见 A.2.2。
 *
 * **线程**: 与旧 `CommsManager` 同语义 —— `Flush` 加锁串行化, 帧序 = `Push` 顺序
 * (模块线程的 `api->send` 与主循环心跳会并发, 串行化后 KE/FirstConnect 不被插队)。
 */
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "Core/Comms/Channel/SendResult.hpp"
#include "Core/Comms/Internal/ChannelRateLimiter.hpp"
#include "Core/Comms/Internal/OutboundQueue.hpp"

namespace cleat {

    class Chain;

    /**
     * @brief 出站唯一出口
     */
    class OutboundSink {
    public:
        /**
         * @brief 限流拦截通知(注入)
         * @param cmd      被拦截的命令名
         * @param decision 拦截原因(Deny/TooLarge/RateLimited)
         * @param frame    被拦截的原始帧(装配方据此取 request_id/session_id 组装错误回执)
         * @details
         * 超限**不静默丢弃**: sink 只负责决策与计数, 由装配方决定如何让操作者感知
         * (今天的做法是回一个错误帧 —— 那帧自己必须 `urgent=true` 放行, 否则回执
         * 又被同一条规则拦下)。sink 不组装协议帧, 故这里只回调不代劳。
         */
        using LimitObserver = std::function<void(const std::string& cmd,
            ChannelRateLimiter::Decision decision, const std::vector<uint8_t>& frame)>;

        /// 限流回执的**声明身份**(A.2.3): 供审计与日志检索用, 不影响线上帧内容
        static constexpr const char* kLimitErrorCmd = "limit_error";

        /**
         * @brief 可疑出站观察者(诊断, A.2.4)
         * @param kind  调用方声明的用途
         * @param bytes 帧长
         * @details 用于抓住"声明为命令承载类、却没声明命令名"的漏填 —— 那会让按命令限流
         *          被静默绕过。**只诊断不拦截**: 漏填不该让帧发不出去。
         */
        using AnomalyObserver = std::function<void(SendKind kind, size_t bytes)>;

        /**
         * @param chain           通道链(唯一的下游)
         * @param max_queue_bytes 出站队列上限(有界; 满则按 `allow_drop` 决策)
         */
        explicit OutboundSink(Chain& chain, size_t max_queue_bytes = 16 * 1024 * 1024);

        OutboundSink(const OutboundSink&) = delete;
        OutboundSink& operator=(const OutboundSink&) = delete;

        /**
         * @brief 投递一帧(**唯一出口**)
         * @param frame 完整出站帧(JSON 帧须已由 `FrameBuilder::Build` 注入 ts)
         * @param opt   投递选项(见 `SendOptions`; `cmd` 是限流依据)
         * @return 本次投递尝试的结果
         * @details
         *   * `Dropped`        —— 帧**未入队**: 被限流拦下, 或队列满且 `allow_drop=true`;
         *   * `Rejected`       —— 帧未入队: 队列满且 `allow_drop=false`(交调用方决策);
         *   * `Sent`/`Queued`  —— 帧已入队: `Sent` = 本次已落线, `Queued` = 仍在队列
         *                        (Poll 类通道常态, 或前方有帧失败导致本次未刷到);
         *   * `ChannelFailure` —— 通道自报故障(**唯一**可驱动换候选与 Health 的取值);
         *                        帧已回插队首, 待重连后随下次 Flush 重发。
         * **调用方不必知道失败的是哪一帧**: 返回值表达的是"本次投递尝试"的结论 ——
         * 只要求把 `ChannelFailure` 与其余四值区分开。
         */
        SendResult Send(const std::vector<uint8_t>& frame,
            const SendOptions& opt = SendOptions{});

        /**
         * @brief 清空出站队列
         * @details R7: 重连握手前作废旧链路遗留帧 —— 若先于 KE/FirstConnect 冲出,
         *          服务端按"非注册首帧"走匿名注册(空参数), 击穿 machine_id 复用。
         */
        void Clear();

        // ---------------- 观测(排障/背压) ----------------

        /// 队列当前字节数
        size_t QueuedBytes() const { return queue_.Bytes(); }

        /// 累计被丢弃帧数(限流 + 队列满)
        uint64_t DroppedCount() const { return dropped_.load(); }

        /// 累计投递成功帧数
        uint64_t SentCount() const { return sent_.load(); }

        /// 累计"静默期(暂停)拒发"帧数(线程治理静默守卫; 生产应恒为 0 —— 非 0 即漏网)
        uint64_t QuietDroppedCount() const { return quiet_dropped_.load(); }

        /// 下限流拦截通知(装配方注入)
        void SetLimitObserver(LimitObserver obs) { limit_obs_ = std::move(obs); }

        /// 下"可疑出站"通知(装配方注入; 可选)
        void SetAnomalyObserver(AnomalyObserver obs) { anomaly_obs_ = std::move(obs); }

        /// 累计可疑出站帧数(未声明命令名的命令承载类帧)
        uint64_t SuspiciousCount() const { return suspicious_.load(); }

        /**
         * @brief 投递限流回执(A.2.3: 强制 `urgent` + `cmd=kLimitErrorCmd`)
         * @param frame 已组好的错误帧(由装配方用 `Packet::Error` + `FrameBuilder` 组装)
         * @return 同 `Send`
         * @details
         * **为什么必须走这个函数**: 限流回执本身若被限流规则拦下(比如它超过大小上限),
         * 结果是"命令永远没有响应" —— 比限流本身更糟(自锁)。故此处**强制**注入
         * `urgent=true` 与专用 `cmd`, 调用方无需(也无法)记错。
         * 另有一道结构性保险: 观察者回调期间**整体豁免限流**(见 `in_limit_callback_`),
         * 故回调内发出的任何帧都不会自锁。
         */
        SendResult SendLimitReceipt(const std::vector<uint8_t>& frame);

        /**
         * @brief 该用途是否必须声明命令名(漏填 → 可疑出站)
         * @details 命令承载类(`Response`/`Poll`/`Module`)与未声明分类(`Other`)必须声明;
         *          `Heartbeat`/`Event`/`Handshake` 无按命令限流身份, 允许为空。
         */
        static bool RequiresCmd(SendKind kind);

        /// 限流器(供测试断言频率窗口状态; 生产不需要直接碰)
        ChannelRateLimiter& Limiter() { return limiter_; }

    private:
        /// 逐帧刷出队列; 返回最后一帧的结果或首个失败值(帧保留在队列里)
        SendResult FlushLocked();

        /**
         * @brief 限流豁免深度(**按线程**; M-1)
         * @details 观察者回调期间整体豁免限流(A.2.3: "限流错误回执被自己限流"这条死路在
         *          结构上不存在, 而不只依赖调用方记得写 urgent)。豁免的语义本就只对
         *          **回调所在线程**成立。
         *          **TLS 已禁**(DLL 形态掩码前提, ): thread_local 会给 DLL
         *          引入静态 TLS 目录 → TLS 回调在掩码窗口期执行镜像内代码 → 崩溃。
          * 改为互斥 map<线程id, 深度> —— 语义逐字保留(嵌套深度/按线程成立); 读点
         *          在低带宽限流判定(仅 DNS/ICMP 触达), 互斥开销可忽略。
         */
        static std::mutex& LimitDepthMutex() {
            static std::mutex m;
            return m;
        }
        static std::map<std::thread::id, int>& LimitDepthMap() {
            static std::map<std::thread::id, int> m;
            return m;
        }
        static int LimitCallbackDepth() {
            std::lock_guard<std::mutex> lock(LimitDepthMutex());
            const auto it = LimitDepthMap().find(std::this_thread::get_id());
            return (it != LimitDepthMap().end()) ? it->second : 0;
        }
        static void LimitCallbackDepthAdjust(int delta) {
            std::lock_guard<std::mutex> lock(LimitDepthMutex());
            const std::thread::id tid = std::this_thread::get_id();
            const auto it = LimitDepthMap().find(tid);
            if (it == LimitDepthMap().end()) {
                if (delta > 0) {
                    LimitDepthMap()[tid] = delta;
                }
                return;
            }
            it->second += delta;
            if (it->second <= 0) {
                LimitDepthMap().erase(it);   // 归零即清槽, map 不随线程数无界增长
            }
        }

        /// RAII: 置位/复位成对 —— 回调抛异常也不会把豁免永久留在打开状态
        class LimitCallbackScope {
        public:
            LimitCallbackScope() { LimitCallbackDepthAdjust(+1); }
            ~LimitCallbackScope() { LimitCallbackDepthAdjust(-1); }
            LimitCallbackScope(const LimitCallbackScope&) = delete;
            LimitCallbackScope& operator=(const LimitCallbackScope&) = delete;
        };

        Chain& chain_;
        OutboundQueue queue_;
        /// 多线程 Flush 并发 Pop 会乱序(模块线程 api->send 与主循环心跳并发),
        /// 加锁串行化: 先拿锁的 Flush 出完队列, 后到的空转; 帧序 = Push 顺序。
        std::mutex flush_mutex_;
        ChannelRateLimiter limiter_;
        LimitObserver limit_obs_;
        AnomalyObserver anomaly_obs_;
        std::atomic<uint64_t> dropped_{ 0 };
        std::atomic<uint64_t> sent_{ 0 };
        std::atomic<uint64_t> suspicious_{ 0 };
        std::atomic<uint64_t> quiet_dropped_{ 0 };   ///< 静默守卫(暂停期拒发)
    };

} // namespace cleat
