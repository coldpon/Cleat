/**
 * @file ApcInjection.hpp
 * @brief apc_injection 手法: APC 触发(shellcode)
 * @details
 * 触发 = APC(early-bird): spawn 目标 = CreateProcessW(CREATE_SUSPENDED) 挂起
 * 主线程 + NtQueueApcThread + Resume(即 native::SpawnAndInject, 失败清尸);
 * pid 目标 = NtOpenProcess + 挂起远线程 + APC + Resume。载荷契约 = Blob
 * (第 0 字节即入口); 裸 DLL 请用 reflective_load。
 */
#pragma once

#include "Core/Inject/IInjectionMethod.hpp"

namespace cleat {
namespace injection {

    class ApcInjection : public IInjectionMethod {
    public:
        const char* Name() const override { return "apc_injection"; }
        std::vector<PayloadKind> SupportedPayloads() const override {
            return { PayloadKind::Blob };
        }
        std::vector<TargetKind> SupportedTargets() const override {
            return { TargetKind::Spawn, TargetKind::Pid };
        }
        InjectionResult Inject(const std::vector<uint8_t>& payload,
            PayloadKind payload_kind, const InjectionTarget& target) override;
    };

} // namespace injection
} // namespace cleat
