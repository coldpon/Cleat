#pragma once
/**
 * @file JobStatusHandler.hpp
 * @brief JobStatus: 单任务详情
 * @details
 * 入参(二选一): {"id": 7} 或 {"name": "Audio"}。
 *   * 按 id: 精确一条;
 *   * 按名: 终态历史可能多条 —— 返回**最新**一条并附 `"matches": N`; 找不到为错误。
 * 输出: { "job": {id,name,kind,state,age_ms,timeout_ms,resource,finished},
 *         "matches": N }        // 仅按名查询时出现
 */

#include "Core/Command/Handlers/IHandler.hpp"

namespace cleat {

    class JobStatusHandler : public IHandler {
    public:
        nlohmann::json Execute(const CommandRequest& req) override;
    };

} // namespace cleat
