#pragma once
/**
 * @file MaskGate.hpp
 * @brief 睡眠掩码资格判定(判定四问) + MaskedSleep 编排(/)
 * @details
 *   ① 配置开关(`AgentConfig` mask 域, 默认关) —— 仅主循环接入点检查;
 *   ② 通道 agency = Poll(由 SessionLoop 以既有 `is_push_channel_` 回调快照);
 *   ③ 静默谓词 —— 名册无 Service/Task/Ephemeral + 无在飞命令 + 出站排空 + QUIC 未污染;
 *   ④ 线程普查兜底 —— 在 `MaskRuntime::SelfTest` 内部执行(名单外镜像内线程 → 拒绝)。
 *
 * 本文件仅在 `CLEAT_WITH_MASK=ON` 时有实现(Core/Mask 整目录随开关裁剪),
 * 包含点(SessionLoop 的 dormant 分支 / 自测命令)各自 #ifdef 门控。
 */
#include <cstdint>
#include <string>

namespace cleat {
namespace MaskGate {

    /// 判定所需的外部可观测快照(调用方一次取齐; 其余项本模块直查全局)
    struct SilenceParams {
        bool channel_is_poll = false;      ///< ② 当前通道 agency == Poll(断链期按"非 Push"视为 Poll)
        size_t outbound_queued_bytes = 0;  ///< ③ 出站队列字节数(OutboundSink::QueuedBytes)
        uint32_t channel_uplink_bytes = 0; ///< ③ 通道内待落线上行分片字节(v1.5 收尾; 稳态门填
                                           ///<   IChannel::PendingUplinkBytes —— sink 排空 ≠ 干完;
                                           ///<   dormant/退避不填: 链路离线时该值只反映"等重连")
    };

    /**
     * @brief 静默谓词(③)
     * @param inflightExempt 在飞命令豁免数(主循环接入点 = 0; 自测命令 = 1, 自身即在飞)
     * @param reason 拒绝原因(英文短码, 供日志/命令回执)
     */
    bool SilenceOk(const SilenceParams& in, uint32_t inflightExempt, std::string& reason);

    /// 判定四问之 ①②③(④ 普查在 SelfTest 内部)。false = 本轮不掩。
    bool Eligible(const SilenceParams& in, std::string& reason);

    /**
     * @brief 一次掩码睡眠尝试: 判定 → 停靠 → 窗口(整镜像 RC4 加密→过窗→解密) → 放行
     * @param requestMs 窗长上限(0 = 用配置窗长 ±jitter, 不截断; dormant 路径传 0)
     * @param reason    未掩原因(判定/停靠/普查/链失败)
     * @param actualMs  非空 = 回写实际窗长(观测用; 未掩不写)
     * @return true = 窗口已过且镜像已解密(调用方直接进入下一拍);
     *         false = 未掩, 调用方走普通睡眠(分片语义照旧)
     * @details 放行时序由 `MaskPark::Session` RAII 保证: 参与者只在解密完成后被放行;
     *          任何失败路径(判定拒绝/停靠收不齐/普查拒绝/链失败)镜像均未被加密。
     */
    bool MaskedSleep(uint32_t requestMs, const SilenceParams& in, std::string& reason,
        uint32_t* actualMs = nullptr);

    /**
     * @brief 自检命令(SleepMaskProbe)的执行前预检
     * @details 与 Eligible 的差异: **不查①配置开关**(自测用于验证运行时本身, 先于启用);
     *          ③ 在飞豁免 1 条(本命令自身), ②通道不限(普查会如实拒绝 Push 通道的
     *          接收线程)。其余(名册/出站/QUIC 污染)与静默谓词同口径 —— 普查是
     *          兜底不变量, 静默谓词是主闸(普查看不见"start 地址在系统 DLL 却会
     *          回调进本镜像"的线程, 那类只能靠谓词排除)。
     */
    bool TestPrecheck(const SilenceParams& in, std::string& reason);

} // namespace MaskGate
} // namespace cleat
