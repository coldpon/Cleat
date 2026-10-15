// WsTransport.cpp
#include "WsTransport.hpp"

#include "Common/Logger/ErrLog.hpp"
#include "Common/Logger/Logger.hpp"
#include "Core/Comms/Transports/WebSocket/WsWireCodec.hpp"

#include <chrono>
#include <thread>
#include <utility>

namespace cleat {

    namespace {
        // recv 线程的收拍间隔: RecvMessage 超时用配置值的 1/4(下限 100ms),
        // 保证 Disconnect 发起后线程在一个可感知周期内看到 running_ 翻转。
        constexpr int kRecvSliceMinMs = 100;
    }

    WsTransport::WsTransport(std::unique_ptr<IWssOps> ops)
        : ops_(std::move(ops)) {
    }

    WsTransport::~WsTransport() {
        Disconnect();
    }

    bool WsTransport::Connect(const TransportConfig& cfg) {
        if (connected_.load() || recv_thread_.joinable()) {
            Disconnect();
        }
        if (!ops_) {
            return false;
        }
        decode_fail_count_ = 0;
        clear_inbox();
        if (!ops_->Handshake(cfg)) {
            return false;
        }
        running_.store(true);
        connected_.store(true);
        try {
            recv_thread_ = std::thread(&WsTransport::recv_loop, this);
        }
        catch (...) {
            running_.store(false);
            connected_.store(false);
            ops_->Close();
            return false;
        }
        return true;
    }

    void WsTransport::Disconnect() {
        running_.store(false);
        connected_.store(false);
        {
            std::lock_guard<std::mutex> lock(cv_mutex_);
            data_cv_.notify_all();
        }
        // 先关 ops(必须唤醒挂起的 RecvMessage), recv 线程随之退出
        if (ops_) {
            ops_->Close();
        }
        // 外部 Disconnect/析构负责 join; detach 与外部 join 并发竞争是 UB。
        try {
            if (recv_thread_.joinable()) {
                if (recv_thread_.get_id() == std::this_thread::get_id()) {
                    // 自己线程: 不 join 不 detach —— 线程即将自然退出(break)
                }
                else {
                    recv_thread_.join();
                    recv_thread_ = std::thread();
                }
            }
        }
        catch (const std::exception& e) {
            CLEAT_LOGE(ErrDom::Net, ErrCode::UnhandledException,
                "WsTransport: Disconnect join 异常: " << e.what());
            if (recv_thread_.joinable()) {
                recv_thread_.detach();   // 兜底: 无法 join 则分离, 防析构 terminate
            }
        }
        catch (...) {
            CLEAT_LOGE(ErrDom::Net, ErrCode::UnhandledException, "WsTransport: Disconnect join 未知异常");
            if (recv_thread_.joinable()) {
                recv_thread_.detach();
            }
        }
        clear_inbox();
    }

    bool WsTransport::IsConnected() const {
        return connected_.load();
    }

    ChannelHealth WsTransport::HealthNow() const {
        // 原则 2: Dead 集合 == connected_=false 的集合(不多不少)
        if (!connected_.load()) return ChannelHealth::Dead;
        // 原则 3: Degraded 仅观测 —— 不参与任何决策
        return degraded_.load() ? ChannelHealth::Degraded : ChannelHealth::Healthy;
    }

    void WsTransport::MarkDegraded(const char* why) {
        // 断链/收尾后 recv 线程闭嘴(TCP 同款: 日志 ostream 与析构并发曾实测 AV)
        if (!running_.load()) {
            return;
        }
        if (!degraded_.exchange(true)) {
            LOG_DEBUG("WsTransport: Health=Degraded (" << why << ")");
        }
    }

    SendResult WsTransport::Write(const std::vector<uint8_t>& frame) {
        if (frame.size() > WsWireCodec::kMaxSessionFrame) {
            // 永久发不出去: 如实报 Dropped(报 Rejected 会被 OutboundSink 回插队首,
            CLEAT_LOGE(ErrDom::Net, ErrCode::ResourceExhausted,
                "WsTransport: 帧超会话上限, 丢弃 len=" << frame.size());
            return SendResult::Dropped;
        }
        const bool ok = Send(frame);
        if (ok) {
            return SendResult::Sent;   // Push/Stream: 立即落线
        }
        return IsConnected() ? SendResult::Rejected : SendResult::ChannelFailure;
    }

    bool WsTransport::Send(const std::vector<uint8_t>& frame) {
        if (!connected_.load() || !ops_) {
            LOG_DEBUG("WsTransport: Send 未连接, 丢弃 len=" << frame.size());
            return false;
        }
        std::vector<uint8_t> wire;
        if (!WsWireCodec::EncodeSessionWire(frame, wire)) {
            return false;
        }
        // 发送路径异常隔离(TCP 同款: 链路死亡瞬间并发路径可能抛异常, 统一吞掉按断线处理)
        try {
            if (!ops_->SendMessageData(wire.data(), wire.size())) {
                CLEAT_LOGE(ErrDom::Net, ErrCode::PlatformApiFailed,
                    "WsTransport: 消息发送失败 len=" << wire.size());
                // (ops 自身串行化), 但解锁后调 Disconnect 的次序纪律不变。
                Disconnect();
                return false;
            }
            return true;
        }
        catch (const std::exception& e) {
            CLEAT_LOGE(ErrDom::Net, ErrCode::UnhandledException,
                "WsTransport: Send 异常(按断线处理): " << e.what());
            Disconnect();
            return false;
        }
        catch (...) {
            CLEAT_LOGE(ErrDom::Net, ErrCode::UnhandledException, "WsTransport: Send 未知异常(按断线处理)");
            Disconnect();
            return false;
        }
    }

    bool WsTransport::Recv(std::vector<uint8_t>& frame, int timeout_ms) {
        std::unique_lock<std::mutex> lock(inbox_mutex_);
        auto extract = [&]() {
            // 取件 + 解码在锁内完成(收件箱互斥兼任解码串行化; 解码纯 CPU, 量小)
            if (inbox_.empty()) {
                return false;
            }
            std::vector<uint8_t> msg = std::move(inbox_.front());
            inbox_.pop_front();
            inbox_bytes_ -= msg.size();
            lock.unlock();
            const bool ok = try_extract(msg, frame);
            lock.lock();
            return ok;
            };
        if (timeout_ms <= 0) {
            return extract();
        }
        const auto deadline = std::chrono::steady_clock::now()
            + std::chrono::milliseconds(timeout_ms);
        while (true) {
            if (extract()) {
                return true;
            }
            if (!connected_.load()) {
                return false;   // 断线后不再等待
            }
            if (data_cv_.wait_until(lock, deadline) == std::cv_status::timeout) {
                return extract();
            }
        }
    }

    bool WsTransport::try_extract(std::vector<uint8_t>& msg, std::vector<uint8_t>& frame) {
        // 解码失败计数在锁外维护(recv/Recv 双入口, 仅计数无并发语义问题:
        // 值性计数, 熔断触发多算一次无害)
        if (!WsWireCodec::DecodeSessionBody(msg)) {
            //  熔断(TCP 同款): 连续解码失败说明连接不可用, 强制断线重连
            MarkDegraded("T9 decode_fail");
            ++decode_fail_count_;
            if (decode_fail_count_ >= kDecodeFailThreshold) {
                CLEAT_LOGE(ErrDom::Net, ErrCode::CryptoOpFailed,
                    "WsTransport: 连续 " << decode_fail_count_ << " 帧解码失败, 强制断线重连");
                decode_fail_count_ = 0;
                Disconnect();
            }
            return false;
        }
        decode_fail_count_ = 0;
        ClearDegraded();
        frame = std::move(msg);
        return true;
    }

    void WsTransport::recv_loop() {
        std::vector<uint8_t> msg;
        while (running_.load()) {
            if (!ops_) {
                break;
            }
            msg.clear();
            const int r = ops_->RecvMessage(msg, kRecvSliceMinMs);
            if (r == 1) {
                // 收件箱写入: 字节总量超上限判死(B3-1 同语义 —— 静默丢消息会破坏
                // 帧边界, "活着但丢帧"比断线重连更危险)
                bool dead = false;
                {
                    std::lock_guard<std::mutex> lock(inbox_mutex_);
                    if (inbox_bytes_ + msg.size() > kInboxBytesCap) {
                        dead = true;
                    }
                    else {
                        inbox_bytes_ += msg.size();
                        inbox_.push_back(std::move(msg));
                    }
                }
                if (dead) {
                    CLEAT_LOGE(ErrDom::Net, ErrCode::ResourceExhausted,
                        "WsTransport: 收件箱超 " << kInboxBytesCap / 1024 << "KB, 判定链路不可用, 强制重连");
                    connected_.store(false);
                    break;
                }
                data_cv_.notify_all();
                continue;
            }
            if (r == 0) {
                continue;   // 拍内无数据, 非致命(TCP 超时同纪律)
            }
            // -1: 通道死亡(对端 close / 协议错 / IO 失败) —— 收摊退出,
            // 由上层(SessionLoop)驱动重连; 收尾发起时(running_=false)静默退出
            connected_.store(false);
            if (running_.load()) {
                CLEAT_LOGE(ErrDom::Net, ErrCode::PeerDisconnected,
                    "WsTransport: 通道死亡(对端断开/协议错), 断线待重连");
            }
            break;
        }
    }

    void WsTransport::clear_inbox() {
        std::lock_guard<std::mutex> lock(inbox_mutex_);
        //  作废全部链路级状态 —— 收件箱是"待交付帧", 断线必须清空,
        // 否则陈旧命令跨重连交付( 实测缺陷的 WS 侧形态)
        inbox_.clear();
        inbox_bytes_ = 0;
    }

} // namespace cleat
