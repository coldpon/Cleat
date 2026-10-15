/**
 * @file Chain.hpp
 * @brief 通道链：多候选降级与回探
 * @details
 *   * 候选按添加顺序 = 优先级(下标 0 最高);
 *   * 当前候选连续失败 `fail_threshold` 次 → 切下一个; 全失败 → 回最高优先 + 外层退避;
 *   * 降级运行时(过 `min_hold`)按 `probe_interval` 回探更高优先候选, 成功即回切;
 *   * 切换后 `min_hold` 内不再切换/回探(防 A/B 震荡);
 *   * 会话确认在通道之上, 由会话层负责(切换后重发 FirstConnect)。
 *
 * **与旧实现的三点结构差异(均为纠正, 行为不变)**:
 *   1. **显式化**: 旧实现靠 `!ok && !current_->IsConnected()` 二次判断决定是否计失败;
 *      新实现用 `SendResult` —— **只有 `ChannelFailure` 计入失败并可能换候选**,
 *      `Queued`/`Sent`/`Dropped`/`Rejected` 一律不影响候选与健康
 *      (若让"写失败"累计失败, 无数据 / 空 body / 节流未到会被误判为通道故障)。
 *   2. **回探切换原子化**: 旧实现的回探在 `try_connect_at(0)` 成功后由调用点补一步
 *      "断开旧降级通道", 顺序写错就会断开刚连上的主通道(已修复)。
 *      新实现把"关旧 + 指新 + 复位游标"收进单一 `SwapTo()`, 顺序只能有一处定义。
 *   3. **回探两段式**: 回探的建链 I/O(`Open`, TCP/HTTPS 可达数秒)在
 *      **chain 锁外**执行 —— 旧实现持锁 `Open`, 降级运行期每次回探都让全部出站
 *      (`Send` 取同一把锁)停摆数秒。候选对象的替换/关闭由 `probe_mutex_` 挡住
 *      (锁序: `probe_mutex_` 先于 `mutex_`)。
 *
 * 线程: 由会话主循环单线程调度; 内部互斥锁保护(与旧实现一致的递归锁语义)。
 */
#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "Core/Comms/Channel/IChannel.hpp"

namespace cleat {

    /// 候选链载体: 通道实例 + 配置(供装配层按通道规格构建后整体替换)
    using ChannelCandidates = std::vector<std::pair<std::unique_ptr<IChannel>, TransportConfig>>;

    class Chain {
    public:
        Chain() = default;

        Chain(const Chain&) = delete;
        Chain& operator=(const Chain&) = delete;

        /// 添加候选(添加顺序 = 优先级, 下标 0 最高)
        void AddCandidate(std::unique_ptr<IChannel> channel, TransportConfig cfg);

        /**
         * @brief 整体替换候选链(运行期热切换, SetConfig channels 域触发)
         * @details 锁内关闭全部旧候选(释放 socket/会话)后整体替换并复位游标。
         *          上层看到 `IsConnected()` 为假后自然走既有重连路径。
         */
        void SetCandidates(ChannelCandidates cands);

        /// 从 `attempt_index_` 起轮询候选(回绕); 全失败则回到最高优先并返回 false
        bool Connect(const std::string& host, uint16_t port);

        /// 断开当前通道
        void Disconnect();

        /// 是否已连接
        bool IsConnected() const;

        /// 当前通道健康档(无当前通道 → Dead)
        ChannelHealth HealthNow() const;

        /**
         * @brief 交出帧: 只有 ChannelFailure 计失败并可能换候选
         */
        SendResult Send(const std::vector<uint8_t>& frame);

        /**
         * @brief 收取一帧(**唯一收包入口**; 降级运行时此处顺带回探更高优先候选)
         * @details 会话循环经本函数推进收侧。回探与收侧失败计数都收口在这一条路径上
         *          —— 收包入口无分叉, 回探间隔等配置必然生效。
         * @note 内部自持锁; 档位变化只记账, 观察者在锁外派发(见 `BandwidthTxn`)。
         */
        bool Recv(std::vector<uint8_t>& frame, int timeout_ms);

        /// 当前通道带宽档(取代 `IsLowBandwidth()` 的 if-else 链)
        BandwidthClass Bandwidth() const;
        bool IsLowBandwidth() const { return Bandwidth() == BandwidthClass::Low; }

        /// 当前通道指针(可能为空)
        IChannel* Current();

        /// 节奏合一: 当前通道距下一次 check-in 到期
        /// (无当前通道 / 通道不适用 = 0; 纯转发, 与 Bandwidth() 同款持锁只读)
        uint32_t NextPollDueInMs() const;

        /// 稳态掩码门输入: 当前通道待落线上行分片字节(sink 排空 ≠ 干完)
        uint32_t PendingUplinkBytes() const;

        /**
         * @brief 带宽档变化观察者
         * @details 取代"每轮轮询 `IsLowBandwidth()` 再调回调"的旧形态 ——
         *          轮询回调无状态, **无法表达"刚进入降级"这一时刻语义**, 也无法保证
         *          "只触发一次"。订阅式只在档位**真的变化**时通知(from/to 均给出)。
         * @note 无当前通道时按既有语义视为 High(Bandwidth() 的约定)。
         */
        using BandwidthObserver = std::function<void(BandwidthClass from, BandwidthClass to)>;
        void SetBandwidthObserver(BandwidthObserver obs) { bandwidth_obs_ = std::move(obs); }

        /// 候选总数
        size_t CandidateCount() const;

        // ---- 观测(只读; 不影响行为) ----
        size_t CurrentIndex() const;
        size_t AttemptIndex() const;
        size_t FailCount() const;

    private:
        // ---- 带宽档变化: 锁内记账, **锁外**派发 ----
        // 观察者(装配层)会同步停高带宽任务/服务, 而 `Stop()` 内部 join 工作线程; 那些线程
        // 要发帧就得取本对象的锁(`Bandwidth()`/`Send()` 均持锁) —— 持锁派发 ⇒ 双向等待
        // 且无超时(命中即"活着但哑了", 只能杀进程)。故锁内只更新记账与待派发槽, 由
        // `BandwidthTxn` 在**锁释放之后**调观察者。
        struct BandwidthChange {
            bool valid = false;
            BandwidthClass from = BandwidthClass::High;
            BandwidthClass to = BandwidthClass::High;
        };

        /// 锁内: 档位真变化则更新记账并写入待派发槽(不回调)
        void NoteBandwidthChangeLocked();

        /// 取走待派发变化并在锁外调观察者(取走后槽位清空, 故至多派发一次)
        void DispatchBandwidth();

        /**
         * @brief 作用域事务: 声明在 `lock_guard` **之前** ⇒ 析构(派发)发生在锁释放之后
         * @details 局部对象逆序析构, 故本对象必须比锁先构造。覆盖全部早退路径(含
         *          `return false`), 不必在每个出口补一次派发。
         * @warning 不得在**已持锁**的函数里构造(那会退化成持锁派发)。嵌套调用(如
         *          `SwapTo` 由 `ProbeBetter` 调用)只写槽位, 由最外层事务统一派发。
         */
        class BandwidthTxn {
        public:
            explicit BandwidthTxn(Chain& owner) : owner_(owner) {}
            ~BandwidthTxn() { owner_.DispatchBandwidth(); }
            BandwidthTxn(const BandwidthTxn&) = delete;
            BandwidthTxn& operator=(const BandwidthTxn&) = delete;
        private:
            Chain& owner_;
        };

        struct Candidate {
            std::unique_ptr<IChannel> channel;
            TransportConfig cfg;
        };

        /// 尝试连接指定索引的候选(失败后 `Close()` 清理, 供下次重试)
        bool try_connect_at(size_t idx, const std::string& host, uint16_t port);

        /// 记录一次失败; 达阈值则前移到下一候选
        void record_failure();

        /**
         * @brief 回探判定(纯读; 需持 `mutex_`): 降级运行 + 过 `min_hold` + 距上次探测
         *        已超 `probe_interval`。
         * @note 无副作用 —— `last_probe_at_` 由真正执行探测的一方(`ProbeBetter` 的
         *       串行段内)写入, 保证"记录的时刻 = 探测时刻"。
         */
        bool ProbeDueLocked() const;

        /**
         * @brief 执行一轮回探(两段式; 全程持 `probe_mutex_`)
         * @details 锁内判定与提交, `Open` 在 `mutex_` **外**执行 —— 期间出站经当前
         *          (降级)通道照常流动。候选对象的替换/关闭(`SetCandidates`/`Connect`/
         *          `Disconnect`)都会等 `probe_mutex_`, 故 `Open` 期间候选不会被
         *          Close/替换/析构。
         */
        void ProbeBetter();

        /**
         * @brief 原子切换当前通道到 idx
         * @details 顺序: 先对**旧**通道 `Close()`(作废其链路级状态), 再指向新通道并
         *          复位游标 —— 顺序写错会断开刚连上的主通道, 故顺序只能有一处定义。
         */
        void SwapTo(size_t idx);

        std::vector<Candidate> candidates_;
        size_t attempt_index_ = 0;   ///< 下次 Connect 起始索引
        size_t current_index_ = 0;   ///< 当前通道索引
        size_t fail_count_ = 0;      ///< 当前候选连续失败次数
        IChannel* current_ = nullptr;///< 当前通道指针

        mutable std::recursive_mutex mutex_;
        /// 回探串行锁: 回探的"判定→锁外 Open→提交"全程持有;
        /// `SetCandidates`/`Connect`/`Disconnect` 也取其 —— 防 Open 期间候选对象被
        /// 替换/关闭(对象级竞争), 并为提交前的指针复验提供依据。
        /// **锁序(硬约束): probe_mutex_ 先于 mutex_**; 任何持 mutex_ 的路径不得再取它。
        std::mutex probe_mutex_;

        std::string last_host_;
        uint16_t last_port_ = 0;
        std::chrono::steady_clock::time_point connected_at_{};
        std::chrono::steady_clock::time_point last_probe_at_{};

        BandwidthObserver bandwidth_obs_;                      ///< 档位变化订阅
        BandwidthClass last_notified_bandwidth_ = BandwidthClass::High;  ///< 上次已通知的档位
        BandwidthChange pending_bw_;                           ///< 待派发变化(锁内写, 锁外取走)
    };

} // namespace cleat
