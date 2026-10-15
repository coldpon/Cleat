#pragma once
// CoffLoader.hpp — COFF/BOF 加载执行。
// 重定位口径参考 AdaptixC2 beacon/bof_loader.cpp(字段布局与重定位类型集合);
// 输出/上下文模型为本项目独立设计(per-execution 缓冲, 见 BofApi.hpp)。

#include <string>
#include <vector>

#ifdef _WIN32

namespace cleat {
    namespace coff {

        struct RunResult {
            bool ok = false;
            std::string error;             ///< 校验/重定位/入口失败的可读原因
            std::string output;            ///< BeaconOutput/BeaconPrintf 收集
            bool output_truncated = false; ///< 输出超过 kMaxOutputBytes 被截断
        };

        /// 同步加载并执行 BOF: 校验架构 → 映射节区 → 重定位 → 找入口 →
        /// 以 (args, args_len) 调用 → 收集输出 → 释放。
        ///
        /// **阻塞当前线程直到 go() 返回** —— 调用方必须处于 CommandWorker 的
        /// 护栏线程(超时由 TaskGuard/reaper 兜底; 慢 BOF 用命令参数
        /// timeout_ms 放宽本任务超时, 见 BofHandler.hpp)。
        ///
        /// 已知边界: go() 挂死时本函数不返回, 线程被 reaper 遗弃后其节区
        /// 内存随线程滞留(与 Adaptix/CS 同代价; 强杀线程 = 泄漏且不可控)。
        RunResult Execute(const std::vector<uint8_t>& blob,
            const std::string& entry,
            const std::vector<uint8_t>& args);

    } // namespace coff
} // namespace cleat

#endif // _WIN32
