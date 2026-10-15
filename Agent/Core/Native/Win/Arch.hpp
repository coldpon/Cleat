#pragma once
// Arch.hpp — 位数判断工具。
//
// 为什么: 此前全仓没有统一的"位数"判据 —— 反射加载器硬编码拒绝 PE32,
// BOF/COFF 加载需要"COFF Machine 字段 vs 本进程可执行架构", 远程注入
// (二期: 注入既有进程)需要"目标进程位数"(选牺牲镜像 System32/SysWOW64、
// 校验注入目标)。三类消费者共用这里, 不要在别处再写一份。
//
// 口径:
//   * "本进程能执行什么架构的机器码"是**编译期**事实(x64 进程跑不了 x86
//     代码, 反之亦然) —— HostIs64() 是唯一事实源;
//   * "远端进程是多少位"是**运行期**事实(IsWow64Process), 只有 x64 进程
//     问它才有意义(x86 进程管不了 64 位目标, 恒 false)。

#include "Common/Obfuscation/StringObf.hpp"

#ifdef _WIN32

#include "Core/Native/Win/ApiResolver.hpp"

#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <string>

namespace cleat {
    namespace native {

        // ---- COFF Machine 常量(WinNT.h 同名宏的可读引用, COFF 路径自持) ----
        inline constexpr uint16_t kCoffMachineI386 = 0x014C;
        inline constexpr uint16_t kCoffMachineAmd64 = 0x8664;

        /// 本进程可执行架构(编译期事实, 唯一事实源)
        inline constexpr bool HostIs64() {
            return sizeof(void*) == 8;
        }

        /// 目标进程是否 64 位(运行期事实, IsWow64Process)。
        /// x86 进程恒 false(管不了 64 位目标); 判定失败(解析不到 API /
        /// 调用失败)按保守口径 false —— 证据不足不得当 64 位处理。
        inline bool RemoteProcessIs64(HANDLE hProcess) {
            if (!HostIs64()) {
                return false;
            }
            using IsWow64Fn = BOOL(WINAPI*)(HANDLE, PBOOL);
            const auto pIsWow64 = ApiResolver::Proc<IsWow64Fn>(
                OBF_STR("kernel32.dll"), OBF_STR("IsWow64Process"));
            if (!pIsWow64 || !hProcess) {
                return false;
            }
            BOOL wow64 = FALSE;
            if (!pIsWow64(hProcess, &wow64)) {
                return false;
            }
            return wow64 == FALSE;
        }

        /// PE OptionalHeader.Magic 是否与本进程位数一致(PE32=0x10B / PE32+=0x20B)
        inline bool PeMagicMatchesHost(uint16_t magic) {
            return magic == (HostIs64() ? 0x020B : 0x010B);
        }

        /// 读 COFF 头 Machine 字段(blob 不足两个字节返回 0)
        inline uint16_t CoffMachine(const uint8_t* p, size_t n) {
            if (!p || n < 2) {
                return 0;
            }
            return static_cast<uint16_t>(p[0] | (p[1] << 8));
        }

        /// COFF Machine 是否与本进程可执行架构一致; 不一致时 err 给出双方值。
        /// BOF/COFF 加载的第一道闸: 错误架构的机器码进不了本进程, 必须在
        /// 分配任何内存之前拒掉(与反射加载器拒 PE32/PE32+ 同取向)。
        inline bool CoffMachineMatchesHost(const uint8_t* p, size_t n, std::string& err) {
            const uint16_t got = CoffMachine(p, n);
            const uint16_t want = HostIs64() ? kCoffMachineAmd64 : kCoffMachineI386;
            if (got == want) {
                return true;
            }
            char buf[80];
            snprintf(buf, sizeof(buf), "COFF machine %s does not match %s host",
                got == kCoffMachineAmd64 ? "x64" : got == kCoffMachineI386 ? "x86" : "unknown",
                HostIs64() ? "x64" : "x86");
            err = buf;
            return false;
        }

    } // namespace native
} // namespace cleat

#endif // _WIN32
