// TcpTransport.cpp
#include "TcpTransport.hpp"

#include "Core/Comms/Transports/Tcp/Internal/RingBuffer.hpp"
#include "Core/Comms/Transports/Tcp/Internal/FrameParser.hpp"
#include "Core/Comms/Transports/Tcp/Internal/TrafficShaper.hpp"
#include "Core/Comms/Transports/Tcp/TcpWireCodec.hpp"
#include "Core/Config/AgentConfig.hpp"
#include "Core/Config/UA/UAProvider.hpp"
#include "Common/Logger/ErrLog.hpp"
#include "Common/Logger/Logger.hpp"

#include <chrono>
#include <thread>
#include <utility>

namespace cleat {

	TcpTransport::TcpTransport(INetworkOps& network_ops)
		: network_ops_(network_ops) {
		// 缓冲/帧解析为 TCP 私有件
		ring_ = std::make_unique<RingBuffer>(1 * 1024 * 1024, 50 * 1024 * 1024);
		parser_ = std::make_unique<FrameParser>(*ring_);
		recv_buffer_.resize(RECV_CHUNK_SIZE);
	}

	TcpTransport::~TcpTransport() {
		Disconnect();
	}

	bool TcpTransport::Connect(const TransportConfig& cfg) {
		LOG_INFO("TcpTransport: Connect called!");
		if (connected_.load() || recv_thread_.joinable()) {
			Disconnect();
		}
		// 流状态复位(B1-3 修复): 旧连接可能断在"帧体已收、下一帧伪装头未剥"的
		// 中间状态, 且可能已积累了解码失败计数 —— 新连接是全新字节流,
		// 不复位会把新流第一个 HTTP 头当帧体解析, 触发熔断重连循环。
		if (AgentConfig::Instance().GetTcpMimic()) {
			awaiting_header_ = true;
		}
		decode_fail_count_ = 0;

		// 连接级粘性 UA(伪装壳纪律: 同一连接内 UA 恒定 —— 每帧变化是异常特征;
		// 远程下发 http.user_agent 非空时取配置值, 否则池随机)
		conn_ua_ = UAProvider::Pick();

		INetworkOps::SocketHandle fd = network_ops_.CreateTcpSocket();
		if (fd < 0) {
			return false;
		}

		if (!network_ops_.SetSocketRecvTimeout(fd, static_cast<int>(cfg.timeout_ms))) {
			network_ops_.CloseSocket(fd);
			return false;
		}

		if (!network_ops_.ConnectSocket(fd, cfg.host, cfg.port)) {
			network_ops_.CloseSocket(fd);
			return false;
		}

		sockfd_ = fd;

		running_.store(true);
		connected_.store(true);
		try {
			recv_thread_ = std::thread(&TcpTransport::recv_loop, this);
		}
		catch (...) {
			running_.store(false);
			connected_.store(false);
			network_ops_.CloseSocket(fd);
			sockfd_ = -1;
			return false;
		}

		return true;
	}

	void TcpTransport::Disconnect() {
		running_.store(false);
		connected_.store(false);
		{
			std::lock_guard<std::mutex> lock(cv_mutex_);
			data_cv_.notify_all();
		}
		close_socket();
		//  根因: recv 线程打日志 → EventReport sink →
		// network_->Send → 发送失败 → Disconnect → join 自己(std::thread::join 对
		// 当前线程抛 resource deadlock)。recv 线程自己的退出路径是 break 后自然
		// 结束, 外部 Disconnect/析构负责 join —— 这里绝不能 join 自己, 分离即可。
		try {
			if (recv_thread_.joinable()) {
				if (recv_thread_.get_id() == std::this_thread::get_id()) {
					// 自己线程: 不 join 不 detach —— 线程即将自然退出(break),
					// 外部 Disconnect/析构会 join 到已退出线程(立即返回);
					// detach 与外部 join 并发竞争 _Thr 是 UB, 绝不能做。
				}
				else {
					recv_thread_.join();
					recv_thread_ = std::thread();
				}
			}
		}
		catch (const std::exception& e) {
			CLEAT_LOGE(ErrDom::Net, ErrCode::UnhandledException, "TcpTransport: Disconnect join 异常: " << e.what());
			if (recv_thread_.joinable()) {
				recv_thread_.detach();   // 兜底: 无法 join 则分离, 防析构 terminate
			}
		}
		catch (...) {
			CLEAT_LOGE(ErrDom::Net, ErrCode::UnhandledException, "TcpTransport: Disconnect join 未知异常");
			if (recv_thread_.joinable()) {
				recv_thread_.detach();
			}
		}
		ring_->Clear();
	}

	void TcpTransport::Reset() {
		Disconnect();
	}

	ChannelHealth TcpTransport::HealthNow() const {
	// 原则 2: Dead 集合 == 今天 connected_=false 的集合(不多不少)
	if (!connected_.load()) return ChannelHealth::Dead;
	// 原则 3: Degraded 仅观测 —— 不参与任何决策
	return degraded_.load() ? ChannelHealth::Degraded : ChannelHealth::Healthy;
	}

	void TcpTransport::MarkDegraded(const char* why) {
	// 断链/收尾后 recv 线程必须闭嘴: Disconnect(close_socket)会让阻塞的 recv 以
	// 瞬态错误(997)返回, 此刻主线程已在收尾路径上 —— 日志的 ostream/堆操作与析构
	// 并发曾实测 AV(soak 两份 dump: recv_loop → MarkDegraded → ostringstream)。
	// running_ 置 false 即视为"本对象对 recv 线程只读降级", 不再产生任何日志。
	if (!running_.load()) {
		return;
	}
	if (!degraded_.exchange(true)) {
		// DEBUG 级: 观测态不触发 EventReport, 不产生上报流量
		LOG_DEBUG("TcpTransport: Health=Degraded (" << why << ")");
	}
	}

	bool TcpTransport::IsConnected() const {
		return connected_.load();
	}

	bool TcpTransport::Send(const std::vector<uint8_t>& frame) {
		if (!connected_.load()) {
			LOG_DEBUG("TcpTransport: Send 未连接, 丢弃 len=" << frame.size());
			return false;
		}

		std::unique_lock<std::recursive_mutex> lock(send_mutex_);   //  recursive(close_socket 重入)

		if (sockfd_ < 0) {
			LOG_DEBUG("TcpTransport: Send 无 socket, 丢弃 len=" << frame.size());
			return false;
		}

		// 帧/加密逻辑在 TcpWireCodec(Phase 0 S2): 明文帧 -> 线上字节
		std::vector<uint8_t> out;
		if (!TcpWireCodec::EncodeWire(frame, out, conn_ua_)) {
			CLEAT_LOGE(ErrDom::Net, ErrCode::CryptoOpFailed, "TcpTransport: Send 帧编码失败 (len=" << frame.size() << ")");
			return false;
		}

		//发送路径异常隔离 —— 链路死亡瞬间(如对端 RST)并发路径
		// 可能抛 C++ 异常(实测 0xE06D7363 逃逸到 VEH), 这里统一吞掉按断线处理,
		// 由上层重连, 异常不应外泄。
		try {
			size_t sent = 0;
			while (sent < out.size()) {
				int n = network_ops_.SendSocket(sockfd_, out.data() + sent, out.size() - sent);
				if (n <= 0) {
					CLEAT_LOGE(ErrDom::Net, ErrCode::PlatformApiFailed, "TcpTransport: Send 帧发送失败 n=" << n
						<< " err=" << network_ops_.GetLastSocketError()
						<< " len=" << out.size() << " sent=" << sent);
					// 持锁调 Disconnect → join(recv_thread_) 时, recv 线程若正在
					// 退出路径(FIN/瞬态/错误)会调 close_socket() 取同一把锁 →
					// 互等死锁(对端不可达写满发送缓冲/RST 且发侧先感知时必然触发,
					// 进程活着但永久失联)。解锁后 recv 退出路径可拿到锁正常关闭。
					lock.unlock();
					Disconnect();
					return false;
				}
				sent += n;
			}
			return true;
		}
		catch (const std::exception& e) {
			CLEAT_LOGE(ErrDom::Net, ErrCode::UnhandledException, "TcpTransport: Send 异常(按断线处理): " << e.what());
			// 调用内部再抛异常会落到本 catch —— 此时锁已不持有, 无条件 unlock()
			// 会抛 system_error(EPERM) 覆盖真实异常。owns_lock 判定后再放。
			if (lock.owns_lock()) lock.unlock();
			Disconnect();
			return false;
		}
		catch (...) {
			CLEAT_LOGE(ErrDom::Net, ErrCode::UnhandledException, "TcpTransport: Send 未知异常(按断线处理)");
			if (lock.owns_lock()) lock.unlock();
			Disconnect();
			return false;
		}
	}

	bool TcpTransport::Recv(std::vector<uint8_t>& frame, int timeout_ms) {
		std::unique_lock<std::mutex> lock(recv_mutex_);
		if (timeout_ms <= 0) {
			return try_extract(frame);
		}
		// 阻塞等待: recv 线程写入 RingBuffer 时通知; 超时前最后再取一次
		const auto deadline = std::chrono::steady_clock::now()
			+ std::chrono::milliseconds(timeout_ms);
		while (true) {
			if (try_extract(frame)) {
				return true;
			}
			if (data_cv_.wait_until(lock, deadline) == std::cv_status::timeout) {
				return try_extract(frame);
			}
		}
	}

	bool TcpTransport::try_extract(std::vector<uint8_t>& frame) {
		// 流量整形(运行时开关): 每帧前剥离 HTTP 伪装头(到 \r\n\r\n 为止)
		if (AgentConfig::Instance().GetTcpMimic() && awaiting_header_) {
			if (!TrafficShaper::TryStripHeader(*ring_)) {
				return false; // 头未完整到达, 等待更多数据
			}
			awaiting_header_ = false;
		}
		std::vector<uint8_t> payload;
		if (!parser_->TryParse(payload)) {
			return false; // 帧未完整
		}
		if (AgentConfig::Instance().GetTcpMimic()) {
			awaiting_header_ = true; // 下一帧前仍需剥头
		}
		// 帧体 -> 明文帧(剥填充 + 解密); 失败则丢弃该帧(错误已在 codec 内记录)
		if (!TcpWireCodec::DecodeWire(payload)) {
			//  熔断: 连续解码失败(key_id 不匹配/PSK 错/坏流)说明连接已不可用,
			// 强制断线让上层重连并重新握手 —— 否则连接"活着"但功能全废, 永不恢复
	MarkDegraded("T9 decode_fail");
	if (++decode_fail_count_ >= kDecodeFailThreshold) {
				CLEAT_LOGE(ErrDom::Net, ErrCode::CryptoOpFailed, "TcpTransport: 连续 " << decode_fail_count_
					<< " 帧解码失败, 强制断线重连");
				decode_fail_count_ = 0;
				Disconnect();
			}
			return false;
		}
		decode_fail_count_ = 0;
		ClearDegraded();   // 成功解出一帧 = 健康信号
		frame = std::move(payload);
		return true;
	}

	void TcpTransport::recv_loop() {
		//瞬态错误(Windows 997=ERROR_IO_PENDING, 超时取消竞态泄露; 10035=
		// WSAEWOULDBLOCK)与超时同属"暂时无数据", 不可判死; 但持续瞬态(如误置
		// 非阻塞)须限时判死, 防"活着但永无数据"的快速自旋 —— 每次退避 500ms,
		// 连续 kSpuriousFatalStreak 次(≈6s)未恢复才断线重连。
		constexpr int kSpuriousFatalStreak = 12;
		constexpr auto kSpuriousReset = std::chrono::seconds(5);
		int spurious_streak = 0;
		auto last_spurious = std::chrono::steady_clock::time_point{};
		bool spurious_warned = false;

		while (running_.load()) {
			const INetworkOps::SocketHandle fd = sockfd_.load();
			if (fd < 0) {
				// 无 socket: 线程由 Connect 创建/Disconnect 回收, 直接退出
				break;
			}

			int n = network_ops_.RecvSome(fd, recv_buffer_.data(), recv_buffer_.size());
			if (n > 0) {
				spurious_streak = 0;
				last_spurious = std::chrono::steady_clock::time_point{};
				const size_t written = ring_->Write(recv_buffer_.data(), static_cast<size_t>(n));
				// B3-1 修复: 环形缓冲写满(50MB 上限, 仅上层消费停滞/
				// 异常流时出现)时, 部分写入会破坏帧边界导致后续解析错位 —— 不能
				// 静默丢字节, 判定链路不可用并断开重连( 熔断同语义)。
				if (written != static_cast<size_t>(n)) {
					CLEAT_LOGE(ErrDom::Net, ErrCode::ResourceExhausted, "TcpTransport: recv 环形缓冲写满(written=" << written
						<< "/" << n << "), 判定链路不可用, 强制重连");
					connected_.store(false);
					close_socket();
					break;
				}
				{
					std::lock_guard<std::mutex> lock(cv_mutex_);
					data_cv_.notify_all();
				}
				continue;
			}
			if (n == 0) {
				// 归因日志: 本分支只在收到对端 FIN 时进入 —— 出现即说明 FIN
				// 来自对端(server 侧)而非本端错误; 健康链路上不应出现
				CLEAT_LOGE(ErrDom::Net, ErrCode::PeerDisconnected, "TcpTransport: recv 收到对端 FIN(n==0), 对端主动断开, fd="
					<< fd);
				connected_.store(false);
				close_socket();
				break;
			}
			const int err = network_ops_.GetLastSocketError();
			if (err == network_ops_.GetSocketErrorTimeout()) {
				continue; // 普通接收超时, 继续等待
			}
			MarkDegraded("T6 transient");
			if (!running_.load()) {
				break;   // Disconnect 已发起: 不再进入瞬态等待, 立即收摊(防与析构竞态)
			}
			if (network_ops_.IsTransientRecvError(err)) {
				const auto now = std::chrono::steady_clock::now();
				if (last_spurious != std::chrono::steady_clock::time_point{}
					&& now - last_spurious > kSpuriousReset) {
					spurious_streak = 0; // 距上次间隔过久: 视为新的孤立事件
				}
				++spurious_streak;
				last_spurious = now;
				if (!spurious_warned) {
					LOG_WARN("TcpTransport: recv 瞬态错误(err=" << err
						<< "), 按可重试处理, fd=" << fd);
					spurious_warned = true;
				}
				if (spurious_streak >= kSpuriousFatalStreak) {
					CLEAT_LOGE(ErrDom::Net, ErrCode::PlatformApiFailed, "TcpTransport: 瞬态错误连续 " << spurious_streak
						<< " 次未恢复, 判定链路异常, fd=" << fd);
					connected_.store(false);
					close_socket();
					break;
				}
				// 防自旋 500ms, 但分片可中断 —— Disconnect/收尾发起时立即退出,
				for (int slice = 0; slice < 10 && running_.load(); ++slice) {
					std::this_thread::sleep_for(std::chrono::milliseconds(50));
				}
				continue;
			}
			// 其他错误: 视为断线, 退出线程, 由上层(ConnectionManager)驱动重连。
			//  此前静默退出 —— 本机瞬时 socket 错误无法归因, 必须打出错误码
			// (但 Disconnect/收尾发起的断链除外: running_=false 即主线程已在收尾
			//  路径上, 这条 LOG 与析构并发曾实测 AV —— 静默退出, 上层不需要被告知)
			connected_.store(false);
			if (running_.load()) {
				CLEAT_LOGE(ErrDom::Net, ErrCode::PlatformApiFailed, "TcpTransport: recv 错误断线 n=" << n << " err=" << err
					<< " fd=" << fd);
			}
			close_socket();
			break;
		}
	}

	void TcpTransport::close_socket() {
		// C1 修复: 关闭 fd 前持 send_mutex_ —— 防 Send 取到 fd 后
		// 被并发 Disconnect 关闭并复用(SOCKET 句柄值复用 → 旧数据写新连接)。
		// send_mutex_ 为 recursive: Send 失败路径(持锁中)调 Disconnect → close_socket
		// 重入本锁, 递归允许同线程重入避免自死锁。
		std::lock_guard<std::recursive_mutex> lock(send_mutex_);
		const INetworkOps::SocketHandle fd = sockfd_.load();
		if (fd >= 0) {
			// L1-10(Linux): close 不唤醒阻塞 recv —— 先 shutdown(SHUT_RDWR) 让
			// recv 线程立即返回, 否则 Disconnect 的 join 会卡满 SO_RCVTIMEO(5s)。
			// Windows 实现为空操作(closesocket 自带唤醒)。
			network_ops_.AbortSocket(fd);
			network_ops_.CloseSocket(fd);
			sockfd_.store(-1);
		}
	}

	std::unique_ptr<ITransport> CreateTcpTransport(INetworkOps& ops) {
		return std::make_unique<TcpTransport>(ops);
	}

} // namespace cleat
