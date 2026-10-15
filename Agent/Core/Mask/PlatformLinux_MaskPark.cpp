/**
 * @file PlatformLinux_MaskPark.cpp
 * @brief 停靠协议 —— Linux 桩(掩码窗口是 Windows 专属能力, Linux 侧恒不可用)
 * @details
 * 接口仍给全(链接面闭合): 参与者可注册(记录 tid, 供普查名单一致的观测), 但
 * `Session::Enter` 恒 false、`ParticipantCheckpoint` 恒空 —— Linux 的
 * `CreateMaskRuntime()` 返回 unsupported, MaskGate 在停靠之前就会拒绝,
 * 本文件只是让"接入点无条件编译"的纪律在 Linux 构建下同样成立。
 */
#include "Core/Mask/MaskPark.hpp"

#ifdef CLEAT_WITH_MASK

#include <pthread.h>

#include <atomic>
#include <vector>

namespace cleat {
namespace MaskPark {
namespace {

    constexpr size_t kSlotCount = 8;
    std::atomic<uint64_t> g_slots[kSlotCount] = {};

    uint64_t SelfKey() {
        return static_cast<uint64_t>(reinterpret_cast<uintptr_t>(pthread_self()));
    }

} // namespace

    Registration::Registration() {
        const uint64_t self = SelfKey();
        for (auto& slot : g_slots) {
            uint64_t expected = 0;
            if (slot.compare_exchange_strong(expected, self)) {
                return;
            }
        }
    }

    Registration::~Registration() {
        const uint64_t self = SelfKey();
        for (auto& slot : g_slots) {
            uint64_t expected = self;
            if (slot.compare_exchange_strong(expected, 0)) {
                return;
            }
        }
    }

    std::vector<uint32_t> RegisteredTids() {
        // Linux 掩码不可用, tid 名单无消费者; 返回空保持"名单 ⊆ 空集"的平凡安全
        return {};
    }

    bool Session::Enter(uint32_t) {
        return false;
    }

    Session::~Session() {}

    void ParticipantCheckpoint() {}

} // namespace MaskPark
} // namespace cleat

#endif // CLEAT_WITH_MASK
