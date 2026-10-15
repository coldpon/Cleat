/**
 * @file IInjectionMethod.hpp
 * @brief 注入方法接口 —— 一方法一文件夹, 加手法 = 建目录 + 注册一行, 核心零改动
 * @details
 * 方法声明自己的契约(SupportedPayloads/SupportedTargets), 编排层(handler)只做
 * 「按名查表 + 声明校验」, 校验不过 = 明确拒绝。契约匹配是**操作员的判断**:
 * Agent 不替他选载荷、不替他挑目标。
 *
 * 方法内部纪律(与迁移链同一口径):
 *   * blob 契约 = 第 0 字节即入口、自设栈、无 TLS 依赖(仓库硬约定);
 *   * attach 系(pid)失败不清目标进程的尸, 严禁对非自有进程 NtTerminateProcess;
 *     spawn 系清尸是手法内部职责(OwnsTarget)。
 */
#pragma once

#include "Core/Inject/InjectionTypes.hpp"

#include <vector>

namespace cleat {
namespace injection {

    class IInjectionMethod {
    public:
        virtual ~IInjectionMethod() = default;

        /// 注册表键, snake_case(如 "spawn_apc")
        virtual const char* Name() const = 0;

        /// 支持的载荷形态(操作员声明 payload_kind 不在其中 = 拒绝)
        virtual std::vector<PayloadKind> SupportedPayloads() const = 0;

        /// 支持的目标形态(操作员声明 target_kind 不在其中 = 拒绝)
        virtual std::vector<TargetKind> SupportedTargets() const = 0;

        /// 执行注入。payload = 帧二进制体(方法按 payload_kind 自行解释, 契约外拒绝)
        virtual InjectionResult Inject(const std::vector<uint8_t>& payload,
            PayloadKind payload_kind, const InjectionTarget& target) = 0;
    };

} // namespace injection
} // namespace cleat
