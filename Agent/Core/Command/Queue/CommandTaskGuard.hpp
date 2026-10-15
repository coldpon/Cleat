#pragma once
/**
 * @file CommandTaskGuard.hpp
 * @brief 命令投递票据 JobTicket（原 TaskGuard， 任务系统 Phase 1 收敛）
 * @details
 * 由投递方（CommandProcessor）创建并随任务入队，承载"命令 → 任务系统"的三件投递信息：
 *   * `name`        — 登记名（"cmd:<命令名>"，job_list/job_status 的操作员寻址名）；
 *   * `on_timeout`  — 超时回调（回超时错误帧；由 JobRuntime::NoteTimeout 在状态守卫下**至多一次**调用）；
 *   * `on_rejected` — 资源被占用回调（worker 准入路径消费，回 RESOURCE_BUSY）；
 *   * `slot`        — 出生前回填槽：worker 经 `JobRuntime::Spawn` 出生命令 job，
 *                     Spawn 在**起线程之前**把 JobRecord 写进槽位。
 *
 * **迟到响应判据的变化**：旧 `TaskGuard::expired` 是超时状态的第二份
 * 副本（权威在 ExecutionTracker），已删除 —— 迟到的响应改为查 `slot->record->state ==
 * JobState::TimedOut`（单一权威）。记录被登记表裁剪也不影响判据：槽位持 shared_ptr，
 * 在跑线程让对象始终存活。
 *
 * 本结构**不持锁、不判超时、不发帧** —— 超时的判定在 exec 内核（ExecutionTracker），
 * 上报的落实在 JobRuntime，帧的组装在回调自身。它只是一张随任务流动的票据。
 */

#include <functional>
#include <memory>
#include <string>

#include "Core/Job/JobDefs.hpp"

namespace cleat {

    /**
     * @brief 命令投递票据
     */
    struct JobTicket {
        std::string name;                       ///< 登记名（"cmd:<命令名>"）
        std::function<void()> on_timeout;       ///< 超时回调（回超时错误帧）
        std::function<void()> on_rejected;      ///< 资源占用回调（P4-6c: 拒绝而非排队）
        std::shared_ptr<JobRecordSlot> slot;    ///< 出生前回填槽（迟到响应判据经它查 record）
    };

} // namespace cleat
