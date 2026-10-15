/**
 * @file CrashGuard.hpp
 * @brief 进程/线程级崩溃兜底（跨平台）
 * @details
 * 提供统一的崩溃捕获和自重启机制，平台差异收敛在本目录下。
 * 设计契约：
	 * 平台差异收敛在本目录 —— PlatformWin_*.cpp/PlatformLinux_*.cpp 由 CMake 按文件名过滤编译，
	 * Core 其余代码与 Agent 入口只依赖本接口；
	 * -所有堆分配只在 InstallCrashGuard（正常上下文）完成，崩溃路径零分配，
	 * 堆损坏时依然可以完成转储与自重启；
	 * -自重启带 --after-crash 参数，已带该参数则不再重启（防崩溃循环）；
	 * -崩溃日志（minidump/标记文件）按**构建期**决定（实测 ,
	 * 用户裁定）: Debug 构建恒开便于排障, Release 构建恒关（零取证痕迹,
	 * 自重启兜底不受影响）。Windows 侧另有 fast-fail 族钩子（invalid-parameter/
	 * purecall/SIGABRT/terminate）兜住 UEF 收不到的死亡形态。
	 *
* */

#pragma once
#include <functional>
#include <string>

	 namespace cleat::Platform {

	 /**
	  * @brief 获取默认转储目录
	  * @return 默认转储目录路径字符串
	  * @details
	  * 通常为可执行文件所在目录下的 "crashes" 子目录。
	  */
	 std::string GetDefaultDumpDir();

	 /**
	  * @brief 安装进程级崩溃兜底
	  * @param dumpDir 崩溃转储文件存放目录，若为空则使用默认目录
	  * @details
	  * 应在 main 函数最前调用。安装后，未处理硬件异常（访问违例、栈溢出、非法指令等）
	  * 会触发崩溃处理流程：写 minidump（Windows）或崩溃标记，并以 --after-crash 参数自重启。
	  * 若进程已经带有 --after-crash 参数，则不再重启（防止无限循环）。
	  */
	 void InstallCrashGuard(const std::string& dumpDir);

	 /**
	  * @brief 检查并消费上次崩溃信息
	  * @param dumpDir 转储目录，若为空则使用默认目录
	  * @return 上次崩溃的标记内容，若无则返回空字符串
	  * @details
	  * 启动时调用此函数检查上次是否异常退出，若有则返回标记内容并自动清除标记文件。
	  */
	 std::string ConsumeLastCrashInfo(const std::string& dumpDir);

	 /**
	  * @brief 在线程内执行带有崩溃兜底的函数
	  * @param fn 要执行的可调用对象
	  * @details
	  * 如果 fn 内发生硬件异常，将触发崩溃处理（转储 + 自重启），函数不返回。
	  * Windows 下使用 SEH __try/__except 捕获异常；
	  * Linux 下由进程级信号处理函数兜底，本函数直接执行 fn。
	  */
	 void RunWorkerGuarded(const std::function<void()>& fn);

 } // namespace cleat::Platform