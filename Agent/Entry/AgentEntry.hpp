/**
 * @file AgentEntry.hpp
 * @brief Agent公共入口声明（形态无关）
 * 
 * 业务核心入口：参数解析 + 崩溃兜底 + Agent启动，由各形态入口文件调用：
 * - EntryExe.cpp (main / WinMain) 
 * - EntryDll.cpp (DllMain + 导出 StartW, rundll32)
 * 
 * 形态切换在 CMake(option CLEAT_AS_DLL)，本文件零平台代码。
 */

namespace cleat {

    /**
     * @brief 启动Agent（阻塞，直到退出信号）
     * @param argc 命令行参数个数
     * @param argv 命令行参数数组
     * @return 进程退出码: 0=正常(外部停止/服务端退出请求); 1=未捕获异常;
     *         3=交付构建加密不可用(拒绝建连); 4=生命周期到点(killdate/窗外退出
     *         策略, 计划内终止 —— 守护脚本据此判断勿再重启)
     *
     * @note argc/argv 语义与 main 一致
     */
    int RunAgent(int argc, char** argv);

} // namespace cleat
