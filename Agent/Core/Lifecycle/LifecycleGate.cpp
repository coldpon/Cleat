#include "Core/Lifecycle/LifecycleGate.hpp"

#include "Core/Config/AgentConfig.hpp"

#include <chrono>
#include <ctime>

namespace cleat {

    bool LifecycleGate::InWorkWindow(uint32_t start_minute, uint32_t end_minute,
                                     uint32_t now_minute) {
        if (start_minute == end_minute) return true;   // 未启用窗口 = 全天
        if (start_minute < end_minute) {               // 常规窗口 [start, end)
            return now_minute >= start_minute && now_minute < end_minute;
        }
        // 跨午夜窗口(如 22:00-06:00): [start, 24h) ∪ [0, end)
        return now_minute >= start_minute || now_minute < end_minute;
    }

    LifecycleGate::Decision LifecycleGate::Evaluate(uint64_t epoch_s, uint32_t local_minute) {
        const auto lc = AgentConfig::Instance().GetLifecycle();

        // 死亡优先: killdate 到点(UTC epoch 直比, 时区无关)
        if (lc.kill_date_utc != 0 && epoch_s >= lc.kill_date_utc) {
            return Decision::Exit;
        }

        // working hours: 本机时区当日分钟
        if (!InWorkWindow(lc.work_start_minute, lc.work_end_minute, local_minute)) {
            return lc.outside_exit ? Decision::Exit : Decision::Pause;
        }
        return Decision::Run;
    }

    LifecycleGate::Decision LifecycleGate::Evaluate() {
        const auto now = std::chrono::system_clock::now();
        const uint64_t epoch_s = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count());
        std::tm tm{};
        const std::time_t t = std::chrono::system_clock::to_time_t(now);
#ifdef _WIN32
        localtime_s(&tm, &t);
#else
        localtime_r(&t, &tm);
#endif
        const uint32_t local_minute = static_cast<uint32_t>(tm.tm_hour) * 60 + tm.tm_min;
        return Evaluate(epoch_s, local_minute);
    }

} // namespace cleat
