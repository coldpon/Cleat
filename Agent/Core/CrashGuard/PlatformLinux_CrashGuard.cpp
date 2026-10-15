/**
 * @file PlatformLinux_CrashGuard.cpp
 * @brief Linux 崩溃兜底实现
 * @details
 * 使用 sigaction 捕获段错误、总线错误、浮点异常、非法指令和 abort。
 * 信号处理函数中写崩溃标记，并通过 fork/exec 自重启。
 * 关键不变量：所有堆分配只在 InstallCrashGuard（正常上下文）完成，信号处理函数内零分配，
 * 仅使用 async-signal-safe 系统调用（open/write/close/fork/execv/_exit）。
 */
#include "CrashGuard.hpp"
#include "Common/Obfuscation/StringObf.hpp"

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <atomic>
#include <string>
#include <vector>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace cleat::Platform {

    namespace {

        /// 需要捕获的信号列表
        constexpr int kSignals[] = { SIGSEGV, SIGBUS, SIGFPE, SIGILL, SIGABRT };
        /// 信号处理函数重入保护标志（实测  sig_atomic_t 的
        /// "检查-置位"在多线程同时崩溃时非原子 —— 双双通过检查 → 双 fork 双实例。
        /// 改 lock-free atomic_flag, 与 EntryExe 信号路径"只做锁自由原子写"同纪律）
        std::atomic_flag g_inHandler = ATOMIC_FLAG_INIT;

        /// 崩溃标记开关（实测 / 与 Windows 同口径 = 构建期决定,
        /// Debug 恒开 / Release 恒关; 原先 Linux 无条件写标记, 默认配置下与 Windows
        /// 的"零取证"取向不一致 —— exe 同级 crashes/last_crash.txt 是现成 IOC）
        bool g_markerEnabled
#ifdef NDEBUG
            = false;
#else
            = true;
#endif

        std::string g_dumpDir;             ///< 转储目录（安装时计算）
        std::string g_markerPath;          ///< 崩溃标记文件路径（安装时计算）
        std::vector<std::string> g_argv;   ///< 原始命令行参数（安装时从 /proc/self/cmdline 读取）
        std::string g_exePath;            ///< 重启用可执行文件绝对路径(/proc/self/exe, 避免 argv[0] 相对路径失效)
        std::vector<char> g_altStack;      ///< sigaltstack 备用栈(信号处理在独立栈执行, 栈溢出可兜底)
        std::vector<char*> g_relaunchArgv; ///< 重启参数列表（原始参数 + "--after-crash" + nullptr，预构建）
        bool g_afterCrash = false;         ///< 当前实例是否已是崩溃重启实例（用于防循环）

        /**
         * @brief 将无符号整数格式化为字符串（信号处理函数安全版本）
         * @param[out] p   缓冲区指针（将被更新）
         * @param[in]  end 缓冲区结束指针
         * @param[in]  v   要格式化的数值
         * @note 此函数不进行动态内存分配，适合在信号处理函数中调用。
         */
        void AppendNum(char*& p, char* end, unsigned long v) {
            char tmp[24]{};
            int n = 0;
            do {
                tmp[n++] = static_cast<char>('0' + v % 10);
                v /= 10;
            } while (v != 0 && n < 24);
            while (n > 0 && p < end) *p++ = tmp[--n];
        }

        /**
         * @brief 信号处理函数（用于崩溃信号）
         * @param sig 信号编号
         * @param info 信号信息（未使用）
         * @param context 上下文（未使用）
         * @details
         * 此函数在信号上下文执行，必须保证 async-signal-safe。
         * 流程：写崩溃标记 -> fork/exec 自重启（除非已带 --after-crash）-> _exit。
         */
        void OnCrashSignal(int sig, siginfo_t*, void*) {
            if (g_inHandler.test_and_set(std::memory_order_acq_rel)) _exit(128 + sig);

            // 1) 写入崩溃标记文件（仅 Debug 构建; 见 g_markerEnabled 注释）
            if (g_markerEnabled) {
                int fd = open(g_markerPath.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
                if (fd >= 0) {
                    char buf[128]{};
                    char* p = buf;
                    char* end = buf + sizeof(buf);
                    static const char kSig[] = "crash signal=";
                    memcpy(p, kSig, sizeof(kSig) - 1);
                    p += sizeof(kSig) - 1;
                    AppendNum(p, end, static_cast<unsigned long>(sig));
                    static const char kPid[] = " pid=";
                    memcpy(p, kPid, sizeof(kPid) - 1);
                    p += sizeof(kPid) - 1;
                    AppendNum(p, end, static_cast<unsigned long>(getpid()));
                    if (p < end) *p++ = '\n';
                    ssize_t r = write(fd, buf, static_cast<size_t>(p - buf));
                    (void)r;
                    close(fd);
                }
            }

            // 2) 自重启（防循环）：使用预构建的 argv，零分配
            if (!g_afterCrash && !g_relaunchArgv.empty()) {
                pid_t pid = fork();
                if (pid == 0) {
                    execv(g_relaunchArgv[0], g_relaunchArgv.data());
                    _exit(127);
                }
                // 父进程不等待，崩溃路径应尽快终止
            }

            _exit(128 + sig);
        }

    } // namespace

    /**
     * @brief 获取默认转储目录
     * @return 默认目录路径
     * @details
     * 通过读取 /proc/self/exe 获取可执行文件路径，在其同目录下创建 "crashes" 子目录。
     */
    std::string GetSelfExePath() {
        char exe[4096]{};
        ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
        if (n <= 0) return "";
        exe[n] = '\0';
        return std::string(exe);
    }

    std::string GetDefaultDumpDir() {
        std::string exe = GetSelfExePath();
        if (exe.empty()) return OBF_STR("crashes");
        char* slash = strrchr(exe.data(), '/');
        if (!slash) return OBF_STR("crashes");
        *slash = '\0';
        return exe + OBF_STR("/crashes");
    }

    /**
     * @brief 安装进程级崩溃兜底
     * @param dumpDir 转储目录，若为空则使用默认目录
     * @details
     * 创建转储目录，读取原始命令行，构建重启参数列表，并注册信号处理函数。
     */
    void InstallCrashGuard(const std::string& dumpDir) {
        g_dumpDir = dumpDir.empty() ? GetDefaultDumpDir() : dumpDir;
        g_markerPath = g_dumpDir + "/last_crash.txt";
        // 与 Windows 同口径: 仅 Debug 构建创建 crashes 目录(Release 不留目录级 IOC)
        if (g_markerEnabled) {
            mkdir(g_dumpDir.c_str(), 0755); // 忽略已存在错误
        }

        // 读取原始命令行（正常上下文，可安全分配）
        std::string cmdline;
        int fd = open("/proc/self/cmdline", O_RDONLY);
        if (fd >= 0) {
            char tmp[4096]{};
            ssize_t n;
            while ((n = read(fd, tmp, sizeof(tmp))) > 0) {
                cmdline.append(tmp, static_cast<size_t>(n));
            }
            close(fd);
        }
        for (size_t i = 0; i < cmdline.size();) {
            g_argv.emplace_back(cmdline.c_str() + i);
            i += strlen(cmdline.c_str() + i) + 1;
        }

        // 预构建重启 argv（安装时分配）—— argv[0] 用 /proc/self/exe 绝对路径:
        // 相对路径/被删除路径重启会 exec 失败, 自重启形同虚设
        g_exePath = GetSelfExePath();
        if (!g_argv.empty()) {
            for (const auto& a : g_argv) {
                if (a == "--after-crash") g_afterCrash = true;
                g_relaunchArgv.push_back(const_cast<char*>(a.c_str()));
            }
            if (!g_exePath.empty()) {
                g_relaunchArgv[0] = const_cast<char*>(g_exePath.c_str());
            }
            if (!g_afterCrash) {
                g_relaunchArgv.push_back(const_cast<char*>("--after-crash"));
            }
            g_relaunchArgv.push_back(nullptr);
        }

        // sigaltstack: 信号处理在独立备用栈执行 —— 栈溢出(SIGSEGV/堆栈耗尽)时
        // 默认栈无空间执行 handler, 会二次崩溃; SA_ONSTACK 让 handler 用备用栈(L4)
        g_altStack.resize(64 * 1024);
        stack_t ss {};
        ss.ss_sp = g_altStack.data();
        ss.ss_size = g_altStack.size();
        ss.ss_flags = 0;
        bool have_alt_stack = (sigaltstack(&ss, nullptr) == 0);

        struct sigaction sa {};
        sa.sa_sigaction = OnCrashSignal;
        sigemptyset(&sa.sa_mask);
        sa.sa_flags = SA_SIGINFO | SA_RESETHAND | SA_NODEFER; // 防止重入
        if (have_alt_stack) sa.sa_flags |= SA_ONSTACK;
        for (int sig : kSignals) {
            sigaction(sig, &sa, nullptr);
        }
    }

    /**
     * @brief 检查并消费上次崩溃信息
     * @param dumpDir 转储目录，若为空则使用默认目录
     * @return 崩溃标记内容，若无则返回空字符串
     * @details
     * 读取并删除标记文件 last_crash.txt。
     */
    std::string ConsumeLastCrashInfo(const std::string& dumpDir) {
        const std::string path = (dumpDir.empty() ? GetDefaultDumpDir() : dumpDir) + "/last_crash.txt";
        FILE* f = fopen(path.c_str(), "r");
        if (!f) return {};
        std::string s;
        char buf[256];
        size_t n;
        while ((n = fread(buf, 1, sizeof(buf), f)) > 0) s.append(buf, n);
        fclose(f);
        remove(path.c_str());
        return s;
    }

    /**
     * @brief 在线程内执行带有崩溃兜底的函数（Linux 实现）
     * @param fn 要执行的可调用对象
     * @details
     * Linux 下由进程级信号处理函数兜底，无需额外包装，直接执行。
     */
    void RunWorkerGuarded(const std::function<void()>& fn) {
        fn();
    }

} // namespace cleat::Platform