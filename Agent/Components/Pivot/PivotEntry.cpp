#include "Components/Pivot/PivotEntry.hpp"
#include "Common/Logger/ErrLog.hpp"
#include "Common/Logger/Logger.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace cleat {

    PivotEntry& PivotEntryInstance() {
        static PivotEntry instance;
        return instance;
    }

    PivotEntry::PivotEntry() : ops_(CreateNetworkOps()) {}

    PivotEntry::~PivotEntry() { Stop(); }

    bool PivotEntry::Start(uint16_t port, const std::string& bind_host,
        const std::string& username, const std::string& password) {
        if (running_.load()) {
            if (port_ == port && bind_host_ == bind_host
                && username_ == username && password_ == password) {
                return true;
            }
            Stop();
        }
        INetworkOps::SocketHandle s = ops_->CreateTcpSocket();
        if (s < 0) return false;
        ops_->SetSocketTimeout(s, 60000);
        if (!ops_->BindListenOn(s, port, bind_host)) {
            CLEAT_LOGE(ErrDom::Piv, ErrCode::PlatformApiFailed,
                "PivotEntry: bind/listen failed on " << bind_host << ":" << port);
            ops_->CloseSocket(s);
            return false;
        }
        listen_sock_ = s;
        port_ = port;
        bind_host_ = bind_host;
        username_ = username;
        password_ = password;
        running_.store(true);
        accept_thread_ = std::make_unique<std::thread>(&PivotEntry::AcceptLoop, this);
        LOG_INFO("PivotEntry: listening on " << (bind_host.empty() ? "0.0.0.0" : bind_host)
            << ":" << port << (username.empty() ? " (no auth)" : " (auth)"));
        return true;
    }

    void PivotEntry::Stop() {
        running_.store(false);
        {
            std::lock_guard<std::mutex> lock(conn_mu_);
            for (auto& w : conn_workers_) {
                w.state->stop.store(true);
            }
        }
        if (listen_sock_ >= 0) {
            ops_->CloseSocket(listen_sock_);
            listen_sock_ = -1;
        }
        if (accept_thread_ && accept_thread_->joinable()) {
            accept_thread_->join();
        }
        accept_thread_.reset();
        // 在途桥接线程不强关 socket(单一关闭者归工作线程), 依赖 stop 标志
        // + 连接 recv 超时(30s)自行退出; 已完成线程在此收割。
        ReapFinished();
        port_ = 0;
    }

    void PivotEntry::ReapFinished() {
        std::vector<ConnWorker> finished;
        {
            std::lock_guard<std::mutex> lock(conn_mu_);
            for (auto it = conn_workers_.begin(); it != conn_workers_.end();) {
                if (it->state->done.load()) {
                    finished.emplace_back(std::move(*it));
                    it = conn_workers_.erase(it);
                }
                else {
                    ++it;
                }
            }
        }
        for (auto& w : finished) {
            if (w.thread && w.thread->joinable()) {
                w.thread->join();
            }
        }
    }

    void PivotEntry::AcceptLoop() {
        while (running_.load()) {
            ReapFinished();
            INetworkOps::SocketHandle c = ops_->AcceptSocket(listen_sock_);
            if (c < 0) {
                if (!running_.load()) break;
                continue;
            }
            ops_->SetSocketTimeout(c, 30000);
            auto state = std::make_shared<ConnState>();
            // 工作线程使用配置快照(按值)与独立 ops 实例, 不依赖本对象存活;
            // Start 换端口重启时 Stop 已 join accept 线程, 成员读取无竞争。
            auto worker_ops = CreateNetworkOps();
            auto th = std::make_unique<std::thread>(&PivotEntry::HandleConnection,
                c, state, std::move(worker_ops), username_, password_);
            std::lock_guard<std::mutex> lock(conn_mu_);
            conn_workers_.push_back(ConnWorker{ state, std::move(th) });
        }
    }

    // ---- 连接工作线程使用的文件级工具(不触碰 PivotEntry 成员) ----

    // 读满 len 字节; 对端关闭/出错/纯超时返回 false
    static bool ReadFull(INetworkOps* ops, INetworkOps::SocketHandle sock,
        uint8_t* buf, size_t len) {
        size_t got = 0;
        while (got < len) {
            // RecvSome(单次语义): 有数据即回 —— RecvSocket 的"收满才回"会让短握手
            // 等到 SO_RCVTIMEO 到期才被交付; 收满由本循环承担。
            int n = ops->RecvSome(sock, buf + got, len - got);
            if (n > 0) { got += static_cast<size_t>(n); continue; }
            if (n == 0) return false;
            if (ops->GetLastSocketError() == ops->GetSocketErrorTimeout()) {
                if (got > 0) continue;
                return false;
            }
            return false;
        }
        return true;
    }

    static void Pump(INetworkOps* ops, INetworkOps::SocketHandle from,
        INetworkOps::SocketHandle to, std::atomic<bool>& done,
        const std::atomic<bool>& stop) {
        std::vector<uint8_t> buf(64 * 1024);
        bool halfClosed = false;
        while (!done.load() && !stop.load()) {
            // RecvSome: 单次交付即转发 —— 旧 RecvSocket 会为凑满 64KB 缓冲停在中继
            // 路径上(本 socket 超时 30s), 交互式流量(如 RDP/SSH 按键回显)被钉死
            // 最长一个超时周期。
            int n = ops->RecvSome(from, buf.data(), buf.size());
            if (n > 0) {
                if (ops->SendSocket(to, buf.data(), n) <= 0) break;
                continue;
            }
            if (n == 0) {
                // 对端写关闭: 半关闭本方向, 继续读直到双向结束
                if (!halfClosed) {
                    ops->ShutdownWrite(to);
                    halfClosed = true;
                    continue;
                }
                break;
            }
            if (ops->GetLastSocketError() == ops->GetSocketErrorTimeout()) continue;
            break;
        }
        if (!done.exchange(true)) {
            ops->CloseSocket(from);
            ops->CloseSocket(to);
        }
    }

    // SOCKS5 方法协商 + RFC1929 鉴权(user 非空时要求 0x02, 否则仅 0x00)。
    // 成功返回 true, 连接保持打开; 失败已回拒并可直接关闭。
    static bool NegotiateMethod(INetworkOps* ops, INetworkOps::SocketHandle sock,
        const std::string& user, const std::string& pass) {
        uint8_t hdr[2];
        if (!ReadFull(ops, sock, hdr, 2) || hdr[0] != 0x05 || hdr[1] == 0) {
            return false;
        }
        std::vector<uint8_t> methods(hdr[1]);
        if (!ReadFull(ops, sock, methods.data(), methods.size())) {
            return false;
        }
        auto offered = [&methods](uint8_t m) {
            return std::find(methods.begin(), methods.end(), m) != methods.end();
            };

        if (!user.empty()) {
            if (!offered(0x02)) {
                const uint8_t noaccept[2] = { 0x05, static_cast<uint8_t>(0xFF) };
                ops->SendSocket(sock, noaccept, 2);
                return false;
            }
            const uint8_t pick[2] = { 0x05, 0x02 };
            if (ops->SendSocket(sock, pick, 2) != 2) return false;

            // RFC1929 子协商: ver(0x01) ulen uname plen passwd
            uint8_t ver = 0, ulen = 0, plen = 0;
            if (!ReadFull(ops, sock, &ver, 1) || ver != 0x01) return false;
            if (!ReadFull(ops, sock, &ulen, 1) || ulen == 0) return false;
            std::vector<uint8_t> u(ulen);
            if (!ReadFull(ops, sock, u.data(), ulen)) return false;
            if (!ReadFull(ops, sock, &plen, 1)) return false;
            std::vector<uint8_t> p(plen > 0 ? plen : 1);
            if (plen > 0 && !ReadFull(ops, sock, p.data(), plen)) return false;

            bool ok = ulen == user.size()
                && plen == pass.size()
                && std::memcmp(u.data(), user.data(), ulen) == 0
                && (plen == 0 || std::memcmp(p.data(), pass.data(), plen) == 0);
            const uint8_t auth[2] = { static_cast<uint8_t>(0x01), ok ? static_cast<uint8_t>(0x00) : static_cast<uint8_t>(0x01) };
            ops->SendSocket(sock, auth, 2);
            if (!ok) {
                LOG_WARN("PivotEntry: auth failed");
            }
            return ok;
        }

        if (!offered(0x00)) {
            const uint8_t noaccept[2] = { 0x05, static_cast<uint8_t>(0xFF) };
            ops->SendSocket(sock, noaccept, 2);
            return false;
        }
        const uint8_t ack[2] = { 0x05, 0x00 };
        return ops->SendSocket(sock, ack, 2) == 2;
    }

    // 每连接工作线程主体: static + 全部按值快照, 不依赖 PivotEntry 存活。
    void PivotEntry::HandleConnection(INetworkOps::SocketHandle client,
        std::shared_ptr<ConnState> state, std::unique_ptr<INetworkOps> ops,
        std::string username, std::string password) {
        // ---- 1. 版本/方法协商(含鉴权) ----
        if (!NegotiateMethod(ops.get(), client, username, password)) {
            LOG_DEBUG("PivotEntry: handshake failed/rejected");
            ops->CloseSocket(client);
            state->done.store(true);
            return;
        }
        LOG_DEBUG("PivotEntry: handshake ok");

        // ---- 2. CONNECT 请求 ----
        uint8_t req[4];
        if (!ReadFull(ops.get(), client, req, 4) || req[1] != 0x01) {
            ops->CloseSocket(client);
            state->done.store(true);
            return;
        }
        std::string host;
        switch (req[3]) {
        case 0x01: { // IPv4
            uint8_t ip[4];
            if (!ReadFull(ops.get(), client, ip, 4)) { ops->CloseSocket(client); state->done.store(true); return; }
            char buf[32];
            std::snprintf(buf, sizeof(buf), "%u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
            host = buf;
            break;
        }
        case 0x03: { // domain
            uint8_t len = 0;
            if (!ReadFull(ops.get(), client, &len, 1) || len == 0) { ops->CloseSocket(client); state->done.store(true); return; }
            std::vector<uint8_t> d(len);
            if (!ReadFull(ops.get(), client, d.data(), len)) { ops->CloseSocket(client); state->done.store(true); return; }
            host.assign(d.begin(), d.end());
            break;
        }
        case 0x04: // IPv6 暂不支持
        default:
            ops->CloseSocket(client);
            state->done.store(true);
            return;
        }
        uint8_t pbuf[2];
        if (!ReadFull(ops.get(), client, pbuf, 2)) { ops->CloseSocket(client); state->done.store(true); return; }
        uint16_t port = (static_cast<uint16_t>(pbuf[0]) << 8) | pbuf[1];

        LOG_DEBUG("PivotEntry: CONNECT to " << host << ":" << port);
        INetworkOps::SocketHandle target = ops->CreateTcpSocket();
        if (target < 0) {
            ops->CloseSocket(client);
            state->done.store(true);
            return;
        }
        ops->SetSocketTimeout(target, 30000);
        if (!ops->ConnectSocket(target, host, port)) {
            CLEAT_LOGE(ErrDom::Piv, ErrCode::PlatformApiFailed,
                "PivotEntry: connect target failed " << host << ":" << port
                << " err=" << ops->GetLastSocketError());
            const uint8_t fail[10] = { 0x05, 0x04, 0x00, 0x01, 0, 0, 0, 0, 0, 0 };
            ops->SendSocket(client, fail, 10);
            ops->CloseSocket(client);
            ops->CloseSocket(target);
            state->done.store(true);
            return;
        }
        const uint8_t ok[10] = { 0x05, 0x00, 0x00, 0x01, 0, 0, 0, 0, 0, 0 };
        if (ops->SendSocket(client, ok, 10) != 10) {
            ops->CloseSocket(client);
            ops->CloseSocket(target);
            state->done.store(true);
            return;
        }
        LOG_DEBUG("PivotEntry: bridge to " << host << ":" << port);

        // ---- 3. 双向桥接(单向退出即关双向; stop 供 Stop() 置位) ----
        std::atomic<bool> done{ false };
        std::thread t1(Pump, ops.get(), client, target, std::ref(done), std::cref(state->stop));
        std::thread t2(Pump, ops.get(), target, client, std::ref(done), std::cref(state->stop));
        t1.join();
        t2.join();
        LOG_DEBUG("PivotEntry: bridge closed " << host << ":" << port);

        state->done.store(true);
    }

} // namespace cleat
