/**
 * @file BofHandler.hpp
 * @brief BofExec: 同步执行 BOF(帧二进制体 = COFF; params = entry / args_b64 / timeout_ms)
 *
 * 执行模型:
 *   * 跑在 CommandWorker 的护栏线程上, 同步等 go() 返回 —— 与其它命令
 *     同一条链路, 无任何特殊线程安排;
 *   * **不覆写 ResourceOf()** —— BOF 不是共享资源, 声明 key 会让遗弃线程
 *     攥着 key 挡住后续 BOF(对照: KeylogStart 的 InputHook 才是真共享
 *     全局单例, 见 ExecutionTracker 的 key 持有语义);
 *   * 输出 per-execution 收集(见 BofApi.hpp), base64 后走正常响应回传
 *     (BOF 输出可能是任意字节, 不宜直接进 JSON 字符串);
 *   * 慢 BOF(内存 Dump/凭据抓取)用 params.timeout_ms 放宽**本任务**超时
 *     (CommandProcessor 读取 → TaskItem 透传 → LaunchGuarded 采用),
 *     缺省 = 全局 worker 超时。
 */

#pragma once
#include "Core/Command/Handlers/IHandler.hpp"

namespace cleat {

    class BofHandler : public IHandler {
    public:
        nlohmann::json Execute(const CommandRequest& req) override;
    };

} // namespace cleat
