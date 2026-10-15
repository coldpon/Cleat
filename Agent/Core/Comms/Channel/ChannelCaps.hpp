/**
 * @file ChannelCaps.hpp
 * @brief 通道能力声明
 * @details
 * 把"推送还是轮询"、"带宽档"、"是否需要握手"从**行为暗示**变成**声明式数据**。
 *
 * 为什么必须声明化( 的根因): 今天这三维全靠 `Send`/`Recv` 的行为差异暗示 ——
 * TCP 的 Send 是立即写、HTTPS 是一次事务并捎带回下行、DNS/ICMP 只入队而在 Recv 里
 * 才真正发包; 上层无法校验这些约定, 于是每加一个通道都要重新推导一遍全部约定。
 *
 * 消费方: 会话层按 `agency` 决定"能否主动推送"、按 `bandwidth` 做**执行前**决策
 * ( E: 带宽决策不该等到出站口)、按 `needsHandshake` 决定首帧顺序。
 */
#pragma once

#include <cstddef>
#include <cstdint>

namespace cleat {

    /// 线格式语义: 字节流(可自定界) / 请求-响应(一事务一帧)
    enum class ChannelKind {
        Stream = 0,        ///< 字节流: 必须自带长度定界(TCP: [4B][body])
        Transaction = 1    ///< 事务: 响应体定界(HTTPS 事务 / DNS 查询应答 / ICMP 回显)
    };

    /// 方向语义: 服务端能否主动写
    enum class ChannelAgency {
        Push = 0,          ///< 服务端可随时下发(TCP 长连接)
        Poll = 1           ///< 服务端只能把帧排进 outbox, 等 Agent 来取(beacon 形态)
    };

    /// 带宽档(取代 `TransportSelector::IsLowBandwidth()` 里的 if-else 链)
    enum class BandwidthClass {
        High = 0,          ///< TCP / HTTPS
        Low = 1            ///< DNS / ICMP: 单帧有硬上限, 大流量必须被拦在**执行之前**
    };

    /**
     * @brief 通道能力
     *          上行积压上限从"两处独立魔数 `kMaxQueuedFrags = 128`"提升为**按通道声明**。
     *          DNS 单分片实测 110B、ICMP 1400B(), 故按片数设同一上限会让两通道
     *          的实际容量差 13 倍 —— 因此**以字节为主, 片数作次要护栏**。
     */
    struct ChannelCaps {
        ChannelKind kind = ChannelKind::Stream;
        ChannelAgency agency = ChannelAgency::Push;
        BandwidthClass bandwidth = BandwidthClass::High;
        /// 是否需要握手前置(会话在发应用帧之前必须先完成握手步骤)。
        /// 说明: 四个通道在 X25519 模式下都为 true; 握手帧**是什么**由会话层按加密模式决定
        /// (PSK 模式下 BuildHandshakeFrame 返回空, 不产生帧), 本字段只表达"顺序约束"。
        bool needsHandshake = true;
        size_t maxFrameBytes = 50 * 1024 * 1024;   ///< 单帧上限(流式)
        size_t maxDatagramBytes = 64 * 1024;       ///< 单报文上限(数据报通道)

        // ----  上行积压(仅 Poll 类通道有意义) ----
        size_t maxQueuedUplinkBytes = 0;           ///< 0 = 不限(由实现给默认)
        uint32_t maxQueuedUplinkFrags = 0;         ///< 0 = 不限(片数次要护栏)
    };

    inline const char* ToString(BandwidthClass b) {
        return b == BandwidthClass::Low ? "Low" : "High";
    }

} // namespace cleat
