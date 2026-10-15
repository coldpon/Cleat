/**
 * @file Schedulers.cpp
 * @brief 会话侧调度器实现
 */

#include "Core/Comms/Session/Schedulers.hpp"

#include "Core/Config/AgentConfig.hpp"

namespace cleat {

    // ---------------------------- HeartbeatScheduler ----------------------------

    void HeartbeatScheduler::Reset(TimePoint now) {
        last_activity_ = now;
        next_ = now;   // 连接成功后立即心跳一次(与合一前一致)
    }

    void HeartbeatScheduler::NoteActivity(TimePoint now) {
        last_activity_ = now;
    }

    bool HeartbeatScheduler::Due(TimePoint now) {
        if (now < next_) return false;
        // 到点前同步配置: SetConfig 下发的心跳域即时生效(AgentConfig 每次读)
        policy_.UpdateConfig(AgentConfig::Instance().GetHeartbeat());
        const bool active = (now - last_activity_) <
            std::chrono::milliseconds(policy_.GetConfig().activity_window_ms);
        next_ = now + std::chrono::milliseconds(policy_.NextIntervalMs(active));
        return true;
    }
    bool HeartbeatScheduler::ShouldSkipBeat() {
        return policy_.ShouldSkipBeat();
    }

    /// 当前策略配置(按值返回 —— 不得写回引用: policy_.GetConfig() 返回的是拷贝, 绑引用即悬垂)
    HeartbeatConfig HeartbeatScheduler::Config() const { 
        return policy_.GetConfig();
    }

    /// 注入种子(夹具确定性; 0 = 随机)
    void HeartbeatScheduler::Seed(uint32_t seed) { 
        policy_.Seed(seed);
    }
} // namespace cleat
