/**
 * @file EntryExe.cpp
 * @brief EXE 形态入口
 * @details
 * Debug 构建使用 main（控制台，便于调试输出）；
 * Release 构建使用 WinMain（无窗口，/SUBSYSTEM:WINDOWS 由 CMake 按 Release 配置设置）。
 *
 * 两种入口统一取 CRT 解析好的 __argc/__argv（MSVC 特有；Linux 走原生 main）。
 */

#include "AgentEntry.hpp"
#include "Core/Concurrency/ThreadGovernor.hpp"
#include <cstdlib>   // __argc/__argv (MSVC)
#include <csignal>   // SIGPIPE/SIGINT/SIGTERM (Linux)

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "Core/Mask/MaskPark.hpp"   // 掩码前提检查( 2; CLEAT_WITH_MASK=OFF 时为空实现)

namespace {
    /**
     * @brief EXE 形态的掩码前提检查
     * @details DllMain 路径已在 DLL_PROCESS_ATTACH 检查; EXE 无 DllMain(DLL_THREAD_*
     *          通知对 EXE 本就不适用), 但**本 EXE 镜像的静态 TLS 回调数组**同样在线程
     *          生灭时被加载器直接调用 → 掩码窗口期 = 执行密文。命中即置 taint,
     *          掩码判定 fail-closed(拒绝窗口, 功能降级但不崩)。
     */
    inline void MaskPreconditionsCheck() {
        if (cleat::MaskPark::DetectTlsCallbacks()) {
            cleat::MaskPark::MarkThreadNotifyUnsafe();
        }
    }
}

#if defined(NDEBUG)
 /**
  * @brief Release 构建的 GUI 入口（无控制台窗口）
  * @param hInstance      当前实例句柄（未使用）
  * @param hPrevInstance  前一个实例句柄（未使用）
  * @param lpCmdLine      命令行字符串（未使用，使用 CRT 解析的 __argv）
  * @param nCmdShow       窗口显示方式（未使用）
  * @return RunAgent 的返回值
  *
  * MSVC CRT 在 WinMain 下同样初始化 __argc/__argv（来自 GetCommandLine）。
  */
int WINAPI WinMain(_In_ HINSTANCE hInstance, _In_opt_ HINSTANCE hPrevInstance, _In_ LPSTR lpCmdLine, _In_ int nCmdShow) {
    MaskPreconditionsCheck();
    return cleat::RunAgent(__argc, __argv);
}
#else
 /**
  * @brief Debug 构建的控制台入口
  * @param argc 参数个数（CRT 解析）
  * @param argv 参数数组（CRT 解析）
  * @return RunAgent 的返回值
  */
int main(int argc, char** argv) {
    MaskPreconditionsCheck();
    return cleat::RunAgent(argc, argv);
}
#endif

#else
 // Linux：统一 main 入口（无 WinMain 概念）
 /**
  * @brief Linux 平台的主入口
  * @param argc 参数个数
  * @param argv 参数数组
  * @return RunAgent 的返回值
  */
namespace {
    // 信号处理器: 只做一次锁自由原子写(async-signal-safe) —— 不得 lock/notify, 不得触碰
    // 魔法静态。原实现调 SessionIdentity::Instance()(函数内静态单例, 严格说非 signal-safe),
    // 现直接写 Governor 的三态字(锁自由原子, 与旧 g_stopping.store 同类操作): 单变量在
    // 信号路径上也不破, **正确性不依赖主循环并轨**(并轨只承担 identity 镜像与 notify 提速)。
    extern "C" void CleatSignalHandler(int) {
        cleat::ThreadGovernor::SetStoppingSignalSafe();
    }
}

int main(int argc, char** argv) {
    // SIGPIPE: 忽略 —— 网络层 send 已带 MSG_NOSIGNAL, 此处双保险覆盖第三方库/子进程管道
    ::signal(SIGPIPE, SIG_IGN);
    // SIGINT/SIGTERM: 优雅退出(走 ClientContext 退出标志与会话清理), 而非直接终止
    struct sigaction sa {};
    sa.sa_handler = CleatSignalHandler;
    ::sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    ::sigaction(SIGINT, &sa, nullptr);
    ::sigaction(SIGTERM, &sa, nullptr);
    return cleat::RunAgent(argc, argv);
}
#endif