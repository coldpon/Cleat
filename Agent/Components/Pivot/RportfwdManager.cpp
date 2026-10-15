#include "Components/Pivot/RportfwdManager.hpp"
#include "Components/Pivot/PivotManager.hpp"
#include "Common/Logger/ErrLog.hpp"
#include "Common/Logger/Logger.hpp"
#include "Core/Concurrency/ThreadGovernor.hpp"

namespace cleat {

    // 监听器状态(shared_ptr: AcceptLoop 线程与 Stop 竞争时保命)
    struct RportfwdListener {
        uint64_t session_id = 0;
        uint16_t listen_port = 0;
        std::string target_host;
        uint16_t target_port = 0;
        INetworkOps::SocketHandle sock = -1;
        std::atomic<bool> running{ false };
        std::thread thread_ref; // accept 线程
    };

    RportfwdManager& RportfwdManager::Instance() {
        static RportfwdManager instance;
        return instance;
    }

    RportfwdManager::RportfwdManager() : ops_(CreateNetworkOps()) {}

    RportfwdManager::~RportfwdManager() { StopAll(); }

    bool RportfwdManager::Start(uint64_t session_id, uint16_t listen_port,
        const std::string& target_host, uint16_t target_port) {
        if (listen_port == 0 || target_host.empty() || target_port == 0) {
            CLEAT_LOGE(ErrDom::Piv, ErrCode::InvalidParam,
                "RportfwdManager: invalid params");
            return false;
        }
        Stop(listen_port);

        INetworkOps::SocketHandle s = ops_->CreateTcpSocket();
        if (s < 0) {
            CLEAT_LOGE(ErrDom::Piv, ErrCode::PlatformApiFailed,
                "RportfwdManager: create socket failed");
            return false;
        }
        ops_->SetSocketTimeout(s, 60000);
        if (!ops_->BindListen(s, listen_port)) {
            CLEAT_LOGE(ErrDom::Piv, ErrCode::PlatformApiFailed,
                "RportfwdManager: bind/listen failed on port " << listen_port);
            ops_->CloseSocket(s);
            return false;
        }

        auto lis = std::make_shared<RportfwdListener>();
        lis->session_id = session_id;
        lis->listen_port = listen_port;
        lis->target_host = target_host;
        lis->target_port = target_port;
        lis->sock = s;
        lis->running.store(true);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            listeners_[listen_port] = lis;
        }
        lis->thread_ref = std::thread(&RportfwdManager::AcceptLoop, this, lis);
        LOG_INFO("RportfwdManager: listening " << listen_port
            << " -> (server) " << target_host << ":" << target_port);
        return true;
    }

    bool RportfwdManager::Stop(uint16_t listen_port) {
        std::shared_ptr<RportfwdListener> lis;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto it = listeners_.find(listen_port);
            if (it == listeners_.end()) return false;
            lis = std::move(it->second);
            listeners_.erase(it);
        }
        lis->running.store(false);
        ops_->CloseSocket(lis->sock);
        if (lis->thread_ref.joinable()) {
            lis->thread_ref.join();
        }
        LOG_INFO("RportfwdManager: stopped " << listen_port);
        return true;
    }

    void RportfwdManager::StopAll() {
        std::vector<std::shared_ptr<RportfwdListener>> all;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            for (auto& pair : listeners_) {
                pair.second->running.store(false);
                all.emplace_back(pair.second);
            }
            listeners_.clear();
        }
        for (auto& lis : all) {
            ops_->CloseSocket(lis->sock);
            if (lis->thread_ref.joinable()) {
                lis->thread_ref.join();
            }
        }
    }

    std::vector<RportfwdManager::Entry> RportfwdManager::List() const {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<Entry> out;
        for (auto& pair : listeners_) {
            out.push_back({ pair.second->listen_port,
                pair.second->target_host, pair.second->target_port });
        }
        return out;
    }

    size_t RportfwdManager::ActiveCount() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return listeners_.size();
    }

    void RportfwdManager::AcceptLoop(std::shared_ptr<RportfwdListener> lis) {
        // 线程治理: 暂停期不 accept 新客 —— Tick 在循环顶阻塞, 新连接积在
        // 内核监听队列(有界); 恢复后照常接客。既有连接由各自 pivot 泵线程治理。
        // 绑定 lis->running: 暂停期该监听被 Stop 时立即收摊, join 不必等窗口重开。
        const std::string gov_name = "rportfwd-accept-" + std::to_string(lis->listen_port);
        ThreadGovernor::GovLoop gov(gov_name.c_str(), JobKind::Infrastructure, &lis->running);
        while (lis->running.load()) {
            if (!gov.Tick()) break;
            INetworkOps::SocketHandle c = ops_->AcceptSocket(lis->sock);
            if (c < 0) {
                if (!lis->running.load()) break;
                continue;
            }
            if (!lis->running.load()) { // Stop 竞态窗口内 accept 到的连接
                ops_->CloseSocket(c);
                break;
            }
            ops_->SetSocketTimeout(c, 30000);

            const uint64_t n = conn_seq_.fetch_add(1) + 1;
            std::string id = "rportfwd-" + std::to_string(lis->session_id)
                + "-" + std::to_string(lis->listen_port)
                + "-" + std::to_string(n);

            // 先通告 Server(其登记后才能承接后续 PivotData), 再托管连接;
            // 托管失败时回发 Close 让 Server 立即回收已拨出的目标连接。
            PivotManager::Instance().SendOpenedFrame(id, lis->listen_port,
                lis->target_host, lis->target_port);
            if (!PivotManager::Instance().AdoptSocket(id, c,
                lis->target_host, lis->target_port)) {
                PivotManager::Instance().SendCloseFrame(id, "adopt failed");
                ops_->CloseSocket(c);
                CLEAT_LOGE(ErrDom::Piv, ErrCode::InternalFailed,
                    "RportfwdManager: adopt failed " << id);
            }
        }
    }

} // namespace cleat
