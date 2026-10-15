// Core/Comms/Transports/Tcp/TcpTransport.hpp
// TCP 传输实现:
// 持有 TCP 专属全部件: socket 生命周期 + recv 线程 + RingBuffer/FrameParser 流解析
// + 伪装头剥离状态; 实现 ITransport(Connect/Disconnect/Send/Recv)。
// 线上落线: [4B 大端长度][加密载荷](可选 HTTP 伪装壳 + 帧填充), 见 TcpWireCodec。
// 线程模型: Connect 启动 recv 线程(读 socket -> RingBuffer), 断线/错误自行退出;
//           上层经 IsConnected 感知并驱动重连(Connect 可重复调用, 内部先拆旧线程)。
// 与旧 CommsManager 行为对齐点
//   - recv 超时(err == GetSocketErrorTimeout)继续等, 不视为断线;
//   - Send 失败触发 Disconnect(与上层重连逻辑配合);
//   - 伪装头剥离状态(awaiting_header_)随流解析, 每帧一个头。
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "Core/Comms/Platform/INetworkOps.hpp"
#include "Core/Comms/Transports/ITransport.hpp"

namespace cleat {

	class RingBuffer;
	class FrameParser;

	class TcpTransport : public ITransport {
	public:
		explicit TcpTransport(INetworkOps& network_ops);
		~TcpTransport() override;

		TcpTransport(const TcpTransport&) = delete;
		TcpTransport& operator=(const TcpTransport&) = delete;
		TcpTransport(TcpTransport&&) = delete;
		TcpTransport& operator=(TcpTransport&&) = delete;

		bool Connect(const TransportConfig& cfg) override;
		void Disconnect() override;
		bool IsConnected() const override;
	
	/**
	 * @brief 通道健康档
	 * @details Dead <=> `connected_ == false`(逐条对应, 不多不少 —— B.1 原则 2);
	 *          **P1 只观测**: 不改变任何重连/调度/丢弃行为(B.1 原则 3)。
	 */
	ChannelHealth HealthNow() const override;
		bool Send(const std::vector<uint8_t>& frame) override;
		bool Recv(std::vector<uint8_t>& frame, int timeout_ms) override;

		TransportType Type() const override { return TransportType::Tcp; }
		int Priority() const override { return 0; }   // TCP 为默认最高优先级
		void Reset() override;                        // 清理通道状态, 供下次 Connect

	private:
		void recv_loop();
		void close_socket();
		// 非阻塞取一帧: 剥伪装头(必要时) + FrameParser 解析 + TcpWireCodec 解码
		// 无完整帧返回 false(数据不足/头未完整/解码失败)
		bool try_extract(std::vector<uint8_t>& frame);

		INetworkOps& network_ops_;
		std::unique_ptr<RingBuffer> ring_;
		std::unique_ptr<FrameParser> parser_;

		std::atomic<bool> running_{ false };
		std::atomic<bool> connected_{ false };
		// 原子句柄: recv 线程与 Connect/Disconnect/close_socket 并发读写(M1 修复)
		std::atomic<INetworkOps::SocketHandle> sockfd_{ -1 };
		// 连续解码失败计数(帧已消费但解密失败): 达阈值强制断线触发重连+重新握手( 修复)
		int decode_fail_count_ = 0;
	
	/// Degraded 信号位
	std::atomic<bool> degraded_{ false };
	void MarkDegraded(const char* why);
	void ClearDegraded() { degraded_.store(false); }
		static constexpr int kDecodeFailThreshold = 16;

		std::thread recv_thread_;

		// 发送互斥(Send 与 Disconnect 共用 socket 生命周期保护)
		//  recursive —— Send 失败路径持锁调 Disconnect→close_socket 重入
		std::recursive_mutex send_mutex_;
		// 接收侧流解析互斥(剥头 + 取帧序列)
		std::mutex recv_mutex_;
		std::mutex cv_mutex_;
		std::condition_variable data_cv_;   // recv 线程写入 RingBuffer 后通知

		std::vector<uint8_t> recv_buffer_;
		static constexpr size_t RECV_CHUNK_SIZE = 64 * 1024;

		// 流量整形: 接收侧等待剥离 HTTP 伪装头的状态(每帧一个头)
		// (GetTcpMimic 运行时开关决定是否启用; 原 CLEAT_HTTP_MIMIC 宏门控已运行时化)
		bool awaiting_header_ = true;
		// 连接级粘性 UA(连接建立时经 UAProvider::Pick 选定, 见 TrafficShaper 注)
		std::string conn_ua_;
	};

	// 工厂(声明见 ITransport.hpp)
	std::unique_ptr<ITransport> CreateTcpTransport(INetworkOps& ops);

} // namespace cleat
