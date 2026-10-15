/**
 * @file QuicTransport.hpp
 * @brief QUIC 传输实现(msquic; 单条双向 stream 承载会话帧字节流)。
 * @details 满血形态:
 *   * **双层加密**: QUIC 自带 TLS1.3, 会话载荷再经 CryptoManager(libsodium) ——
 *     与 Tcp/Ws/Https 同纪律(见 QuicWireCodec);
 *   * 线格式: [4B 大端长度][Encrypt(frame)][随机填充] —— 与 Tcp/Ws 逐条同构,
 *     服务端复用同一 strip 逻辑(server frame.go);
 *   * 帧定界: 单 stream 字节流自定界(4B 头 + kMaxWireFrame 上限防病态头);
 *   * 线程模型与实测护栏(逐条照抄 Tcp/Ws, 不重新发明):
 *         绝不 join 自己;
 *         连续解码失败达阈值强制断线;
 *       B3-1 未凑帧缓冲超上限判死(丢字节会破坏 4B 定界, 比断线更危险);
 *       关闭 = 作废全部链路级状态(缓冲/收件/计数, 不跨重连交付)。
 * 依赖: IQuicOps(平台缝; 生产实现 MsQuicOps)—— 构造注入, 夹具可替换。
 */
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "Core/Comms/Channel/ChannelHealth.hpp"
#include "Core/Comms/Channel/SendResult.hpp"
#include "Core/Comms/Transports/ITransport.hpp"
#include "Core/Comms/Transports/Quic/IQuicOps.hpp"

namespace cleat {

    class QuicTransport : public ITransport {
    public:
        /// 持有 ops 所有权(注册表/夹具经此注入, 生命周期随通道)
        explicit QuicTransport(std::unique_ptr<IQuicOps> ops);
        ~QuicTransport() override;

        QuicTransport(const QuicTransport&) = delete;
        QuicTransport& operator=(const QuicTransport&) = delete;
        QuicTransport(QuicTransport&&) = delete;
        QuicTransport& operator=(QuicTransport&&) = delete;

        bool Connect(const TransportConfig& cfg) override;
        /// 关闭 = 作废全部链路级状态
        void Disconnect() override;
        bool IsConnected() const override;
        ChannelHealth HealthNow() const override;
        bool Send(const std::vector<uint8_t>& frame) override;
        bool Recv(std::vector<uint8_t>& frame, int timeout_ms) override;

        TransportType Type() const override { return TransportType::Quic; }
        int Priority() const override { return 1; }   // 高带宽流式, 仅次于 TCP(与 https/wss 同档)
        void Reset() override { Disconnect(); }

        /// 覆写: QUIC 是 Push/Stream, 发送即落线(Sent); 帧超会话上限报 Dropped
        /// (重试永远不会成功, 不得报 Rejected 队首阻塞 —— 覆写豁免口径, 与 Ws 同源)
        SendResult Write(const std::vector<uint8_t>& frame) override;

    private:
        void recv_loop();
        /// 断线/重连时作废字节缓冲
        void clear_buffers();
        void MarkDegraded(const char* why);
        void ClearDegraded() { degraded_.store(false); }

        std::unique_ptr<IQuicOps> ops_;

        std::atomic<bool> running_{ false };
        std::atomic<bool> connected_{ false };

        // 字节缓冲 + 等待: **一把锁**(免"检查空→进 wait"之间的丢唤醒窗口;
        // pending_ 为待凑帧字节, kPendingBytesCap 超限判死 —— B3-1 同语义)
        std::mutex mtx_;
        std::condition_variable data_cv_;
        std::vector<uint8_t> pending_;
        static constexpr size_t kPendingBytesCap = 50 * 1024 * 1024;
        static constexpr int kRecvSliceMs = 100;   // recv 线程收拍(对齐 Ws kRecvSliceMinMs)

        // 仅 Recv 侧读写(recv_loop 不碰), 无需锁 —— 对齐 Ws 同款纪律。
        int decode_fail_count_ = 0;
        static constexpr int kDecodeFailThreshold = 16;

        /// Degraded 信号位(仅观测; 由一次成功收帧清除)
        std::atomic<bool> degraded_{ false };

        std::thread recv_thread_;
    };

} // namespace cleat
