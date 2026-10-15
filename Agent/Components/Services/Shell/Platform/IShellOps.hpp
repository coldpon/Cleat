#pragma once
#include <string>
#include <memory>
#include <cstdint>

namespace cleat {

    class IShellOps {
    public:
        virtual ~IShellOps() = default;

        // 启动 shell 进程（如 cmd.exe）
        virtual bool StartProcess() = 0;

        // 终止进程（强制）
        virtual void TerminateProcess() = 0;

        // 检查进程是否存活
        virtual bool IsProcessAlive() const = 0;

        // 向 stdin 写入命令（自动追加换行）
        virtual bool WriteInput(const std::string& command) = 0;

        // 读取 stdout（非阻塞，返回当前可读数据，若无可读返回空）
        virtual std::string ReadOutput() = 0;

        // 获取进程退出码（若有）
        virtual int GetExitCode() const = 0;
    };

    std::unique_ptr<IShellOps> CreateShellOps();

} // namespace cleat