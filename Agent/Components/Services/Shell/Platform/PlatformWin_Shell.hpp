#pragma once
#include "IShellOps.hpp"
#include <windows.h>
#include <atomic>
#include <mutex>
#include <string>

#undef min
namespace cleat {

    class ShellOpsWin : public IShellOps {
    public:
        ShellOpsWin();
        ~ShellOpsWin() override;

        bool StartProcess() override;
        void TerminateProcess() override;
        bool IsProcessAlive() const override;
        bool WriteInput(const std::string& command) override;
        std::string ReadOutput() override;
        int GetExitCode() const override;

    private:
        HANDLE hStdoutRd_ = nullptr;
        HANDLE hStdoutWr_ = nullptr;
        HANDLE hStdinRd_ = nullptr;
        HANDLE hStdinWr_ = nullptr;
        HANDLE hProcess_ = nullptr;
        std::atomic<bool> running_{ false };
        mutable std::mutex outputMutex_;
        std::string outputBuffer_;   // 累积输出
        int exitCode_ = -1;
    };

} // namespace cleat