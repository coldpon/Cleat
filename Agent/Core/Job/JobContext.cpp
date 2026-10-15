#include "Core/Job/JobContext.hpp"

#include "Common/Logger/Logger.hpp"
#include "Core/Job/JobRuntime.hpp"

namespace cleat {

    JobContext::JobContext(std::shared_ptr<JobRecord> rec, std::optional<ThreadGovernor::Token> token)
        : rec_(std::move(rec)), token_(std::move(token)) {}

    bool JobContext::Tick() const {
        if (!rec_->alive.load(std::memory_order_relaxed)) return false;
        if (token_) {
            // 门闸判决: 暂停期在门闸内事件等待(恢复→true / 收尾→false);
            // alive 绑定使"暂停期被 Cancel"在 1s 兜底片内收摊。
            if (!token_->Checkpoint()) return false;
        }
        else if (ThreadGovernor::Stopping()) {
            return false;
        }
        // 再查一次: 覆盖"Checkpoint 返回 true 与 Cancel 并发"的窗口
        return rec_->alive.load(std::memory_order_relaxed);
    }

    bool JobContext::SleepFor(std::chrono::milliseconds ms) const {
        std::unique_lock<std::mutex> lock(rec_->m);
        rec_->cv.wait_for(lock, ms, [this] {
            return !rec_->alive.load(std::memory_order_relaxed) || ThreadGovernor::Stopping();
            });
        return rec_->alive.load(std::memory_order_relaxed) && !ThreadGovernor::Stopping();
    }

    bool JobContext::Cancelled() const {
        return !rec_->alive.load(std::memory_order_relaxed);
    }

    void JobContext::Fail() const {
        rec_->fail_requested.store(true, std::memory_order_release);
    }

    bool JobContext::Paused() const {
        return ThreadGovernor::Paused();
    }

    bool JobContext::Send(const std::vector<uint8_t>& frame) const {
        //  静默自愈(取代 ServiceManager 的"数据回调里 detach 自停"hack): 高带宽
        // **服务**在 https 传输下第一帧数据就地自停 —— 置取消令牌, 循环下一拍收摊。
        // 补丁式兜底, 正常路径是 StartService 的双门拦截。仅服务: 下载等任务在 https
        // 下允许(维持既有语义;  只禁流式服务)。
        if (rec_->kind == JobKind::Service && rec_->high_bandwidth &&
            JobRuntime::HttpsStreamForbidden()) {
            LOG_WARN("JobContext: https 传输, 流服务 '" << rec_->name
                << "' 数据被拒, 自停(#" << rec_->id << ")");
            JobRuntime::Cancel(rec_->id);
            return false;
        }
        return JobRuntime::Emit(frame);
    }

} // namespace cleat
