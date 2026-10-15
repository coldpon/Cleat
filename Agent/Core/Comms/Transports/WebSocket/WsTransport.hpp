#pragma once
// WebSocket 传输实现(wss):
// 持有 WS 通道全部会话件: IWssOps 生命周期 + recv 线程 + 消息收件箱;
// 实现 ITransport(Connect/Disconnect/Send/Recv)。
//           见 WsWireCodec; 伪装壳不适用 —— WS 升级握手本身即壳。
// 线程模型: Connect 完成 IWssOps::Handshake 后启动 recv 线程(收完整消息 -> 收件箱),
//           断线/错误自行退出; 上层经 IsConnected 感知并驱动重连(Connect 可重复调用,
//           内部先拆旧线程)。收件箱存**原始 WS 消息**, 解码在 try_extract(与 TCP 的
//           "ring 存字节、取帧时解码"同构)。
// 与 TcpTransport 对齐的实测护栏(逐条照抄, 不重新发明):
//   - B3-1: 收件箱写满判死(不能静默丢消息, 半交付 = 帧边界破坏)。
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "Core/Comms/Channel/ChannelHealth.hpp"
#include "Core/Comms/Channel/SendResult.hpp"
#include "Core/Comms/Transports/ITransport.hpp"
#include "Core/Comms/Transports/WebSocket/IWssOps.hpp"

namespace cleat {

    class WsTransport : public ITransport {
    public:
        /// 持有 ops 所有权(注册表/夹具经此注入, 生命周期随通道)
        explicit WsTransport(std::unique_ptr<IWssOps> ops);
        ~WsTransport() override;

        WsTransport(const WsTransport&) = delete;
        WsTransport& operator=(const WsTransport&) = delete;
        WsTransport(WsTransport&&) = delete;
        WsTransport& operator=(WsTransport&&) = delete;

        bool Connect(const TransportConfig& cfg) override;
        void Disconnect() override;
        bool IsConnected() const override;
        /**
         * @brief 通道健康档(, TCP 同口径)
         */
        ChannelHealth HealthNow() const override;
        bool Send(const std::vector<uint8_t>& frame) override;
        bool Recv(std::vector<uint8_t>& frame, int timeout_ms) override;

        TransportType Type() const override { return TransportType::Ws; }
        int Priority() const override { return 1; }   // 高带宽流式, 仅次于 TCP
        void Reset() override { Disconnect(); }

        /**
         * @brief 覆写: WS 是 Push/Stream, 发送即落线(Sent);
         *        帧超会话上限报 Dropped(重试永远不会成功, 不得报 Rejected 队首阻塞 ——
         *        覆写豁免口径, 与 DNS/ICMP 整帧超片同源; 见 ITransport::Write 注)
         */
        SendResult Write(const std::vector<uint8_t>& frame) override;

    private:
        void recv_loop();
        // 原始 WS 消息 -> 明文会话帧(WsWireCodec::DecodeSessionBody); 失败按  熔断计数
        bool try_extract(std::vector<uint8_t>& msg, std::vector<uint8_t>& frame);
        void clear_inbox();
        void MarkDegraded(const char* why);
        void ClearDegraded() { degraded_.store(false); }

        std::unique_ptr<IWssOps> ops_;

        std::atomic<bool> running_{ false };
        std::atomic<bool> connected_{ false };

        // 收件箱: 原始 WS 消息(未解码) + 字节总量上限(B3-1 同语义判死)
        std::mutex inbox_mutex_;
        std::deque<std::vector<uint8_t>> inbox_;
        size_t inbox_bytes_ = 0;
        static constexpr size_t kInboxBytesCap = 50 * 1024 * 1024;   // 对齐 ring 口径

        std::mutex cv_mutex_;
        std::condition_variable data_cv_;

        int decode_fail_count_ = 0;
        static constexpr int kDecodeFailThreshold = 16;

        /// Degraded 信号位(仅观测; 由一次成功收帧清除)
        std::atomic<bool> degraded_{ false };

        std::thread recv_thread_;
    };

} // namespace cleat
