#pragma once
#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "Core/Comms/Platform/INetworkOps.hpp"

namespace cleat {

    /**
     * @brief 多跳级联的"转发入口"(SOCKS5 服务端 + 本地桥接)
     *
     * 监听本地端口; 每个入站连接按 SOCKS5 协商出目标地址后, 在本地建立
     * 到目标的连接并双向桥接。上游节点(如前一跳的 Pivot)只把它当普通
     * TCP 目标, 因此无法得知真正的目标地址 —— 实现"节点只知道前后一跳"。
     *
     * 并发: 每个入站连接一个独立工作线程(桥接占连接整个生命周期, 不能
     * 占用 accept 线程——串行处理会让第二条并行连接卡死在队后)。工作
     * 线程持有独立 INetworkOps 实例与配置快照(按值捕获), 不依赖本对象
     * 存活; Stop/析构后在途桥接在连接 recv 超时周期(30s)内自灭。
     *
     * 停止契约: 只有工作线程自己关闭自己的连接 socket(单一关闭者, 避免
     * 句柄释放被复用时误关他人 socket); Stop 只关监听 socket 并置
     * ConnState::stop 标志, 桥接循环据此退出。
     *
     * 鉴权: 传入 user/pass 时要求 RFC1929 user/pass 子协商(客户端未提供
     * 0x02 方法直接拒绝); 无凭据时仅接受 NO-AUTH(严格按客户端提供的方法
     * 列表选择, 不再无条件回 05 00)。绑定地址由调用方显式给定。
     *
     * 平台无关: 全部走 INetworkOps 抽象(限 IPv4 监听; CONNECT 暂不
     * 支持 ATYP=0x04 IPv6 目标)。
     */
    class PivotEntry {
    public:
        PivotEntry();
        ~PivotEntry();

        // 每连接的工作状态(供 Stop 置停止标志/观察完成)
        struct ConnState {
            std::atomic<bool> done{ false };
            std::atomic<bool> stop{ false };
        };

        // 绑定并监听; bind_host 为空或 "0.0.0.0" 表示所有网卡。
        // 已在监听同一配置时返回 true, 否则先停旧实例。
        bool Start(uint16_t port, const std::string& bind_host,
            const std::string& username, const std::string& password);
        void Stop();
        bool IsRunning() const { return running_.load(); }
        uint16_t GetPort() const { return port_; }

    private:
        void AcceptLoop();
        // 回收已完成的工作线程(done 置位); join 在锁外进行
        void ReapFinished();
        // 每连接工作线程主体: static + 配置按值快照 + 独立 ops,
        // 不依赖 PivotEntry 实例存活(Stop/重启无生命周期竞争)
        static void HandleConnection(INetworkOps::SocketHandle client,
            std::shared_ptr<ConnState> state, std::unique_ptr<INetworkOps> ops,
            std::string username, std::string password);

        struct ConnWorker {
            std::shared_ptr<ConnState> state;
            std::unique_ptr<std::thread> thread;
        };

        std::unique_ptr<INetworkOps> ops_;
        std::atomic<bool> running_{ false };
        INetworkOps::SocketHandle listen_sock_ = -1;
        uint16_t port_ = 0;
        std::string bind_host_;
        std::string username_;
        std::string password_;
        std::unique_ptr<std::thread> accept_thread_;

        std::mutex conn_mu_;
        std::vector<ConnWorker> conn_workers_;
    };

    // 全局转发入口(单例, 当前版本单端口)
    PivotEntry& PivotEntryInstance();

} // namespace cleat
