/**
 * @file QuicTransport.cpp
 * @brief QUIC 传输实现(msquic stream 制; 实现纪律见 QuicTransport.hpp 文件头)
 */
#include "Core/Comms/Transports/Quic/QuicTransport.hpp"

#include "Common/Logger/ErrLog.hpp"
#include "Common/Logger/Logger.hpp"
#include "Core/Comms/Transports/Quic/QuicWireCodec.hpp"

#include <chrono>
#include <exception>
#include <utility>

namespace cleat {

    namespace {

        /// 大端读 4B(uint32) —— 帧长定界
        uint32_t ReadBe32(const uint8_t* p) {
            return (static_cast<uint32_t>(p[0]) << 24)
                | (static_cast<uint32_t>(p[1]) << 16)
                | (static_cast<uint32_t>(p[2]) << 8)
                | static_cast<uint32_t>(p[3]);
        }

    } // namespace

    QuicTransport::QuicTransport(std::unique_ptr<IQuicOps> ops)
        : ops_(std::move(ops)) {
    }

    QuicTransport::~QuicTransport() {
        Disconnect();
    }

    bool QuicTransport::Connect(const TransportConfig& cfg) {
        if (connected_.load() || recv_thread_.joinable()) {
            Disconnect();
        }
        if (!ops_) {
            return false;
        }
        decode_fail_count_ = 0;
        clear_buffers();
        if (!ops_->Handshake(cfg)) {
            return false;
        }
        running_.store(true);
        connected_.store(true);
        try {
            recv_thread_ = std::thread(&QuicTransport::recv_loop, this);
        }
        catch (...) {
            running_.store(false);
            connected_.store(false);
            ops_->Close();
            return false;
        }
        return true;
    }

    void QuicTransport::Disconnect() {
        running_.store(false);
        connected_.store(false);
        data_cv_.notify_all();   // 唤醒挂起的 Recv
        // 先关 ops(必须唤醒挂起的 RecvStreamData), recv 线程随之退出
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
                "QuicTransport: Disconnect join 异常: " << e.what());
            if (recv_thread_.joinable()) {
                recv_thread_.detach();   // 兜底: 无法 join 则分离, 防析构 terminate
            }
        }
        catch (...) {
            CLEAT_LOGE(ErrDom::Net, ErrCode::UnhandledException,
                "QuicTransport: Disconnect join 未知异常");
            if (recv_thread_.joinable()) {
                recv_thread_.detach();
            }
        }
        clear_buffers();
    }

    bool QuicTransport::IsConnected() const {
        return connected_.load();
    }

    ChannelHealth QuicTransport::HealthNow() const {
        // 原则 2: Dead 集合 == connected_=false 的集合(不多不少)
        if (!connected_.load()) return ChannelHealth::Dead;
        // 原则 3: Degraded 仅观测 —— 不参与任何决策
        return degraded_.load() ? ChannelHealth::Degraded : ChannelHealth::Healthy;
    }

    void QuicTransport::MarkDegraded(const char* why) {
        // 断链/收尾后 recv 侧闭嘴(TCP/Ws 同款: 日志 ostream 与析构并发曾实测 AV)
        if (!running_.load()) {
            return;
        }
        if (!degraded_.exchange(true)) {
            LOG_DEBUG("QuicTransport: Health=Degraded (" << why << ")");
        }
    }

    SendResult QuicTransport::Write(const std::vector<uint8_t>& frame) {
        if (frame.size() > QuicWireCodec::kMaxSessionFrame) {
            // 永久发不出去: 如实报 Dropped(报 Rejected 会被 OutboundSink 回插队首,
            // 队首阻塞静默瘫痪整条出站 —— 覆写豁免口径, 与 Ws 同源)
            CLEAT_LOGE(ErrDom::Net, ErrCode::ResourceExhausted,
                "QuicTransport: 帧超会话上限, 丢弃 len=" << frame.size());
            return SendResult::Dropped;
        }
        const bool ok = Send(frame);
        if (ok) {
            return SendResult::Sent;   // Push/Stream: 立即落线
        }
        return IsConnected() ? SendResult::Rejected : SendResult::ChannelFailure;
    }

    bool QuicTransport::Send(const std::vector<uint8_t>& frame) {
        if (!connected_.load() || !ops_) {
            LOG_DEBUG("QuicTransport: Send 未连接, 丢弃 len=" << frame.size());
            return false;
        }
        std::vector<uint8_t> wire;
        if (!QuicWireCodec::EncodeWire(frame, wire)) {
            return false;
        }
        // 发送路径异常隔离(TCP/Ws 同款: 链路死亡瞬间并发路径可能抛异常, 统一吞掉按断线处理)
        try {
            if (!ops_->SendStreamData(wire.data(), wire.size())) {
                CLEAT_LOGE(ErrDom::Net, ErrCode::PlatformApiFailed,
                    "QuicTransport: stream 写入失败 len=" << wire.size());
                Disconnect();
                return false;
            }
            return true;
        }
        catch (const std::exception& e) {
            CLEAT_LOGE(ErrDom::Net, ErrCode::UnhandledException,
                "QuicTransport: Send 异常(按断线处理): " << e.what());
            Disconnect();
            return false;
        }
        catch (...) {
            CLEAT_LOGE(ErrDom::Net, ErrCode::UnhandledException,
                "QuicTransport: Send 未知异常(按断线处理)");
            Disconnect();
            return false;
        }
    }

    bool QuicTransport::Recv(std::vector<uint8_t>& frame, int timeout_ms) {
        const auto deadline = std::chrono::steady_clock::now()
            + std::chrono::milliseconds(timeout_ms > 0 ? timeout_ms : 0);
        std::unique_lock<std::mutex> lock(mtx_);
        while (true) {
            // ---- 锁内: 从 pending_ 凑出一个完整帧的线上字节 ----
            std::vector<uint8_t> wire;
            bool have_wire = false;
            bool bad_header = false;
            if (pending_.size() >= 4) {
                const uint32_t declared = ReadBe32(pending_.data());
                if (declared == 0 || declared > QuicWireCodec::kMaxWireFrame) {
                    // 病态长度头: 定界已不可信, 丢弃全部缓冲(坏数据后面也无从 resync)
                    CLEAT_LOGE(ErrDom::Net, ErrCode::IntegrityMismatch,
                        "QuicTransport: 长度头非法 declared=" << declared << ", 丢弃缓冲");
                    pending_.clear();
                    bad_header = true;
                }
                else if (pending_.size() >= static_cast<size_t>(4) + declared) {
                    const size_t total = static_cast<size_t>(4) + declared;
                    wire.assign(pending_.begin(), pending_.begin() + total);
                    pending_.erase(pending_.begin(), pending_.begin() + total);
                    have_wire = true;
                }
            }
            lock.unlock();

            // ---- 锁外: 解码 + 熔断(Disconnect 需在锁外调, 防自锁) ----
            if (have_wire) {
                if (QuicWireCodec::DecodeWire(wire)) {
                    decode_fail_count_ = 0;
                    ClearDegraded();
                    frame = std::move(wire);
                    return true;
                }
                //  熔断: 连续解码失败(填充非法/解密失败)说明链路不可用
                MarkDegraded("T9 decode_fail");
                if (++decode_fail_count_ >= kDecodeFailThreshold) {
                    CLEAT_LOGE(ErrDom::Net, ErrCode::CryptoOpFailed,
                        "QuicTransport: 连续 " << decode_fail_count_ << " 帧解码失败, 强制断线重连");
                    decode_fail_count_ = 0;
                    Disconnect();
                }
            }
            else if (bad_header) {
                MarkDegraded("T9 bad_len_header");
                if (++decode_fail_count_ >= kDecodeFailThreshold) {
                    CLEAT_LOGE(ErrDom::Net, ErrCode::IntegrityMismatch,
                        "QuicTransport: 长度头连续非法, 强制断线重连");
                    decode_fail_count_ = 0;
                    Disconnect();
                }
            }

            // ---- 锁内: 无帧可交付 → 退出或等待 ----
            lock.lock();
            if (!connected_.load()) {
                return false;   // 断线后不再等待
            }
            if (timeout_ms <= 0) {
                return false;
            }
            // 缓冲里若仍有"可处理物"(完整帧/待清理的非法头)立即再走一轮, 不空等到超时
            {
                bool has_work = false;
                if (pending_.size() >= 4) {
                    const uint32_t declared = ReadBe32(pending_.data());
                    has_work = (declared == 0
                        || declared > QuicWireCodec::kMaxWireFrame
                        || pending_.size() >= static_cast<size_t>(4) + declared);
                }
                if (has_work) {
                    continue;
                }
            }
            if (std::chrono::steady_clock::now() >= deadline) {
                return false;   // 超时且无任何可处理物
            }
            data_cv_.wait_until(lock, deadline);
            // 唤醒(数据到达/断线/超时)后回到循环头重试
        }
    }

    void QuicTransport::recv_loop() {
        std::vector<uint8_t> chunk;
        while (running_.load()) {
            if (!ops_) {
                break;
            }
            chunk.clear();
            const int r = ops_->RecvStreamData(chunk, kRecvSliceMs);
            if (r == 1) {
                // 缓冲写入: 字节总量超上限判死(B3-1 同语义 —— 丢字节会破坏 4B 定界,
                // "活着但数据断流"比断线重连更危险)
                bool dead = false;
                {
                    std::lock_guard<std::mutex> lock(mtx_);
                    if (pending_.size() + chunk.size() > kPendingBytesCap) {
                        dead = true;
                    }
                    else {
                        pending_.insert(pending_.end(), chunk.begin(), chunk.end());
                    }
                }
                if (dead) {
                    CLEAT_LOGE(ErrDom::Net, ErrCode::ResourceExhausted,
                        "QuicTransport: 未凑帧缓冲超 " << kPendingBytesCap / 1024
                        << "KB, 判定链路不可用, 强制重连");
                    connected_.store(false);
                    break;
                }
                data_cv_.notify_all();
                continue;
            }
            if (r == 0) {
                continue;   // 拍内无数据, 非致命(TCP/Ws 超时同纪律)
            }
            // -1: 通道死亡(对端 close / 传输错误 / ops 层判死) —— 收摊退出,
            // 由上层(SessionLoop)驱动重连; 收尾发起时(running_=false)静默退出
            connected_.store(false);
            if (running_.load()) {
                CLEAT_LOGE(ErrDom::Net, ErrCode::PeerDisconnected,
                    "QuicTransport: 通道死亡(对端断开/传输错误), 断线待重连");
            }
            break;
        }
    }

    void QuicTransport::clear_buffers() {
        std::lock_guard<std::mutex> lock(mtx_);
        //  作废全部链路级状态 —— 未凑帧字节是"待交付数据", 断线必须清空,
        // 否则陈旧数据会拼进新会话的帧流( 实测缺陷的 QUIC 侧形态)
        pending_.clear();
    }

} // namespace cleat
