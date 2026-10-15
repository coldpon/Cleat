#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "Core/Comms/Transports/Smb/PipeChannel.hpp"

namespace cleat {

    /**
     * @brief 会话借道枢纽
     *
     * 父 Agent 侧: 为每个入口(pipe_name + 配对密钥)持有一个 PipeChannel
     * (server 模式, pre-auth), 子 Agent 连入后, 其会话帧(端到端密文)经本
     * 枢纽以 PivotData{"sess-<pipe_name>"} 混合帧透传上送 TS; TS 下行的
     * sess- PivotData 由 PivotDataHandler 分流回 WriteToSession 写管道。
     * 本枢纽只搬密文, 不解释、不缓存明文( 约定)。
     *
     * 与 PivotManager 的关系: 独立组件 —— 会话借道是长寿命低频密文流,
     * 与内网目标透传(socket 语义/背压关闭语义)不同构; 入口连接的生命
     * 周期由本类自管(断线销毁重建管道实例, 子端以既有重试逻辑重连)。
     *
     * 生命周期: 入口跨断线存续(监护线程检测断开 → 重建管道等待重连);
     * StopAll 仅进程退出/模块卸载用。
     */
    class PipeSessionHub {
    public:
        static PipeSessionHub& Instance();

        // 上行发送回调(AgentContext 装配注入, 走统一出站收口)
        using DataSender = std::function<void(const std::vector<uint8_t>&)>;

        void SetDataSender(DataSender sender);

        // 开入口: 本机创建 pipe_name 管道等待子 Agent 连入(key 为 pre-auth
        // 口令, 空 = 不认证——仅测试/本机同用户场景)。同名重启会替换。
        bool Listen(const std::string& pipe_name, const std::string& key);

        // 关入口(在途子连接随之终结)
        bool Stop(const std::string& pipe_name);

        void StopAll();

        size_t ActiveCount() const;

        // TS 下行分流(PivotDataHandler 调用): sess_id = "sess-<pipe_name>",
        // data 为子 Agent 会话层密文帧, 原样写管道
        bool WriteToSession(const std::string& sess_id, const std::vector<uint8_t>& data);

        // sess- 前缀判定(PivotDataHandler 分流用)
        static bool IsSessionId(const std::string& pivot_id);
        // "sess-<pipe_name>" → pipe_name
        static std::string PipeNameOf(const std::string& sess_id);

    private:
        PipeSessionHub() = default;
        ~PipeSessionHub();
        PipeSessionHub(const PipeSessionHub&) = delete;
        PipeSessionHub& operator=(const PipeSessionHub&) = delete;

        struct Entry;
        // shared_ptr 保命: Stop 先出表再 join, keeper 线程仍持有 entry 引用
        // (UAF 教训同 RportfwdListener)
        void KeeperLoop(std::shared_ptr<Entry> entry);

        struct Entry {
            std::string pipe_name;
            std::string key;
            std::atomic<bool> running{ false };
            std::unique_ptr<std::thread> keeper;          // 监护线程
            std::unique_ptr<PipeChannel> channel;         // 当前管道通道(server 模式)
            std::mutex chan_mu;                           // 保护 channel 的重建/写
        };

        void SendUp(const std::string& pipe_name, const std::vector<uint8_t>& body);

        mutable std::mutex mutex_;
        std::map<std::string, std::shared_ptr<Entry>> entries_;
        DataSender sender_;
    };

} // namespace cleat
