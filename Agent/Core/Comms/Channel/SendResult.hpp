/**
 * @file SendResult.hpp
 * @brief 出站投递结果
 * @details
 * **硬约束(的 C5, 本缝的全部意义所在)**:
 *   `Queued` / `Sent` / `Dropped` / `Rejected` **不得**驱动 Chain 换候选, 也不得改变
 *   Health; **只有 `ChannelFailure` 可以**。
 *
 * 今天这条语义靠 `TransportSelector.cpp:210/:229` 的 `!ok && !IsConnected()` 二次判断
 * 隐式兜着 —— 一旦新模型让"写失败"直接累计失败计数或判 Degraded/Dead, 则
 * 且只在低带宽通道上随机翻车。故五值必须区分清楚。
 */
#pragma once

#include <string>

namespace cleat {

    /// 出站投递结果(五值, 冻结)
    enum class SendResult {
        Queued = 0,        ///< 已入队待投递(尚未落线; Poll 类通道的常态)
        Sent = 1,          ///< 本轮已投递成功
        Dropped = 2,       ///< 投递前被丢弃(队列满 / allow_drop / 代际失效 /
                           ///<  通道侧永久不可投 —— 如 DNS/ICMP 整帧超分片上限,
                           ///<  见两传输的 Write 覆写; 均为"已消费", 不得重试)
        Rejected = 3,      ///< 本轮拒绝发送(节流未到 / 无 socket / 未连接) —— 可重试
        ChannelFailure = 4 ///< 通道自报故障 —— **唯一**可驱动换候选与 Health→Dead 的取值
    };

    /// 出站帧用途(供限流分类与"紧急帧跳过节流"判定)
    enum class SendKind {
        Response = 0,      ///< 命令响应
        Heartbeat = 1,
        Poll = 2,
        Event = 3,         ///< 遥测/事件上报
        Module = 4,        ///< 模块上报
        Handshake = 5,     ///< 握手帧(KE; 明文旁路, 协议规定**不做任何限流/节流**)
        Other = 6
    };

    /**
     * @brief 出站选项
     * @details
     * **A.2.1**: 新增 `cmd` 字段 —— 限流(`ChannelRateLimiter`)按命令决策,
     * 决策依据必须由**调用方声明**, 不得由出口解析 JSON 帧体(那会让底层依赖上层的
     * 编码细节, 耦合方向反了)。字段为空 = 不参与按命令限流(走 default 规则)。
     */
    struct SendOptions {
        bool urgent = false;          ///< 跳过限流/节流(紧急帧; 如限流回执自身)
        bool allow_drop = true;       ///< 队列满时允许丢弃; false → Rejected(交调用方决策)
        SendKind kind = SendKind::Other;
        std::string cmd;              ///< A.2.1: 帧的命令名(限流依据); 空 = 不参与按命令限流
    };

    inline const char* ToString(SendResult r) {
        switch (r) {
        case SendResult::Queued: return "Queued";
        case SendResult::Sent: return "Sent";
        case SendResult::Dropped: return "Dropped";
        case SendResult::Rejected: return "Rejected";
        case SendResult::ChannelFailure: return "ChannelFailure";
        }
        return "?";
    }

} // namespace cleat
