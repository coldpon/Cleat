#pragma once
/**
 * @file JobContext.hpp
 * @brief 任务函数可见的全部运行时面
 * @details
 * 门闸判决 + 取消令牌 + (Phase 2 起)出站包装, 都经这一个对象送达业务循环 ——
 * **业务代码不再直接接触 ThreadGovernor**。治理从"公共契约"降为"出生口自带的器官":
 * 经 `JobRuntime::Spawn` 出生的每个非 Command 类 job 自动 Attach 进线程名册,
 * 且取消令牌已绑定为门闸的 alive 判据(暂停期 Cancel 也在兜底片内收摊, P5 类别②)。
 *
 * 用法(任务/服务的标准循环形):
 *   while (ctx.Tick()) {            // 门闸: 暂停在此事件等待; 收尾/取消 → false
 *       if (!ctx.SleepFor(1s)) break;   // 可打断睡眠: 取消/收尾立即醒
 *       ... 本拍工作 ...
 *   }
 *
 * 命令类(JobKind::Command)**有意不接暂停**: 同步 handler 没有协作点,
 * 其静默由"派发循环不开新工 + OutboundSink 出口拒发 + deadline"三件套保证。
 * 命令函数仍可持有本对象(取消查询可用), 但不得依赖 Tick 的暂停等待。
 */

#include <chrono>
#include <memory>
#include <optional>
#include <string>

#include "Core/Concurrency/ThreadGovernor.hpp"
#include "Core/Job/JobDefs.hpp"

namespace cleat {

    class JobContext {
    public:
        /// token: 非 Command 类的门闸注册句柄(出生时 Attach); Command 类传 nullopt。
        /// Token 的默认构造是私有的(只有 Attach 能给), 故用 optional 的"未持有"表达空。
        JobContext(std::shared_ptr<JobRecord> rec, std::optional<ThreadGovernor::Token> token);

        JobContext(const JobContext&) = delete;
        JobContext& operator=(const JobContext&) = delete;

        uint64_t id() const { return rec_->id; }
        const std::string& name() const { return rec_->name; }
        JobKind kind() const { return rec_->kind; }

        /**
         * @brief 每拍判决(循环顶部)。
         * @return false = 收摊, 循环须立即 break; true = 继续本拍工作。
         * @details 非 Command 类经门闸 `Checkpoint`(暂停期事件等待, 恢复放行);
         *          Command 类无门闸绑定, 只查全局收尾与自身取消。
         */
        bool Tick() const;

        /**
         * @brief 可打断睡眠: 睡满 ms, 或在本 job 被取消 / 全局收尾时**立即醒**。
         * @return false = 已被取消或收尾(调用方应立即收摊); true = 正常(睡满或例行醒来)。
         */
        bool SleepFor(std::chrono::milliseconds ms) const;

        /// 本 job 是否已被取消(kill / 超时遗弃后的清场请求)
        bool Cancelled() const;

        /**
         * @brief 申报业务失败: fn 仍正常返回, 但终态落 **Failed** 而非 Completed。
         * @details 真机冒烟发现: "文件不可读 → error 帧 → return" 的下载
         *          job 曾落 Completed —— 发了 error 帧的失败不是成功。幂等, 任意线程。
         */
        void Fail() const;

        /// 纯查询不阻塞: 全局暂停中返回 true(丢弃型接入点据此丢弃采集, 不积不存)
        bool Paused() const;

        /**
         * @brief 本 job 的出站唯一口(Phase 2)。
         * @return 出口的 bool(背压契约 B2-6: false = 未发出/积压, 调用方决定重试或放弃)。
         * @details 经 JobRuntime::Emit 路由到装配注入的数据出口; 途中即有 OutboundSink
         *          的静默守卫与限流(出口链路不变)。命令类不用它(响应走 handler 返回)。
         */
        bool Send(const std::vector<uint8_t>& frame) const;

    private:
        std::shared_ptr<JobRecord> rec_;
        /// 门闸注册句柄(仅非 Command 类持有; Command 为 nullopt)
        std::optional<ThreadGovernor::Token> token_;
    };

} // namespace cleat
