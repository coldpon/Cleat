/**
 * @file OutboundSink.cpp
 * @brief 出站唯一出口实现
 */

#include "Core/Comms/OutboundSink.hpp"

#include "Common/Logger/Logger.hpp"
#include "Core/Concurrency/ThreadGovernor.hpp"
#include "Core/Comms/Channel/Chain.hpp"

#include <utility>

namespace cleat {

    OutboundSink::OutboundSink(Chain& chain, size_t max_queue_bytes)
        : chain_(chain), queue_(max_queue_bytes) {
    }

    bool OutboundSink::RequiresCmd(SendKind kind) {
        switch (kind) {
        case SendKind::Response:
        case SendKind::Poll:
        case SendKind::Module:
        case SendKind::Other:      // 未声明分类 = 连"它是什么"都没说清, 同样可疑
            return true;
        case SendKind::Heartbeat:  // 以下三类无"按命令限流"身份, 允许为空
        case SendKind::Event:
        case SendKind::Handshake:
            return false;
        }
        return true;
    }

    SendResult OutboundSink::SendLimitReceipt(const std::vector<uint8_t>& frame) {
        // 强制标记: 回执绝不能被限流规则拦下(否则命令永远无响应 —— 比限流本身更糟)
        SendOptions opt;
        opt.kind = SendKind::Response;
        opt.cmd = kLimitErrorCmd;
        opt.urgent = true;         // 显式; 另有 in_limit_callback_ 的结构性豁免兜底
        return Send(frame, opt);
    }

    SendResult OutboundSink::Send(const std::vector<uint8_t>& frame, const SendOptions& opt) {
        // ---- ⓪-前置 静默守卫(线程治理, ) ----
        // 门闸说"安静"(Paused)时,**出站唯一出口**拒发 —— 把"窗外零出站"从"链路断了的
        // 副产物"变成结构不变量: 漏网循环不再静默出站(发不出去只落一条"发送未消费"),
        // 而是在此响亮留痕(独立计数 + WARN), 且"暂停期 sink 零调用"成为可测性质。
        // 只认 Paused, **不认 Stopping**: 收尾期放行, 免得把 ExitProgram 的响应之类
        // 正常收尾帧误杀(收尾不该凭空多一类丢帧, 其正确性由 OutboundSink 既有语义管)。
        if (ThreadGovernor::Paused()) {
            ++quiet_dropped_;
            LOG_WARN("OutboundSink: 静默期(暂停)拒发出站帧 —— kind=" << static_cast<int>(opt.kind)
                << " cmd='" << opt.cmd << "' bytes=" << frame.size()
                << " 累计=" << quiet_dropped_.load() << "(该值非 0 即漏网, 请查调用点)");
            return SendResult::Dropped;
        }

        // ---- ⓪ 可疑出站诊断(A.2.4: 声明为命令承载类却未声明命令名) ----
        // 判定依据是**调用方声明的 kind**, 不是出口去解析帧体 —— 后者要求底层理解上层
        // 编码细节, 与 A.2.1 同一条原则。只诊断不拦截: 漏填不该让帧发不出去, 但必须留痕。
        if (opt.cmd.empty() && RequiresCmd(opt.kind)) {
            ++suspicious_;
            LOG_WARN("OutboundSink: 可疑出站 —— kind=" << static_cast<int>(opt.kind)
                << " 未声明 cmd, 该帧不参与按命令限流(疑似调用点漏填); bytes=" << frame.size());
            if (anomaly_obs_) anomaly_obs_(opt.kind, frame.size());
        }

        // ---- ① 低带宽限流(仅低带宽通道; urgent / 握手帧 / 回执回调期间放行) ----
        // 握手帧(KE)是明文旁路: 密钥都还没协商, 谈不上限流(决策三)。
        // 回执回调期间整体豁免 —— 回执路径不可能自锁(A.2.3); 豁免**按线程**成立(M-1):
        // 跨线程共享的豁免标志会让回调窗口内其它线程的帧跳过低带宽限流。
        const bool limit_applies = !opt.urgent && opt.kind != SendKind::Handshake
            && LimitCallbackDepth() == 0 && chain_.IsLowBandwidth();
        if (limit_applies) {
            const ChannelRateLimiter::Decision d = limiter_.Check(opt.cmd, frame.size());
            if (d != ChannelRateLimiter::Decision::Allow) {
                ++dropped_;
                LOG_WARN("OutboundSink: 低带宽限流拦截 cmd='" << opt.cmd << "' bytes="
                    << frame.size() << " 原因=" << ChannelRateLimiter::ReasonOf(d)
                    << " code=" << ChannelRateLimiter::CodeOf(d));
                if (limit_obs_) {
                    LimitCallbackScope cb_scope;   // 本线程豁免(结构性防自锁); 异常也成对复位
                    limit_obs_(opt.cmd, d, frame);
                }
                return SendResult::Dropped;
            }
        }

        // ---- ② 容量预检 + 入队(与 ③ 共用同一临界区) ----
        // OutboundQueue::Push **恒返回 true**(超限时自行丢最旧并告警), 故必须由出口显式
        // 预检容量, 否则 `SendOptions::allow_drop` 这条契约(A.2)无处兑现 —— 调用方说
        // "不许丢" 时会得到静默丢帧, 正是本缝要消灭的那类沉默失败。
        //
        // N47: 预检与入队**必须原子**。此前预检在锁外(`Bytes()` 与 `Push()` 各取各的内部
        // 锁), 两个生产者可同时通过预检再各自 `Push` -> `MaxBytes` 在并发下失守(实测
        // 256 -> 320, 恰好多一帧), 并触发 `Push` 内的溢出循环挤掉**更旧的帧**。故锁从
        // ② 段起持有, 直到本次刷出结束(下方 ③ 段不再加锁)。
        // 注: `allow_drop=true` 分支的 LOG_WARN 因此落在临界区内 —— 只是把"写一行 stderr"
        // 的耗时计入持锁时间(且仅发生在队列已满时), 正确性无关; 若要挪出需另做快照。
        std::lock_guard<std::mutex> lock(flush_mutex_);
        if (queue_.Bytes() + frame.size() > queue_.MaxBytes()) {
            if (!opt.allow_drop) {
                LOG_WARN("OutboundSink: 出站队列满(" << queue_.Bytes() << "/" << queue_.MaxBytes()
                    << "B), 按 allow_drop=false 拒绝 " << frame.size()
                    << "B 帧(交调用方决策; 队列内容未变)");
                return SendResult::Rejected;   // 帧未入队 → 不计入"被丢弃"(调用方自己拿着)
            }
            ++dropped_;
            LOG_WARN("OutboundSink: 出站队列满(" << queue_.Bytes() << "/" << queue_.MaxBytes()
                << "B), 按 allow_drop=true 接收并挤掉最旧帧(kind="
                << static_cast<int>(opt.kind) << ")");
        }
        queue_.Push(frame);

        // ---- ③ 立即刷出(与旧 CommsManager::Send 同语义: Push + Flush) ----
        // 锁已由 ② 段持有(**不要重复加锁** —— `flush_mutex_` 是普通 mutex, 重入即自死锁)。
        // 队列要么被本次刷空(返回最后一帧的结果 = 本次帧的结果), 要么在某个帧上停住
        // 并把失败值返回(该帧已回插队首, 本次帧安全留在队列里)。
        return FlushLocked();
    }

    SendResult OutboundSink::FlushLocked() {
        std::vector<uint8_t> frame;
        SendResult last = SendResult::Queued;
        while (queue_.Pop(frame)) {
            const SendResult r = chain_.Send(frame);
            const bool consumed = (r == SendResult::Sent || r == SendResult::Queued ||
                r == SendResult::Dropped);
            if (!consumed) {
                // 未消费 → 回插队首, 队列保留其余待下次 Flush(重连后自然重发)。
                // 旧行为 Clear() 丢弃全部待发帧 —— 已由 R7 的"重连前显式作废"取代。
                queue_.PushFront(std::move(frame));
                LOG_WARN("OutboundSink: 发送未消费(" << ToString(r) << "), 队列保留 "
                    << queue_.Bytes() << "B 待重发");
                return r;
            }
            last = r;
            if (r == SendResult::Sent || r == SendResult::Queued) ++sent_;
            // 通道侧永久不可投(如 DNS/ICMP 整帧超分片上限, 报 Dropped)—— 计入丢弃账,
            // 与出口的限流/队列满丢弃同口径。
            else if (r == SendResult::Dropped) ++dropped_;
            frame.clear();
        }
        return last;
    }

    void OutboundSink::Clear() {
        std::lock_guard<std::mutex> lock(flush_mutex_);
        queue_.Clear();
    }

} // namespace cleat
