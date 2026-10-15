/**
 * @file MachineIdentity.hpp
 * @brief N44/身份重构②: 本机身份生成器
 * @details
 * machine_id = SHA256(第一个可读源 + 固定盐) 前 16 字节 hex(32 字符)。
 *
 * 规则(定稿):
 *   - 按平台优先级取**第一个**读到的源, **不拼接**(旧实现的 guid@hostname
 *     拼接使 hostname 进入身份 —— 同名机器/改名即漂移, 移除)。
 *   - 固定盐经 StringObf 混淆(防产物内直接检索盐值)。
 *   - **不落地文件**(持久化随机是"捡了芝麻丢了西瓜", 已裁定不做);
 *     全源读不到(概率极低) → SHA256(hostname+盐), 接受可能不稳定。
 *   - 服务器认不出(mid 变化)在开发阶段可接受, 旧会话记录直接删。
 *
 * 数据流: MachineIdentity 生成**上报值**(FirstConnect 帧内) → 服务端以此归并
 * 会话 → confirm 下发的正式 machine_id 由 SessionIdentity 缓存 —— 单向, 不回写。
 */
#pragma once

#include "Core/Identity/Platform/IMachineIdOps.hpp"

#include <memory>
#include <string>

namespace cleat {
namespace identity {

    /// 平台工厂(Platform/ 下实现按条件编译; AgentContext 装配调用)
    std::unique_ptr<IMachineIdOps> CreateMachineIdOps();

    class MachineIdentity {
    public:
        explicit MachineIdentity(IMachineIdOps& ops);

        /**
         * @brief 生成本机 machine_id(确定性: 同源同结果; 幂等可重复调用)
         * @return 32 字符 hex(SHA256 前 16 字节)
         */
        std::string Compute() const;

    private:
        IMachineIdOps& ops_;
    };

} // namespace identity
} // namespace cleat
