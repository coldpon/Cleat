#pragma once

#include <memory>
#include <string>

namespace cleat {

    /**
     * @brief 命名管道操作接口（纯虚, Windows 命名管道 / 其他平台无实现）
     *
     * 职责: 与 INetworkOps 同法的平台隔离层 —— PipeChannel 只依赖本接口,
     * 不直接调 CreateNamedPipe/CreateFile 等 Win32 API。
     * 工厂 CreatePipeOps(): Windows 返回真实现, 其他平台返回 nullptr
     * (ChannelRegistry::Create 据此返回空通道, 与 make_http 同法)。
     *
     * 帧定界由上层(PipeChannel)负责: [4B 大端长度][body], 本接口只管字节流。
     * 管道为字节模式(PIPE_TYPE_BYTE) + 同步句柄; 读侧必须先 AvailableBytes
     * 确认有数据再 Read(Read 内部同样先探测, 保证不无限阻塞)。
     */
    class IPipeOps {
    public:
        virtual ~IPipeOps() = default;

        using PipeHandle = void*;   ///< HANDLE; 无效值 nullptr

        // ---------- 服务端 ----------
        // 创建命名管道服务端实例(\\.\pipe\<name>), 字节模式, 双向, 1MB 收发缓冲
        virtual PipeHandle CreateServer(const std::string& name) = 0;
        // 等待客户端连入(阻塞; 客户端已连入的竞态 ERROR_PIPE_CONNECTED 视为成功)。
        // 打断契约: 另一线程 Close 该实例(CloseHandle)后此调用立即失败返回 ——
        // 供监听线程退出; DisconnectNamedPipe 唤醒不了阻塞中的 ConnectNamedPipe。
        virtual bool WaitForClient(PipeHandle handle) = 0;

        // ---------- 客户端 ----------
        // 连接 \\host\pipe\<name>(host 空 = 本机); 内部含 WaitNamedPipe 重试,
        // 总预算 timeout_ms, 超时返回 nullptr
        virtual PipeHandle ConnectClient(const std::string& host,
            const std::string& name, int timeout_ms) = 0;

        // ---------- 字节流 ----------
        // 当前可读字节数; 失败/对端断开返回 -1
        virtual int AvailableBytes(PipeHandle handle) = 0;
        // 读(调用方须先确认 AvailableBytes>0, 内部以 available 限长保证不阻塞);
        // 返回实际字节, -1 = 错误/断开
        virtual int Read(PipeHandle handle, void* buf, size_t len) = 0;
        // 写; 返回实际字节, -1 = 错误/断开。管道写缓冲(1MB)满时可能阻塞 ——
        // 与 TcpTransport 的 send 阻塞同级, 由"对端常驻 Pump"保证排空
        virtual int Write(PipeHandle handle, const void* buf, size_t len) = 0;

        // ---------- 生命周期 ----------
        // 服务端: 断开当前客户端(实例可再次 WaitForClient 复用); 客户端: 等价 Close
        virtual void Disconnect(PipeHandle handle) = 0;
        virtual void Close(PipeHandle handle) = 0;
    };

    // 平台工厂: Windows 返回真实现, 其他平台返回 nullptr(通道创建时降级)
    std::unique_ptr<IPipeOps> CreatePipeOps();

} // namespace cleat
