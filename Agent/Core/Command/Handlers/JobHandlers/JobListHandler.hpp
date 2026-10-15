#pragma once
/**
 * @file JobListHandler.hpp
 * @brief JobList: 全量任务快照 + 线程名册对账差集
 * @details
 * 输出(只增字段, 不改既有字段语义):
 *   {
 *     "jobs": [ {"id","name","kind","state","age_ms","timeout_ms","resource"} ... ],
 *     "roster_mismatch": [ {...} ]   // 对账差集: 见下
 *   }
 * 过滤参数(均可选): {"kind": "task"|"command"|"service"|"infrastructure"}、
 *                  {"state": "running"|"stopped"|"finished"}。
 *
 * **对账差集(roster_mismatch)**: Service/Task 类 job 按设计必须同时出现在两处 ——
 * JobCenter 登记表(控制面)与 ThreadGovernor 名册(政策面)。此处输出"在登记表中
 * Phase 1 只登记命令类(有意不进名册), 差集恒空; Phase 2/3 服务/任务迁入后开始起作用。
 */

#include "Core/Command/Handlers/IHandler.hpp"

namespace cleat {

    class JobListHandler : public IHandler {
    public:
        nlohmann::json Execute(const CommandRequest& req) override;
    };

} // namespace cleat
