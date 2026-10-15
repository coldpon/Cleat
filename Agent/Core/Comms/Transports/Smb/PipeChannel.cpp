#include "Core/Comms/Transports/Smb/PipeChannel.hpp"
#include "Core/Comms/Transports/Smb/PipeFrame.hpp"
#include "Common/Logger/ErrLog.hpp"
#include "Common/Logger/Logger.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>

#include <sodium.h>
#include <sodium/utils.h>

namespace cleat {

    PipeChannel::PipeChannel(std::unique_ptr<IPipeOps> ops) : ops_(std::move(ops)) {}

    PipeChannel::~PipeChannel() { Close(); }

    ChannelCaps PipeChannel::Caps() const {
        ChannelCaps c;
        c.kind = ChannelKind::Stream;
        c.agency = ChannelAgency::Push;
        c.bandwidth = BandwidthClass::High;
        c.needsHandshake = false;      // 握手由会话层决定(PSK 模式无帧), 管道层只做 pre-auth
        c.maxFrameBytes = kMaxFrameBytes;
        return c;
    }

    bool PipeChannel::ParseUrl(const std::string& url,
        std::string& host, std::string& name, bool& is_server) {
        const std::string scheme = "pipe://";
        std::string rest;
        if (url.rfind(scheme, 0) == 0) {
            rest = url.substr(scheme.size());
        }
        else if (url.rfind("\\\\", 0) == 0) {
            rest = url;                    // 原生路径(\\.\pipe\name / \\host\pipe\name)直传
        }
        else {
            return false;
        }
        // 规范化: 允许直接给 \\.\pipe\name 或 \\host\pipe\name
        if (rest.rfind("\\\\", 0) == 0) {
            const std::string marker = "\\pipe\\";
            const size_t pos = rest.find(marker);
            if (pos == std::string::npos) return false;
            host = rest.substr(2, pos - 2);
            name = rest.substr(pos + marker.size());
            // \\.\pipe\ 的 "." = 本机 → server 形态(与 pipe://./name 的
            // "显式 host 段 = 客户端"语义区分)
            if (host == "." || host == "localhost") host.clear();
        }
        else {
            const size_t slash = rest.find('/');
            if (slash == std::string::npos) {
                host.clear();
                name = rest;
            }
            else {
                host = rest.substr(0, slash);
                name = rest.substr(slash + 1);
            }
        }
        if (name.empty()) return false;
        is_server = host.empty();
        return true;
    }

    bool PipeChannel::Open(const TransportConfig& cfg) {
        if (!ops_) {
            CLEAT_LOGE(ErrDom::Net, ErrCode::NotSupported, "PipeChannel: 平台无管道实现");
            return false;
        }
        if (opened_.load()) Close();      // 重复 Open: 先作废旧链路

        std::string host, name;
        bool is_server = false;
        if (!ParseUrl(cfg.url, host, name, is_server)) {
            CLEAT_LOGE(ErrDom::Net, ErrCode::InvalidParam, "PipeChannel: 非法 pipe url: " << cfg.url);
            return false;
        }

        pipe_key_ = cfg.pipe_key;         // pre-auth 口令快照
        pipe_name_ = name;

        if (is_server) {
            handle_ = ops_->CreateServer(name);
            if (!handle_) {
                CLEAT_LOGE(ErrDom::Net, ErrCode::PlatformApiFailed, "PipeChannel: CreateServer 失败 name=" << name);
                return false;
            }
            opened_.store(true);
            // 句柄按值传副本: 线程体不读 handle_ 成员(拷贝 happen-before 线程体,
            // Close 改写成员无竞争)
            listen_thread_ = std::make_unique<std::thread>(
                &PipeChannel::ListenLoop, this, handle_);
            LOG_INFO("PipeChannel: server listening " << name
                << (pipe_key_.empty() ? " (no auth)" : " (auth)"));
        }
        else {
            const int budget = cfg.timeout_ms > 0 ? cfg.timeout_ms : 5000;
            handle_ = ops_->ConnectClient(host, name, budget);
            if (!handle_) {
                CLEAT_LOGE(ErrDom::Net, ErrCode::PlatformApiFailed, "PipeChannel: ConnectClient 失败 " << host << "/" << name);
                return false;
            }
            opened_.store(true);
            if (!pipe_key_.empty()) {
                uint8_t key[32];
                PipeFrame::DeriveKey(pipe_key_, key);
                std::vector<uint8_t> leftover;
                if (!ClientHandshake(handle_, key, leftover)) {
                    CLEAT_LOGE(ErrDom::Net, ErrCode::CryptoOpFailed, "PipeChannel: 握手失败 " << host << "/" << name);
                    Close();
                    return false;
                }
                if (!leftover.empty()) {
                    // 无锁: 注入先于下方 connected_.store, Pump 后于 connected_
                    partial_.insert(partial_.begin(), leftover.begin(), leftover.end());
                }
            }
            connected_.store(true);
            LOG_INFO("PipeChannel: connected " << host << "/" << name);
        }
        return true;
    }

    // 握手消息统一走帧定界(与会话帧同构): 裸读字节会把帧头当 challenge。
    // 收一帧(轮询 budget 毫秒); 失败/超时返回 false。
    // leftover: 提取握手帧后缓冲里的剩余字节(对端背靠背抢先发的会话帧)。
    bool RecvHandshakeFrame(IPipeOps* ops, IPipeOps::PipeHandle handle,
        std::vector<uint8_t>& out, std::vector<uint8_t>& leftover, int budget_ms) {
        std::vector<uint8_t> buf;
        const auto deadline = std::chrono::steady_clock::now()
            + std::chrono::milliseconds(budget_ms);
        while (std::chrono::steady_clock::now() < deadline) {
            std::vector<uint8_t> frame;
            if (PipeFrame::TryExtractFrame(buf, frame) == 0) {
                out = std::move(frame);
                leftover = std::move(buf);
                return true;
            }
            int avail = ops->AvailableBytes(handle);
            if (avail < 0) return false;              // 对端拒绝/断开
            if (avail > 0) {
                std::vector<uint8_t> tmp(static_cast<size_t>(avail));
                const int got = ops->Read(handle, tmp.data(), tmp.size());
                if (got <= 0) return false;
                buf.insert(buf.end(), tmp.begin(), tmp.begin() + got);
                continue;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return false;
    }

    // 客户端 pre-auth: 收 16B challenge 帧 → 回 HMAC(key, challenge) 32B 帧
    bool PipeChannel::ClientHandshake(IPipeOps::PipeHandle handle, const uint8_t key[32],
        std::vector<uint8_t>& leftover) {
        std::vector<uint8_t> chal;
        if (!RecvHandshakeFrame(ops_.get(), handle, chal, leftover, 3000)
            || chal.size() != 16) {
            return false;
        }
        uint8_t mac[32];
        PipeFrame::AuthCode(key, chal.data(), 16, mac);
        std::vector<uint8_t> resp(mac, mac + 32);
        return PipeFrame::WriteAll(ops_.get(), handle, resp);
    }

    // 服务端 pre-auth: 发 16B challenge 帧 → 收 HMAC 帧 → 常量时间比较
    bool PipeChannel::ServerHandshake(IPipeOps::PipeHandle handle, const uint8_t key[32],
        std::vector<uint8_t>& leftover) {
        PipeFrame::EnsureSodium();
        uint8_t challenge[16];
        randombytes_buf(challenge, sizeof(challenge));
        std::vector<uint8_t> chal(challenge, challenge + 16);
        if (!PipeFrame::WriteAll(ops_.get(), handle, chal)) return false;

        std::vector<uint8_t> resp;
        if (!RecvHandshakeFrame(ops_.get(), handle, resp, leftover, 3000)
            || resp.size() != 32) {
            return false;
        }
        uint8_t expect[32];
        PipeFrame::AuthCode(key, challenge, 16, expect);
        return sodium_memcmp(expect, resp.data(), 32) == 0;   // 常量时间, 防时序侧信道
    }

    void PipeChannel::ListenLoop(IPipeOps::PipeHandle handle) {
        // 阻塞等待客户端; Close → CloseHandle → 本调用失败返回(线程退出)
        if (!ops_->WaitForClient(handle)) return;

        // pre-auth 通过后才对会话层可见(失败静默断开, 不置 connected_)
        if (!pipe_key_.empty()) {
            uint8_t key[32];
            PipeFrame::DeriveKey(pipe_key_, key);
            std::vector<uint8_t> leftover;
            if (!ServerHandshake(handle, key, leftover)) {
                LOG_WARN("PipeChannel: 入口认证失败 " << pipe_name_);
                ops_->Disconnect(handle);
                return;
            }
            if (!leftover.empty()) {
                // 对端背靠背抢先发的会话帧: 注入重组缓冲(丢帧实测修复)。
                // 无锁: 本注入先于下方 connected_.store, Pump 后于 connected_
                partial_.insert(partial_.begin(), leftover.begin(), leftover.end());
            }
            LOG_DEBUG("PipeChannel: client authenticated");
        }
        connected_.store(true);
    }

    void PipeChannel::Close() {
        if (!opened_.exchange(false) && !handle_) return;   // 未打开: 幂等
        // IChannel 契约 Write/Pump/Close 由会话层单线程调度, 热路径无跨线程
        // 竞争; 监听线程持有句柄副本, 不碰重组缓冲。
        // 关停顺序: CloseHandle 打断阻塞中的 ConnectNamedPipe(内核 I/O 持有
        // 对管道对象的引用, 跨线程关句柄是该阻塞调用的唯一可靠打断手法;
        // DisconnectNamedPipe 唤醒不了它) → join → 丢弃句柄值。
        // 已连接场景: CloseHandle 同时终结管道连接(数据完整性由上层帧协议
        // 的 "作废全部链路级状态"语义兜底, 不依赖优雅排空)。
        closed_.store(true);
        connected_.store(false);
        if (handle_) {
            ops_->Close(handle_);
        }
        if (listen_thread_ && listen_thread_->joinable()) {
            listen_thread_->join();
        }
        listen_thread_.reset();
        handle_ = nullptr;
        // join 已保证 listen 线程的残留注入完成, 此处清空无竞争
        partial_.clear();
        partial_.shrink_to_fit();
    }

    ChannelHealth PipeChannel::HealthNow() const {
        if (closed_.load()) return ChannelHealth::Dead;
        if (connected_.load()) return ChannelHealth::Healthy;
        if (opened_.load()) return ChannelHealth::Degraded;   // 服务端待连入/握手中
        return ChannelHealth::Dead;
    }

    SendResult PipeChannel::Write(const std::vector<uint8_t>& frame) {
        if (closed_.load() || !connected_.load() || !handle_) {
            return SendResult::Rejected;      // 未连接可重试(连入后即恢复)
        }
        if (frame.empty()) return SendResult::Sent;
        if (frame.size() > kMaxFrameBytes) {
            LOG_WARN("PipeChannel: 帧超上限 " << frame.size() << "B > " << kMaxFrameBytes << "B");
            return SendResult::Dropped;
        }

        if (!PipeFrame::WriteAll(ops_.get(), handle_, frame)) {
            connected_.store(false);
            return SendResult::ChannelFailure;
        }
        return SendResult::Sent;
    }

    bool PipeChannel::Pump(std::vector<uint8_t>& out) {
        if (closed_.load() || !connected_.load() || !handle_) return false;

        // 先尝试从重组缓冲提取一帧: 上次 Pump 可能一次读入了多帧(粘包),
        // 管道缓冲此时已空 —— 若先查缓冲会永久滞留已收到的帧。
        // 残留注入先于 connected_(release), 本读后于 connected_(acquire):
        // 注入内容对本函数可见
        if (TryPopFrame(out)) return true;

        while (true) {
            int avail = ops_->AvailableBytes(handle_);
            if (avail < 0) {
                // 对端断开: 作废链路状态, 部分帧随 partial_ 清除
                connected_.store(false);
                LOG_DEBUG("PipeChannel: peer disconnected, 丢弃部分帧 "
                    << partial_.size() << "B");
                partial_.clear();
                return false;
            }
            if (avail == 0) return false;   // 暂无数据(非错误)

            std::vector<uint8_t> buf(static_cast<size_t>(avail));
            const int got = ops_->Read(handle_, buf.data(), buf.size());
            if (got <= 0) {
                if (got < 0) {
                    connected_.store(false);
                    partial_.clear();
                }
                return false;
            }
            partial_.insert(partial_.end(), buf.begin(), buf.begin() + got);
            if (TryPopFrame(out)) return true;
        }
    }

    // 前置: 仅 Pump 路径调用(会话线程单线程语义)
    bool PipeChannel::TryPopFrame(std::vector<uint8_t>& out) {
        const int r = PipeFrame::TryExtractFrame(partial_, out, kMaxFrameBytes);
        if (r == 0) return true;
        if (r < 0) {
            CLEAT_LOGE(ErrDom::Net, ErrCode::ResourceExhausted, "PipeChannel: 帧长超限, 作废链路");
            connected_.store(false);
            partial_.clear();
        }
        return false;
    }

} // namespace cleat
