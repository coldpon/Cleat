#pragma once

#include <atomic>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "Core/Comms/Channel/IChannel.hpp"
#include "Core/Comms/Platform/IPipeOps.hpp"

namespace cleat {

    /**
     * @brief 命名管道通道(多级 Pivot L1: SMB 传输)
     *
     * 角色由 url 决定:
     *   "pipe://<name>"            → 服务端(本机创建 \\.\pipe\<name>)
     *   "pipe://<host>/<name>"     → 客户端(连 \\host\pipe\<name>)
     *   另接受原生路径 \\.\pipe\name / \\host\pipe\name 直传。
     *
     * 帧定界: [4B 大端长度][body](PipeFrame 共用工具), 与 TCP 传输一致。
     *
     * pre-auth: cfg.pipe_key 非空时, 连入(服务端)/连接(客户端)
     * 成功后先做 challenge-HMAC 握手 —— 服务端发 16B 随机 challenge, 客户端回
     * HMAC-SHA512-256(key, challenge) 32B, 失败即断开; 双端 authenticated_ 置位
     * 后会话层帧才开始流转。key 为空 = 无认证(L1 兼容形态)。
     *
     * 并发模型:
     *   - 服务端 Open 后起监听线程(WaitForClient + 握手), 通过后置 connected_;
     *     Close 用 CloseHandle 打断(DisconnectNamedPipe 唤醒不了阻塞调用)。
     *   - Write/Pump 由会话层单线程调度(IChannel 契约)。
     *
     * Close 语义: 作废全部链路级状态 —— 重组缓冲清空,
     * 监听线程收割, 句柄关闭; 不得有第二套清理路径。
     *
     * 限制: 单实例管道(一条管道一个对端, 隐含路由, 见设计);
     * 写缓冲(1MB)满时 Write 阻塞, 由"对端常驻 Pump"排空(与 TCP send 同级)。
     */
    class PipeChannel : public IChannel {
    public:
        explicit PipeChannel(std::unique_ptr<IPipeOps> ops);
        ~PipeChannel() override;

        PipeChannel(const PipeChannel&) = delete;
        PipeChannel& operator=(const PipeChannel&) = delete;

        // ---- IChannel ----
        ChannelCaps Caps() const override;
        bool Open(const TransportConfig& cfg) override;
        void Close() override;
        ChannelHealth HealthNow() const override;
        SendResult Write(const std::vector<uint8_t>& frame) override;
        bool Pump(std::vector<uint8_t>& out) override;
        TransportType Type() const override { return TransportType::Smb; }
        int Priority() const override { return 1; }   // 高带宽流式, 仅次于 TCP

        // 解析 "pipe://..." / 原生路径 → (host, name); 非法返回 false(纯逻辑, 公开供测试)
        static bool ParseUrl(const std::string& url,
            std::string& host, std::string& name, bool& is_server);


    private:
        // 服务端监听线程主体; 句柄为启动时的副本(不读成员, 避免 Close 竞态)
        void ListenLoop(IPipeOps::PipeHandle handle);
        // pre-auth: 客户端等 challenge 回 HMAC; 服务端发 challenge 验 HMAC。
        // 返回 false = 握手失败(调用方断开); key 为空 = 跳过(返回 true)。
        // leftover: 握手读取时粘连进来的后续字节(对端抢先发的会话帧),
        // 成功后必须注入重组缓冲, 否则丢帧(实测 hub 上行首帧丢失)。
        bool ClientHandshake(IPipeOps::PipeHandle handle, const uint8_t key[32],
            std::vector<uint8_t>& leftover);
        bool ServerHandshake(IPipeOps::PipeHandle handle, const uint8_t key[32],
            std::vector<uint8_t>& leftover);
        // 从重组缓冲提取一帧(够 4B 头 + body 才交付); 超限帧作废链路
        bool TryPopFrame(std::vector<uint8_t>& out);

        std::unique_ptr<IPipeOps> ops_;
        IPipeOps::PipeHandle handle_ = nullptr;
        std::atomic<bool> opened_{ false };     // Open 成功(服务端: 管道已创建)
        std::atomic<bool> connected_{ false };  // 对端已连入且通过认证(可收发)
        std::atomic<bool> closed_{ false };

        // 服务端监听线程(WaitForClient + 握手; Close 用 CloseHandle 打断)
        std::unique_ptr<std::thread> listen_thread_;

        // Open 时的配置快照(监听线程用, 与成员生命周期解耦)
        std::string pipe_key_;
        std::string pipe_name_;

        // Pump 流式重组缓冲(4B 头 + body; Close 清空; 可能压有多帧)。
        // 同步: 握手残留注入(listen 线程/Open)先于 connected_.store(true),
        // Pump 仅在 connected_ 后读写 —— happens-before 经 connected_ 原子量
        // 建立, 无需互斥量(Close 的 join 进一步保证注入完成后才清空)。
        std::vector<uint8_t> partial_;

        static constexpr size_t kMaxFrameBytes = 4 * 1024 * 1024;
    };

} // namespace cleat
