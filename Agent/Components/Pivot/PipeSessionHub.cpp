#include "Components/Pivot/PipeSessionHub.hpp"

#include "Common/Protocol/CmdDef.hpp"
#include "Common/Logger/ErrLog.hpp"
#include "Common/Logger/Logger.hpp"
#include "Common/Protocol/FrameBuilder.hpp"
#include "Core/Comms/Platform/IPipeOps.hpp"
#include "Core/Comms/Transports/Smb/PipeChannel.hpp"

#include <chrono>

namespace cleat {

    PipeSessionHub& PipeSessionHub::Instance() {
        static PipeSessionHub instance;
        return instance;
    }

    PipeSessionHub::~PipeSessionHub() { StopAll(); }

    void PipeSessionHub::SetDataSender(DataSender sender) {
        sender_ = std::move(sender);
    }

    bool PipeSessionHub::IsSessionId(const std::string& pivot_id) {
        return pivot_id.rfind("sess-", 0) == 0;
    }

    std::string PipeSessionHub::PipeNameOf(const std::string& sess_id) {
        return IsSessionId(sess_id) ? sess_id.substr(5) : std::string();
    }

    bool PipeSessionHub::Listen(const std::string& pipe_name, const std::string& key) {
        if (pipe_name.empty()) {
            CLEAT_LOGE(ErrDom::Piv, ErrCode::InvalidParam, "PipeSessionHub: 空管道名");
            return false;
        }
        Stop(pipe_name);   // 同名替换

        auto entry = std::make_shared<Entry>();
        entry->pipe_name = pipe_name;
        entry->key = key;
        entry->running.store(true);
        auto raw = entry;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            entries_[pipe_name] = std::move(entry);
        }
        raw->keeper = std::make_unique<std::thread>(&PipeSessionHub::KeeperLoop, this, raw);
        LOG_INFO("PipeSessionHub: 入口开启 " << pipe_name
            << (key.empty() ? " (no auth)" : " (auth)"));
        return true;
    }

    bool PipeSessionHub::Stop(const std::string& pipe_name) {
        std::shared_ptr<Entry> entry;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto it = entries_.find(pipe_name);
            if (it == entries_.end()) return false;
            entry = std::move(it->second);       // 出表 + 保命(shared_ptr)
            entries_.erase(it);
        }
        entry->running.store(false);
        {
            std::lock_guard<std::mutex> cl(entry->chan_mu);
            if (entry->channel) {
                entry->channel->Close();
                entry->channel.reset();
            }
        }
        if (entry->keeper && entry->keeper->joinable()) {
            entry->keeper->join();
        }
        LOG_INFO("PipeSessionHub: 入口关闭 " << pipe_name);
        return true;
    }

    void PipeSessionHub::StopAll() {
        std::vector<std::shared_ptr<Entry>> all;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            for (auto& pair : entries_) {
                pair.second->running.store(false);
                all.push_back(pair.second);
            }
            entries_.clear();
        }
        for (auto& e : all) {
            {
                std::lock_guard<std::mutex> cl(e->chan_mu);
                if (e->channel) {
                    e->channel->Close();
                    e->channel.reset();
                }
            }
            if (e->keeper && e->keeper->joinable()) {
                e->keeper->join();
            }
        }
    }

    size_t PipeSessionHub::ActiveCount() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return entries_.size();
    }

    bool PipeSessionHub::WriteToSession(const std::string& sess_id,
        const std::vector<uint8_t>& data) {
        const std::string name = PipeNameOf(sess_id);
        if (name.empty()) return false;
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = entries_.find(name);
        if (it == entries_.end()) return false;
        Entry* e = it->second.get();
        std::lock_guard<std::mutex> cl(e->chan_mu);
        if (!e->channel) return false;
        return e->channel->Write(data) == SendResult::Sent;
    }

    void PipeSessionHub::SendUp(const std::string& pipe_name,
        const std::vector<uint8_t>& body) {
        if (!sender_) return;
        nlohmann::json header;
        header["cmd"] = Cmd::Pivot::Data();
        header["data"]["pivot_id"] = "sess-" + pipe_name;
        sender_(FrameBuilder::Build(header, body));
    }

    void PipeSessionHub::KeeperLoop(std::shared_ptr<Entry> entry) {
        const std::string name = entry->pipe_name;
        const std::string key = entry->key;
        int backoff_ms = 200;

        while (entry->running.load()) {
            // ---- 建通道(server 模式, pre-auth 由 PipeChannel 内完成) ----
            TransportConfig cfg;
            cfg.url = "pipe://" + name;
            cfg.pipe_key = key;
            cfg.timeout_ms = 2000;
            auto ch = std::make_unique<PipeChannel>(CreatePipeOps());
            if (!ch->Open(cfg)) {
                ch.reset();
                LOG_WARN("PipeSessionHub: 管道创建失败 " << name << ", "
                    << backoff_ms << "ms 后重试");
                std::this_thread::sleep_for(std::chrono::milliseconds(backoff_ms));
                if (backoff_ms < 2000) backoff_ms *= 2;
                continue;
            }
            backoff_ms = 200;

            {
                std::lock_guard<std::mutex> cl(entry->chan_mu);
                entry->channel = std::move(ch);
            }
            LOG_DEBUG("PipeSessionHub: 入口已连接 " << name);

            // ---- 收帧上送(长寿命; 断开 → 重建) ----
            std::vector<uint8_t> frame;
            while (entry->running.load()) {
                {
                    std::lock_guard<std::mutex> cl(entry->chan_mu);
                    if (!entry->channel) break;
                    if (!entry->channel->Pump(frame)) {
                        // 无数据 or 断开: 靠 Health 区分
                        if (entry->channel->HealthNow() == ChannelHealth::Dead) break;
                        std::this_thread::sleep_for(std::chrono::milliseconds(10));
                        continue;
                    }
                }
                SendUp(name, frame);
            }

            // ---- 断开清理, 循环重建 ----
            {
                std::lock_guard<std::mutex> cl(entry->chan_mu);
                if (entry->channel) {
                    entry->channel->Close();
                    entry->channel.reset();
                }
            }
            if (entry->running.load()) {
                LOG_WARN("PipeSessionHub: 子连接断开, 重建入口 " << name);
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
            }
        }
    }

} // namespace cleat
