#pragma once
#include "Components/Services/Shell/Platform/IShellOps.hpp"
#include <memory>
#include <atomic>
#include <thread>
#include <functional>
#include <string>

namespace cleat {

    class ShellProvider {
    public:
        explicit ShellProvider(std::unique_ptr<IShellOps> ops);
        ~ShellProvider();

        using OutputCallback = std::function<void(const std::string&)>;
        void SetOutputCallback(OutputCallback cb);

        bool Start();
        void Stop();
        bool IsRunning() const;
        bool ExecuteCommand(const std::string& cmd);
        bool IsProcessAlive() const;

        // 直接读取输出（用于测试）
        std::string ReadOutput() { return ops_ ? ops_->ReadOutput() : ""; }

    private:
        void readerThread();

        std::unique_ptr<IShellOps> ops_;
        std::atomic<bool> running_{ false };
        std::unique_ptr<std::thread> readerThread_;
        OutputCallback callback_;
    };

} // namespace cleat