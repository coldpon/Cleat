/**
 * @file PlatformWin_InjectCore.hpp
 * @brief 注入手法共享原语: 进程附着 / 载荷落地 / APC 触发 / spawn 牺牲进程注入
 *        (平台文件, 规范 1)
 * @details
 * 触发方式(远程线程 vs APC)是各手法自己的事, 住在各自文件夹; 本层提供公共动作:
 * "附着到目标 + 把载荷写成可执行内存 + spawn 牺牲进程并 early-bird APC 注入"。
 * 全部要求 native::Init() 已完成(各手法入口先行调用), 失败一律 fail-closed
 * 返回 InjectionResult(error/failed_at 可读)。
 * (: SpawnAndInject 自 Core/Native/Win/Injector 整合入本层 ——
 *  它本来就是 apc_injection/reflective_load 的 spawn 分支实现, 原先住在
 *  Native 造成两套 L2 并存; MigrateSpawn 同步降格为注册表调用的特例。)
 */
#pragma once

#include "Core/Inject/InjectionTypes.hpp"

#include <string>
#include <vector>

#ifdef _WIN32

namespace cleat {
namespace injection {

    /// pid 附着: NtOpenProcess(CREATE_THREAD|VM_OPERATION|VM_WRITE|QUERY)
    InjectionResult AttachToPid(unsigned long pid, void** hProcess);

    /// 载荷落地: 远端 RW 分配 → 写入 → VirtualProtectEx 翻 RX(迁移链同款,
    /// NtProtectVirtualMemory 的 thunk 在远端句柄上不可靠)
    InjectionResult WriteExecutableMemory(void* hProcess,
        const std::vector<uint8_t>& payload, void** out_base);

    /// APC 触发(pid 附着系): 挂起远线程(start = 载荷入口, APC 交付异常时兜底)
    /// → NtQueueApcThread(载荷入口) → NtResumeThread
    InjectionResult TriggerApcNewThread(void* hProcess, void* base);

    /// spawn 牺牲进程注入: CreateProcessW(CREATE_SUSPENDED, ApiResolver 解析) →
    /// RW 写入 → RX 翻转 → NtQueueApcThread(挂起主线程, early-bird) → Resume。
    /// 失败且未 keep_on_fail → TerminateProcess 清尸(OwnsTarget; Debug 保留现场)。
    /// blob 满足 Blob 契约(第 0 字节 = 入口) —— shellcode 原样, 裸 DLL 由
    /// ReflectiveLoad 先经 RemoteMapper 打包。
    InjectionResult SpawnAndInject(const std::wstring& target_image,
        const std::vector<uint8_t>& blob, bool keep_on_fail);

} // namespace injection
} // namespace cleat

#endif // _WIN32
