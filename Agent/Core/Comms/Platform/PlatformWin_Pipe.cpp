#include "Core/Comms/Platform/PlatformWin_Pipe.hpp"

#include <windows.h>

namespace cleat {

    std::string WinPipeOps::FullPipePath(const std::string& host, const std::string& name) {
        if (host.empty() || host == "." || host == "localhost") {
            return "\\\\.\\pipe\\" + name;
        }
        return "\\\\" + host + "\\pipe\\" + name;
    }

    IPipeOps::PipeHandle WinPipeOps::CreateServer(const std::string& name) {
        // 字节模式 + 同步句柄; 双向; 1MB 收发缓冲(帧上限 4MB 由上层分块写覆盖)
        // ACL: 默认(DACL 继承自创建者令牌, 默认仅本用户/管理员可连) ——
        // 跨用户链路由上层认证(P1' L3 配对密钥)兜底, 不在此开放 Everyone。
        HANDLE h = CreateNamedPipeA(FullPipePath("", name).c_str(),
            PIPE_ACCESS_DUPLEX,
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
            1,                      // 单实例: 一条管道一个对端(隐含路由, 见设计)
            1024 * 1024,            // out
            1024 * 1024,            // in
            0, nullptr);
        return (h == INVALID_HANDLE_VALUE) ? nullptr : static_cast<PipeHandle>(h);
    }

    bool WinPipeOps::WaitForClient(PipeHandle handle) {
        if (!handle) return false;
        // 阻塞至客户端连入; Close/Disconnect 该实例后本调用失败返回(供监听线程退出)
        BOOL ok = ConnectNamedPipe(static_cast<HANDLE>(handle), nullptr);
        if (ok) return true;
        // 客户端在 CreateNamedPipe 与 ConnectNamedPipe 之间连入的竞态: 视为成功
        return GetLastError() == ERROR_PIPE_CONNECTED;
    }

    IPipeOps::PipeHandle WinPipeOps::ConnectClient(const std::string& host,
        const std::string& name, int timeout_ms) {
        const std::string path = FullPipePath(host, name);
        const DWORD deadline = GetTickCount() + static_cast<DWORD>(timeout_ms < 0 ? 0 : timeout_ms);
        while (true) {
            HANDLE h = CreateFileA(path.c_str(), GENERIC_READ | GENERIC_WRITE,
                0, nullptr, OPEN_EXISTING, 0, nullptr);
            if (h != INVALID_HANDLE_VALUE) {
                // 客户端按服务端模式置字节读模式(两端一致)
                DWORD mode = PIPE_READMODE_BYTE;
                SetNamedPipeHandleState(h, &mode, nullptr, nullptr);
                return static_cast<PipeHandle>(h);
            }
            const DWORD err = GetLastError();
            if (err == ERROR_PIPE_BUSY) {
                WaitNamedPipeA(path.c_str(), 500);
            }
            else {
                Sleep(100);
            }
            if (GetTickCount() >= deadline) return nullptr;
        }
    }

    int WinPipeOps::AvailableBytes(PipeHandle handle) {
        if (!handle) return -1;
        DWORD avail = 0;
        if (!PeekNamedPipe(static_cast<HANDLE>(handle), nullptr, 0, nullptr, &avail, nullptr)) {
            return -1;   // 对端断开(ERROR_BROKEN_PIPE)或句柄无效
        }
        return static_cast<int>(avail);
    }

    int WinPipeOps::Read(PipeHandle handle, void* buf, size_t len) {
        if (!handle || !buf || len == 0) return 0;
        // 先探测可读量, 限制本次读取长度 —— 保证同步 ReadFile 不无限阻塞
        int avail = AvailableBytes(handle);
        if (avail <= 0) return avail;              // -1 断开 / 0 暂无
        DWORD want = static_cast<DWORD>(len);
        if (want > static_cast<DWORD>(avail)) want = static_cast<DWORD>(avail);
        DWORD got = 0;
        if (!ReadFile(static_cast<HANDLE>(handle), buf, want, &got, nullptr)) {
            return -1;
        }
        return static_cast<int>(got);
    }

    int WinPipeOps::Write(PipeHandle handle, const void* buf, size_t len) {
        if (!handle || !buf || len == 0) return 0;
        DWORD wrote = 0;
        if (!WriteFile(static_cast<HANDLE>(handle), buf, static_cast<DWORD>(len),
            &wrote, nullptr)) {
            return -1;
        }
        return static_cast<int>(wrote);
    }

    void WinPipeOps::Disconnect(PipeHandle handle) {
        if (!handle) return;
        FlushFileBuffers(static_cast<HANDLE>(handle));
        DisconnectNamedPipe(static_cast<HANDLE>(handle));
    }

    void WinPipeOps::Close(PipeHandle handle) {
        if (!handle) return;
        // 先 CancelIoEx 再 CloseHandle: 同步(非 OVERLAPPED)ConnectNamedPipe 阻塞
        // 在另一线程时, 仅 CloseHandle 不保证唤醒(19041.7725 实测: 监听线程
        // 永挂于 NtFsControlFile, join 跟着永挂 ——  管道用例复现);
        // CancelIoEx(句柄, NULL) 是文档支持的跨线程取消同步 I/O 手法, 唤醒后
        // ConnectNamedPipe 返回失败, 监听线程按既有失败路径退出。
        CancelIoEx(static_cast<HANDLE>(handle), nullptr);
        CloseHandle(static_cast<HANDLE>(handle));
    }

    std::unique_ptr<IPipeOps> CreatePipeOps() {
        return std::make_unique<WinPipeOps>();
    }

} // namespace cleat
