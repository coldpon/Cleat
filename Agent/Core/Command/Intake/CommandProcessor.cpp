/**
 * @file CommandProcessor.cpp
 * @brief 命令处理器实现
 */

#include "CommandProcessor.hpp"
#include "Core/Comms/CommsManager.hpp"
#include "Core/Comms/OutboundSink.hpp"
#include "Core/Comms/OutboundMessages.hpp"
#include "Core/Command/Dispatch/CommandDispatcher.hpp"
#include "Common/Logger/Logger.hpp"
#include "Core/Command/Queue/CommandQueue.hpp"
#include "Core/Comms/Session/FrameClassifier.hpp"
#include "Core/Command/Queue/CommandTaskGuard.hpp"
#include <vector>


namespace cleat {

    namespace {
        /// 未声明优先级的兜底档位(订正 N24: 语义显式, 不再是散落的字面量 4)。
        /// 取值与历史行为一致(4 = 最低档), 故本批**不改变调度顺序**;
        /// 7 个未声明的注册点(模块子系统/anti/persistence)待 逐个显式声明。
        constexpr int kUndeclaredFallbackPriority = 4;
    } // namespace

    /**
     * @brief 构造函数
     * @param sink 出站唯一出口
     * @param dispatcher 命令分发器
     * @details N32/L-1: 本类不再持通信层引用 —— 收包由会话循环经 `Chain::Recv`(唯一入口)
     *          取出并分类一次后调用 `HandleClassified`; 旧的"注册包回调 + Process() 轮询"
     */
    CommandProcessor::CommandProcessor(OutboundSink& sink,
        CommandDispatcher& dispatcher, CommandQueue& queue)
        : sink_(sink), dispatcher_(dispatcher), queue_(queue) {
    }

    /**
     * @brief 设置会话事件回调
     * @param cb 回调函数
     */
    void CommandProcessor::SetOnSessionEvent(SessionEventCallback cb) {
        on_session_event_ = std::move(cb);
    }

    /**
     * @brief 消费并重置命令帧计数器
     * @return 自上次调用以来的命令帧数量
     * @details 以原子方式交换并返回当前计数，然后清零。
     */
    size_t CommandProcessor::ConsumeCommandCount() {
        return command_count_.exchange(0);
    }

    /**
     * @brief 处理分类后的帧(唯一副作用入口)
     * @details
     * 支持两种帧格式：
     * 1. 纯 JSON 帧（普通命令 / 响应）
     * 2. 混合帧：JSON头 + '\0' + 二进制体（流数据上传 / 文件传输等）
     * 处理顺序：
     * - 首先解析会话确认帧（FirstConnect 响应）
     * - 然后处理 Poll 响应帧（含待执行命令）
     * - 最后处理普通命令帧
     * 若解析或分发失败，会构造错误响应并发送回服务器。
     */
    void CommandProcessor::HandleClassified(const ClassifiedFrame& f, uint64_t recv_epoch) {
        if (!f.parse_error.empty()) {
            // 解析异常: 保持既有行为(回错误帧, 带当前 session_id —— FrameBuilder 信封注入)
            // 帧不可解析 → 命令身份无从归属, 如实不声明 cmd(该帧确实无法参与按命令
            // 限流; A.2.4 会把它记为可疑出站 —— 这是**如实**而非噪音)
            msg::Send(sink_, msg::Error("Command parse error: " + f.parse_error, "", ""));
            return;
        }

        switch (f.kind) {
        case FrameKind::Denied:
            // 服务端明确拒绝(防抖 DENY_BUSY)。起: 会话状态由 SessionRunner 从分类
            // 回归用例用它记录事件轨迹, 它不再驱动会话状态。
            LOG_WARN("CommandProcessor: Server 拒绝连接（防抖 busy）");
            if (on_session_event_) on_session_event_(false, 0);
            return;

        case FrameKind::SessionConfirm:
            // 原则: **分类层不碰状态**。此处只留观测, 身份写入已移交
            // `SessionMachine`(Registering --Confirm--> Ready 的**唯一**被接受转移里)。
            // 此前这里无条件 `SetSessionId(f.session_id)` 且**先于**状态判定执行 ——
            // Ready 态的意外确认(服务端为不带 session_id 的帧新建了会话)会照样覆盖
            // 本机身份, 把后续所有上行帧引到那个野会话上。移走后:
            //   * 状态推进与身份写入同在状态机内, "拒绝"与"不污染"由同一段代码保证;
            if (on_session_event_) on_session_event_(true, f.session_id);
            LOG_INFO("Session ID:" << f.session_id);
            return;

        case FrameKind::Command:
            SubmitCommand(f.cmd, f.params, f.request_id, f.binary, recv_epoch);
            return;

        case FrameKind::Ignored:
        case FrameKind::Unknown:
        default:
            return;
        }
    }

    /**
     * @brief 将命令提交到执行队列
     * @param cmd         命令名称
     * @param params      命令参数
     * @param request_id  请求 ID
     * @param binary_data 二进制附加数据
     * @details
     * 1. 递增命令计数器（用于活动判定）
     * 2. 根据命令名称获取优先级
     * 3. 创建投递票据（JobTicket），超时时自动发送超时错误响应
     * 4. 将任务和票据一起推入 CommandQueue
     * 5. 任务执行完毕后，若未超时则发送结果响应
     */
    void CommandProcessor::SubmitCommand(const std::string& cmd,
        const nlohmann::json& params, const std::string& request_id,
        const std::vector<uint8_t>& binary_data, uint64_t recv_epoch) {
        // 命令帧计数：仅真实 S→A 命令计入，供心跳活动判定使用
        // （Poll 响应帧本身不计，派发出的命令才计）
        ++command_count_;

        // 优先级(订正 N24): 只认**注册时显式声明**的值。原来这里注释写"回退内置分类表
        // (fallbackPriority)"—— 那张表**已不存在**, 全仓零命中, 注释指向空气。
        uint8_t declared = dispatcher_.PriorityOf(cmd);
        int priority = static_cast<int>(declared);
        if (declared == CommandDispatcher::kPriorityUnset) {
            // 兜底语义**显式化**: 未声明 → 最低档, 并且**留痕**(计数 + 节流告警)。
            // 不去猜"它应该是哪一档": 猜错会静默改变调度顺序, 比"最低档 + 可见"更糟。
            priority = kUndeclaredFallbackPriority;
            ++undeclared_priority_count_;
            LOG_WARN("CommandProcessor: 命令 '" << cmd << "' 未声明调度优先级, 按最低档 "
                << priority << " 执行(该命令将在重负载下被后置; 请在注册点显式声明)");
        }

        // 命令投递票据（JobTicket， Phase 1 取代 TaskGuard）：
        // 超时时由 reaper 经 JobRuntime::NoteTimeout 触发 on_timeout（回超时错误）；
        // 迟到完成的响应经"记录状态 == TimedOut"检查丢弃（单一权威，不再有 expired 副本），
        // 避免同一 request_id 双响应。
        auto ticket = std::make_shared<JobTicket>();
        ticket->name = "cmd:" + cmd;
        ticket->slot = std::make_shared<JobRecordSlot>();
        ticket->on_timeout = [this, cmd, request_id]() {
            msg::Send(sink_, msg::Error("handler timeout: " + cmd, "", request_id, cmd));
            };

        // /6c: 资源 key 在**入队时**就带上(执行角色据此决定占什么), 取值走注册期声明。
        // 未覆写 `ResourceOf()` 的命令得到空 key = 不参与互斥, 与今天行为一致。
        CommandContext key_ctx;                 // 仅用于取 key; 执行期的 ctx 在任务内另行组装
        const CommandRequest key_req{ params, binary_data, key_ctx };
        const ResourceKey key = dispatcher_.ResourceOf(cmd, key_req);

        ticket->on_rejected = [this, cmd, request_id]() {
            auto out = msg::Error("resource busy: " + cmd, "RESOURCE_BUSY", request_id, cmd);
            out.opt.urgent = true;              // 这是"让 operator 知情"的回执, 不该被限流吃掉
            msg::Send(sink_, std::move(out));
            };

        // per-task 超时覆盖: params.timeout_ms > 0 时覆盖全局
        // worker 超时(下限 1s, 与 SetWorkerTimeout 同口径); 缺省/0 = 用全局。
        // 慢 BOF(内存 Dump/凭据抓取)靠它避免"超时回执已发、真结果迟到被丢"。
        // static_cast<uint32_t>, 大值(≥2^32)截断回绕(如 2^32 → 0 = 意外退回全局超时)。
        uint32_t item_timeout_ms = 0;
        if (auto it = params.find("timeout_ms"); it != params.end() && it->is_number()) {
            const long long v = it->get<long long>();
            if (v > 0) {
                const long long clamped = v < 1000 ? 1000
                    : (v > 0xFFFFFFFFLL ? 0xFFFFFFFFLL : v);
                item_timeout_ms = static_cast<uint32_t>(clamped);
            }
        }

        const bool accepted = queue_.Push(priority,
            [this, cmd, params, request_id, binary_data, ticket, key, recv_epoch]() {
                //  单参数请求对象。`ctx` 的 epoch 待 接上
                // 接收时代际(需要把 `SessionRunner` 的 recv_epoch 一路带到本层) —— 现在
                // 填 0, 语义是"未接线", 而不是一个假装有值的数字。
                // 二进制响应体槽: handler 经 ctx.binary_out 写入(一次执行一份产物,
                // 截图只能拍一张)。出站以混合帧形态携带(帧体 = 大载荷, data 段只放
                // 元数据), 见 msg::Response 带体重载。
                std::vector<uint8_t> binary_response;

                CommandContext ctx;
                ctx.epoch = recv_epoch;      //  命令归属其**接收时代际**
                ctx.resource = key;
                ctx.binary_out = &binary_response;
                const CommandRequest req{ params, binary_data, ctx };
                nlohmann::json response = dispatcher_.Dispatch(cmd, req);

                //  **null = 不响应**契约, 必须在挂 origin_cmd 之前判定 ——
                // nlohmann 对 null 值调 operator[] 会把 null **就地转成 object**,
                // 使下方 `is_null()` 判定恒真(死判定)。契约的依赖方:
                // PivotDataHandler 数据透传显式 `return nlohmann::json();` 并注释
                // "CommandProcessor 对 null 响应静默" —— 若先转 object 再判, 每块
                // 透传数据都会多回一帧 `{origin_cmd, session_id, request_id}`,
                // 上行帧数翻倍并白吃 ChannelRateLimiter 的按命令配额。
                if (response.is_null()) {
                    return;
                }

                // 可见产物的两条丢弃判据(超时语义方案 b):
                //   ①超时遗弃: 该命令的响应已由 reaper 回过超时错误 —— 判据改为查
                //     线程内读无竞态; 记录被裁剪也不影响(槽位持 shared_ptr);
                //   ②**代际失效**: 提交时代际与当前不符(其间重连过) —— 响应属已死链路的产物。
                if (ticket->slot && ticket->slot->record &&
                    ticket->slot->record->state.load(std::memory_order_relaxed) ==
                        JobState::TimedOut) {
                    LOG_DEBUG("CommandProcessor: 丢弃超时任务的迟到响应: " << cmd);
                    return;
                }
                const uint64_t now_epoch = epoch_provider_ ? epoch_provider_() : 0;
                if (now_epoch == 0 && recv_epoch != 0) {
                    // 代际未知(装配未接线 / 提供者返回 0): **fail-open** 放行并留痕。
                    // 理由: 一个未知值不该成为"静默吞掉响应"的理由 —— 那类失败(命令有请求、
                    // 无响应、日志里只有一条 WARN)比不做代际判定难查得多。实测踩到过一次:
                    // 生产路径上真的丢了一条响应, 而当时提供者返回 0。
                    LOG_WARN("CommandProcessor: 代际提供者返回 0(未接线?), 跳过代际判定放行 cmd="
                        << cmd << " (提交于代际 " << recv_epoch << ")");
                }
                else if (now_epoch != recv_epoch) {
                    LOG_WARN("CommandProcessor: 丢弃跨代际的迟到响应 cmd=" << cmd
                        << " (提交于代际 " << recv_epoch << ", 当前 " << now_epoch << ")");
                    return;
                }

                // 标记响应归属的命令：供 server 端关联展示、供低带宽通道
                // 限流器（ChannelRateLimiter）按命令分类限流。
                // 组包收口(M1): origin_cmd/session_id/request_id 挂载与 SendOptions
                // 声明收口到 msg::Response(此前 4 条路径各有一份手工挂载)。
                msg::Send(sink_, msg::Response(cmd, request_id, std::move(response),
                    std::move(binary_response)));
            }, ticket, key, item_timeout_ms);

        if (!accepted) {
            //  队列满 → **可区分**错误, 而不是静默丢(那会让 operator 以为命令还在排队)
            auto out = msg::Error("command queue full: " + cmd, "QUEUE_FULL", request_id, cmd);
            out.opt.urgent = true;              // "让 operator 知情"的回执不该被限流吃掉
            msg::Send(sink_, std::move(out));
        }
    }

} // namespace cleat