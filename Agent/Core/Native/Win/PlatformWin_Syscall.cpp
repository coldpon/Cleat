#include "Syscall.hpp"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "Common/Logger/ErrLog.hpp"
#include "Common/Logger/Logger.hpp"

#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace cleat {
    namespace native {

        namespace {

            // ---- v1 函数名表(顺序 = NtApi 成员槽位) ----
            const char* kFuncNames[] = {
                "NtAllocateVirtualMemory", "NtProtectVirtualMemory", "NtWriteVirtualMemory",
                "NtCreateUserProcess", "NtCreateThreadEx", "NtOpenProcess", "NtOpenThread",
                "NtQueueApcThread", "NtQueueApcThreadEx", "NtResumeThread",
                "NtQueryInformationProcess", "NtQueryInformationThread",
                "NtTerminateProcess", "NtClose", "NtDelayExecution",
            };
            constexpr int kFuncCount = sizeof(kFuncNames) / sizeof(kFuncNames[0]);

            struct SsnEntry {
                std::string name;
                unsigned long ssn = 0;
            };

            std::mutex g_mu;
            bool g_ready = false;
            NtApi g_api{};
            std::vector<SsnEntry> g_ssns;

            // ---- PE 导出按名解析(对任意映射基址通用) ----
            bool FindExportRva(uint8_t* base, const char* func, uint32_t& rva) {
                const auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
                if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
                const auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
                if (nt->Signature != IMAGE_NT_SIGNATURE) return false;
                const auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
                if (!dir.VirtualAddress) return false;
                const auto* exp = reinterpret_cast<IMAGE_EXPORT_DIRECTORY*>(base + dir.VirtualAddress);
                const auto* names = reinterpret_cast<uint32_t*>(base + exp->AddressOfNames);
                const auto* ords = reinterpret_cast<uint16_t*>(base + exp->AddressOfNameOrdinals);
                const auto* funcs = reinterpret_cast<uint32_t*>(base + exp->AddressOfFunctions);
                for (unsigned long i = 0; i < exp->NumberOfNames; ++i) {
                    const char* n = reinterpret_cast<const char*>(base + names[i]);
                    if (std::strcmp(n, func) == 0) {
                        rva = funcs[ords[i]];
                        return true;
                    }
                }
                return false;
            }

            // ---- stub 内解析 SSN: 定位 mov eax, imm32 且其后短距内有 syscall(0F 05) ----
            bool ParseSsnFromStub(const uint8_t* stub, size_t scan, unsigned long& ssn) {
                for (size_t p = 0; p + 6 <= scan; ++p) {
                    if (stub[p] == 0xB8) {
                        for (size_t q = p + 5; q < p + 16 && q + 1 < scan; ++q) {
                            if (stub[q] == 0x0F && stub[q + 1] == 0x05) {
                                ssn = static_cast<unsigned long>(stub[p + 1])
                                    | (static_cast<unsigned long>(stub[p + 2]) << 8)
                                    | (static_cast<unsigned long>(stub[p + 3]) << 16)
                                    | (static_cast<unsigned long>(stub[p + 4]) << 24);
                                return true;
                            }
                        }
                    }
                }
                return false;
            }

            // ---- 真实 ntdll stub 内定位 syscall gadget(0F 05) ----
            bool FindGadget(const uint8_t* stub, size_t scan, uintptr_t& gadget) {
                for (size_t p = 0; p + 1 < scan; ++p) {
                    if (stub[p] == 0x0F && stub[p + 1] == 0x05) {
                        gadget = reinterpret_cast<uintptr_t>(stub) + p;
                        return true;
                    }
                }
                return false;
            }

        } // namespace

        bool Utf8ToWideNT(const std::string& utf8, std::wstring& out) {
            if (utf8.empty()) {
                out.clear();
                return true;
            }
            int len = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(),
                static_cast<int>(utf8.size()), nullptr, 0);
            if (len <= 0) return false;
            out.resize(len);
            MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(),
                static_cast<int>(utf8.size()), &out[0], len);
            return true;
        }

        bool Init() {
            std::lock_guard<std::mutex> lock(g_mu);
            if (g_ready) return true;

            // ---- 1. 干净副本: 以数据+映像双重身份映射磁盘 ntdll(不经注册表/模块表) ----
            HMODULE clean = LoadLibraryExW(L"ntdll.dll", nullptr,
                DONT_RESOLVE_DLL_REFERENCES | LOAD_LIBRARY_AS_IMAGE_RESOURCE
                | LOAD_LIBRARY_AS_DATAFILE);
            if (!clean) {
                CLEAT_LOGE(ErrDom::Nat, ErrCode::SyscallResolveFailed, "Syscall: 干净 ntdll 映射失败 err=" << GetLastError());
                return false;
            }

            // ---- 2. 真实 ntdll(gadget 来源) ----
            HMODULE real = GetModuleHandleW(L"ntdll.dll");
            if (!real) {
                CLEAT_LOGE(ErrDom::Nat, ErrCode::SyscallResolveFailed, "Syscall: 真实 ntdll 句柄获取失败");
                FreeLibrary(clean);
                return false;
            }

            NtApi api{};   // 全零
            std::vector<SsnEntry> ssns;
            void* thunkPage = nullptr;

            for (int i = 0; i < kFuncCount; ++i) {
                const char* name = kFuncNames[i];

                uint32_t cleanRva = 0;
                unsigned long ssn = 0;
                if (!FindExportRva(reinterpret_cast<uint8_t*>(clean), name, cleanRva)) {
                    CLEAT_LOGE(ErrDom::Nat, ErrCode::SyscallResolveFailed, "Syscall: 干净副本未导出 " << name);
                    VirtualFree(thunkPage, 0, MEM_RELEASE);
                    FreeLibrary(clean);
                    return false;
                }
                const uint8_t* cleanStub = reinterpret_cast<const uint8_t*>(clean) + cleanRva;
                if (!ParseSsnFromStub(cleanStub, 32, ssn)) {
                    CLEAT_LOGE(ErrDom::Nat, ErrCode::SyscallResolveFailed, "Syscall: SSN 解析失败 " << name << "(stub 模式不识别)");
                    VirtualFree(thunkPage, 0, MEM_RELEASE);
                    FreeLibrary(clean);
                    return false;
                }

                uint32_t realRva = 0;
                if (!FindExportRva(reinterpret_cast<uint8_t*>(real), name, realRva)) {
                    CLEAT_LOGE(ErrDom::Nat, ErrCode::SyscallResolveFailed, "Syscall: 真实 ntdll 未导出 " << name);
                    VirtualFree(thunkPage, 0, MEM_RELEASE);
                    FreeLibrary(clean);
                    return false;
                }
                const uint8_t* realStub = reinterpret_cast<const uint8_t*>(real) + realRva;
                uintptr_t gadget = 0;
                if (!FindGadget(realStub, 32, gadget)) {
                    CLEAT_LOGE(ErrDom::Nat, ErrCode::SyscallResolveFailed, "Syscall: syscall gadget 未定位 " << name
                        << "(stub 可能被整段替换)");
                    VirtualFree(thunkPage, 0, MEM_RELEASE);
                    FreeLibrary(clean);
                    return false;
                }

                ssns.push_back({ name, ssn });

                // ---- thunk 槽位(每函数 32B): mov r10,rcx; mov eax,SSN; jmp [rip+0]; gadget ----
                // 首个函数触发整页分配(RW 写入 → RX 锁定, 内存属性纪律从本层做起)
                if (!thunkPage) {
                    thunkPage = VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE,
                        PAGE_READWRITE);
                    if (!thunkPage) {
                        CLEAT_LOGE(ErrDom::Nat, ErrCode::PlatformApiFailed, "Syscall: thunk 页分配失败 err=" << GetLastError());
                        FreeLibrary(clean);
                        return false;
                    }
                }
                uint8_t* slot = static_cast<uint8_t*>(thunkPage) + i * 32;
                slot[0] = 0x4C; slot[1] = 0x8B; slot[2] = 0xD1;          // mov r10, rcx
                slot[3] = 0xB8;                                          // mov eax, ssn
                std::memcpy(slot + 4, &ssn, 4);
                slot[8] = 0xFF; slot[9] = 0x25;                          // jmp [rip+0]
                const uint32_t disp = 0;                                  // 目标紧随其后
                std::memcpy(slot + 10, &disp, 4);
                std::memcpy(slot + 14, &gadget, 8);                       // ntdll syscall gadget

                // 槽位 → NtApi 成员(类型化指针, 与 kFuncNames 顺序一一对应)
                void* thunk = slot;
                switch (i) {
                case 0: api.NtAllocateVirtualMemory = reinterpret_cast<
                    decltype(api.NtAllocateVirtualMemory)>(thunk); break;
                case 1: api.NtProtectVirtualMemory = reinterpret_cast<
                    decltype(api.NtProtectVirtualMemory)>(thunk); break;
                case 2: api.NtWriteVirtualMemory = reinterpret_cast<
                    decltype(api.NtWriteVirtualMemory)>(thunk); break;
                case 3: api.NtCreateUserProcess = reinterpret_cast<
                    decltype(api.NtCreateUserProcess)>(thunk); break;
                case 4: api.NtCreateThreadEx = reinterpret_cast<
                    decltype(api.NtCreateThreadEx)>(thunk); break;
                case 5: api.NtOpenProcess = reinterpret_cast<
                    decltype(api.NtOpenProcess)>(thunk); break;
                case 6: api.NtOpenThread = reinterpret_cast<
                    decltype(api.NtOpenThread)>(thunk); break;
                case 7: api.NtQueueApcThread = reinterpret_cast<
                    decltype(api.NtQueueApcThread)>(thunk); break;
                case 8: api.NtQueueApcThreadEx = reinterpret_cast<
                    decltype(api.NtQueueApcThreadEx)>(thunk); break;
                case 9: api.NtResumeThread = reinterpret_cast<
                    decltype(api.NtResumeThread)>(thunk); break;
                case 10: api.NtQueryInformationProcess = reinterpret_cast<
                    decltype(api.NtQueryInformationProcess)>(thunk); break;
                case 11: api.NtQueryInformationThread = reinterpret_cast<
                    decltype(api.NtQueryInformationThread)>(thunk); break;
                case 12: api.NtTerminateProcess = reinterpret_cast<
                    decltype(api.NtTerminateProcess)>(thunk); break;
                case 13: api.NtClose = reinterpret_cast<
                    decltype(api.NtClose)>(thunk); break;
                case 14: api.NtDelayExecution = reinterpret_cast<
                    decltype(api.NtDelayExecution)>(thunk); break;
                default: break;
                }
            }

            // ---- 3. 整页锁定 RX(写完再执行, 内存属性纪律) ----
            DWORD old = 0;
            if (!VirtualProtect(thunkPage, 4096, PAGE_EXECUTE_READ, &old)) {
                CLEAT_LOGE(ErrDom::Nat, ErrCode::PlatformApiFailed, "Syscall: thunk 页锁 RX 失败 err=" << GetLastError());
                VirtualFree(thunkPage, 0, MEM_RELEASE);
                FreeLibrary(clean);
                return false;
            }

            FreeLibrary(clean);
            g_api = api;
            g_ssns = std::move(ssns);
            g_api.ready = true;
            g_ready = true;
            LOG_INFO("Syscall: 初始化完成, 函数=" << g_ssns.size()
                << " thunk=0x" << std::hex << reinterpret_cast<uintptr_t>(thunkPage));
            return true;
        }

        const NtApi& Api() {
            return g_api;
        }

        bool GetSsn(const char* func, unsigned long& ssn) {
            std::lock_guard<std::mutex> lock(g_mu);
            for (const auto& e : g_ssns) {
                if (e.name == func) {
                    ssn = e.ssn;
                    return true;
                }
            }
            return false;
        }

        bool SelfTest() {
            std::lock_guard<std::mutex> lock(g_mu);
            if (!g_ready) {
                CLEAT_LOGE(ErrDom::Nat, ErrCode::InternalFailed, "Syscall: SelfTest 于初始化前调用");
                return false;
            }

            // ① SSN 与真实 ntdll Zw 查询对齐(同号: NtClose/ZwClose 同 SSN)
            HMODULE real = GetModuleHandleW(L"ntdll.dll");
            if (!real) return false;
            uint32_t zwRva = 0;
            if (!FindExportRva(reinterpret_cast<uint8_t*>(real), "ZwClose", zwRva)) return false;
            const uint8_t* zwStub = reinterpret_cast<const uint8_t*>(real) + zwRva;
            unsigned long zwSsn = 0;
            if (!ParseSsnFromStub(zwStub, 32, zwSsn)) return false;
            unsigned long ntSsn = 0;
            for (const auto& e : g_ssns) {          // 持锁中: 直读本 TU 表, 不得走 GetSsn(会重入 g_mu)
                if (e.name == std::string("NtClose")) { ntSsn = e.ssn; break; }
            }
            if (ntSsn == 0) return false;
            if (zwSsn != ntSsn) {
                CLEAT_LOGE(ErrDom::Nat, ErrCode::SyscallResolveFailed, "Syscall: SelfTest SSN 不对齐 NtClose=" << ntSsn
                    << " ZwClose=" << zwSsn);
                return false;
            }
            LOG_INFO("Syscall: SelfTest SSN 对齐 NtClose=" << ntSsn);

            // ② 间接调用 NtDelayExecution(1ms) → 期望 STATUS_SUCCESS
            //
            // **为何不用 NtClose(无效句柄)**: 被调试进程对无效句柄
            // 的 NtClose 会先由内核 raise user exception(MSDN CloseHandle Remarks
            // 文档化行为), VS first-chance 断住 —— 自检被观测后行为分叉, 且
            // IsDebuggerPresent 规避本身就是可疑特征(EDR 监控点)。
            // NtDelayExecution: 合法参数/无句柄/无异常路径, 同样走完整
            // thunk→syscall 指令链 —— 验证目标一致且调试/直启行为一致。
            LARGE_INTEGER delay{};
            delay.QuadPart = -10000;   // 1ms 相对时间(负值 = 相对当前)
            const NTSTATUS st = g_api.NtDelayExecution(FALSE, &delay);
            if (st == 0) {
                LOG_INFO("Syscall: SelfTest 间接调用通过 (NtDelayExecution 1ms)");
                return true;
            }
            CLEAT_LOGE(ErrDom::Nat, ErrCode::SyscallResolveFailed, "Syscall: SelfTest 间接调用返回 0x" << std::hex << st);
            return false;
        }

    } // namespace native
} // namespace cleat
