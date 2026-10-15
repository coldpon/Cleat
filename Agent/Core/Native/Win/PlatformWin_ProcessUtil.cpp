#include "Core/Native/Win/ProcessUtil.hpp"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

// 导入表脱敏: ApiResolver(内含 windows.h)置于 LEAN 定义之后
#include "Core/Native/Win/ApiResolver.hpp"

namespace cleat {
    namespace native {

        static const auto pOpenProcess =
            cleat::ApiResolver::Proc<HANDLE(WINAPI*)(DWORD, BOOL, DWORD)>(
                OBF_STR("kernel32.dll"), OBF_STR("OpenProcess"));
        static const auto pGetExitCodeProcess =
            cleat::ApiResolver::Proc<BOOL(WINAPI*)(HANDLE, LPDWORD)>(
                OBF_STR("kernel32.dll"), OBF_STR("GetExitCodeProcess"));
        static const auto pCloseHandle =
            cleat::ApiResolver::Proc<BOOL(WINAPI*)(HANDLE)>(
                OBF_STR("kernel32.dll"), OBF_STR("CloseHandle"));

        bool IsProcessAlive(unsigned long pid) {
            if (pid == 0 || !pOpenProcess || !pGetExitCodeProcess || !pCloseHandle) {
                return false;
            }
            // QUERY_LIMITED_INFORMATION: 抬不到完整权限也能打开大多数进程;
            // 打不开通常意味着已退出(或受保护) —— 均返回 false。
            HANDLE h = pOpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE,
                static_cast<DWORD>(pid));
            if (!h) return false;
            DWORD code = 0;
            // 误放行(源实例自退丢会话)。失败方向保守(打不开=不活), 彻底修复需
            // 核对镜像名/创建时间, 排期另做。
            const bool alive = pGetExitCodeProcess(h, &code) && code == STILL_ACTIVE;
            pCloseHandle(h);
            return alive;
        }

    } // namespace native
} // namespace cleat
