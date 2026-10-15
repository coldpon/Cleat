/**
 * @file IQuicOps.hpp
 * @brief QUIC 通道平台缝(IQuicOps)。
 * @details msquic 的注册/配置/连接/流/事件细节全部关在 MsQuicOps 内, QuicTransport
 *          只依赖本接口 —— 与 IWssOps 之于 WsTransport 同一模式(依赖注入, 夹具可替换)。
 * 语义单位 = 一段 stream 字节(保序); 会话帧定界([4B][Encrypt][填充], QuicWireCodec)
 *          由上层完成 —— 平台层只保证"字节进、字节出"。
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "Core/Comms/Channel/ChannelConfig.hpp"

namespace cleat {

    class IQuicOps {
    public:
        virtual ~IQuicOps() = default;

        /**
         * @brief 建连 + QUIC/TLS1.3 握手 + 开单条双向 stream(一次调用完成)
         * @param cfg cfg.url = quic://host[:port][/path](必填); cfg.timeout_ms 约束
         *            "连接+握手+开流"总时长
         * @return false 的唯一语义 = 通道未建立(可 Close 清理后重试)
         */
        virtual bool Handshake(const TransportConfig& cfg) = 0;

        /// 发送一段字节(保序; 发送缓冲生命周期由实现管理至落线完成)。
        /// false 的唯一语义 = 本条未发出且通道已不可用(调用方按断线处理)。
        virtual bool SendStreamData(const uint8_t* data, size_t len) = 0;

        /**
         * @brief 接收下一段 stream 字节
         * @return 1 = out 收到数据; 0 = timeout_ms 内无数据(**非致命**, 可继续等);
         *         -1 = 通道死亡(对端关闭/传输错误/内部判死)
         * @note 超时不作废在飞接收 —— 下次调用继续等(实现内部保序), 这是
         *       "超时 ≠ 断线"(TCP recv_loop 同纪律)在 stream 制下的形态。
         */
        virtual int RecvStreamData(std::vector<uint8_t>& out, int timeout_ms) = 0;

        /// 主动关闭(幂等; 必须唤醒挂起的 RecvStreamData)
        virtual void Close() = 0;

        /// 底层是否可用(握手完成且未发生致命错误); 决定发送失败的归因()
        virtual bool IsAlive() const = 0;
    };

    /**
     * @brief 工厂(平台 + 构建期开关条件编译)
     * @return Windows 且 CLEAT_WITH_QUIC=ON 时返回 msquic 实现; 其他平台
     *         (ThirdParty/msquic 仅含 Windows 产物)或构建期 CLEAT_WITH_QUIC=OFF
     *         (体积裁剪, CMakeLists 定义 CLEAT_NO_QUIC=1)返回 nullptr —— 装配层按
     *         "该平台不支持"跳过, SelfCheck (c) 类会点名, 与 make_pipe 的平台行为同模式。
     */
    std::unique_ptr<IQuicOps> CreateQuicOps();

} // namespace cleat
