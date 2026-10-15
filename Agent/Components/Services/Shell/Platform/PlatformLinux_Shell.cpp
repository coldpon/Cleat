#include "PlatformLinux_Shell.hpp"
#include "Common/Logger/ErrLog.hpp"
#include "Common/Logger/Logger.hpp"

#include <pty.h>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

namespace cleat {

    ShellOpsLinux::ShellOpsLinux() = default;

    ShellOpsLinux::~ShellOpsLinux() {
        TerminateProcess();
    }

    bool ShellOpsLinux::StartProcess() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (running_.load()) {
            LOG_WARN("ShellOpsLinux: Process already running");
            return false;
        }

        // forkpty: 创建伪终端 + fork; 子进程已 setsid 并连上 slave 端
        int master = -1;
        pid_t pid = forkpty(&master, nullptr, nullptr, nullptr);
        if (pid < 0) {
            CLEAT_LOGE(ErrDom::Svc, ErrCode::PlatformApiFailed, "ShellOpsLinux: forkpty failed, errno=" << errno);
            return false;
        }

        if (pid == 0) {
            // 子进程: exec /bin/sh —— stdin 是 tty 时 shell 自动进入交互模式
            // (与 Windows 侧硬编码 cmd.exe 保持一致, 需要可配置时再扩展接口)
            execl("/bin/sh", "/bin/sh", static_cast<char*>(nullptr));
            _exit(127);   // exec 失败
        }

        // 父进程: master 端设为非阻塞读, 避免 ReadOutput 阻塞 readerThread
        int flags = fcntl(master, F_GETFL, 0);
        if (flags == -1 || fcntl(master, F_SETFL, flags | O_NONBLOCK) == -1) {
            LOG_WARN("ShellOpsLinux: fcntl O_NONBLOCK failed, errno=" << errno);
            // 非致命: 非阻塞失败时读不到数据最多返回空
        }

        masterFd_ = master;
        childPid_ = pid;
        exitCode_ = -1;
        running_.store(true);
        LOG_DEBUG("ShellOpsLinux: /bin/sh started pid=" << pid);
        return true;
    }

    void ShellOpsLinux::TerminateProcess() {
        std::lock_guard<std::mutex> lock(mutex_);
        // 无 early-return: 即使 shell 已退出也必须关闭 fd / 回收子进程
        running_.store(false);

        if (childPid_ > 0) {
            ::kill(childPid_, SIGKILL);
            int status = 0;
            pid_t r = waitpid(childPid_, &status, 0);
            if (r == childPid_ && WIFEXITED(status)) {
                exitCode_ = WEXITSTATUS(status);
            }
            childPid_ = -1;
        }

        if (masterFd_ >= 0) {
            close(masterFd_);
            masterFd_ = -1;
        }
        LOG_DEBUG("ShellOpsLinux: terminated");
    }

    bool ShellOpsLinux::IsProcessAlive() const {
        // 只读判断: 子进程由 ReadOutput()(master EIO/EOF 时)回收, 回收后 childPid_ 置 -1。
        // Provider::readerThread 的时序是 ReadOutput() 先于 IsProcessAlive(), 不存在中间窗口。
        std::lock_guard<std::mutex> lock(mutex_);
        return childPid_ > 0;
    }

    bool ShellOpsLinux::WriteInput(const std::string& command) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (masterFd_ < 0 || !running_.load()) {
            LOG_DEBUG("ShellOpsLinux: WriteInput failed (pty closed or not running)");
            return false;
        }

        const std::string cmd = command + "\n";
        size_t off = 0;
        while (off < cmd.size()) {
            ssize_t n = write(masterFd_, cmd.data() + off, cmd.size() - off);
            if (n > 0) {
                off += static_cast<size_t>(n);
                continue;
            }
            if (n < 0 && (errno == EAGAIN || errno == EINTR)) {
                continue;   // 重试
            }
            CLEAT_LOGE(ErrDom::Svc, ErrCode::PlatformApiFailed, "ShellOpsLinux: write failed, errno=" << errno);
            return false;
        }
        LOG_DEBUG("ShellOpsLinux: wrote " << cmd.size() << " bytes");
        return true;
    }

    std::string ShellOpsLinux::ReadOutput() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (masterFd_ < 0 || !running_.load()) {
            return {};
        }

        const size_t maxRead = static_cast<size_t>(64) * 1024;
        std::string buffer(maxRead, '\0');
        ssize_t n = read(masterFd_, &buffer[0], maxRead);
        if (n > 0) {
            buffer.resize(static_cast<size_t>(n));
            LOG_DEBUG("ShellOpsLinux: read " << n << " bytes");
            return buffer;   // Linux 终端输出默认 UTF-8, 无需 GBK 转换
        }
        if (n == 0 || (n < 0 && errno != EAGAIN && errno != EINTR)) {
            // master 端 EOF/EIO 表示对端已关闭(shell 退出), 立即回收子进程并复位运行态
            running_.store(false);
            ReapIfExited();
            LOG_DEBUG("ShellOpsLinux: pty closed (n=" << n << " errno=" << errno << ")");
        }
        return {};
    }

    void ShellOpsLinux::ReapIfExited() {
        if (childPid_ <= 0) return;
        int status = 0;
        pid_t r = waitpid(childPid_, &status, WNOHANG);
        if (r == childPid_) {
            if (WIFEXITED(status)) exitCode_ = WEXITSTATUS(status);
            else if (WIFSIGNALED(status)) exitCode_ = 128 + WTERMSIG(status);
            childPid_ = -1;
        }
        // r == 0: 尚未退出; r == -1(ECHILD): 已被其他路径回收
    }
    int ShellOpsLinux::GetExitCode() const {
        return exitCode_;
    }

} // namespace cleat
