/**
 * @file ITransport.hpp
 * @brief 传输层抽象接口
 * @details
 * 职责：隔离“传输协议”与上层会话逻辑 —— ConnectionManager/CommsManager 只依赖本接口，
 *        不感知 TCP 帧头 / HTTP 事务 / 数据报分片等落线细节。
 * 分层：会话逻辑层 → TransportSelector → ITransport → 平台层（INetworkOps / WinHTTP 等）。
 *   TCP    : [4B 大端长度][Encrypt(frame)]            —— 字节流必须自定界
 *   HTTPS  : POST body / GET 响应体 = Encrypt(frame)   —— Content-Length 定界，无帧头
 *   DNS/ICMP: DatagramCodec 分片报文
 * 线程：Connect/Disconnect/Send/Recv 由上层单线程调度（ConnectionManager 主循环）；
 *       具体传输内部可自行开线程（如 TcpTransport 的 recv 线程），对外保持简单同步语义。
 */

#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "Core/Comms/Channel/ChannelConfig.hpp"
#include "Core/Comms/Channel/IChannel.hpp"

namespace cleat {

    class INetworkOps;

    /**
     * @brief 传输层抽象接口
     * @details
     * 所有具体传输（TCP、HTTPS、DNS、ICMP）必须实现此接口。
     *
     * **迁移形态**: `ITransport` 现继承 `IChannel`, 并在本接口内为新契约的
     * 五个方法给出**默认实现**(映射到既有的 `Connect/Disconnect/IsConnected/Send/Recv`):
     *
     *     Open       → Connect
     *     Close      → Disconnect        (语义 = 作废全部链路级状态, 见下)
     *     HealthNow  → IsConnected() ? Healthy : Dead
     *     Write      → Send, 按类型映射到 {Sent, Queued, Rejected, ChannelFailure}
     *     Pump       → Recv(out, 0)
     *
     * 这样做的直接收益: **四个传输的实现文件一行都不用改** —— 它们立刻成为合法通道,
     */
    class ITransport : public IChannel {
    public:
        virtual ~ITransport() = default;

        /**
         * @brief 建立通道（含握手/认证）
         * @param cfg 传输配置
         * @return 成功返回 true，失败返回 false
         * @note 失败后通道内部状态可被 Reset() 清理后重试。
         */
        virtual bool Connect(const TransportConfig& cfg) = 0;

        /**
         * @brief 断开通道
         * @details **不变量: 关闭 = 作废全部链路级状态。**
         *          任何"属于这条链路"的排队/缓存/计数(待发队列、待交付帧、
         *          分片重组中的分片、节流时间戳、解密失败计数、流式重组缓冲)
         *          都必须在 Disconnect 中被清空 —— 否则陈旧数据会跨越重连被交付
         *          给新会话(实测 N6: 旧链路 pending_ 帧在新会话确认前优先交付,
         *          导致陈旧命令重复派发)。
 *          `Reset()` 与 Disconnect 同义(允许只转发), **不得存在第二套清理路径**:
 *          只在失败路径清理、正常重连漏清, 正是  的成因。
 *          **已登记豁免**: DNS 上行待发队列与
 *          UDP socket —— 分片带会话身份戳, 关闭时仅作废异会话片, 同会话片
 *          跨重连续发; socket 保活以稳定服务端 peer 身份(源端口)。"陈旧数据
 *          不落线"的意图由"发送前 sid 校验"承接(比整队清空更精确), 见
 *          `DnsTransport` 的 `QueuedFrag` 注。下行数据状态不在豁免之列。
         */
        virtual void Disconnect() = 0;

        /// 检查是否已连接
        virtual bool IsConnected() const = 0;

        /**
         * @brief 发送数据帧
         * @param frame 明文帧（JSON 或 JSON+'\0'+二进制）
         * @return 成功返回 true，失败返回 false
         * @details 加密和落线格式由传输内部处理。
         *          流式传输立即写；请求-响应传输入队，随下次轮询发出。
         */
        virtual bool Send(const std::vector<uint8_t>& frame) = 0;

        /**
         * @brief 接收一个完整明文帧（已解密）
         * @param[out] frame 接收到的帧数据
         * @param timeout_ms 超时（毫秒），0 表示非阻塞
         * @return 成功收到帧返回 true，超时或无数据返回 false（非致命）
         */
        virtual bool Recv(std::vector<uint8_t>& frame, int timeout_ms) = 0;

        /// 获取传输类型
        virtual TransportType Type() const = 0;

        /// 获取优先级（降级链排序，值越小越优先）
        virtual int Priority() const = 0;

        /// 重置通道（失败后清理，供下次尝试）
        virtual void Reset() = 0;

        // --------------------------------------------------------------------
        // IChannel 默认实现: 映射到上面的既有虚函数。
        // 四个传输无需覆写即可满足契约; 需要通道特有语义时
        // 再由具体传输覆盖。
        // --------------------------------------------------------------------

        ChannelCaps Caps() const override { return CapsForType(Type()); }

        bool Open(const TransportConfig& cfg) override { return Connect(cfg); }

        /// 与 Disconnect 同义(见 ITransport::Disconnect 的不变量)
        void Close() override { Disconnect(); }

        /// 过渡期: 只有二值。Degraded 由 的逐信号判定产生(原则 3)
        ChannelHealth HealthNow() const override {
            return IsConnected() ? ChannelHealth::Healthy : ChannelHealth::Dead;
        }

        /**
         * @brief 交出帧(可延迟落线), 并把失败区分为可重试与通道故障
         * @details 映射规则(硬约束, C5):
         *   * `Send` 成功 + TCP/HTTPS → `Sent`(立即落线)
         *   * `Send` 成功 + DNS/ICMP   → `Queued`(仅入队, 随下次 `Pump` 捎带)
         *   * `Send` 失败 + 通道仍连   → `Rejected`(本轮不发, 可重试)
         *   * `Send` 失败 + 通道已断   → `ChannelFailure`(**唯一**可驱动换候选的取值)
         * 最后两行正是 `TransportSelector.cpp:210/:229` 那条 `!ok && !IsConnected()`
         * 隐式判断的显式化。
         *
         * **覆写豁免**: 具体传输可覆写本方法, 把"**永久**发不出去"
         * (重试永远不会成功, 如 DNS/ICMP 整帧超 255 片)如实报为 `Dropped` ——
         * 报 `Rejected` 会被 `OutboundSink` 回插队首, 队首阻塞静默瘫痪整条出站。
         * 覆写仍必须遵守上表: 两类失败都不得取 `ChannelFailure`。
         */
        SendResult Write(const std::vector<uint8_t>& frame) override {
            const bool ok = Send(frame);
            if (ok) {
                const TransportType t = Type();
                return (t == TransportType::Dns || t == TransportType::Icmp)
                    ? SendResult::Queued
                    : SendResult::Sent;
            }
            return IsConnected() ? SendResult::Rejected : SendResult::ChannelFailure;
        }

        /// 至多一次往返, 至多一帧; 无数据返回 false(非错误)
        bool Pump(std::vector<uint8_t>& out) override { return Recv(out, 0); }
    };

    // ---- 工厂函数（各传输实现提供） ----

    /// 创建 TCP 传输（平台依赖，需注入 INetworkOps）
    std::unique_ptr<ITransport> CreateTcpTransport(INetworkOps& ops);

    /// 创建 HTTPS 传输（平台无关，内部使用 WinHTTP / libcurl）
    std::unique_ptr<ITransport> CreateHttpsTransport();

    /// 创建 DNS 传输（平台依赖，需注入 INetworkOps）
    std::unique_ptr<ITransport> CreateDnsTransport(INetworkOps& ops);

    /// 创建 ICMP 传输（平台依赖，需注入 INetworkOps）
    std::unique_ptr<ITransport> CreateIcmpTransport();

} // namespace cleat