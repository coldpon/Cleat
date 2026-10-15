/**
 * @file ReflectiveLoad.hpp
 * @brief reflective_load 手法: 反射加载(裸 PE DLL)
 * @details
 * 载荷 = NativeDll(裸 x64 PE32+ DLL); 载荷准备层(RemoteMapper)把它在线打包成
 * [boot][loader][PE] 反射加载 shellcode(ShellcodeLoader 同源 loader, 目标内
 * 完成段映射/重定位/IAT/DllMain), 随后按目标走 APC 触发: spawn 目标 =
 * CreateProcessW 挂起 + early-bird APC + Resume(失败清尸); pid 目标 =
 * NtOpenProcess + 挂起远线程 + APC + Resume(attach 语义, 失败不清尸)。
 * shellcode 请用 apc_injection / create_remote_thread。
 */
#pragma once

#include "Core/Inject/IInjectionMethod.hpp"

namespace cleat {
namespace injection {

    class ReflectiveLoad : public IInjectionMethod {
    public:
        const char* Name() const override { return "reflective_load"; }
        std::vector<PayloadKind> SupportedPayloads() const override {
            return { PayloadKind::NativeDll };
        }
        std::vector<TargetKind> SupportedTargets() const override {
            return { TargetKind::Spawn, TargetKind::Pid };
        }
        InjectionResult Inject(const std::vector<uint8_t>& payload,
            PayloadKind payload_kind, const InjectionTarget& target) override;
    };

} // namespace injection
} // namespace cleat
