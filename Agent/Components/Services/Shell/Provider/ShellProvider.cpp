#include "ShellProvider.hpp"
#include "Common/Logger/ErrLog.hpp"
#include "Common/Logger/Logger.hpp"
#include "Core/Concurrency/ThreadGovernor.hpp"
#include <chrono>

namespace cleat {

    ShellProvider::ShellProvider(std::unique_ptr<IShellOps> ops) : ops_(std::move(ops)) {}
    ShellProvider::~ShellProvider() { Stop(); }

    void ShellProvider::SetOutputCallback(OutputCallback cb) {
        callback_ = std::move(cb);
    }

    bool ShellProvider::Start() {
        if (running_.load()) {
            LOG_WARN("ShellProvider already running");
            return false;
        }
        if (!ops_->StartProcess()) {
            CLEAT_LOGE(ErrDom::Svc, ErrCode::PlatformApiFailed, "ShellProvider: StartProcess failed");
            return false;
        }
        running_.store(true);
        readerThread_ = std::make_unique<std::thread>(&ShellProvider::readerThread, this);
        LOG_DEBUG("ShellProvider started");
        return true;
    }

    void ShellProvider::Stop() {
        // no early-return: readerThread may have already self-stopped
        // (running_ = false when cmd exited) but must still be joined;
        // destroying a joinable std::thread calls std::terminate
        running_.store(false);
        ops_->TerminateProcess();
        if (readerThread_ && readerThread_->joinable()) {
            readerThread_->join();
        }
        LOG_DEBUG("ShellProvider stopped");
    }

    bool ShellProvider::IsRunning() const {
        return running_.load();
    }

    bool ShellProvider::ExecuteCommand(const std::string& cmd) {
        return ops_->WriteInput(cmd);
    }

    bool ShellProvider::IsProcessAlive() const {
        return ops_->IsProcessAlive();
    }

    void ShellProvider::readerThread() {
        //LOG_DEBUG("ShellProvider readerThread started");
        // 线程治理: 暂停期不读管道 —— cmd 输出积在 OS 管道缓冲(有界), 子进程写侧
        // 自然背压; 恢复后排空, 输出不丢。绑定 running_: 暂停期被 Stop 时立即收摊。
        ThreadGovernor::GovLoop gov("svc-shell-reader", JobKind::Service, &running_);
        while (running_.load()) {
            if (!gov.Tick()) break;
            std::string output = ops_->ReadOutput();
            if (!output.empty() && callback_) {
                callback_(output);
            }
            if (!ops_->IsProcessAlive()) {
                running_.store(false);
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        //LOG_DEBUG("ShellProvider readerThread exited");
    }

} // namespace cleat