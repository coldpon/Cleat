/**
 * @file PlatformLinux_MaskRuntime.cpp
 * @brief 睡眠掩码运行时 —— Linux 桩实现（不支持）
 * @details
 * 睡眠掩码为 Windows 专属技术（Ekko/TimerObf 家族依赖 ntdll 定时器 + NtContinue 上下文链）。
 * Linux 侧不提供实现，`Supported()` 恒 false，`SelfTest` 恒返回 false（err 说明）。
 */
#include "Core/Mask/MaskRuntime.hpp"

namespace cleat {
namespace {

    class MaskRuntimeUnsupported final : public IMaskRuntime {
    public:
        bool Supported() const override { return false; }

        bool SelfTest(bool /*execute*/, uint32_t /*windowMs*/,
            const std::vector<uint32_t>& /*allowTids*/,
            MaskSelfTestReport& out, std::string& err) override {
            out.supported = false;
            out.note = "睡眠掩码仅 Windows 平台可用";
            err = "mask runtime: Windows only";
            return false;
        }
    };

} // namespace

std::unique_ptr<IMaskRuntime> CreateMaskRuntime() {
    return std::make_unique<MaskRuntimeUnsupported>();
}

} // namespace cleat
