/**
 * @file IChannel.hpp
 * @brief 通道层抽象
 * @details
 * **两个数据原语, 语义统一**:
 *   * `Write(frame)` —— 交出帧。允许**延迟落线**(Poll 类通道只入队, 随下次 Pump 捎带)。
 *   * `Pump(out)`    —— 做**至多一次往返**, 返回至多一帧。
 *        TCP   = 非阻塞取 ring
 *        HTTPS = 一次 GET(或交付 pending)
 *        DNS   = 一次查询应答, **捎带一片上行**
 *        ICMP  = 一次回显, 同上
 *     三者由此统一, 会话层不再需要知道"谁在何时发包"。
 *
 * **与 `ITransport` 的关系(过渡期)**:
 *   本批只**新增**契约与适配器(`LegacyTransportChannel`), 不改任何现有传输;
 *   有可比对象。
 *
 * **`Close()` 语义沿用 已确立的不变量**: 作废**全部**链路级状态。
 *
 * 依赖方向: `ChannelCaps → ChannelConfig → IChannel → ITransport`(单向)

配置载体说明: 设计文档中的 `ChannelSpec` 目前即 `TransportConfig`
 * (为免与 `AgentConfig::ChannelSpec`(配置文件的通道规格)混淆, 本层继续用
 * `TransportConfig`; 若 需要重命名再统一处理)。
 */
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "Core/Comms/Channel/ChannelCaps.hpp"
#include "Core/Comms/Channel/ChannelHealth.hpp"
#include "Core/Comms/Channel/ChannelConfig.hpp"   // TransportConfig / TransportType
#include "Core/Comms/Channel/SendResult.hpp"

namespace cleat {

    /**
     * @brief 上行积压统计(/ )
     * @details "溢出**不**回错误帧"(A.7): 低带宽通道上回错误帧本身要占一次上行, 拥塞时会
     *          放大拥塞 —— 故溢出只做**本地记账 + 节流 WARN**。
     *          **不上报**: "汇报带宽不够"要占用
     *          同一份稀缺上行, 与"别放大拥塞"同源; 且 DNS/ICMP 设计上不承载大文件, 溢出属
     *          操作层误用 —— 误用由**准入**拦(`ChannelRateLimiter` 的 default 规则 4KB/10次每分
     *          + 通道物理上限 + 服务级 `IsHighBandwidth`), 不靠事后上报。
     *          本结构即**本地记账**的载体, 供观测/夹具查询。
     */
    struct ChannelOverflowStats {
        uint64_t overflow_count = 0;   ///< 溢出次数
        uint64_t dropped_bytes = 0;    ///< 因溢出丢弃的字节数
    };

    class IChannel {
    public:
        virtual ~IChannel() = default;

        /// 能力声明(纯数据; 会话层据此决策, 不再靠行为暗示)
        virtual ChannelCaps Caps() const = 0;

        /// 打开通道(含握手/认证外的连接建立); 失败后应可 Close 再重试
        virtual bool Open(const TransportConfig& cfg) = 0;

        /**
         * @brief 关闭通道
         * @details **不变量: 作废全部链路级状态** —— 待发队列、待交付帧、
         *          重组中的分片、节流时间戳、解密失败计数、流式重组缓冲。
         *          与 `ITransport::Disconnect` 同义, 且**不得存在第二套清理路径**。
         */
        virtual void Close() = 0;

        /// 当前健康档(判据见; Degraded 由逐信号实现产生, 见 ChannelHealth.hpp)
        virtual ChannelHealth HealthNow() const = 0;

        /// 交出帧(可延迟落线)。返回值语义见 SendResult.hpp 的硬约束
        virtual SendResult Write(const std::vector<uint8_t>& frame) = 0;

        /// 做至多一次往返, 返回至多一帧; 无数据返回 false(非错误)
        virtual bool Pump(std::vector<uint8_t>& out) = 0;

        /**
         * @brief 取走并复位上行积压统计()
         * @details "取走"语义: 调用方拿到的是**自上次调用以来**的增量(读后即复位)。
         *          默认返回空(Push 类通道无上行积压概念)。
         * @note 这是**本地记账的查询接口**, 不是"待上报"的取数口。
         */
        virtual ChannelOverflowStats TakeOverflowStats() { return {}; }

        /**
         * @brief 距下一次 check-in 到期的毫秒数(v1.5 节奏合一: 掩码窗长源; 设计)
         * @return 0 = 不可得/不适用(默认) —— 调用方据此回退固定窗(现状行为原样保留);
         *         请求-响应(Poll)通道 = 各自节拍钟(PollClock)的固化剩余
         * @note 可选虚函数: 仅 https/dns/icmp 覆写; Push 通道/适配器/夹具用默认 0。
         */
        virtual uint32_t NextPollDueInMs() const { return 0; }

        /**
         * @brief 通道内**已接管但未落线**的上行字节(分片型通道的队列尾巴)
         * @return 0 = 无(默认; 同步落线的通道天然为 0)
         * @note v1.5 收尾: 掩码静默判据里"sink 已排空"**不等于**"干完"——DNS/ICMP
         *       的分片出队后要经多次 Recv 才逐片落线; 稳态门按本值拒绝进窗,
         *       防止尚未发完的分片在窗口内冻结(干到一半睡觉)。
         *       dormant/退避期链路离线, 分片本就等重连 —— 那两处不消费本查询。
         */
        virtual uint32_t PendingUplinkBytes() const { return 0; }

        // ---- 过渡期兼容 ----
        virtual TransportType Type() const = 0;
        virtual int Priority() const = 0;
    };

} // namespace cleat
