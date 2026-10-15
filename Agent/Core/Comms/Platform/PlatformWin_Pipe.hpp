#pragma once

#include "Core/Comms/Platform/IPipeOps.hpp"

namespace cleat {

    /**
     * @brief Windows 命名管道操作实现(字节模式 + 同步句柄)
     *
     * 仅文件名带 PlatformWin_ 前缀, Linux 构建经 CMake 平台排除规则自动剔除;
     * 对应的 Linux 工厂(CreatePipeOps)在 PlatformLinux_Pipe.cpp 返回 nullptr。
     */
    class WinPipeOps : public IPipeOps {
    public:
        WinPipeOps() = default;
        ~WinPipeOps() override = default;

        PipeHandle CreateServer(const std::string& name) override;
        bool WaitForClient(PipeHandle handle) override;
        PipeHandle ConnectClient(const std::string& host,
            const std::string& name, int timeout_ms) override;
        int AvailableBytes(PipeHandle handle) override;
        int Read(PipeHandle handle, void* buf, size_t len) override;
        int Write(PipeHandle handle, const void* buf, size_t len) override;
        void Disconnect(PipeHandle handle) override;
        void Close(PipeHandle handle) override;

    private:
        static std::string FullPipePath(const std::string& host, const std::string& name);
    };

} // namespace cleat
