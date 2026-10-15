#pragma once
#include "IShellOps.hpp"
#include <atomic>
#include <mutex>
#include <string>
#include <sys/types.h>

namespace cleat {

    // Linux 交互式 Shell: forkpty + /bin/sh, master 端读写伪终端
    // 与 Windows 管道版共用 IShellOps 接口, Provider/Job/Handler 层零改动
    class ShellOpsLinux : public IShellOps {
    public:
        ShellOpsLinux();
        ~ShellOpsLinux() override;

        bool StartProcess() override;
        void TerminateProcess() override;
        bool IsProcessAlive() const override;
        bool WriteInput(const std::string& command) override;
        std::string ReadOutput() override;
        int GetExitCode() const override;

    private:
        // 非阻塞回收已退出的子进程并记录退出码。
        // 仅由 ReadOutput()(EIO/EOF 时)与 TerminateProcess() 调用, 调用方须已持有 mutex_。
        void ReapIfExited();

        int masterFd_ = -1;            // pty master fd
        pid_t childPid_ = -1;          // shell 子进程 pid
        std::atomic<bool> running_{ false };
        mutable std::mutex mutex_;     // 保护 masterFd_/childPid_ 生命周期
        mutable int exitCode_ = -1;
    };

} // namespace cleat
