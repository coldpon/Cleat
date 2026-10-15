/**
 * @file CommandProcessor.hpp
 * @brief 命令处理回路（接收 → 解析 → 分发 → 发送响应）
 * @details
 * 负责接收、解析和分发来自服务器的命令，并将执行结果作为响应发送回去。
 * 支持两种帧格式：纯 JSON 帧和混合帧（JSON头 + '\0' + 二进制体）。
 *
 * **N32/L-1 起只有一条入站路径**: 帧由会话循环经 `Chain::Recv`(**唯一收包入口**)取出,
 * `ClassifyFrame` 分类**一次**后交 `HandleClassified` 执行副作用。
 * 此前还有一条"夹具/旧路径": `Process()` → `CommsManager::ProcessIncoming` →
 * `recv_epoch = 0` 执行), 并与主循环争抢同一批帧(一方取走后另一方永久看不到); 已删除。
 */

#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <vector>
#include "Common/ThirdParty/json.hpp"
// (): 构造函数默认实参需 CommandQueue::Instance(), 故要完整定义(非前向声明)
#include "Core/Command/Queue/CommandQueue.hpp"

namespace cleat {

    class CommsManager;
    class CommandDispatcher;
    class OutboundSink;
    struct ClassifiedFrame;   // 前向声明(P2.6 解环): 本头只在方法签名用其引用, 定义在 .cpp 取

    /**
     * @brief 命令处理器类
     * @details
     * 负责接收、解析和分发来自服务器的命令，并将执行结果作为响应发送回去。
     * 支持两种帧格式：纯 JSON 帧和混合帧（JSON头 + '\0' + 二进制体）。
     * 命令执行采用优先级队列和超时护栏机制。
     *
     * **N32/L-1 起只有一条入站路径**: 帧由会话循环经 `Chain::Recv`(唯一收包入口)取出、
     * `ClassifyFrame` 分类一次后交 `HandleClassified` 执行副作用。此前还存在
     * "`Process()` → `CommsManager::ProcessIncoming` → `ProcessCommandPacket`"这条
     * 并与主循环争抢同一批帧, 故已删除; 本类不再持有通信层引用。
     */
    class CommandProcessor {
    public:
        /**
         * @brief 构造函数
         * @param sink    出站唯一出口: 响应/超时/错误三路一律经此, 不再直连网络
         * @param dispatcher 命令分发器引用，用于执行具体命令
         * @param queue 命令队列引用（默认全局单例；允许注入独立队列，
         *              使 SessionHarness 能驱动"收包 → 执行 → 回发"整链路）
         */
        CommandProcessor(OutboundSink& sink,
            CommandDispatcher& dispatcher,
            CommandQueue& queue = CommandQueue::Instance());
        ~CommandProcessor() = default;

        // 禁止拷贝和移动
        CommandProcessor(const CommandProcessor&) = delete;
        CommandProcessor& operator=(const CommandProcessor&) = delete;
        CommandProcessor(CommandProcessor&&) = delete;
        CommandProcessor& operator=(CommandProcessor&&) = delete;

        /**
         * @brief 消费自上次调用以来的命令帧计数
         * @return 命令帧数量（带 cmd 字段的 S→A 命令）
         * @details
         * 心跳响应和 FirstConnect 确认等无 cmd 字段的帧不计入，
         * 仅用于活动判定（判断是否有新命令到达）。
         */
        size_t ConsumeCommandCount();

        /**
         * @brief 消费"未声明调度优先级"的计数(诊断; 订正  的留痕)
         * @details 未声明优先级的命令会落到最低档 —— 这是**可见**的隐患而非静默行为。
         *          正常应为 0; 非 0 说明有注册点漏声明。
         */
        uint64_t ConsumeUndeclaredPriorityCount() {
            return undeclared_priority_count_.exchange(0);
        }

        /**
         * @brief 按分类结果执行副作用(单一分类点的下游)
         * @param f 已分类的帧(见 `FrameClassifier`)
         * @details
         *  会话循环的入站路径是 `SessionRunner::Tick`(Pump → **分类**)→ 本函数。
         * 分类只做一次 —— 本函数**不再**自己调 `ClassifyFrame`, 否则同一帧被分类两次,
         * 且"哪次才是真值"又会变成两份真相。
         *
         * 会话归属(确认/拒绝)的**状态推进**自 起由 `SessionRunner` 直接从分类结果
         * 不再驱动会话状态。
         */
        void HandleClassified(const ClassifiedFrame& f, uint64_t recv_epoch = 0);

        /**
         * @brief 注入"当前代际"的提供者
         * @details 迟到响应的丢弃有两条判据: ①超时遗弃(`JobRecord.state == TimedOut` ——
         *           起为单一权威, 原 `guard->expired` 副本已删除); ②**代际失效** ——
         *          命令在代际 N 提交, 响应生成时当前代际已 > N(重连过), 该响应属"已死链路的
         *          可见产物", 不得发出。二者共同构成超时语义方案 (b) 的"可见产物隔离"。
         */
        void SetEpochProvider(std::function<uint64_t()> provider) {
            epoch_provider_ = std::move(provider);
        }

        /**
         * @brief 会话事件回调类型
         * @param ok true 表示收到 FirstConnect 确认（携带 session_id）
         *           false 表示收到服务器明确拒绝（防抖 DENY_BUSY）
         * @param session_id 会话 ID（仅 ok=true 时有效）
         * @details **观测钩子**: 生产侧已无订阅者; 会话基线
         *          (`session_events.json`)与 `SessionHarness` 靠它记录事件轨迹。
         *          的 FrameRouter 落地后此钩子退休。
         */
        using SessionEventCallback = std::function<void(bool ok, uint64_t session_id)>;

        /**
         * @brief 设置会话事件回调
         * @param cb 回调函数
         */
        void SetOnSessionEvent(SessionEventCallback cb);

    private:
        OutboundSink& sink_;                      ///< 出站唯一出口
        CommandDispatcher& dispatcher_;           ///< 命令分发器引用
        CommandQueue& queue_;                     ///< 命令队列引用(注入或全局单例)
        std::atomic<size_t> command_count_{ 0 };  ///< 命令帧计数器（原子操作）
        std::atomic<uint64_t> undeclared_priority_count_{ 0 };  ///< 未声明优先级的命令数
        SessionEventCallback on_session_event_;   ///< 会话事件回调
        std::function<uint64_t()> epoch_provider_; ///< 当前代际

        /**
         * @brief 命令入队执行公共入口（内部）
         * @param cmd         命令名称
         * @param params      命令参数（JSON 对象）
         * @param request_id  请求 ID（用于关联响应）
         * @param binary_data 二进制体(混合帧; 由本层装箱进 CommandRequest)
         * @details
         * 按优先级将命令放入 CommandQueue，带超时护栏。
         * 执行完成后自动发送响应到服务器。
         * 该函数被 Push 直发和 Poll 派发共用。
         */
        void SubmitCommand(const std::string& cmd, const nlohmann::json& params,
            const std::string& request_id, const std::vector<uint8_t>& binary_data,
            uint64_t recv_epoch);
    };

} // namespace cleat