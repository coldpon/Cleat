/**
 * @file PlatformWin_CrashGuard.cpp
 * @brief Windows 崩溃兜底实现
 * @details
 * 使用 SEH（SetUnhandledExceptionFilter + __try/__except）捕获异常，
 * 配合 MiniDumpWriteDump 生成 minidump，并写入崩溃标记，最后自重启。
 * 关键不变量：所有堆分配只在 InstallCrashGuard（正常上下文）完成；
 * 崩溃路径（CrashAndRestart）零分配 —— 仅使用栈缓冲与内核 API，
 * 因此即使堆已损坏，依然可以完成转储并重启。
 */
#include "CrashGuard.hpp"
#include "Common/Obfuscation/StringObf.hpp"

#include <windows.h>
#include <dbghelp.h>

#include <atomic>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <string>

namespace cleat::Platform {

    namespace {

        std::wstring g_origCmdline;     ///< 原始命令行（安装时复制）
        std::wstring g_relaunchCmdline; ///< 重启命令行（原始命令 + " --after-crash"）
        std::wstring g_dumpDirW;        ///< 转储目录（宽字符）
        bool g_afterCrash = false;      ///< 当前实例是否已带 --after-crash（防循环）
        bool g_installed = false;       ///< 是否已安装

        /**
         * @brief 将 UTF-8 字符串转换为宽字符
         * @param s UTF-8 输入字符串
         * @return 宽字符字符串
         */
        std::wstring Utf8ToWide(const std::string& s) {
            if (s.empty()) return {};
            int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
            std::wstring w(static_cast<size_t>(n), L'\0');
            MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), &w[0], n);
            return w;
        }

        /**
         * @brief 将宽字符字符串转换为 UTF-8
         * @param w 宽字符输入
         * @return UTF-8 字符串
         */
        std::string WideToUtf8(const std::wstring& w) {
            if (w.empty()) return {};
            int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
            std::string s(static_cast<size_t>(n), '\0');
            WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), &s[0], n, nullptr, nullptr);
            return s;
        }

        /// 崩溃落盘开关（实测  改为**构建期**决定 —— Debug 构建恒开
        /// （minidump + 崩溃标记, 排障是 Debug 的意义）; Release 构建恒关（零取证痕迹,
        /// 自重启兜底不受影响）。原 runtime.crash_dump_enabled 运行期键无生效路径
        /// （InstallCrashGuard 先于 EmbeddedConfigApply, 且该键仅安装时读一次）,
        /// 已随 ConfigContract 一并移除。
        std::atomic<bool> g_dumpEnabled{
#ifdef NDEBUG
            false
#else
            true
#endif
        };

        /**
         * @brief 崩溃处理核心（永不返回）
         * @param ep 异常指针
         * @return 仅用于 SEH 过滤器语法，实际不会返回
         * @details
         * 调用上下文为 UnhandledExceptionFilter 或 __except 过滤器，栈可能已损坏，
         * 故不得使用任何可能分配内存的代码（C++ 字符串、异常等）。
         * 流程：生成 minidump（如果启用）-> 写崩溃标记 -> CreateProcess 自重启 -> TerminateProcess。
         */
        int CrashAndRestart(EXCEPTION_POINTERS* ep) {
            // 第一个线程走完"落盘+重启"流程, 其余直接终止, 防止同 machine_id
            // 多实例风暴(服务端有防惊群, 客户端侧同样需要)。std::atomic_flag 为
            // lock-free, 崩溃路径(栈可能损坏)下使用安全, 不分配内存。
            static std::atomic_flag g_restarting = ATOMIC_FLAG_INIT;
            if (g_restarting.test_and_set()) {
                TerminateProcess(GetCurrentProcess(), 0xDEADBEEF);
                return EXCEPTION_EXECUTE_HANDLER;
            }

            SYSTEMTIME st;
            GetLocalTime(&st);

            // 1) 生成 minidump（仅当配置启用时落盘）
            if (g_dumpEnabled.load()) {
                wchar_t dumpName[MAX_PATH] = {};
                // AD6(修复 ): 必须**先量长再格式化** —— swprintf_s 在发生截断时会
                // 走 invalid parameter handler(默认 __fastfail 终止进程); 实测本工具链
                // 下 _TRUNCATE 同样终止(退出码 0xC0000409), 深目录会让崩溃处理死在这一步,
                // 连后面的自重启都走不到。放不下就只跳过落盘。
                // 上界 = 目录长 + "\crash_"(7) + 时间(15) + "_"(1) + pid(10) + ".dmp"(4) + NUL(1)
                const bool dumpOk = g_dumpDirW.size() + 38 <= MAX_PATH;
                if (dumpOk) {
                    swprintf_s(dumpName, L"%s\\crash_%04d%02d%02d_%02d%02d%02d_%lu.dmp",
                        g_dumpDirW.c_str(), st.wYear, st.wMonth, st.wDay,
                        st.wHour, st.wMinute, st.wSecond, GetCurrentProcessId());
                }
                HANDLE hFile = INVALID_HANDLE_VALUE;
                if (dumpOk) {
                    hFile = CreateFileW(dumpName, GENERIC_WRITE, 0, nullptr,
                        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
                }
                if (hFile != INVALID_HANDLE_VALUE) {
                    MINIDUMP_EXCEPTION_INFORMATION mei{};
                    mei.ThreadId = GetCurrentThreadId();
                    mei.ExceptionPointers = ep;
                    mei.ClientPointers = FALSE;
                    MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), hFile,
                        MiniDumpNormal, ep ? &mei : nullptr, nullptr, nullptr);
                    CloseHandle(hFile);
                }

                // 2) 写入崩溃标记文件（供下次启动时显示）
                wchar_t marker[MAX_PATH] = {};
                // AD6: 同上 —— "\last_crash.txt" 定长 15 + NUL, 放不下就跳过落盘
                // (标记文件是诊断辅助, 不得因此中断崩溃处理)。
                const bool markerOk = g_dumpDirW.size() + 16 <= MAX_PATH;
                if (markerOk) {
                    swprintf_s(marker, L"%s\\last_crash.txt", g_dumpDirW.c_str());
                }
                hFile = INVALID_HANDLE_VALUE;
                if (markerOk) {
                    hFile = CreateFileW(marker, GENERIC_WRITE, 0, nullptr,
                        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
                }
                if (hFile != INVALID_HANDLE_VALUE) {
                    char buf[256];
                    int n = sprintf_s(buf,
                        "crash code=0x%08lX addr=%p time=%04d%02d%02d_%02d%02d%02d pid=%lu\n",
                        ep ? ep->ExceptionRecord->ExceptionCode : 0,
                        ep ? ep->ExceptionRecord->ExceptionAddress : nullptr,
                        st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond,
                        GetCurrentProcessId());
                    DWORD written = 0;
                    // AD6: n < 0 只来自格式化失败; 此时绝不能把 DWORD(-1) 当长度传给
                    // WriteFile(会按 4GB 从 256B 栈缓冲读)。当前格式串定长上界约 80B,
                    // 属兜底。
                    if (n > 0) {
                        WriteFile(hFile, buf, static_cast<DWORD>(n), &written, nullptr);
                    }
                    CloseHandle(hFile);
                }
            }   // if (g_dumpEnabled)

            // 3) 自重启（防循环：若当前已带 --after-crash 则不重启）
            if (!g_relaunchCmdline.empty()) {
                STARTUPINFOW si{ sizeof(si) };
                PROCESS_INFORMATION pi{};
                // CREATE_NEW_PROCESS_GROUP 避免共享 Ctrl+C 处理
                if (CreateProcessW(nullptr, &g_relaunchCmdline[0], nullptr, nullptr, FALSE,
                    CREATE_NEW_PROCESS_GROUP, nullptr, nullptr, &si, &pi)) {
                    CloseHandle(pi.hThread);
                    CloseHandle(pi.hProcess);
                }
            }

            TerminateProcess(GetCurrentProcess(), 0xDEADBEEF);
            return EXCEPTION_EXECUTE_HANDLER; // 不可达
        }

        /**
         * @brief 未处理异常过滤器
         * @param ep 异常指针
         * @return 实际不会返回
         */
        LONG WINAPI OnUnhandledException(EXCEPTION_POINTERS* ep) {
            return CrashAndRestart(ep);
        }

        // ----  fast-fail 族兜底 ----
        // UEF 收不到 fast-fail: __fastfail(int 0x29, 0xC0000409)绕过全部异常分发
        // (VEH/SEH/UEF), 而 abort()(std::terminate 的落点)、invalid-parameter、
        // /GS 栈溢出、purecall 都走它 —— 这些恰是堆损坏/栈打穿的主要呈现形态,
        // 原先这类死亡无标记无自重启。把 CRT 侧四条"终止前钩子"全部路由进
        // CrashAndRestart(nullptr)(其 ep==nullptr 路径本已安全: 标记 code=0,
        // dump 不带异常上下文)。钩子在正常上下文安装, 崩溃路径零分配不变。
        void FastFailRestart(const wchar_t*, const wchar_t*, const wchar_t*,
            unsigned int, uintptr_t) {
            CrashAndRestart(nullptr);
        }
        void FastFailRestartV() { CrashAndRestart(nullptr); }
        void FastFailRestartS(int) { CrashAndRestart(nullptr); }

    } // namespace

    /**
     * @brief 获取默认转储目录
     * @return 默认目录路径
     * @details
     * 可执行文件所在目录下的 "crashes" 子目录。
     */
    std::string GetDefaultDumpDir() {
        wchar_t exe[MAX_PATH];
        DWORD n = GetModuleFileNameW(nullptr, exe, MAX_PATH);
        if (n == 0 || n >= MAX_PATH) return OBF_STR("crashes");
        wchar_t* slash = wcsrchr(exe, L'\\');
        if (slash) {
            *slash = L'\0';
        }
        else {
            wcscpy_s(exe, L".");
        }
        return WideToUtf8(std::wstring(exe) + OBF_WSTR(L"\\crashes"));
    }

    /**
     * @brief 安装进程级崩溃兜底
     * @param dumpDir 转储目录，若为空则使用默认目录
     * @details
     * 创建转储目录（仅 Debug 构建落盘开），预构建重启命令行（绝对路径），并设置
     * UnhandledExceptionFilter 与 fast-fail 族钩子（invalid-parameter/purecall/
     * SIGABRT/terminate）。
     * 若编译为 DLL 形态（_WINDLL 定义），则跳过安装（避免污染宿主进程）。
     */
    void InstallCrashGuard(const std::string& dumpDir) {
#ifdef _WINDLL
        // DLL 形态：自重启语义错误（GetCommandLineW/CreateProcessW 会重启宿主进程如 rundll32），
        // 且 SetUnhandledExceptionFilter 会污染宿主异常处理 —— 跳过安装
        return;
#else
        if (g_installed) return;

        g_dumpDirW = Utf8ToWide(dumpDir.empty() ? GetDefaultDumpDir() : dumpDir);
        // 崩溃落盘 = 构建期决定（见 g_dumpEnabled 注释）; Debug 构建需要目录
        if (g_dumpEnabled.load()) {
            CreateDirectoryW(g_dumpDirW.c_str(), nullptr); // 忽略已存在错误
        }

        // 预构建重启命令行（安装时分配，崩溃路径零分配、无截断风险）
        g_origCmdline = GetCommandLineW();
        //  原始命令行首 token 可能是相对路径/PATH 依赖形态,
        // 崩溃时 cwd 已变则 CreateProcessW 解析失败 → 自重启静默失效(Linux 侧已用
        // /proc/self/exe, 本侧对齐)。把首 token(引号感知)替换为当前映像绝对路径。
        {
            wchar_t exePath[MAX_PATH]{};
            const DWORD exeLen = GetModuleFileNameW(nullptr, exePath, MAX_PATH);
            if (exeLen > 0 && exeLen < MAX_PATH) {
                size_t i = 0;
                bool inQuote = false;
                while (i < g_origCmdline.size()) {
                    const wchar_t ch = g_origCmdline[i];
                    if (ch == L'"') inQuote = !inQuote;
                    else if (ch == L' ' && !inQuote) break;
                    ++i;
                }
                g_origCmdline = L"\"" + std::wstring(exePath) + L"\""
                    + (i < g_origCmdline.size() ? g_origCmdline.substr(i) : L"");
            }
        }
        // C5 修复: 精确匹配独立参数 "--after-crash" —— 子串匹配会
        // 误命中 "--after-crash-report-url" 等含该前缀的参数(防循环判定错误)。
        g_afterCrash = false;
        {
            // 按空格分割命令行, 逐 token 精确比较(支持引号包裹的路径)
            std::wstring tok;
            bool inQuote = false;
            for (wchar_t ch : g_origCmdline) {
                if (ch == L'"') { inQuote = !inQuote; continue; }
                if (ch == L' ' && !inQuote) {
                    if (tok == L"--after-crash") { g_afterCrash = true; break; }
                    tok.clear();
                }
                else {
                    tok += ch;
                }
            }
            if (!g_afterCrash && tok == L"--after-crash") {
                g_afterCrash = true;   // 最后一个 token
            }
        }
        if (!g_afterCrash) {
            g_relaunchCmdline = g_origCmdline + L" --after-crash";
        }

        //  fast-fail 族钩子(见 FastFailRestart 注释) —— 与 UEF 同批安装
        _set_invalid_parameter_handler(FastFailRestart);
        _set_purecall_handler(FastFailRestartV);
        // abort 默认直走 __fastfail(_CALL_REPORTFAULT)绕过一切钩子 —— 关掉后
        // abort 经 SIGABRT 到达本处理器; 不再让 WER 弹窗/挂起阻断自重启。
        _set_abort_behavior(0, _CALL_REPORTFAULT);
        signal(SIGABRT, FastFailRestartS);
        std::set_terminate(FastFailRestartV);

        SetUnhandledExceptionFilter(OnUnhandledException);
        g_installed = true;
#endif // _WINDLL
    }

    /**
     * @brief 检查并消费上次崩溃信息
     * @param dumpDir 转储目录，若为空则使用默认目录
     * @return 崩溃标记内容，若无则返回空字符串
     * @details
     * 读取并删除 last_crash.txt 文件。
     */
    std::string ConsumeLastCrashInfo(const std::string& dumpDir) {
        const std::wstring dir = Utf8ToWide(dumpDir.empty() ? GetDefaultDumpDir() : dumpDir);
        const std::wstring path = dir + L"\\last_crash.txt";
        HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) return {};
        std::string s;
        char buf[512]{};
        DWORD read = 0;
        while (ReadFile(h, buf, sizeof(buf), &read, nullptr) && read > 0) {
            s.append(buf, read);
        }
        CloseHandle(h);
        DeleteFileW(path.c_str());
        return s;
    }

    /**
     * @brief 在线程内执行带有崩溃兜底的函数（Windows 实现）
     * @param fn 要执行的可调用对象
     * @details
     * 使用 __try/__except 包裹 fn，并预留 32KB 线程栈保证，以应对栈溢出异常。
     * 崩溃时调用 CrashAndRestart，该函数不会返回。
     */
    void RunWorkerGuarded(const std::function<void()>& fn) {
        // 为栈溢出异常保留备用栈空间
        ULONG stackReserve = 32 * 1024;
        SetThreadStackGuarantee(&stackReserve);
        __try {
            fn();
        }
        __except (CrashAndRestart(GetExceptionInformation())) {
            // 不可达：CrashAndRestart 内部 TerminateProcess
        }
    }

} // namespace cleat::Platform