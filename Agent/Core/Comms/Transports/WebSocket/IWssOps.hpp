#pragma once
// WebSocket 通道平台缝(IWssOps):
// 平台 TLS / 升级握手 / WS 帧收发细节全部关在 PlatformWin_Wss / PlatformLinux_Wss 内,
// WsTransport 只依赖本接口 —— 与 IHttpOps 之于 HttpsTransport 同一模式。
// 语义单位 = **一条完整 WS 消息**(binary): RFC6455 分片重组与控制帧应答由实现内部处理,
// 上层(通道/会话)只见"整条消息进、整条消息出"。
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "Core/Comms/Channel/ChannelConfig.hpp"

namespace cleat {

    class INetworkOps;

    class IWssOps {
    public:
        virtual ~IWssOps() = default;

        /**
         * @brief 建连 + TLS + WebSocket 升级握手(一次调用完成, 含 key/Accept 校验)
         *            cfg.timeout_ms 约束"连接+握手"总时长
         * @return false 的唯一语义 = 通道未建立(可 Close 清理后重试)
         */
        virtual bool Handshake(const TransportConfig& cfg) = 0;

        /// 发送一条完整 binary 消息(整条一次落线)。
        /// false 的唯一语义 = 本条未发出且通道已不可用(调用方按断线处理)。
        virtual bool SendMessageData(const uint8_t* data, size_t len) = 0;

        /**
         * @brief 接收下一条完整 binary 消息(内部重组分片; ping 自动 pong)
         * @return 1 = out 收到完整消息; 0 = timeout_ms 内无数据(**非致命**, 可继续等);
         *         -1 = 通道死亡(对端 close / 协议错 / IO 失败)
         * @note 超时不作废在飞接收 —— 下次调用继续等同一操作(实现内部保序),
         *       这是"超时 ≠ 断线"(TCP recv_loop 同纪律)在消息制下的形态。
         */
        virtual int RecvMessage(std::vector<uint8_t>& out, int timeout_ms) = 0;

        /// 主动关闭(尽力发 close 帧 + 释放底层连接; 幂等; 必须唤醒挂起的收发)
        virtual void Close() = 0;

        /// 底层是否可用(握手成功且未发生致命错误); 决定发送失败的归因()
        virtual bool IsAlive() const = 0;
    };

    /**
     * @brief 工厂(平台条件编译, CMake 按文件名过滤)
     * @param net_ops 预留的统一网络原语注入口(当前双平台实现均自持实例:
     *                Win=WinHTTP 全托管, Linux=自持 NetworkOpsLinux, 与 HttpOpsLinux
     *                同款) —— 参数保留以维持"注册表统一传 deps"的装配形状。
     *                平台不支持时返回 nullptr(装配层按"通道构建失败"跳过)。
     */
    std::unique_ptr<IWssOps> CreateWssOps(INetworkOps& net_ops);

} // namespace cleat
