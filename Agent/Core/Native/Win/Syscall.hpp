#pragma once
// Syscall.hpp — L1 syscall 层公开接口(注入与迁移设计, 一期)。
//
// 设计要点:
//   - 无 .asm 文件: 间接 stub 以运行时构 RX thunk 实现(数据形态的机器码,
//     `mov r10,rcx; mov eax,SSN; jmp [rip+0]→ntdll!syscall;ret`), syscall 后
//     返回地址落在真实 ntdll —— 免疫"unbacked syscall"调用栈回溯。
//   - SSN 来源 = KnownDlls 式干净副本(LoadLibraryExW AS_IMAGE|AS_DATA 映射的
//     磁盘新本), 按名解析 —— 不依赖排序推断, 免疫 Win11 未排序 stub 变体与
//     ntdll inline hook; gadget 取真实 ntdll 对应 Zw 函数内的 `0F 05 C3`。
//   - Linux: Windows syscall 层天然平台特定, 头文件整体 _WIN32 守卫。

#include "Core/Native/Win/NtDefs.hpp"

#include <string>

#ifdef _WIN32

namespace cleat {
    namespace native {

        // v1 函数集
        struct NtApi {
            // 内存
            NTSTATUS(NTAPI* NtAllocateVirtualMemory)(void* ProcessHandle, void** BaseAddress,
                std::uintptr_t ZeroBits, std::size_t* RegionSize, unsigned long AllocationType,
                unsigned long Protect);
            NTSTATUS(NTAPI* NtProtectVirtualMemory)(void* ProcessHandle, void** BaseAddress,
                std::size_t* RegionSize, unsigned long NewProtect, unsigned long* OldProtect);
            NTSTATUS(NTAPI* NtWriteVirtualMemory)(void* ProcessHandle, void* BaseAddress,
                void* Buffer, std::size_t NumberOfBytesToWrite, std::size_t* NumberOfBytesWritten);
            // 进程/线程创建
            NTSTATUS(NTAPI* NtCreateUserProcess)(void** ProcessHandle, void** ThreadHandle,
                unsigned long DesiredProcessAccess, unsigned long DesiredThreadAccess,
                OBJECT_ATTRIBUTES_NT* ProcessObjectAttributes,
                OBJECT_ATTRIBUTES_NT* ThreadObjectAttributes, unsigned long ProcessFlags,
                unsigned long ThreadFlags, void* ProcessParameters /*RTL_USER_PROCESS_PARAMETERS*/,
                PS_CREATE_INFO* CreateInfo, PS_ATTRIBUTE_LIST* AttributeList);
            NTSTATUS(NTAPI* NtCreateThreadEx)(void** ThreadHandle, unsigned long DesiredAccess,
                OBJECT_ATTRIBUTES_NT* ObjectAttributes, void* ProcessHandle, void* StartRoutine,
                void* Argument, unsigned long CreateFlags, std::size_t ZeroBits,
                std::size_t StackSize, std::size_t MaximumStackSize, PS_ATTRIBUTE_LIST* AttributeList);
            // 句柄获取
            NTSTATUS(NTAPI* NtOpenProcess)(void** ProcessHandle, unsigned long DesiredAccess,
                OBJECT_ATTRIBUTES_NT* ObjectAttributes, CLIENT_ID_NT* ClientId);
            NTSTATUS(NTAPI* NtOpenThread)(void** ThreadHandle, unsigned long DesiredAccess,
                OBJECT_ATTRIBUTES_NT* ObjectAttributes, CLIENT_ID_NT* ClientId);
            // APC / 恢复
            NTSTATUS(NTAPI* NtQueueApcThread)(void* ThreadHandle, void* ApcRoutine,
                void* ApcArgument1, void* ApcArgument2, void* ApcArgument3);
            NTSTATUS(NTAPI* NtQueueApcThreadEx)(void* ThreadHandle, void* ReserveHandle,
                void* ApcRoutine, void* ApcArgument1, void* ApcArgument2, void* ApcArgument3);
            NTSTATUS(NTAPI* NtResumeThread)(void* ThreadHandle, unsigned long* PreviousSuspendCount);
            //  自检用: 无副作用的延时 syscall(替代 NtClose 无效句柄 —— 后者在
            // 被调试进程会触发内核 user exception, 详见 PlatformWin_Syscall.cpp SelfTest)
            NTSTATUS(NTAPI* NtDelayExecution)(unsigned char Alertable, void* DelayInterval);
            // 查询/生命周期
            NTSTATUS(NTAPI* NtQueryInformationProcess)(void* ProcessHandle,
                unsigned long ProcessInformationClass, void* ProcessInformation,
                unsigned long ProcessInformationLength, unsigned long* ReturnLength);
            NTSTATUS(NTAPI* NtQueryInformationThread)(void* ThreadHandle,
                unsigned long ThreadInformationClass, void* ThreadInformation,
                unsigned long ThreadInformationLength, unsigned long* ReturnLength);
            NTSTATUS(NTAPI* NtTerminateProcess)(void* ProcessHandle, NTSTATUS ExitStatus);
            NTSTATUS(NTAPI* NtClose)(void* Handle);

            bool ready = false;   // Init() 成功后为 true
        };

        /// 幂等初始化: 解析 SSN 表 + 构建 thunk + 填充 Api()。失败返回 false(fail-closed)。
        bool Init();

        /// 初始化后的函数表引用(Init() 未成功前调用 = 未定义行为, 先判 ready)
        const NtApi& Api();

        /// 诊断: 按名取 SSN(Init 后有效); 供自测与日志
        bool GetSsn(const char* func, unsigned long& ssn);

        /// 自测(仅诊断用): SSN 与真实 ntdll Zw 查询对齐 + 间接调用 NtClose(无效句柄)
        /// 期望 STATUS_INVALID_HANDLE。返回 false = 层不可用。
        bool SelfTest();

        /// UTF-8 → wide 转换(平台实现导出, 供 Injector 等非 Platform 调用方使用)
        bool Utf8ToWideNT(const std::string& utf8, std::wstring& out);

    } // namespace native
} // namespace cleat

#endif // _WIN32
