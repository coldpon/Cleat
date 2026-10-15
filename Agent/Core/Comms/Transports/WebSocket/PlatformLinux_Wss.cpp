// PlatformLinux_Wss.cpp
// Linux WebSocket 传输: vendored mbedTLS(v3.6.3) TLS + 自管 RFC6455 握手/帧(WsWireCodec)。
// - 静态链接, 目标机零运行时依赖(与 libsodium/mbedTLS 同模式)
// - 网络 IO 经 INetworkOps(同 HttpOpsLinux 纪律, 不绕统一网络层); bio 回调把
//   超时/瞬态映射为 WANT_READ/WANT_WRITE, 长连接空闲不算致命
// - 证书口径与 HttpOpsLinux 一致: 默认严格(系统 CA), http.ignore_cert 跳过
// - v0 直连模式
#include "Core/Comms/Transports/WebSocket/IWssOps.hpp"

#include "Common/Logger/ErrLog.hpp"
#include "Common/Logger/Logger.hpp"
#include "Core/Config/AgentConfig.hpp"
#include "Core/Comms/Platform/PlatformLinux_Network.hpp"
#include "Core/Comms/Transports/WebSocket/WsWireCodec.hpp"

#include "mbedtls/ssl.h"
#include "mbedtls/entropy.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/x509_crt.h"
#include "mbedtls/error.h"
#include "psa/crypto.h"

#include <chrono>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace cleat {

    namespace {

        constexpr size_t kMaxHeadBytes = 16 * 1024;      // 握手响应头上限(防恶意)
        constexpr int kReadSliceMs = 100;                // socket 收拍切片(空闲→WANT_READ)
        constexpr uint16_t kDefaultWssPort = 443;

        // 常见 Linux 发行版系统 CA 路径(严格校验模式加载; 与 HttpOpsLinux 同表)
        const char* kSystemCaPaths[] = {
            "/etc/ssl/certs/ca-certificates.crt",
            "/etc/pki/tls/certs/ca-bundle.crt",
            "/etc/ssl/ca-bundle.pem",
            "/etc/pki/tls/cacert.pem",
        };

        int64_t NowMs() {
            return std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count();
        }

        // URL 解析: wss://host[:port][/path](query 原样带在 path 里)
        bool ParseWssUrl(const std::string& url, std::string& host, uint16_t& port,
            std::string& path) {
            const std::string scheme = "wss://";
            if (url.compare(0, scheme.size(), scheme) != 0) {
                LOG_WARN("WssOpsLinux: 非 wss URL: " << url);
                return false;
            }
            const std::string rest = url.substr(scheme.size());
            const size_t slash = rest.find('/');
            const std::string authority = (slash == std::string::npos) ? rest : rest.substr(0, slash);
            path = (slash == std::string::npos) ? std::string("/") : rest.substr(slash);
            size_t colon = authority.find(':');
            if (authority.find(']') != std::string::npos) {
                colon = std::string::npos;   // IPv6 字面量: v0 不支持, 走域名
            }
            if (colon == std::string::npos) {
                host = authority;
                port = kDefaultWssPort;
                return !host.empty();
            }
            host = authority.substr(0, colon);
            if (host.empty()) return false;
            const std::string port_str = authority.substr(colon + 1);
            if (port_str.empty()) {
                port = kDefaultWssPort;
                return true;
            }
            uint16_t p = 0;
            for (const char c : port_str) {
                if (c < '0' || c > '9') return false;
                p = static_cast<uint16_t>(p * 10 + (c - '0'));
            }
            port = p == 0 ? kDefaultWssPort : p;
            return true;
        }

        std::string SslErrText(int rc) {
            char buf[128]{};
            mbedtls_strerror(rc, buf, sizeof(buf));
            return buf;
        }

    } // namespace

    class WssOpsLinux : public IWssOps {
    public:
        WssOpsLinux() {
            mbedtls_entropy_init(&entropy_);
            mbedtls_ctr_drbg_init(&drbg_);
            mbedtls_ssl_init(&ssl_);
            mbedtls_ssl_config_init(&ssl_cfg_);
            mbedtls_x509_crt_init(&cacert_);
        }

        ~WssOpsLinux() override {
            Close();
            mbedtls_x509_crt_free(&cacert_);
            mbedtls_ssl_config_free(&ssl_cfg_);
            mbedtls_ssl_free(&ssl_);
            mbedtls_ctr_drbg_free(&drbg_);
            mbedtls_entropy_free(&entropy_);
        }

        bool Handshake(const TransportConfig& cfg) override {
            if (!cfg.url.empty() && cfg.url.compare(0, 5, "ws://") == 0) {
                CLEAT_LOGE(ErrDom::Net, ErrCode::InvalidParam,
                    "WssOpsLinux: 明文 ws:// 已拒绝(仅 wss)");
                return false;
            }
            std::string host, path;
            uint16_t port = 0;
            if (!ParseWssUrl(cfg.url, host, port, path)) {
                CLEAT_LOGE(ErrDom::Net, ErrCode::InvalidParam, "WssOpsLinux: URL 解析失败 " << cfg.url);
                return false;
            }
            const int64_t deadline = NowMs() + static_cast<int64_t>(cfg.timeout_ms);

            if (!EnsureTls()) return false;

            // ---- TCP 建连(INetworkOps 统一口; v0 直连, 无代理 CONNECT) ----
            sock_ = ops_.CreateTcpSocket();
            if (sock_ < 0) {
                CLEAT_LOGE(ErrDom::Net, ErrCode::PlatformApiFailed, "WssOpsLinux: 创建 socket 失败");
                return false;
            }
            // 收拍切片: 空闲 → WANT_READ(长连接纪律), deadline 层面另有总时限
            ops_.SetSocketRecvTimeout(sock_, kReadSliceMs);
            if (!ops_.ConnectSocket(sock_, host, port)) {
                CLEAT_LOGE(ErrDom::Net, ErrCode::PlatformApiFailed,
                    "WssOpsLinux: 连接失败 " << host << ":" << port);
                CleanupSocket();
                return false;
            }

            // ---- TLS 握手 ----
            if (mbedtls_ssl_set_hostname(&ssl_, host.c_str()) != 0) {
                CLEAT_LOGE(ErrDom::Net, ErrCode::PlatformApiFailed, "WssOpsLinux: set_hostname 失败");
                Teardown();
                return false;
            }
            mbedtls_ssl_set_bio(&ssl_, this, SslSendCb, SslRecvCb, nullptr);
            int rc = 0;
            while ((rc = mbedtls_ssl_handshake(&ssl_)) != 0) {
                if (rc == MBEDTLS_ERR_SSL_WANT_READ || rc == MBEDTLS_ERR_SSL_WANT_WRITE) {
                    if (NowMs() >= deadline) {
                        LOG_WARN("WssOpsLinux: TLS 握手超时 " << host << ":" << port);
                        Teardown();
                        return false;
                    }
                    continue;
                }
                LOG_WARN("WssOpsLinux: TLS 握手失败: " << SslErrText(rc));
                Teardown();
                return false;
            }

            // ---- RFC6455 升级握手(自管; key/Accept 校验在 WsWireCodec) ----
            client_key_ = WsWireCodec::MakeClientKey();
            const std::string req = WsWireCodec::BuildHandshakeRequest(host, port, path, client_key_);
            if (!SslWriteAll(reinterpret_cast<const uint8_t*>(req.data()), req.size(), deadline)) {
                LOG_WARN("WssOpsLinux: 升级请求发送失败");
                Teardown();
                return false;
            }
            std::string head;
            while (head.find("\r\n\r\n") == std::string::npos) {
                if (head.size() > kMaxHeadBytes) {
                    LOG_WARN("WssOpsLinux: 握手响应头超限");
                    Teardown();
                    return false;
                }
                if (NowMs() >= deadline) {
                    LOG_WARN("WssOpsLinux: 握手响应超时");
                    Teardown();
                    return false;
                }
                uint8_t chunk[1024];
                const int n = SslReadOnce(chunk, sizeof(chunk), deadline);
                if (n <= 0) {
                    LOG_WARN("WssOpsLinux: 握手响应读取失败");
                    Teardown();
                    return false;
                }
                head.append(reinterpret_cast<char*>(chunk), static_cast<size_t>(n));
            }
            const size_t head_end = head.find("\r\n\r\n") + 4;
            if (!WsWireCodec::ValidateUpgradeResponse(head.substr(0, head_end), client_key_)) {
                LOG_WARN("WssOpsLinux: 升级响应校验失败(Accept/状态码)");
                Teardown();
                return false;
            }
            // 响应头之后可能已捎带 WS 帧: 留入解析缓冲
            rbuf_.assign(head.begin() + static_cast<std::ptrdiff_t>(head_end), head.end());
            alive_ = true;
            LOG_DEBUG("WssOpsLinux: WebSocket 建立 " << host << ":" << port << path);
            return true;
        }

        bool SendMessageData(const uint8_t* data, size_t len) override {
            if (!alive_) return false;
            std::vector<uint8_t> wire;
            if (!WsWireCodec::EncodeClientFrame(WsWireCodec::kOpBinary, data, len, wire)) {
                CLEAT_LOGE(ErrDom::Net, ErrCode::ResourceExhausted,
                    "WssOpsLinux: 帧编码失败 len=" << len);
                return false;
            }
            if (!SslWriteAll(wire.data(), wire.size(), NowMs() + kIoDeadlineMs)) {
                CLEAT_LOGE(ErrDom::Net, ErrCode::PlatformApiFailed, "WssOpsLinux: 帧发送失败 len=" << len);
                alive_ = false;
                return false;
            }
            return true;
        }

        int RecvMessage(std::vector<uint8_t>& out, int timeout_ms) override {
            if (!alive_) return -1;
            const int64_t deadline = (timeout_ms <= 0)
                ? 0 : NowMs() + static_cast<int64_t>(timeout_ms);
            WsFrame frame;
            while (true) {
                const int pr = WsWireCodec::TryParseFrame(rbuf_, frame);
                if (pr == 1) {
                    const int disp = DispatchFrame(frame, out);
                    if (disp != 0) return disp;
                    continue;   // 控制帧已应答/忽略, 继续解析
                }
                if (pr == -1) {
                    CLEAT_LOGE(ErrDom::Net, ErrCode::IntegrityMismatch, "WssOpsLinux: WS 帧协议错误");
                    alive_ = false;
                    return -1;
                }
                // 数据不足: 再读一段(拍内超时语义 = 返回 0, 缓冲原状)
                uint8_t chunk[4096];
                const int n = SslReadOnce(chunk, sizeof(chunk), deadline);
                if (n > 0) {
                    rbuf_.insert(rbuf_.end(), chunk, chunk + n);
                    if (rbuf_.size() > WsWireCodec::kMaxFramePayload + 16) {
                        CLEAT_LOGE(ErrDom::Net, ErrCode::ResourceExhausted,
                            "WssOpsLinux: 解析缓冲超限 len=" << rbuf_.size());
                        alive_ = false;
                        return -1;
                    }
                    continue;
                }
                if (n == kReadIdle) {
                    return 0;   // timeout_ms 内无数据(非致命)
                }
                alive_ = false;
                return -1;      // EOF / 致命错误
            }
        }

        void Close() override {
            if (alive_ && sock_ >= 0) {
                // 尽力发 close 帧(1000 正常关闭), 不等对端回执
                std::vector<uint8_t> wire;
                if (WsWireCodec::EncodeClientFrame(WsWireCodec::kOpClose, nullptr, 0, wire)) {
                    (void)SslWriteAll(wire.data(), wire.size(), NowMs() + 500);
                }
            }
            alive_ = false;
            if (sock_ >= 0) {
                mbedtls_ssl_close_notify(&ssl_);
                ops_.AbortSocket(sock_);
                ops_.CloseSocket(sock_);
                sock_ = -1;
            }
            mbedtls_ssl_free(&ssl_);
            mbedtls_ssl_init(&ssl_);   // free 后需 re-init 才能下次 setup(HttpOpsLinux 同款)
            rbuf_.clear();
            recv_acc_.clear();
        }

        bool IsAlive() const override {
            return alive_;
        }

    private:
        static constexpr int kReadIdle = -2;   // SslReadOnce 内部语义: 拍内无数据
        static constexpr int64_t kIoDeadlineMs = 30000;

        // ---- TLS 上下文(HttpOpsLinux 同款初始化/证书口径) ----
        bool EnsureTls() {
            if (tls_ok_) return true;
            if (mbedtls_ctr_drbg_seed(&drbg_, mbedtls_entropy_func, &entropy_,
                nullptr, 0) != 0) {
                CLEAT_LOGE(ErrDom::Net, ErrCode::PlatformApiFailed, "WssOpsLinux: ctr_drbg seed 失败");
                return false;
            }
            if (mbedtls_ssl_config_defaults(&ssl_cfg_, MBEDTLS_SSL_IS_CLIENT,
                MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT) != 0) {
                CLEAT_LOGE(ErrDom::Net, ErrCode::PlatformApiFailed, "WssOpsLinux: ssl_config_defaults 失败");
                return false;
            }
            mbedtls_ssl_conf_rng(&ssl_cfg_, mbedtls_ctr_drbg_random, &drbg_);
            // 证书: 默认严格(系统 CA); http.ignore_cert=true 跳过(自签场景, 对齐 CS/Win)
            if (AgentConfig::Instance().GetHttpsIgnoreCert()) {
                mbedtls_ssl_conf_authmode(&ssl_cfg_, MBEDTLS_SSL_VERIFY_NONE);
            }
            else {
                bool ca_loaded = false;
                for (const char* ca : kSystemCaPaths) {
                    if (mbedtls_x509_crt_parse_file(&cacert_, ca) == 0) {
                        ca_loaded = true;
                        break;
                    }
                }
                if (ca_loaded) {
                    mbedtls_ssl_conf_authmode(&ssl_cfg_, MBEDTLS_SSL_VERIFY_REQUIRED);
                    mbedtls_ssl_conf_ca_chain(&ssl_cfg_, &cacert_, nullptr);
                }
                else {
                    // PlatformLinux_Http.cpp 同款静默降级缺陷(见该处说明)。
                    // 统一改为 fail-closed: 拒绝建连, 自签场景须显式 ignore_cert。
                    CLEAT_LOGE(ErrDom::Net, ErrCode::Unauthenticated,
                        "WssOpsLinux: 系统 CA 不可用且未显式允许跳过校验, 拒绝建连"
                        "(如需自签场景, 请显式设置 https_ignore_cert)");
                    return false;
                }
            }
            // TLS 1.3 走 PSA 驱动; 失败不致命(降级 TLS 1.2, HttpOpsLinux 同款)
            const int psa_rc = psa_crypto_init();
            if (psa_rc != 0) {
                LOG_WARN("WssOpsLinux: PSA crypto init 失败(" << psa_rc << "), 仅可用 TLS1.2");
            }
            if (mbedtls_ssl_setup(&ssl_, &ssl_cfg_) != 0) {
                CLEAT_LOGE(ErrDom::Net, ErrCode::PlatformApiFailed, "WssOpsLinux: ssl_setup 失败");
                return false;
            }
            tls_ok_ = true;
            return true;
        }

        // ---- bio 回调: 超时/瞬态 → WANT_*(长连接空闲不算致命, HttpOpsLinux 同款) ----
        static int SslSendCb(void* ctx, const unsigned char* buf, size_t len) {
            auto* self = static_cast<WssOpsLinux*>(ctx);
            if (self->sock_ < 0) return MBEDTLS_ERR_NET_SEND_FAILED;
            const int n = self->ops_.SendSocket(self->sock_, buf, len);
            if (n < 0) {
                const int err = self->ops_.GetLastSocketError();
                if (self->ops_.IsTransientRecvError(err) ||
                    err == self->ops_.GetSocketErrorTimeout()) {
                    return MBEDTLS_ERR_SSL_WANT_WRITE;
                }
                return MBEDTLS_ERR_NET_SEND_FAILED;
            }
            return n;
        }

        static int SslRecvCb(void* ctx, unsigned char* buf, size_t len) {
            auto* self = static_cast<WssOpsLinux*>(ctx);
            if (self->sock_ < 0) return MBEDTLS_ERR_NET_RECV_FAILED;
            // mbedtls 回调契约 = "至多 len 字节, 有多少交多少"(RecvSome 单次语义)
            const int n = self->ops_.RecvSome(self->sock_, buf, len);
            if (n <= 0) {
                const int err = self->ops_.GetLastSocketError();
                if (self->ops_.IsTransientRecvError(err) ||
                    err == self->ops_.GetSocketErrorTimeout()) {
                    return MBEDTLS_ERR_SSL_WANT_READ;
                }
                return MBEDTLS_ERR_NET_RECV_FAILED;
            }
            return n;
        }

        /// 整段写完(WANT_WRITE 可重试; 超时/失败返回 false)
        bool SslWriteAll(const uint8_t* data, size_t len, int64_t deadline) {
            size_t sent = 0;
            while (sent < len) {
                const int rc = mbedtls_ssl_write(&ssl_, data + sent, len - sent);
                if (rc > 0) {
                    sent += static_cast<size_t>(rc);
                    continue;
                }
                if (rc == MBEDTLS_ERR_SSL_WANT_WRITE || rc == MBEDTLS_ERR_SSL_WANT_READ) {
                    if (NowMs() >= deadline) return false;
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                    continue;
                }
                return false;
            }
            return true;
        }

        /// 至多一次 TLS 读; >0 字节 / kReadIdle(拍内无数据) / <=0(EOF/致命)
        int SslReadOnce(uint8_t* buf, size_t cap, int64_t deadline) {
            const int rc = mbedtls_ssl_read(&ssl_, buf, cap);
            if (rc > 0) return rc;
            if (rc == MBEDTLS_ERR_SSL_WANT_READ || rc == MBEDTLS_ERR_SSL_WANT_WRITE) {
                if (deadline != 0 && NowMs() >= deadline) return kReadIdle;
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                return kReadIdle;
            }
            if (rc == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY || rc == 0) {
                LOG_WARN("WssOpsLinux: 对端 TLS 关闭");
                return MBEDTLS_ERR_NET_RECV_FAILED;
            }
            LOG_WARN("WssOpsLinux: TLS 读失败: " << SslErrText(rc));
            return MBEDTLS_ERR_NET_RECV_FAILED;
        }

        /// 单帧分发: 控制帧内部应答/忽略(返回 0 继续), 数据帧重组交付(返回 1), 死亡返回 -1
        int DispatchFrame(const WsFrame& frame, std::vector<uint8_t>& out) {
            using WC = WsWireCodec;
            switch (frame.opcode) {
            case WC::kOpBinary: {
                if (!recv_acc_.empty()) {
                    // 新数据帧出现在未完成分片中间: 协议错
                    CLEAT_LOGE(ErrDom::Net, ErrCode::IntegrityMismatch, "WssOpsLinux: 分片中途出现新数据帧");
                    alive_ = false;
                    return -1;
                }
                out = std::move(frame.payload);
                return 1;
            }
            case WC::kOpContinuation: {
                if (recv_acc_.empty()) {
                    CLEAT_LOGE(ErrDom::Net, ErrCode::IntegrityMismatch, "WssOpsLinux: 无起始帧的 continuation");
                    alive_ = false;
                    return -1;
                }
                recv_acc_.insert(recv_acc_.end(), frame.payload.begin(), frame.payload.end());
                if (recv_acc_.size() > WC::kMaxFramePayload) {
                    CLEAT_LOGE(ErrDom::Net, ErrCode::ResourceExhausted, "WssOpsLinux: 分片累积超限");
                    alive_ = false;
                    return -1;
                }
                if (frame.fin) {
                    out = std::move(recv_acc_);
                    recv_acc_.clear();
                    recv_acc_.shrink_to_fit();
                    return 1;
                }
                return 0;
            }
            case WC::kOpText:
                // 会话协议只用 binary —— 文本帧 = 协议错(与 Win 侧同口径)
                CLEAT_LOGE(ErrDom::Net, ErrCode::IntegrityMismatch, "WssOpsLinux: 收到意外文本帧");
                alive_ = false;
                return -1;
            case WC::kOpPing: {
                // RFC6455: 必须以 pong 应答(载荷原样); 尽力而为不判死
                std::vector<uint8_t> wire;
                if (WsWireCodec::EncodeClientFrame(WC::kOpPong, frame.payload.data(),
                    frame.payload.size(), wire)) {
                    (void)SslWriteAll(wire.data(), wire.size(), NowMs() + 1000);
                }
                return 0;
            }
            case WC::kOpPong:
                return 0;   // 本端不主动 ping, 对端 pong 忽略
            case WC::kOpClose:
                alive_ = false;   // 对端 close: 通道死亡(上层驱动重连)
                return -1;
            default:
                CLEAT_LOGE(ErrDom::Net, ErrCode::IntegrityMismatch,
                    "WssOpsLinux: 未知帧类型 op=" << static_cast<int>(frame.opcode));
                alive_ = false;
                return -1;
            }
        }

        void CleanupSocket() {
            if (sock_ >= 0) {
                ops_.AbortSocket(sock_);
                ops_.CloseSocket(sock_);
                sock_ = -1;
            }
        }

        void Teardown() {
            CleanupSocket();
            mbedtls_ssl_free(&ssl_);
            mbedtls_ssl_init(&ssl_);
            rbuf_.clear();
            recv_acc_.clear();
        }

        NetworkOpsLinux ops_;   // 自持平台原语实例(HttpOpsLinux 同款)
        INetworkOps::SocketHandle sock_ = -1;
        bool alive_ = false;
        bool tls_ok_ = false;

        mbedtls_entropy_context entropy_{};
        mbedtls_ctr_drbg_context drbg_{};
        mbedtls_ssl_context ssl_{};
        mbedtls_ssl_config ssl_cfg_{};
        mbedtls_x509_crt cacert_{};

        std::string client_key_;
        std::vector<uint8_t> rbuf_;      // 入站字节流缓冲(ssl_read → TryParseFrame)
        std::vector<uint8_t> recv_acc_;  // 二进制分片累积(消息重组)
    };

    std::unique_ptr<IWssOps> CreateWssOps(INetworkOps& net_ops) {
        (void)net_ops;   // 自持 NetworkOpsLinux(与 HttpOpsLinux 同款), 见 IWssOps 工厂注
        return std::make_unique<WssOpsLinux>();
    }

} // namespace cleat
